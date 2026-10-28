#!/usr/bin/env python3
"""Adreno650 (drivers/Adreno650, NativeAdreno for pipa's Adreno 650: a6xx with a GMU): build/adreno650/libAdreno650.a, the
archive that CONFIG_XNU_NATIVE_ADRENO links (CONFIG_XNU_NATIVE_ADRENO_ARCHIVE). After UniXNU-tb128fu's build-native-adreno-a6xx.py
(the Adreno 610 half: GPU context, ring, submission, timer, user client and publication objects with the SMMU and PM4 kits;
Adreno650 began as the pipa branch of the NativeAdreno fork, off its tb128fu branch), plus what the A650's GMU needs: the GMU's
SMMU context and memory (NativeAdrenoGmuContext/Memory), the HFI kit (kits/AdrenoFirmware) and the A650 cold start
(package/A650Gmu.c). NativeAdrenoService.cpp (sheng's A740 service) stays out; the board probe (package/GpuProbe.cpp,
pipa-gpu=<mode>) drives these objects. No firmware is embedded here: the GMU, SQE and zap images are the board probe's
(build-gop-legacy-display.py)."""
import json, os, re, shutil, subprocess, sys
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
# The board probe and its firmware: both are package files. The probe exports
# native_gpu_probe(), which the late display probe calls; the firmware object defines the
# three arrays it weak-imports.
PACKAGE = Path(os.environ.get('ADRENO650_PACKAGE', str(ROOT / 'package')))
sources += [PACKAGE / 'GpuProbe.cpp', PACKAGE / 'GpuFirmwareA650.c', PACKAGE / 'alioth_pcie.c']
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
                '-I', str(driver), '-I', str(PACKAGE), '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-Wno-incompatible-sysroot', '-Wno-c99-designator',
                # alioth_pcie.c leaves struct fields implicit on purpose (the ios-sweet tree
                # compiled it without -Werror); the kernel's record turns that into an error.
                '-Wno-missing-field-initializers',
                '-Wno-reorder-init-list', '-Wno-deprecated-enum-enum-conversion',
                '-Wno-unnecessary-virtual-specifier']
    # Mode 12: the staged AGXAccelerator registers the IOAcceleratorES service that the original
    # Metal discovery needs (IOAcceleratorES -> MetalPluginName -> NSBundle -> the plugin).
    if source.name == 'GpuProbe.cpp':
        command.append('-DPIPA_GPU_ACCELERATOR=1')
    result = subprocess.run(command, cwd=recipe['directory'], capture_output=True, text=True)
    if result.returncode:
        sys.exit(str(source) + ':\n' + result.stderr[-2500:])
    objects.append(obj)
archive = out / 'libAdreno740.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', archive, *objects], check=True)
print('PASS: %d NativeAdreno a650 objects archived' % len(objects))

# Mode 12's staged accelerator, the job scripts/build-adreno740-accelerator.py does for the A740
# checkout. The class derives from the original IOAcceleratorES, which this kernel does not
# build, so its three base symbols are bound to the addresses the audited record names
# (scripts/data/ioaccelerator-base-abi.json), and its initializer registration is stripped from
# __mod_init_func so the board probe can run it after IOGPUFamily has loaded. build-xnu links
# only libAdreno740.a, so it rides in the same archive.
plan = json.loads(Path('scripts/data/ioaccelerator-base-abi.json').read_text())
bindings = {
    '__ZN15IOAcceleratorESC2EPK11OSMetaClass': int(plan['protected_compatible_constructor_aliases'][0], 16),
    '__ZN15IOAcceleratorESD2Ev': int(plan['nondeleting_destructor'], 16),
    '__ZN15IOAcceleratorES10gMetaClassE': int(plan['metaclass'], 16),
}
compiler = cxx_recipe['arguments'][0]
llvm_bin = Path(compiler).parent
accel_obj = out / 'NativeAdrenoAccelerator.o'
command, i = [], 0
while i < len(cxx_recipe['arguments']):
    value = cxx_recipe['arguments'][i]
    if value == cxx_recipe['file']:
        command.append(str(driver / 'NativeAdrenoAccelerator.cpp'))
    elif value == '-o':
        command += ['-o', str(accel_obj)]
        i += 1
    elif value in ('-MF', '-MJ'):
        i += 1
    elif value == '-Xclang' and cxx_recipe['arguments'][i + 1].startswith('-W'):
        i += 1
    elif not value.startswith('-W') and value not in ('-MD', '-MP'):
        command.append(value)
    i += 1
command += ['-I', str(Path('.cache/xnu-work/osfmk')), '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
            '-Wno-unnecessary-virtual-specifier', '-Wno-c99-designator', '-Wno-reorder-init-list',
            '-Wno-deprecated-enum-enum-conversion', '-Wno-overloaded-virtual']
result = subprocess.run(command, cwd=cxx_recipe['directory'], capture_output=True, text=True)
if result.returncode:
    sys.exit('NativeAdrenoAccelerator.cpp:\n' + result.stderr[-2500:])
asm = out / 'original-bindings.S'
asm.write_text(''.join('.globl %s\n.set %s, %#x\n' % (name, name, address) for name, address in sorted(bindings.items())))
bind_obj = out / 'original-bindings.o'
subprocess.run([compiler, '-c', '--target=arm64-apple-ios17.0', '-o', str(bind_obj), str(asm)], check=True)
late = out / 'NativeAdrenoAccelerator.late.o'


def tool(name):
    found = shutil.which(name) or str(llvm_bin / name)
    if not Path(found).exists():
        sys.exit('missing ' + name + ' (the accelerator needs the LLVM binutils)')
    return found


subprocess.run([tool('llvm-objcopy'), '--remove-section=__DATA,__mod_init_func',
                '--globalize-symbol=__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp', str(accel_obj), str(late)], check=True)
subprocess.run([tool('llvm-ar'), 'rcs', str(archive), str(late), str(bind_obj)], check=True)
print('PASS: staged accelerator appended, IOAcceleratorES bound to %s'
      % ', '.join('%#x' % address for _, address in sorted(bindings.items())))

# Every NativeAdreno or kit symbol the objects use must be defined by them; the rest is the kernel's.
nm = subprocess.run(['xcrun', 'nm', '-gP', archive], capture_output=True, text=True, check=True).stdout.splitlines()
defined = {f.split()[0] for f in nm if len(f.split()) > 1 and f.split()[1] in 'TSDBC'}
undefined = {f.split()[0] for f in nm if len(f.split()) > 1 and f.split()[1] == 'U'}
missing = sorted(s for s in undefined - defined if re.search(r'NativeAdreno|native_adreno|ArmSmmu4K|AdrenoPM4|AdrenoHfi|AdrenoFirmware|A650Gmu|Sheng', s))
if missing:
    sys.exit('NativeAdreno symbols used but not built here: ' + ', '.join(missing))
required = ('_NativeAdrenoGpuContextBringup', '_NativeAdrenoGpuContextBuffers', '_NativeAdrenoGpuContextVm', '_native_adreno_publish',
            '_ArmSmmu4KAttachStreams', '_AdrenoPM4Type7', '_NativeAdrenoGmuContextBringupRoutes', '_NativeAdrenoGmuContextPayloads',
            '_A650GmuColdStart', '_A650GmuBuildStartup', '_native_adreno_accelerator_allocate')
absent = [s for s in required if s not in defined]
if absent:
    sys.exit('Required definitions absent: ' + ', '.join(absent))
print('PASS: archive verified with %d objects' % len(objects))
