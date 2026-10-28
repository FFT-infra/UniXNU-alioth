#!/usr/bin/env python3
"""Adreno650 (drivers/Adreno650, NativeAdreno for pipa's Adreno 650: a6xx with a GMU): build/adreno650/libAdreno650.a, the
archive that CONFIG_XNU_NATIVE_ADRENO links (CONFIG_XNU_NATIVE_ADRENO_ARCHIVE). After UniXNU-tb128fu's build-native-adreno-a6xx.py
(the Adreno 610 half: GPU context, ring, submission, timer, user client and publication objects with the SMMU and PM4 kits;
Adreno650 began as the pipa branch of the NativeAdreno fork, off its tb128fu branch), plus what the A650's GMU needs: the GMU's
SMMU context and memory (NativeAdrenoGmuContext/Memory), the HFI kit (kits/AdrenoFirmware) and the A650 cold start
(package/A650Gmu.c). NativeAdrenoService.cpp (sheng's A740 service) stays out; the board probe (package/GpuProbe.cpp,
pipa-gpu=<mode>) drives these objects. No firmware is embedded here: the GMU, SQE and zap images are the board probe's
(build-gop-legacy-display.py)."""
import json, os, re, subprocess, sys
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]
c_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/bsd_init.o.json').read_text().strip().rstrip(','))
cxx_recipe = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/iokit/DEVELOPMENT/IOPlatformExpert.cpo.json').read_text().strip().rstrip(','))
# scripts/build-xnu-4k-baseline.sh links CONFIG_XNU_NATIVE_ADRENO from
# build/adreno740/libAdreno740.a -- the A740 name is hard-coded there, not taken from
# CONFIG_XNU_NATIVE_ADRENO_ARCHIVE -- so the A650 archive is produced under that name.
out = ROOT / 'build/adreno740'
out.mkdir(parents=True, exist_ok=True)
# The driver checkout is an input for the same reason as the kits: Adreno650 is not a
# component in UniXNU's catalogue, so the tree has no drivers/Adreno650 unless this names it.
driver = Path(os.environ.get('ADRENO650_DRIVER', str(ROOT / 'drivers/Adreno650')))
# The three upstream kits this recipe compiles are not copied into the generated tree unless a
# selected component asks for them, so the directory is an input rather than an assumption:
# ADRENO650_KITS names it, and a build passes this repository's kits directory.
import os
KITS = Path(os.environ.get('ADRENO650_KITS', str(ROOT / 'kits')))
sources = sorted(KITS.joinpath('ArmSmmu4K').glob('ArmSmmu4K*.c')) + [KITS / 'AdrenoPM4' / 'AdrenoPM4.c']
# The HFI kit and the A650 cold start that uses it.
sources += [KITS / 'AdrenoFirmware' / (name + '.c') for name in ('AdrenoFirmware', 'AdrenoHfi', 'AdrenoHfiStartup', 'AdrenoHfiVotes')]
sources += [KITS / 'pipa' / 'A650Gmu.c']
# The GPU half of NativeAdreno and the GMU's SMMU context. Left out: sheng's GMU platform nodes and service
# (NativeAdrenoService.cpp, NativeAdrenoGmuService/Platform/Config) and the accelerator.
sources += [driver / (name + '.cpp') for name in (
    'NativeAdrenoGpuContext', 'NativeAdrenoSmmuContext', 'NativeAdrenoSmmuMemory',
    'NativeAdrenoMemory', 'NativeAdrenoCpu', 'NativeAdrenoVm', 'NativeAdrenoRing', 'NativeAdrenoSubmit', 'NativeAdrenoTimer',
    'NativeAdrenoUserClient', 'NativeAdrenoPublish', 'NativeAdrenoCpInit', 'NativeAdrenoIrq',
    'NativeAdrenoGmuContext', 'NativeAdrenoGmuMemory')]
objects = []
for source in sources:
    recipe = c_recipe if source.suffix == '.c' else cxx_recipe
    obj = out / (source.stem + ('.kit.o' if source.suffix == '.c' else '.o'))
    command, i = [], 0
    while i < len(recipe['arguments']):
        value = recipe['arguments'][i]
        if value == recipe['file']:
            command.append(str(source))
        elif value == '-o':
            command += ['-o', str(obj)]
            i += 1
        elif value in ('-MF', '-MJ'):
            i += 1
        elif value == '-Xclang' and recipe['arguments'][i + 1].startswith('-W'):
            i += 1
        elif not value.startswith('-W') and value not in ('-MD', '-MP'):
            command.append(value)
        i += 1
    command += ['-I', str(ROOT / 'kits/AdrenoPM4'), '-I', str(ROOT / 'kits/ArmSmmu4K'), '-I', str(ROOT / 'kits/AdrenoFirmware'),
                '-I', str(KITS / 'pipa'), '-I', str(KITS / 'AdrenoFirmware'), '-I', str(KITS / 'AdrenoPM4'), '-I', str(KITS / 'ArmSmmu4K'), '-I', str(ROOT / '.cache/xnu-work/osfmk'),
                '-I', str(driver), '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-Wno-incompatible-sysroot', '-Wno-c99-designator',
                '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier']
    result = subprocess.run(command, cwd=recipe['directory'], capture_output=True, text=True)
    if result.returncode:
        sys.exit(str(source) + ':\n' + result.stderr[-2500:])
    objects.append(obj)
archive = out / 'libAdreno740.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
# Every NativeAdreno or kit symbol the objects use must be defined by them; the rest is the kernel's.
nm = subprocess.run(['xcrun', 'nm', '-gP', archive], capture_output=True, text=True, check=True).stdout.splitlines()
defined = {f.split()[0] for f in nm if len(f.split()) > 1 and f.split()[1] in 'TSDBC'}
undefined = {f.split()[0] for f in nm if len(f.split()) > 1 and f.split()[1] == 'U'}
missing = sorted(s for s in undefined - defined if re.search(r'NativeAdreno|native_adreno|ArmSmmu4K|AdrenoPM4|AdrenoHfi|AdrenoFirmware|A650Gmu|Sheng', s))
if missing:
    sys.exit('NativeAdreno symbols used but not built here: ' + ', '.join(missing))
required = ('_NativeAdrenoGpuContextBringup', '_NativeAdrenoGpuContextBuffers', '_NativeAdrenoGpuContextVm', '_native_adreno_publish',
            '_ArmSmmu4KAttachStreams', '_AdrenoPM4Type7', '_NativeAdrenoGmuContextBringupRoutes', '_NativeAdrenoGmuContextPayloads',
            '_A650GmuColdStart', '_A650GmuBuildStartup')
absent = [s for s in required if s not in defined]
if absent:
    sys.exit('Required definitions absent: ' + ', '.join(absent))
print('PASS: %d NativeAdreno a650 objects archived' % len(objects))
