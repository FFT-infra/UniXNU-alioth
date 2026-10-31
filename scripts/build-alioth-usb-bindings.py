#!/usr/bin/env python3
"""Bind AliothUSBDevice to the prelinked IOUSBDeviceController symbols of this board's kernel.

Why this exists at all: `IOUSBDeviceFamily.kext` is prelinked, not linked. `scripts/unixnu/
kernel.py` links the kernel body at :204 and merges the prelinked kexts at :207, so at link time
`__ZTV21IOUSBDeviceController` does not exist anywhere in the link inputs. Our driver subclasses
that class, so the compiler emits ordinary relocations against it and the link fails with
"symbol(s) not found" -- the A-5 run of 2026-10-31 05:38. Adding the kext to `[kernel.prelink]
roots` puts it in the image but not in the link, which is why both steps were needed and why
step one alone did not help.

The way out is the same one `scripts/build-sheng-hid-input.py` and
`scripts/build-storage-bindings.py` use: write an assembly file whose `.set` directives give each
name the absolute address it has after prelink, assemble it, and add that object to the driver
archive. No C++ changes.

Two address sources, and they must not be confused (the evidence note §4.4 got this right):

  vtable slots   a method's address is the *contents* of its slot in the original vtable, at
                 `vtable_base + 16 + slot * 8`. The slot holds the function entry point.
  fixed          a vtable or a metaclass object is a data address; a constructor or destructor is
                 a code address that appears in no vtable slot at all (`brk #1` fills slot 0/1).

Every address in TABLE below is measured on iPadOS 17.7.11 / 21H461 and recorded with its
derivation in docs/evidence/2026-10-31-a5-entry-plan.md §4.4. It is written out here on purpose:
the evidence file is for people, this script has to reproduce the table with no other input.

  scripts/build-alioth-usb-bindings.py --check
  scripts/build-alioth-usb-bindings.py --component AliothUSBDevice --archive libAliothUSBDevice.a --directory build/alioth-usb

--check needs no build tree: it prints the table and refuses a malformed one. The build form runs
inside `unixnu build kernel` as a `[kernel] archives` step, after build-alioth-usb.py has produced
the object, and it refuses to archive a binding set that does not cover every import the object
actually has -- see the note at the end of `main`.
"""
import argparse
import re
import struct
import subprocess
import sys
from pathlib import Path

# --- the measured table -----------------------------------------------------------------------
# Addresses are absolute post-prelink addresses. `kind` decides the alignment that is physically
# possible, not a formatting preference: a vtable or a metaclass object is pointer data and is
# 8-byte aligned; an A64 function entry is 4-byte aligned and in this image is often 4 mod 8.
# Requiring 8 bytes of every entry would reject 10 of the 20 real addresses.
#
# `slot` is recorded for the 16 vtable entries so the table can be re-derived from the image
# (vtable_base + 16 + slot * 8 holds this address). None means the address came from code/data
# rather than a slot; the derivation column says where.
DATA, CODE = 'data', 'code'
VTABLE = 0xfffffff006e81da0          # IOUSBDeviceController's own vtable, 234 slots

