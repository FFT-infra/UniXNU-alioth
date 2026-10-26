#!/usr/bin/env python3
"""Build the opt-in board diagnostic against this source kernel's actual ABI.

This is build-boot-progress.py of UniXNU-sheng with the board coupling removed.
The sources are the device package's copy (package/BootProgress.c and its eight
companions), not a shared kit, and the one UEFI header the display layer needs
(Font.h) comes from this board's Mu-Silicium checkout, which the driver passes
as UNIXNU_UEFI, rather than from a sibling repository's fixed path.
"""
import hashlib
import json
import os
import pathlib
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def require(value, message):
    if not value:
        raise SystemExit(message)


OUT = ROOT / 'build/boot-progress'
OUT.mkdir(parents=True, exist_ok=True)
# The device package is linked into the tree as kits/<kit> (see scripts/unixnu/tree.py), so locate it the way
# build-platform-power.py does instead of assuming a fixed directory name.
sources = sorted(ROOT.glob('kits/*/BootProgress.c'))
require(len(sources) == 1, 'Expected one device package with BootProgress.c')
PACKAGE = sources[0].parent


def font_dir():
    """The Mu-Silicium checkout this board builds its firmware from.

    BootProgressDisplay.h includes Font.h, which belongs to the UEFI tree rather than to XNU. The driver passes
    that checkout as UNIXNU_UEFI; without it, find the sibling checkout by walking up from the tree, whose depth
    depends on the layout (out/<board>/tree/ before the migration, out/<device>/<os_profile>/<config>/tree/ after).
    """
    candidates = [os.environ.get('UNIXNU_UEFI')]
    candidates += [str(parent / 'uefi-alioth') for parent in ROOT.parents]
    for candidate in candidates:
        if not candidate:
            continue
        directory = Path(candidate) / 'Silicon/Silicium/SiliciumPkg/Library/FrameBufferSerialPortLib'
        if (directory / 'Font.h').is_file():
            return directory
    raise SystemExit('BootProgressDisplay.h includes Font.h from the Mu-Silicium checkout; run with UNIXNU_UEFI set')


record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/osfmk/DEVELOPMENT/startup.o.json').read_text().strip().rstrip(','))
source = PACKAGE / 'BootProgress.c'
# Compile from a private copy on local disk. Reading straight off the virtiofs mount makes
# clang-24 abort with `MemoryBuffer::init: Buffer is not null terminated` when the shared
# directory's cached size and content disagree for a moment, which happens while this workspace is
# edited from the other side. The copy pins one consistent inode; the hash check proves it is the
# same file. build-gop-legacy-display.py does the same for the same reason.
import hashlib
import shutil
import tempfile
def _sha(path):
    return hashlib.sha256(pathlib.Path(path).read_bytes()).hexdigest()
_stage = pathlib.Path(tempfile.mkdtemp(prefix='unixnu-boot-progress-'))
_staged = _stage / 'BootProgress.c'
shutil.copyfile(source, _staged)
# BootProgress.c includes its own headers from the package directory, so the copy has to bring
# them or the compile fails on the first include.
for _header in ('BootProgressCore.h', 'BootProgressDisplay.h', 'BootProgressLifecycle.h',
                'BootProgressPulse.h', 'BootResetCore.h'):
    _from = source.parent / _header
    if _from.is_file():
        shutil.copyfile(_from, _stage / _header)
if _sha(_staged) != _sha(source):
    raise SystemExit(f'{source} changed while it was copied for compilation; rerun the build')
source = _staged
obj = OUT / 'BootProgress.o'
command = []
args = record['arguments']
i = 0
while i < len(args):
    arg = args[i]
    if arg == record['file']:
        command.append(str(source))
    elif arg in ('-o', '-MF', '-MT', '-MJ'):
        if arg == '-o':
            command += ['-o', str(obj)]
        i += 1
    elif arg == '-Xclang' and args[i+1].startswith('-W'):
        i += 1
    elif arg == '-D' and args[i+1].startswith('XNU_BOOT_PROGRESS='):
        i += 1
    elif not arg.startswith(('-W', '-DXNU_BOOT_PROGRESS=')):
        command.append(arg)
    i += 1
command += ['-DXNU_BOOT_PROGRESS=1', '-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror',
            '-I', str(ROOT / '.cache/xnu-work/osfmk'), '-I', str(PACKAGE), '-I', str(font_dir())]
