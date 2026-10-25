#!/usr/bin/env python3
"""Static checks of the alioth firmware tree before the EDK II build.

The unixnu driver runs this in the build tree before "build firmware". It
reads the Mu-Silicium checkout from UNIXNU_UEFI and checks what can be checked
without the tablet:

  - alioth.dsc, alioth.fdf and Include/DXE.inc carry UniXNU, its registrar, the
    ramdisk block device driver and DarwinBoot;
  - Resources/Configs/alioth.toml agrees with the "UEFI FD" and "UEFI Stack"
    descriptors of MemoryMapLib, the descriptors are page aligned and do not
    overlap, and the Android boot image parameters match the alioth contract;
  - every Binaries/alioth input the include files name exists;
  - the frame buffer region of Resources/DTBs/alioth.dtb (the tree appended to
    the boot image) lies inside the "Display Reserved" descriptor.

It writes out/alioth/check.json when invoked from the assembled build tree. A
pass says nothing about the hardware.
"""
import json
import os
import re
import struct
import sys
from pathlib import Path

PLATFORM = Path('Platforms/Xiaomi/aliothPkg')
REQUIRED = {
    PLATFORM / 'alioth.dsc': ['UniXNUPkg/Application/UniXNU/UniXNU.inf', 'UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf',
                            'shengPkg/Drivers/ShengBootMemoryDxe/ShengBootMemoryDxe.inf',
                            'DarwinBootPkg/Driver/DarwinBoot.inf',
                            'UniXNULib|UniXNUPkg/Library/UniXNULib/UniXNULib.inf',
                            'FdtLib|MdePkg/Library/BaseFdtLib/BaseFdtLib.inf'],
    PLATFORM / 'alioth.fdf': ['INF UniXNUPkg/Application/UniXNU/UniXNU.inf', 'INF UniXNUPkg/Driver/UniXNUDxe/UniXNUDxe.inf'],
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
        sys.exit('check-alioth: set UNIXNU_UEFI to the uefi-alioth checkout (or pass --skip-preflight)')
    uefi = Path(uefi_text).expanduser()
    if not (uefi / PLATFORM / 'alioth.dsc').is_file():
        sys.exit(f'check-alioth: {uefi} has no {PLATFORM}/alioth.dsc')
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

    config = (uefi / 'Resources/Configs/alioth.toml').read_text()
    fd_base, fd_size = toml_int(config, 'uefi_fd', 'base'), toml_int(config, 'uefi_fd', 'size')
    if descriptor.get('UEFI FD') != (fd_base, fd_size):
        errors.append(f'alioth.toml [uefi_fd] {fd_base}/{fd_size} differs from the "UEFI FD" descriptor {descriptor.get("UEFI FD")}')
    expected_image = {
        'header_version': 1,
        'base': 0x10000000,
        'kernel_offset': 0x8000,
        'ramdisk_offset': 0x16000000,
        'tags_offset': 0x100,
        'pagesize': 2048,
    }
    observed_image = {key: toml_int(config, 'boot_image', key) for key in expected_image}
    for key, wanted in expected_image.items():
        if observed_image[key] != wanted:
            errors.append(f'alioth.toml [boot_image] {key}={observed_image[key]!r}, expected {wanted:#x}'
                          if isinstance(wanted, int) and key != 'pagesize' and key != 'header_version'
                          else f'alioth.toml [boot_image] {key}={observed_image[key]!r}, expected {wanted!r}')
    dsc = (uefi / PLATFORM / 'alioth.dsc').read_text()
    stack = re.search(r'PcdCPUCoresStackBase\|(0x[0-9a-fA-F]+)', dsc)
    if not stack or descriptor.get('UEFI Stack', (None,))[0] != int(stack[1], 16):
        errors.append('PcdCPUCoresStackBase differs from the "UEFI Stack" descriptor')
    elif fd_base is not None and fd_size is not None and fd_base + fd_size > int(stack[1], 16):
        errors.append('the firmware volume runs into the UEFI stack')

    inputs = set()
    for include in (uefi / PLATFORM / 'Include').glob('*.inc'):
        inputs.update(re.findall(r'Binaries/alioth/[^\s}]+', include.read_text()))
    missing = sorted(p for p in inputs if not (uefi / p).is_file())
    if missing:
        errors.append(f'{len(missing)} Binaries/alioth inputs are missing (Binaries submodule not checked out?)')

    dtb = uefi / 'Resources/DTBs/alioth.dtb'
    splash = []
    if dtb.is_file():
        for path, props in fdt_nodes(dtb.read_bytes()):
            if path.startswith('/reserved-memory/') and re.search(r'splash', path) and len(props.get('reg', b'')) == 16:
                splash.append((path, *struct.unpack('>QQ', props['reg'])))
        display = descriptor.get('Display Reserved')
        if not splash:
            errors.append('alioth.dtb has no splash region: the stock ABL may power the panel off')
        for path, base, size in splash:
            if not display or not (display[0] <= base and base + size <= display[0] + display[1]):
                errors.append(f'{path} {base:#x}+{size:#x} lies outside the "Display Reserved" descriptor {display}')
            else:
                notes.append(f'frame buffer {path} {base:#x}+{size:#x} inside "Display Reserved"')
    else:
        errors.append(f'{dtb} is missing; it is the tree "unixnu bootimg --dtb" appends')

    result = {'uefi': str(uefi), 'descriptors': len(rows), 'vendor_inputs': len(inputs),
              'missing_vendor_inputs': missing, 'boot_image': observed_image,
              'static_errors': errors, 'notes': notes, 'hardware_boot_demonstrated': False}
    script = Path(__file__).resolve()
    work = script.parents[2] if len(script.parents) > 2 and script.parents[1].name == 'tree' else Path('build/alioth').resolve()
    work.mkdir(parents=True, exist_ok=True)
    out = work / 'check.json'
    out.write_text(json.dumps(result, indent=2) + '\n')
    print(f'{len(rows)} descriptors, {len(inputs)} vendor inputs ({len(missing)} missing); hardware unverified')
    for line in notes:
        print(line)
    for line in errors:
        print(line)
    raise SystemExit(bool(errors))


if __name__ == '__main__':
    main()