TABLE = [
    # symbol,                                        address,             kind,  slot, derivation
    ('__ZTV21IOUSBDeviceController',                  0xfffffff006e81da0, DATA,  None, 'vtable itself'),
    ('__ZN21IOUSBDeviceControllerC2EPK11OSMetaClass', 0xfffffff00617ba04, CODE,  None, 'store of 0xe81da0, unique hit'),
    ('__ZN21IOUSBDeviceControllerD2Ev',               0xfffffff00617ba24, CODE,  None, 'subclass dtor chain -> GOT[0x120]'),
    ('__ZN21IOUSBDeviceController10gMetaClassE',      0xfffffff007b1a4c8, DATA,  None, 'getMetaClass at 0x617ba30'),
    ('__ZNK21IOUSBDeviceController10getAddressEv',    0xfffffff00617db10, CODE,  171,  'slot content'),
    ('__ZN21IOUSBDeviceController15createPipeGatedER21tCreatePipeParametersRP15IOUSBDevicePipe',
                                                      0xfffffff00617d978, CODE,  172,  'slot content'),
    ('__ZN21IOUSBDeviceController17activatePipeGatedEP15IOUSBDevicePipe',
                                                      0xfffffff00617da04, CODE,  173,  'slot content'),
    ('__ZN21IOUSBDeviceController19deactivatePipeGatedEP15IOUSBDevicePipe',
                                                      0xfffffff00617dad4, CODE,  174,  'slot content'),
    ('__ZN21IOUSBDeviceController21handleUserlandCommandEP8OSStringP8OSObject',
                                                      0xfffffff00617ba9c, CODE,  178,  'slot content'),
    ('__ZN21IOUSBDeviceController16handleUSBSuspendEv', 0xfffffff006181b08, CODE, 195,  'slot content'),
    ('__ZN21IOUSBDeviceController23handleUSBConnectionDoneEv',
                                                      0xfffffff006182518, CODE,  197,  'slot content'),
    ('__ZN21IOUSBDeviceController15handleUSBResumeEv', 0xfffffff006182734, CODE, 198,  'slot content'),
    ('__ZN21IOUSBDeviceController21handleUSBCableConnectEv',
                                                      0xfffffff00618285c, CODE,  204,  'slot content'),
    ('__ZN21IOUSBDeviceController24handleUSBCableDisconnectEv',
                                                      0xfffffff006181f14, CODE,  205,  'slot content'),
    ('__ZN21IOUSBDeviceController21getMaxSpeedCapabilityEv',
                                                      0xfffffff00617de78, CODE,  207,  'slot content'),
    ('__ZN21IOUSBDeviceController16getDeviceAddressEv', 0xfffffff00617e434, CODE, 209,  'slot content'),
    ('__ZN21IOUSBDeviceController31handleControlRequestStateChangeEjj',
                                                      0xfffffff006180860, CODE,  216,  'slot content'),
    ('__ZN21IOUSBDeviceController27createControlRequestTrackerEv',
                                                      0xfffffff00617bc84, CODE,  224,  'slot content'),
    ('__ZN21IOUSBDeviceController2ioEiP18IOMemoryDescriptoryPvPyj',
                                                      0xfffffff00617e8b8, CODE,  226,  'slot content'),
    ('__ZN21IOUSBDeviceController15allocateRequestEv', 0xfffffff00617eb10, CODE, 228,  'slot content'),
]

# The kernel image window. Static kernel text, data and the kexts merged into it all sit here;
# heap and pageable ranges start above 0xfffffff1..., so this window is also what separates a
# kernel address from an object address.
KERNEL_LO, KERNEL_HI = 0xfffffff000000000, 0xfffffff100000000

EXPECTED_COUNT = 20


def check_table():
    """Validate the table's shape. Returns the list of problems; empty means it is well formed."""
    problems = []
    if len(TABLE) != EXPECTED_COUNT:
        problems.append('%d entries, expected %d' % (len(TABLE), EXPECTED_COUNT))
    names = [row[0] for row in TABLE]
    for name in sorted({n for n in names if names.count(n) > 1}):
        problems.append('duplicate symbol %s' % name)
    for name, address, kind, slot, _ in TABLE:
        if not re.fullmatch(r'0x[0-9a-f]{9,16}', hex(address)):
            problems.append('%s: %r is not a hexadecimal address' % (name, address))
            continue
        if not KERNEL_LO <= address < KERNEL_HI:
            problems.append('%s: %#x is outside the kernel image window [%#x, %#x)'
                            % (name, address, KERNEL_LO, KERNEL_HI))
        # Alignment follows the kind of thing the address names, see the note above.
        align = 8 if kind == DATA else 4
        if address % align:
            problems.append('%s: %#x is not %d-byte aligned (%d mod %d), and it is %s'
                            % (name, address, align, address % align, align, kind))
        if kind == DATA and slot is not None:
            problems.append('%s: a vtable slot holds code, not %s' % (name, kind))
        if slot is not None and not 0 <= slot < 234:
            problems.append('%s: slot %d is outside the 234-slot layout' % (name, slot))
    # A vtable slot address must be reachable from the vtable: base + 16 + slot * 8.
    for name, address, kind, slot, _ in TABLE:
        if slot is not None and not VTABLE + 16 <= VTABLE + 16 + slot * 8 < VTABLE + 16 + 234 * 8:
            problems.append('%s: slot %d lies outside the vtable' % (name, slot))
    return problems


