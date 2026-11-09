#!/usr/bin/env python3
"""Merge the entries of one GPU addon into another.

  merge-gpu-addon.py BASE EXTRA OUTPUT [--take PREFIX ...]

Why this exists: make-gpu-addon.py packs the plugin, the probe daemon and, with
--wallpaper, one wallpaper overlay. It has no switch for the QCA6390 firmware, and the addon this
board has been booting was made by a one-off packer that is not in the tree. The wallpaper overlay
and the firmware therefore have to be combined after the fact rather than by one call.

The container is NGPUADD2 (make-gpu-addon.py's docstring): "NGPUADD2", u32 count, u32 0, u64 total
bytes, then per file u32 path bytes with its NUL, u32 mode, u64 size, u64 compressed size, the path
padded to 8 bytes and the zlib stream padded to 8 bytes. Nothing here decompresses anything: an
entry is copied as the bytes it already is, so a merge cannot change a file's content.

An entry whose path already exists in BASE is taken from BASE and the merge says so; the two
inputs are never silently interleaved.
"""
import struct
import sys
from pathlib import Path

MAGIC = b'NGPUADD2'


def pad8(data):
    return data + bytes(-len(data) % 8)


def read(path):
    data = Path(path).read_bytes()
    if data[:8] != MAGIC:
        sys.exit(str(path) + ' is not an NGPUADD2 addon')
    count, _, total = struct.unpack_from('<IIQ', data, 8)
    if total != len(data):
        sys.exit(f'{path}: header says {total} bytes, file is {len(data)}')
    entries, at = [], 24
    for _ in range(count):
        plen, mode, size, packed_len = struct.unpack_from('<IIQQ', data, at)
        at += 24
        name = data[at:at + plen].rstrip(b'\0').decode()
        at += len(pad8(data[at:at + plen]))
        body = data[at:at + packed_len]
        at += len(pad8(body))
        entries.append((name, mode, size, body))
    return entries


def write(path, entries):
    body = b''
    for name, mode, size, body_bytes in entries:
        encoded = name.encode() + b'\0'
        if len(encoded) > 160:
            sys.exit('path too long: ' + name)
        body += struct.pack('<IIQQ', len(encoded), mode, size, len(body_bytes))
        body += pad8(encoded) + pad8(body_bytes)
    Path(path).write_bytes(MAGIC + struct.pack('<IIQ', len(entries), 0, 24 + len(body)) + body)


def main():
    if len(sys.argv) < 4:
        sys.exit(__doc__)
    base, extra, out = sys.argv[1], sys.argv[2], sys.argv[3]
    takes = [sys.argv[i + 1] for i, a in enumerate(sys.argv) if a == '--take']
    if not takes:
        takes = ['']
    merged = read(base)
    have = {name for name, _, _, _ in merged}
    added = 0
    for name, mode, size, body in read(extra):
        if not any(name.startswith(t) for t in takes):
            continue
        if name in have:
            print('kept from base:', name)
            continue
        merged.append((name, mode, size, body))
        have.add(name)
        added += 1
    write(out, merged)
    print(f'{out}: {len(merged)} entries, {added} added from {extra}')


if __name__ == '__main__':
    main()
