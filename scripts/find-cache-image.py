#!/usr/bin/env python3
"""Name the image and offset of one or more runtime addresses in a dyld shared cache.

  find-cache-image.py <cache-dir> <address> [address ...]

The addresses to give are the PCs from an EXITCRASH line; they are already in the
cache's address space because the cache is mapped at its preferred base. Nothing is
loaded or executed: the headers and the image table are read, so a wedged
DiskArbitration does not matter.

The cache itself comes from the SystemOS volume. On this project's host that image
is `out/ipsw/21H461/044-76034-038.dmg`, which carries no UDIF trailer and whose
first NXSB sits at offset 32; `apfs-fuse -o ro <dmg> <mount>` opens it as it is,
and the cache is then under
`<mount>/root/System/Library/Caches/com.apple.dyld`.
"""
import struct
import sys
from pathlib import Path

MAGICS = (b'dyld_v1   arm64', b'dyld_v1  arm64e')
SKIP = ('.symbols', '.dylddata', '.dyldlinkedit')


def mappings(path):
    with path.open('rb') as stream:
        header = stream.read(0x400)
        if header[:16].rstrip(b'\0') not in MAGICS:
            return None
        offset, count = struct.unpack_from('<II', header, 0x10)
        stream.seek(offset)
        body = stream.read(32 * count)
    out = []
    for index in range(count):
        address, size, file_offset, _, _ = struct.unpack_from('<QQQII', body, index * 32)
        out.append((address, size, file_offset))
    return out


def image_at(path, address):
    with path.open('rb') as stream:
        header = stream.read(0x400)
        offset, count = struct.unpack_from('<QQ', header, 0x88)
        stream.seek(offset)
        table = stream.read(32 * count)
        for index in range(count):
            load, size, name_offset = struct.unpack_from('<QII', table, index * 32 + 16)
            if load <= address < load + size:
                stream.seek(name_offset)
                name = b''
                while True:
                    byte = stream.read(1)
                    if byte in (b'\0', b''):
                        break
                    name += byte
                return name.decode('utf-8', 'replace'), address - load
    return None, 0


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    directory = Path(sys.argv[1])
    addresses = [int(value, 0) for value in sys.argv[2:]]
    caches = []
    for path in sorted(directory.glob('dyld_shared_cache_arm64*')):
        if path.name.endswith(SKIP):
            continue
        try:
            spans = mappings(path)
        except OSError as error:
            # A subcache this owner cannot read says nothing about the others.
            print('# %s: %s' % (path.name, error), file=sys.stderr)
            continue
        if spans:
            caches.append((path, spans))
    if not caches:
        sys.exit('no cache files in ' + str(directory))
    for address in addresses:
        for path, spans in caches:
            hit = next(((base, size, offset) for base, size, offset in spans if base <= address < base + size), None)
            if not hit:
                continue
            base = hit[0]
            name, delta = image_at(path, address)
            if name:
                print('%#x %s +%#x  (%s)' % (address, name, delta, path.name))
            else:
                print('%#x <no image header covers it>  (%s)' % (address, path.name))
            break
        else:
            print('%#x not in any cache' % address)


if __name__ == '__main__':
    main()