def print_table():
    for name, address, kind, slot, derivation in TABLE:
        where = 'slot %-3d' % slot if slot is not None else '%-7s' % kind
        print('%-4s %s -> %#x   (%s)' % ('', name, address, where))


def object_imports(path):
    """Every IOUSBDeviceController name the object leaves undefined, from its own symbol table."""
    data = path.read_bytes()
    if data[:4] != b'\xcf\xfa\xed\xfe':
        raise SystemExit('%s is not a Mach-O object' % path)
    commands = []
    ncmds = struct.unpack_from('<I', data, 16)[0]
    at = 32
    for _ in range(ncmds):
        cmd, size = struct.unpack_from('<2I', data, at)
        commands.append((cmd, at, size))
        at += size
    symtab = next((c for c in commands if c[0] == 2), None)
    if symtab is None:
        raise SystemExit('%s has no symbol table' % path)
    symoff, nsyms, stroff, _ = struct.unpack_from('<4I', data, symtab[1] + 8)
    names = []
    for index in range(nsyms):
        strx, kind, _, _, _ = struct.unpack_from('<IBBHQ', data, symoff + index * 16)
        if kind != 1:                                  # N_UNDF: imported, not defined here
            continue
        name = data[stroff + strx:data.index(b'\0', stroff + strx)].decode()
        if '21IOUSBDeviceController' in name:
            names.append(name)
    return sorted(set(names))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true',
                        help='print and validate the 20-entry table; needs no build tree')
    parser.add_argument('--component', default='AliothUSBDevice')
    parser.add_argument('--archive', default='libAliothUSBDevice.a')
    parser.add_argument('--directory', type=Path, default=Path('build/alioth-usb'))
    args = parser.parse_args()

    problems = check_table()
    if args.check:
        print_table()
        for problem in problems:
            print('FAIL: ' + problem)
        if problems:
            return 1
        print('PASS: %d bindings, every address hexadecimal, kernel-resident and aligned to its kind'
              % len(TABLE))
        return 0

    if problems:
        for problem in problems:
            print('FAIL: ' + problem)
        return 1

    # The build form. It is deliberately strict: it binds exactly the imports the object has,
    # so a name that is imported but absent from TABLE stops the build instead of producing an
    # archive that links some calls to garbage.
    out = args.directory
    obj = out / (args.component + '.o')
    if not obj.is_file():
        raise SystemExit('%s not found; run build-alioth-usb.py first' % obj)
    imports = object_imports(obj)
    known = {name: (address, kind, slot) for name, address, kind, slot, _ in TABLE}
    missing = [name for name in imports if name not in known]
    if missing:
        raise SystemExit('the object imports %d IOUSBDeviceController names that TABLE does not '
                         'bind (%d of them); binding only the rest would leave those calls '
                         'unresolved:\n  %s' % (len(imports), len(missing), '\n  '.join(missing)))
    bindings = {name: known[name][0] for name in imports}
    source = out / 'original-bindings.S'
    source.write_text(''.join('.globl %s\n.set %s, %#x\n' % (n, n, a)
                              for n, a in sorted(bindings.items())))
    bindings_obj = out / 'original-bindings.o'
    subprocess.run(['xcrun', 'clang', '-target', 'arm64-apple-ios17.0', '-Werror',
                    '-Wno-incompatible-sysroot', '-c', str(source), '-o', str(bindings_obj)],
                   check=True)
    archive = out / args.archive
    subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj), str(bindings_obj)], check=True)
    print('BUILT %s  (%d original IOUSBDeviceController bindings)' % (archive, len(bindings)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
