#!/usr/bin/env python3
"""Merge the owner job into the system's signed launchd cache and re-sign it.

  make-launchd-cache.py [OUTDIR]

launchd takes its system jobs from /System/Library/xpc/launchd.plist (a binary plist keyed by each
job's plist path, read by launchd_cache_loader), not from the LaunchDaemons directory; a plist placed
there is never loaded (measured 2026-11-09, lx1). The documented way to add a service is to merge its
job into that cache and ship a matching signature and trust hash
(docs/zh/drivers/ath12k-wifi.md "sheng 的 iPadOS 27 集成"; docs/zh/drivers/qca-bluetooth.md).

The shipped cache is ad-hoc signed: its .sig is a code signature SuperBlob with a CodeDirectory
(hashType 2, 4 KiB pages, identifier launchd-<sha1 of the file>) and no CMS signature, so the trust
comes from the CDHash. This writes OUTDIR/launchd.plist with the owner job added, OUTDIR/launchd.plist.sig
in the same layout, and prints the CDHash the owner trust module must carry.
OS_MOUNT names the mounted read-only OS volume (default /private/tmp/wpvol2).
"""
import hashlib
import os
import plistlib
import struct
import sys
from pathlib import Path

LABEL = 'org.blu-sharky.adrenoprobed'
KEY = '/System/Library/LaunchDaemons/%s.plist' % LABEL
PAGE = 4096
MAGIC_SUPERBLOB = 0xfade0cc0
MAGIC_CODEDIRECTORY = 0xfade0c02
MAGIC_REQUIREMENTS = 0xfade0c01
MAGIC_BLOBWRAPPER = 0xfade0b01
CSSLOT_CODEDIRECTORY = 0
CSSLOT_REQUIREMENTS = 2
CSSLOT_ALTERNATE_CODEDIRECTORIES = 0x1000
CSSLOT_SIGNATURESLOT = 0x10000
CSSLOT_LAUNCHDCACHE = 0x10001


def code_directory(template, identifier, size, hash_type, hash_size, data):
    """A CodeDirectory over the whole file with no special slots, on the shipped 96-byte header."""
    cd = bytearray(template)
    ident = identifier.encode() + b'\0'
    ident_offset = len(cd)
    hash_offset = ident_offset + len(ident)
    count = (size + PAGE - 1) // PAGE
    struct.pack_into('>I', cd, 16, hash_offset)
    struct.pack_into('>I', cd, 20, ident_offset)
    struct.pack_into('>I', cd, 24, 0)
    struct.pack_into('>I', cd, 28, count)
    struct.pack_into('>I', cd, 32, size)
    cd[36] = hash_size
    cd[37] = hash_type
    cd[39] = 12
    digest = hashlib.sha1 if hash_type == 1 else hashlib.sha256
    hashes = b''.join(digest(data[at:at + PAGE]).digest() for at in range(0, size, PAGE))
    out = bytearray(bytes(cd) + ident + hashes)
    struct.pack_into('>I', out, 4, len(out))
    return bytes(out)


def super_blob(slots):
    """slots: [(type, blob)] in output order."""
    header = struct.pack('>III', MAGIC_SUPERBLOB, 0, len(slots))
    offset = 12 + 8 * len(slots)
    index = b''
    body = b''
    for kind, blob in slots:
        index += struct.pack('>II', kind, offset)
        body += blob
        offset += len(blob)
    out = bytearray(header + index + body)
    struct.pack_into('>I', out, 4, len(out))
    return bytes(out)


def main():
    mount = Path(os.environ.get('OS_MOUNT', '/private/tmp/wpvol2'))
    source = mount / 'System/Library/xpc/launchd.plist'
    if not source.is_file():
        raise SystemExit('no launchd cache at %s (mount the OS volume or set OS_MOUNT)' % source)
    out_dir = Path(sys.argv[1] if len(sys.argv) > 1 else 'build/launchd-cache')
    out_dir.mkdir(parents=True, exist_ok=True)
    cache = plistlib.loads(source.read_bytes())
    jobs = cache.get('LaunchDaemons')
    if not isinstance(jobs, dict) or KEY in jobs:
        raise SystemExit('the cache has no LaunchDaemons table or already carries %s' % KEY)
    jobs[KEY] = {
        'Label': LABEL,
        'ProgramArguments': ['/private/var/ngpu/bin/adrenoprobed'],
        'RunAtLoad': True,
        'KeepAlive': False,
        'POSIXSpawnType': 'Adaptive',
        'ThrottleInterval': 30,
        'UserName': 'root',
        'StandardErrorPath': '/dev/null',
    }
    original = (mount / 'System/Library/xpc/launchd.plist.sig').read_bytes()
    templates = {}
    for index in range(struct.unpack_from('>I', original, 8)[0]):
        kind, offset = struct.unpack_from('>II', original, 12 + 8 * index)
        blob = original[offset:offset + struct.unpack_from('>I', original, offset + 4)[0]]
        if kind == CSSLOT_CODEDIRECTORY:
            templates[1] = blob[:96]
        elif kind == CSSLOT_ALTERNATE_CODEDIRECTORIES:
            templates[2] = blob[:96]
    if len(templates) != 2:
        raise SystemExit('the shipped signature has no plain and alternate CodeDirectory')
    data = plistlib.dumps(cache, fmt=plistlib.FMT_BINARY)
    plist = out_dir / 'launchd.plist'
    plist.write_bytes(data)
    identity = hashlib.sha1(data).hexdigest()
    identifier = 'launchd-' + identity
    requirements = struct.pack('>III', MAGIC_REQUIREMENTS, 12, 0)
    alternate = code_directory(templates[2], identifier, len(data), 2, 32, data)
    signature = super_blob([
        (CSSLOT_CODEDIRECTORY, code_directory(templates[1], identifier, len(data), 1, 20, data)),
        (CSSLOT_REQUIREMENTS, requirements),
        (CSSLOT_ALTERNATE_CODEDIRECTORIES, alternate),
        (CSSLOT_SIGNATURESLOT, struct.pack('>II', MAGIC_BLOBWRAPPER, 8)),
        (CSSLOT_LAUNCHDCACHE, struct.pack('>II', MAGIC_BLOBWRAPPER, 28) + bytes.fromhex(identity)),
    ])
    (out_dir / 'launchd.plist.sig').write_bytes(signature)
    cdhash = hashlib.sha256(alternate).hexdigest()
    print('PASS: launchd cache with %s, %d jobs, cdhash %s' % (LABEL, len(jobs), cdhash))


if __name__ == '__main__':
    main()
