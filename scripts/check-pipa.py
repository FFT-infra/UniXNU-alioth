#!/usr/bin/env python3
"""Static checks of the pipa firmware tree before the EDK II build.

The unixnu driver runs this in the build tree before "build firmware". It
reads the Mu-Silicium checkout from UNIXNU_UEFI and checks what can be checked
without the tablet:

  - pipa.dsc, pipa.fdf and Include/DXE.inc carry UniXNU, its registrar, the
    ramdisk block device driver and DarwinBoot;
  - Resources/Configs/pipa.toml agrees with the "UEFI FD" and "UEFI Stack"
    descriptors of MemoryMapLib, and the descriptors are page aligned and do
    not overlap;
  - every Binaries/pipa input the include files name exists;
  - the frame buffer region of Resources/DTBs/pipa.dtb (the tree appended to
    the boot image) lies inside the "Display Reserved" descriptor.

It writes build/pipa/check.json. A pass says nothing about the hardware.
"""
import json
import os
import re
import struct
import sys
from pathlib import Path

PLATFORM = Path('Platforms/Xiaomi/pipaPkg')
REQUIRED = {
    PLATFORM / 'pipa.dsc': ['UniXNUPkg/Application/UniXNU/UniXNU.inf', 'UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf',
                            'shengPkg/Drivers/ShengBootMemoryDxe/ShengBootMemoryDxe.inf',
                            'DarwinBootPkg/Driver/DarwinBoot.inf',
                            'UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf'],
    PLATFORM / 'pipa.fdf': ['INF UniXNUPkg/Application/UniXNU/UniXNU.inf', 'INF UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf'],
    PLATFORM / 'Include/DXE.inc': ['INF shengPkg/Drivers/ShengBootMemoryDxe/ShengBootMemoryDxe.inf',
                                   'INF DarwinBootPkg/Driver/DarwinBoot.inf'],
}


def toml_int(text, section, key):
    """An integer key of a flat TOML section, without needing tomllib (Python 3.11)."""
    body = re.search(r'^\[%s\]\s*$(.*?)(?=^\[|\Z)' % re.escape(section), text, re.M | re.S)
    value = body and re.search(r'^%s\s*=\s*(\S+)' % re.escape(key), body.group(1), re.M)
    return int(value.group(1), 0) if value else None


def fdt_nodes(blob):
    """(path, {property: bytes}) of every node of a flattened device tree."""
    magic, _, off_struct, off_strings = struct.unpack_from('>4I', blob, 0)
    if magic != 0xd00dfeed:
        raise ValueError('not a flattened device tree')
    nodes, stack, pos = [], [], off_struct
    while True:
        (token,) = struct.unpack_from('>I', blob, pos)
        pos += 4
        if token == 1:
            end = blob.index(b'\0', pos)
            stack.append(blob[pos:end].decode())
            pos = (end + 4) & ~3
            nodes.append(('/' + '/'.join(stack[1:]), {}))
        elif token == 2:
            stack.pop()
        elif token == 3:
            length, name = struct.unpack_from('>II', blob, pos)
            value = blob[pos + 8:pos + 8 + length]
            pos = (pos + 8 + length + 3) & ~3
            key = blob[off_strings + name:blob.index(b'\0', off_strings + name)].decode()
            nodes[-1][1][key] = value
        elif token == 9:
            return nodes
        elif token != 4:
            raise ValueError('bad token 0x%x' % token)


