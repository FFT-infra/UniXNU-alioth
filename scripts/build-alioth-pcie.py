#!/usr/bin/env python3
"""Build package/alioth_pcie.c into build/alioth-pcie/libAliothPcie.a.

The source is the PCIe bring-up that ran on this phone in the ios-sweet series; the
device package appears in the generated tree as kits/alioth, so that is where it is
read from. Compiled with the kernel's own C recipe so its flags match.
"""
import json, os, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(','))
out = ROOT / 'build/alioth-pcie'
out.mkdir(parents=True, exist_ok=True)
src = ROOT / 'kits/alioth/alioth_pcie.c'
if not src.is_file():
    sys.exit('alioth_pcie.c not found at ' + str(src))
obj = out / 'alioth_pcie.o'
cmd, i = [], 0
while i < len(record['arguments']):
    v = record['arguments'][i]
    if v == record['file']:
        cmd.append(str(src))
    elif v == '-o':
        cmd += ['-o', str(obj)]
        i += 1
    elif v in ('-MF', '-MJ'):
        i += 1
    elif v == '-Xclang' and record['arguments'][i + 1].startswith('-W'):
        i += 1
    elif not v.startswith('-W') and v not in ('-MD', '-MP'):
        cmd.append(v)
    i += 1
cmd += ['-I', str(ROOT / 'kits/alioth'), '-Wall', '-Wno-unused-parameter',
        '-Wno-c99-designator', '-Wno-reorder-init-list']
r = subprocess.run(cmd, cwd=record['directory'], capture_output=True, text=True)
if r.returncode:
    sys.exit(str(src) + ':\n' + r.stderr[-2500:])
archive = out / 'libAliothPcie.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, obj], check=True)
print('BUILT', archive)
