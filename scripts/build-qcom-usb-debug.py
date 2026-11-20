#!/usr/bin/env python3
"""Route the QcomXnuUsbDebug build through the feed checkout.

The catalog entry names this wrapper so the build input stays in the device
scripts tree. The component's own scripts/build.py remains the implementation.
"""
import glob
import os
import subprocess
import sys

here = os.path.dirname(os.path.abspath(__file__))
root = here
while True:
    for pattern in (
        os.path.join(root, "out", "feeds", "QcomXnuUsbDebug-*", "scripts", "build.py"),
        os.path.join(root, "src", "UniXNU", "out", "feeds", "QcomXnuUsbDebug-*", "scripts", "build.py"),
    ):
        hits = glob.glob(pattern)
        if hits:
            raise SystemExit(subprocess.call([sys.executable, hits[0], *sys.argv[1:]]))
    parent = os.path.dirname(root)
    if parent == root:
        break
    root = parent
raise SystemExit("NO QCOM FEED")
