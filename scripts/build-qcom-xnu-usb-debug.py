#!/usr/bin/env python3
"""Build the CDC-ACM debug console archive from this repository's QcomXnuUsbDebug submodule.

The driver used to be a catalog component in the shared base repository. It is board-private,
so it now lives as a submodule of this repository and is built here. The component's own
scripts/build.py does the compile; this recipe only resolves the tree, the device checkout and
the kernel compile record it needs.

Run after `unixnu build kernel`, from the UniXNU tree root, like build-alioth-usb.py.
"""
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
DEVICE = next((c for c in _candidates if (c / 'drivers' / 'QcomXnuUsbDebug' / 'scripts' / 'build.py').is_file()),
              _candidates[0])
src = DEVICE / 'drivers' / 'QcomXnuUsbDebug'
if not (src / 'scripts' / 'build.py').is_file():
    sys.exit('QcomXnuUsbDebug/scripts/build.py not found under %s' % DEVICE)

record = ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/pexpert/DEVELOPMENT/pe_kprintf.o.json'
if not record.is_file():
    sys.exit('no kernel compile record at %s; run "unixnu build kernel" first' % record)

out = ROOT / '.cache' / 'qcom-usb-debug'
subprocess.run([sys.executable, str(src / 'scripts' / 'build.py'),
                '--compile-record', str(record),
                '--soc', 'soc/Sm8250.h',
                '--output', str(out), '--replace'],
               cwd=src, check=True)
lib = out / 'libQcomXnuUsbDebug.a'
if not lib.is_file():
    sys.exit('build.py did not produce %s' % lib)
print('QCOM_USB_DEBUG_OK %s' % lib)
