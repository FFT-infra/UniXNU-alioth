#!/usr/bin/env python3
"""Build package/AliothUSBDevice.cpp into build/alioth-usb/libAliothUSBDevice.a.

The board-private USB device controller: an IOUSBDeviceController subclass for this board's DWC3
core. Compiled with the kernel's own C++ recipe so its flags and ABI match the kernel it links
into, then archived under a name the board lists in [kernel] archives.

Run after `unixnu build kernel`, from the UniXNU tree root (ROOT), like build-alioth-touch.py.
The device repository supplies the source; scripts/unixnu/tree.py links its package as
kits/alioth, so resolving that link names the device checkout even though unixnu does not export
UNIXNU_DEVICE to archive steps.

Why a package recipe and not a component: components are catalog entries in the shared base
repository (`UniXNU/components.toml`), and this driver is board-private. GpuProbe,
BacklightProbe and the PCIe bring-up all arrive the same way, through scripts/build-*.py.

The header is package/IOUSBDeviceController.h, reconstructed from this board's own kernelcache
(iPadOS 17.7.11 / 21H461). Its 234-slot layout is measured, not copied from another iOS version;
docs/evidence/2026-10-31-msgbuf-frozen-snapshot.md and the A-1/A-2 notes record how.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent          # UniXNU tree

# The device checkout. The kits/alioth link is authoritative: it points at the device repository,
# while a plain kits/alioth directory inside the tree can be a stale copy from an earlier build.
_candidates = []
if os.environ.get('UNIXNU_DEVICE'):
    _candidates.append(Path(os.environ['UNIXNU_DEVICE']))
_device = ROOT / 'kits' / 'alioth'
if _device.is_symlink():
    _candidates.append(_device.resolve().parent)
if not _candidates:
    sys.exit('no device checkout: set UNIXNU_DEVICE or restore the kits/alioth link')
DEVICE = next((c for c in _candidates if (c / 'package' / 'AliothUSBDevice.cpp').is_file()), _candidates[0])
src = DEVICE / 'package' / 'AliothUSBDevice.cpp'
if not src.is_file():
    sys.exit('AliothUSBDevice.cpp not found (tried %s)' % [str(c) for c in _candidates])

out = ROOT / 'build' / 'alioth-usb'
out.mkdir(parents=True, exist_ok=True)

record_path = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json'
if not record_path.is_file():
    sys.exit('no kernel build records at %s; run "unixnu build kernel" first' % record_path.parent)
record = json.loads(record_path.read_text().strip().rstrip(','))

obj = out / 'AliothUSBDevice.o'
cmd, i = [], 0
args = record['arguments']
while i < len(args):
    v = args[i]
    if v == record['file']:
        cmd.append(str(src))
    elif v == '-o':
        cmd += ['-o', str(obj)]
        i += 1
    elif v in ('-MF', '-MJ'):
        i += 1
    elif v == '-Xclang' and args[i + 1].startswith('-W'):
        i += 1
    elif not v.startswith('-W') and v not in ('-MD', '-MP'):
        cmd.append(v)
    i += 1
cmd += ['-I', str(DEVICE / 'package'), '-Wall', '-Wextra', '-Wno-unused-parameter',
        '-Wno-c99-designator', '-Wno-reorder-init-list']
r = subprocess.run(cmd, cwd=record['directory'], capture_output=True, text=True)
(out / 'compile.log').write_text(r.stdout + r.stderr)
if r.returncode:
    sys.exit(str(src) + ':\n' + (r.stdout + r.stderr)[-2500:])

# The vtable check. An IOUSBDeviceController subclass must emit exactly as many slots as this
# board's own class does, with the pure ones filled by our overrides. A short vtable means the
# header disagrees with the firmware, and every call would be misrouted at load time. This
# refuses the archive rather than shipping a driver that cannot work.
def vtable(obj):
    out_ = subprocess.run(['llvm-objdump', '-r', str(obj)], capture_output=True, text=True)
    if out_.returncode:
        sys.exit('llvm-objdump failed on ' + str(obj))
    sec, rows = None, {}
    for line in out_.stdout.splitlines():
        if 'RELOCATION RECORDS FOR' in line:
            sec = line.split('[')[1].split(']')[0] if '[' in line else '?'
            continue
        parts = line.split()
        if len(parts) >= 3 and parts[1].startswith('ARM64_RELOC_UNSIGNED') and sec == '__const':
            rows[int(parts[0], 16)] = parts[2]
    offs = sorted(rows)
    if not offs:
        return rows, []
    runs, cur = [], [offs[0]]
    for a in offs[1:]:
        if a - cur[-1] == 8:
            cur.append(a)
        else:
            runs.append(cur)
            cur = [a]
    runs.append(cur)
    return rows, max(runs, key=len)

rows, vt = vtable(obj)
if len(vt) != 234:
    sys.exit('vtable has %d slots, expected 234 (the 21H461 IOUSBDeviceController layout)' % len(vt))
base = vt[0]
for slot, want in ((0, 'AliothUSBDeviceD1Ev'), (168, 'IOUSBDeviceController10createPipe'),
                   (233, 'IOUSBDeviceController13nativeSlot233Ev')):
    if want not in rows.get(base + slot * 8, ''):
        sys.exit('vtable slot %d is %r, expected %s'
                 % (slot, rows.get(base + slot * 8, '<empty>'), want))
pure = (175, 177, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189,
        192, 193, 199, 202, 227)
for slot in pure:
    if 'AliothUSBDevice' not in rows.get(base + slot * 8, ''):
        sys.exit('pure slot %d is %r, expected an AliothUSBDevice override'
                 % (slot, rows.get(base + slot * 8, '<empty>')))

archive = out / 'libAliothUSBDevice.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, obj], check=True)
print('BUILT %s  (%d vtable slots, %d pure overrides verified)' % (archive, len(vt), len(pure)))
