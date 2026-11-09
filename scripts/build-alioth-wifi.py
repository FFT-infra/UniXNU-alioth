#!/usr/bin/env python3
"""Build drivers/AliothWifi into build/alioth-wifi/libAliothWifi.a.

The platform file is compiled with the kernel's own C++ recipe, so its flags match every other
package object. The include paths are the two inputs the file needs outside the tree:
ALIOTH_WIFI_SHARED (the Ath12kWifi checkout that carries include/nw_abi.h) and ALIOTH_WIFI_DIR
(defaults to <tree>/drivers/AliothWifi; a plain checkout passes this repository's directory).

The ath11k half is a separate archive: scripts/build-alioth-ath11k.py --archive produces it with
the same clang, and scripts/build-xnu-4k-baseline.sh links both when this recipe is listed in
board.toml [kernel].archives and CONFIG_XNU_NATIVE_ALIOTH_WIFI selects it. This recipe does not
add itself to the build: the first MHI round does that after the call site exists.
"""
import json
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json')
                    .read_text().strip().rstrip(','))
driver = Path(os.environ.get('ALIOTH_WIFI_DIR', str(ROOT / 'drivers/AliothWifi')))
shared = Path(os.environ.get('ALIOTH_WIFI_SHARED', ''))
source = driver / 'AliothWifiPlatform.cpp'
if not source.is_file():
    sys.exit('AliothWifiPlatform.cpp not found at ' + str(source))
if not (shared / 'include/nw_abi.h').is_file():
    sys.exit('set ALIOTH_WIFI_SHARED to the Ath12kWifi checkout (include/nw_abi.h missing)')
out = ROOT / 'build/alioth-wifi'
out.mkdir(parents=True, exist_ok=True)
obj = out / 'AliothWifiPlatform.o'
cmd, i = [], 0
while i < len(recipe['arguments']):
    v = recipe['arguments'][i]
    if v == recipe['file']:
        cmd.append(str(source))
    elif v == '-o':
        cmd += ['-o', str(obj)]
        i += 1
    elif v in ('-MF', '-MJ'):
        i += 1
    elif v == '-Xclang' and recipe['arguments'][i + 1].startswith('-W'):
        i += 1
    elif not v.startswith('-W') and v not in ('-MD', '-MP'):
        cmd.append(v)
    i += 1
cmd += ['-I', str(shared / 'include'), '-I', str(driver), '-Wall', '-Wno-unused-parameter']
result = subprocess.run(cmd, cwd=recipe['directory'], capture_output=True, text=True)
if result.returncode:
    sys.exit(str(source) + ':\n' + result.stderr[-3000:])
archive = out / 'libAliothWifi.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, obj], check=True)
print('BUILT', archive)