subprocess.run(command, cwd=record['directory'], check=True, timeout=90)
(OUT / 'compile-command.json').write_text(json.dumps(command, indent=2) + '\n')
fault_source = PACKAGE / 'BootFaultProbe.c'
fault_obj = OUT / 'BootFaultProbe.o'
fault_command = [str(fault_source) if arg == str(source) else str(fault_obj) if arg == str(obj) else arg for arg in command]
subprocess.run(fault_command, cwd=record['directory'], check=True, timeout=90)
policy_source = PACKAGE / 'BootWatchdogPolicy.c'
policy_obj = OUT / 'BootWatchdogPolicy.o'
policy_record = json.loads((ROOT / 'build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/kern_exec.o.json').read_text().strip().rstrip(','))
policy_command = []
policy_args = policy_record['arguments']
j = 0
while j < len(policy_args):
    arg = policy_args[j]
    if arg == policy_record['file']:
        policy_command.append(str(policy_source))
    elif arg in ('-o', '-MF', '-MT', '-MJ'):
        if arg == '-o':
            policy_command += ['-o', str(policy_obj)]
        j += 1
    elif arg == '-Xclang' and policy_args[j+1].startswith('-W'):
        j += 1
    elif not arg.startswith('-W'):
        policy_command.append(arg)
    j += 1
policy_command += ['-mgeneral-regs-only', '-Wall', '-Wextra', '-Werror', '-I', str(PACKAGE)]
subprocess.run(policy_command, cwd=policy_record['directory'], check=True, timeout=90)
# The diagnostic file export (BootDiagExport.c) reads files through the VFS: the same BSD-side flags.
diag_source = PACKAGE / 'BootDiagExport.c'
diag_obj = OUT / 'BootDiagExport.o'
diag_command = [str(diag_source) if arg == str(policy_source) else str(diag_obj) if arg == str(policy_obj) else arg for arg in policy_command]
subprocess.run(diag_command, cwd=policy_record['directory'], check=True, timeout=90)
# The firmware file reader of the audio DSP bring-up (AdspProbe.cpp) uses the VFS too.
fw_source = PACKAGE / 'AdspFirmware.c'
fw_obj = OUT / 'AdspFirmware.o'
fw_command = [str(fw_source) if arg == str(policy_source) else str(fw_obj) if arg == str(policy_obj) else arg for arg in policy_command]
subprocess.run(fw_command, cwd=policy_record['directory'], check=True, timeout=90)

# The UFS diagnostic log writer (boot argument sheng-log=1) and the guarded fastboot reset
# (sheng-autoreset=<seconds>). BootLog.c reaches the board only through weak imports, so the
# sheng copy is used unchanged; BootReset.c is this board's own (IMEM, not the SPMI arbiter).
log_source = PACKAGE / 'BootLog.c'
log_obj = OUT / 'BootLog.o'
log_command = [str(log_source) if arg == str(source) else str(log_obj) if arg == str(obj) else arg for arg in command]
subprocess.run(log_command, cwd=record['directory'], check=True, timeout=90)
reset_source = PACKAGE / 'BootReset.c'
reset_obj = OUT / 'BootReset.o'
reset_command = [str(reset_source) if arg == str(source) else str(reset_obj) if arg == str(obj) else arg for arg in command]
subprocess.run(reset_command, cwd=record['directory'], check=True, timeout=90)

archive = OUT / 'libBootProgress.a'
archive.unlink(missing_ok=True)
subprocess.run(['xcrun', 'ar', 'rcs', str(archive), str(obj), str(fault_obj), str(policy_obj), str(diag_obj), str(fw_obj),
                str(log_obj), str(reset_obj)], check=True, timeout=30)
(OUT / 'build-report.json').write_text(json.dumps({
    'watchdog_policy_sha256': hashlib.sha256(policy_source.read_bytes()).hexdigest(),
    'diag_sha256': hashlib.sha256(diag_source.read_bytes()).hexdigest(),
    'fault_sha256': hashlib.sha256(fault_source.read_bytes()).hexdigest(),
    'source_sha256': hashlib.sha256(source.read_bytes()).hexdigest(),
    'lifecycle_sha256': hashlib.sha256((source.parent / 'BootProgressLifecycle.h').read_bytes()).hexdigest(),
    'core_sha256': hashlib.sha256((source.parent / 'BootProgressCore.h').read_bytes()).hexdigest(),
    'display_sha256': hashlib.sha256((source.parent / 'BootProgressDisplay.h').read_bytes()).hexdigest(),
    'pulse_sha256': hashlib.sha256((source.parent / 'BootProgressPulse.h').read_bytes()).hexdigest(),
    'boot_log_sha256': hashlib.sha256(log_source.read_bytes()).hexdigest(),
    'boot_reset_sha256': hashlib.sha256(reset_source.read_bytes()).hexdigest(),
    'boot_reset_core_sha256': hashlib.sha256((source.parent / 'BootResetCore.h').read_bytes()).hexdigest(),
    'archive_sha256': hashlib.sha256(archive.read_bytes()).hexdigest(),
    'hardware_execution': False,
}, indent=2) + '\n')
print('PASS: strict source-kernel ARM64 boot diagnostic build')