def main():
    uefi_text = os.environ.get('UNIXNU_UEFI')
    if not uefi_text:
        sys.exit('check-pipa: set UNIXNU_UEFI to the uefi-pipa checkout (or pass --skip-preflight)')
    uefi = Path(uefi_text).expanduser()
    if not (uefi / PLATFORM / 'pipa.dsc').is_file():
        sys.exit(f'check-pipa: {uefi} has no {PLATFORM}/pipa.dsc')
    errors, notes = [], []

    for path, lines in REQUIRED.items():
        text = (uefi / path).read_text()
        live = '\n'.join(line.split('#', 1)[0] for line in text.splitlines())
        for wanted in lines:
            if wanted not in live:
                errors.append(f'{path} does not list {wanted}')
    if not (uefi / 'Platforms/Xiaomi/shengPkg/Drivers/ShengBootMemoryDxe/ShengBootMemoryDxe.c').is_file():
        errors.append('the ramdisk block device driver (shengPkg/Drivers/ShengBootMemoryDxe) is missing')

    source = (uefi / PLATFORM / 'Library/MemoryMapLib/MemoryMapLib.c').read_text()
    rows = [(m[1], int(m[2], 16), int(m[3], 16), m[4]) for m in re.finditer(
        r'\{"([^"]+)"\s*,\s*(0x[0-9a-fA-F]+),\s*(0x[0-9a-fA-F]+),([^}]+)\}', source)]
    if not rows:
        errors.append('MemoryMapLib.c has no descriptors')
    # A NoHob descriptor creates no resource, so a region nested inside one (IMEM Cookie Base in IMEM Base) is fine.
    placed = [row for row in rows if not re.search(r'\bNoHob\b', row[3])]
    for index, (name, base, size, _) in enumerate(rows):
        if base % 4096 or size % 4096 or not size:
            errors.append(f'descriptor {name} is not page aligned')
    for index, (name, base, size, _) in enumerate(placed):
        for other, other_base, other_size, _ in placed[index + 1:]:
            if max(base, other_base) < min(base + size, other_base + other_size):
                errors.append(f'descriptors {name} and {other} overlap')
    descriptor = {name: (base, size) for name, base, size, _ in rows}

    config = (uefi / 'Resources/Configs/pipa.toml').read_text()
    fd_base, fd_size = toml_int(config, 'uefi_fd', 'base'), toml_int(config, 'uefi_fd', 'size')
    if descriptor.get('UEFI FD') != (fd_base, fd_size):
        errors.append(f'pipa.toml [uefi_fd] {fd_base}/{fd_size} differs from the "UEFI FD" descriptor {descriptor.get("UEFI FD")}')
    for key in ('header_version', 'base', 'kernel_offset', 'tags_offset', 'pagesize'):
        if toml_int(config, 'boot_image', key) is None:
            errors.append(f'pipa.toml [boot_image] has no {key}')
    dsc = (uefi / PLATFORM / 'pipa.dsc').read_text()
    stack = re.search(r'PcdCPUCoresStackBase\|(0x[0-9a-fA-F]+)', dsc)
    if not stack or descriptor.get('UEFI Stack', (None,))[0] != int(stack[1], 16):
        errors.append('PcdCPUCoresStackBase differs from the "UEFI Stack" descriptor')
    elif fd_base is not None and fd_size is not None and fd_base + fd_size > int(stack[1], 16):
        errors.append('the firmware volume runs into the UEFI stack')

    inputs = set()
    for include in (uefi / PLATFORM / 'Include').glob('*.inc'):
        inputs.update(re.findall(r'Binaries/pipa/[^\s}]+', include.read_text()))
    missing = sorted(p for p in inputs if not (uefi / p).is_file())
    if missing:
        errors.append(f'{len(missing)} Binaries/pipa inputs are missing (Binaries submodule not checked out?)')

    dtb = uefi / 'Resources/DTBs/pipa.dtb'
    splash = []
    if dtb.is_file():
        for path, props in fdt_nodes(dtb.read_bytes()):
            if path.startswith('/reserved-memory/') and re.search(r'splash', path) and len(props.get('reg', b'')) == 16:
                splash.append((path, *struct.unpack('>QQ', props['reg'])))
        display = descriptor.get('Display Reserved')
        if not splash:
            errors.append('pipa.dtb has no splash region: the stock ABL may power the panel off')
        for path, base, size in splash:
            if not display or not (display[0] <= base and base + size <= display[0] + display[1]):
                errors.append(f'{path} {base:#x}+{size:#x} lies outside the "Display Reserved" descriptor {display}')
            else:
                notes.append(f'frame buffer {path} {base:#x}+{size:#x} inside "Display Reserved"')
    else:
        errors.append(f'{dtb} is missing; it is the tree "unixnu bootimg --dtb" appends')

    result = {'uefi': str(uefi), 'descriptors': len(rows), 'vendor_inputs': len(inputs),
              'missing_vendor_inputs': missing, 'static_errors': errors, 'notes': notes,
              'hardware_boot_demonstrated': False}
    out = Path('build/pipa')
    out.mkdir(parents=True, exist_ok=True)
    (out / 'check.json').write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(rows)} descriptors, {len(inputs)} vendor inputs ({len(missing)} missing); hardware unverified')
    for line in notes:
        print(line)
    for line in errors:
        print(line)
    raise SystemExit(bool(errors))


if __name__ == '__main__':
    main()
