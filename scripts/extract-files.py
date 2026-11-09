#!/usr/bin/env python3
"""Write the firmware inputs of proprietary-files.txt into .cache/firmware.

  extract-files.py [--check]

The device repository specification keeps proprietary inputs out of git: the build tree links
.cache/firmware, so every tree and every rebuild reads one copy (UniXNU scripts/unixnu/tree.py).
This script is what fills that directory. A line is `path|sha256`, optionally prefixed with `-`
when the file may be absent. An `@fetch URL` line sets a base URL; a file that is not already in
the cache is downloaded from base+path and must match its recorded digest.

--check verifies the cache without writing or downloading anything, which is what a CI or a
record review runs.
"""
import argparse
import hashlib
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CACHE = ROOT / '.cache' / 'firmware'
LIST = ROOT / 'proprietary-files.txt'


def entries():
    base = None
    for number, line in enumerate(LIST.read_text().splitlines(), 1):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        if line.startswith('@fetch '):
            base = line[len('@fetch '):].strip()
            if not base.endswith('/'):
                base += '/'
            continue
        optional = line.startswith('-')
        if optional:
            line = line[1:]
        path, _, digest = line.partition('|')
        if not path or len(digest) != 64:
            raise SystemExit('%s:%d: expected path|sha256' % (LIST.name, number))
        yield path.strip(), digest.strip().lower(), optional, base


def digest_of(path):
    value = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b''):
            value.update(chunk)
    return value.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='verify only; never write')
    args = parser.parse_args()
    failures = 0
    for path, digest, optional, base in entries():
        target = CACHE / path
        if target.is_file():
            got = digest_of(target)
            if got == digest:
                print('OK      %s' % path)
                continue
            if args.check or optional:
                print('MISMATCH %s %s' % (path, got) if not optional else 'OPTIONAL %s (digest differs)' % path)
                failures += not optional
                continue
            print('STALE   %s %s' % (path, got))
        elif args.check:
            if optional:
                print('OPTIONAL %s (absent)' % path)
            else:
                print('MISSING %s' % path)
                failures += 1
            continue
        if base is None:
            print('MISSING %s (no @fetch base)' % path)
            failures += not optional
            continue
        target.parent.mkdir(parents=True, exist_ok=True)
        part = target.with_name(target.name + '.part')
        try:
            print('FETCH   %s' % (base + path))
            # cgit answers 403 to the default Python agent; curl's is accepted.
            request = urllib.request.Request(base + path, headers={'User-Agent': 'curl/8.7'})
            with urllib.request.urlopen(request, timeout=120) as source, part.open('wb') as sink:
                while True:
                    chunk = source.read(1 << 20)
                    if not chunk:
                        break
                    sink.write(chunk)
        except OSError as error:
            print('FETCH FAILED %s: %s' % (path, error))
            part.unlink(missing_ok=True)
            failures += not optional
            continue
        got = digest_of(part)
        if got != digest:
            part.unlink()
            print('HASH MISMATCH %s %s' % (path, got))
            failures += 1
            continue
        part.rename(target)
        print('WROTE   %s' % path)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
