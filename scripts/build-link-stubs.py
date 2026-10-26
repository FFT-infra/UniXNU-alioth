#!/usr/bin/env python3
"""Archive the no-op probe hooks of kits/alioth/LinkStubs.c for the kernel link (CONFIG_XNU_EXTRA_LD_ARCHIVES).

Some of those hooks exist only while the component that owns the real definition is off, so the
file needs the same switch the kernel is built with. XNU_NATIVE_LEGACY_DISPLAY is the one that
matters today: on, LinkStubs.c leaves native_legacy_display_probe() to the display archive."""
import os
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
out = ROOT / 'build/link-stubs'
out.mkdir(parents=True, exist_ok=True)
legacy_display = os.environ.get('XNU_NATIVE_LEGACY_DISPLAY', '0')
# BootProgressStubs.c carries the zero-valued definitions of the diagnostics BootProgress.c and
# BootLog.c reference as weak_import. It is in the same archive because both files answer the same
# question at the link: which names the kernel needs that no selected component defines.
sources = [ROOT / 'kits/alioth/LinkStubs.c', ROOT / 'kits/alioth/BootProgressStubs.c']
objects = []
for source in sources:
    obj = out / (source.stem + '.o')
    subprocess.run(['xcrun', '--sdk', 'iphoneos', 'clang', '-arch', 'arm64', '-mkernel', '-miphoneos-version-min=17.0',
                    f'-DXNU_NATIVE_LEGACY_DISPLAY={legacy_display}',
                    '-Os', '-Wall', '-Werror', '-c', source, '-o', obj], check=True)
    objects.append(obj)
archive = out / 'libLinkStubs.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
print(archive)
