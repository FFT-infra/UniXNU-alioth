#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause-Patent
# alioth (K40, SM8250) FocalTech FT5652 touch bring-up: compiles the board probe
# (FocalTouchProbe.cpp), the FTS5652 protocol core (FocalTouchProtocol.c), the
# GENI SPI engine (GeniSpi.c) and the HID digitizer consumer (NativeHidInput.cpp)
# with the kernel's own flags, then packs them into an archive the prelink stage
# can merge (the same pattern the sm8550 port uses for its touch archive).
#
# Run after `unixnu build kernel`, from the UniXNU tree root (ROOT). The device
# repository (this tree's parent) supplies the kit files; the HID consumer comes
# from the ShengHidInput driver feed.
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent          # UniXNU tree
# The kit comes from the device checkout. unixnu runs this script from the build tree
# with the device repository as the working directory's parent only on a plain checkout,
# so try the declared device, then the tree's own kits/ link (scripts/unixnu/tree.py links
# every device kit there), then the parent as a last resort.
_candidates = []
if os.environ.get("UNIXNU_DEVICE"):
    _candidates.append(Path(os.environ["UNIXNU_DEVICE"]) / "kits" / "alioth-touch")
_candidates.append(ROOT / "kits" / "alioth-touch")
_candidates.append(ROOT.parent / "kits" / "alioth-touch")
KIT = next((c for c in _candidates if c.exists()), _candidates[0])
DEVICE = KIT.parent.parent
if not KIT.exists():
    sys.exit(f"alioth-touch kit not found (tried {[str(c) for c in _candidates]})")
OUT = ROOT / "build" / "alioth-touch"
OUT.mkdir(parents=True, exist_ok=True)

def run(cmd, **kw):
    return subprocess.check_call(cmd, **kw)

def require(cond, msg):
    if not cond:
        sys.exit(msg)

def code(data, macho_):
    return hashlib.sha256(data).hexdigest()

def sha(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()

# HID consumer: NativeHidInput.cpp from the ShengHidInput driver feed. When the
# feed is absent (public compile check) the probe alone is still built: it only
# needs the HID consumer at link time, which the port's own build provides.
# The HID consumer lives in the ShengHidInput feed. The tree links it under drivers/ only
# while the component is selected in board.toml, and this board selects only the touch kit,
# so fall back to the fetched feed itself.
_hid_candidates = [ROOT / "drivers" / "ShengHidInput"]
# ROOT is the build tree (out/<device>/<profile>/<config>/tree), so the fetched feeds sit
# beside the whole out/ directory, three levels up.
# tree = out/<device>/<profile>/<config>/tree, so out/feeds is parents[3]/feeds.
for _feeds in (ROOT / "out" / "feeds", ROOT.parents[3] / "feeds"):
    if _feeds.is_dir():
        _hid_candidates += sorted(_feeds.glob("ShengHidInput-*"))
        break
hid = next((c for c in _hid_candidates if (c / "NativeHidInput.cpp").exists()), _hid_candidates[0])
has_hid = (hid / "NativeHidInput.cpp").exists()

# Compile-command templates from the kernel build (both live under build/xnu/ios-obj).
ios = ROOT / "build" / "xnu" / "ios-obj"
expert = ios / "DEVELOPMENT_ARM64_BCM2837" / "iokit" / "DEVELOPMENT" / "IOPlatformExpert.cpo.json"
startup = ios / "DEVELOPMENT_ARM64_BCM2837" / "osfmk" / "DEVELOPMENT" / "startup.o.json"
require(expert.exists(), "IOPlatformExpert.cpo.json missing: run `unixnu build kernel` first")
require(startup.exists(), "startup.o.json missing: run `unixnu build kernel` first")

def cpp_cmd(source, obj, json_path, extra_inc=None):
    """C++ objects: the IOPlatformExpert compile line with source/object swapped."""
    r = json.loads(json_path.read_text().strip().rstrip(","))
    cmd, i = [], 0
    while i < len(r["arguments"]):
        value = r["arguments"][i]
        if value == r["file"]:
            cmd.append(str(source))
        elif value == "-o":
            cmd += ["-o", str(obj)]
            i += 1
        elif value == "-Xclang" and r["arguments"][i + 1].startswith("-W"):
            i += 1
        elif not value.startswith("-W"):
            cmd.append(value)
        i += 1
    cmd += ["-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
            "-Wno-unnecessary-virtual-specifier", "-Wno-c99-designator", "-Wno-reorder-init-list",
            "-Wno-deprecated-enum-enum-conversion", "-I", str(ROOT / ".cache/xnu-work/osfmk")]
    if extra_inc:
        cmd += ["-I" + str(p) for p in extra_inc]
    return cmd, r["directory"]

def c_cmd(source, obj, json_path, extra_inc=None):
    """C objects: the osfmk startup compile line with source/object swapped."""
    r = json.loads(json_path.read_text().strip().rstrip(","))
    cmd, i = [], 0
    while i < len(r["arguments"]):
        value = r["arguments"][i]
        if value == r["file"]:
            cmd.append(str(source))
        elif value in ("-o", "-MF", "-MT", "-MJ"):
            if value == "-o":
                cmd += ["-o", str(obj)]
            i += 1
        elif value == "-Xclang" and r["arguments"][i + 1].startswith("-W"):
            i += 1
        elif not value.startswith("-W"):
            cmd.append(value)
        i += 1
    cmd += ["-Wall", "-Wextra", "-Werror", "-mgeneral-regs-only",
            "-I", str(ROOT / ".cache/xnu-work/osfmk")]
    if extra_inc:
        cmd += ["-I" + str(p) for p in extra_inc]
    return cmd, r["directory"]

inc = [KIT]
if has_hid:
    inc.append(hid)

touch_cpp = KIT / "FocalTouchProbe.cpp"
touch_obj = OUT / "FocalTouchProbe.o"
cmd, cwd = cpp_cmd(touch_cpp, touch_obj, expert, inc)
run(cmd, cwd=cwd)

objs = [touch_obj]
if has_hid:
    hid_obj = OUT / "NativeHidInput.o"
    cmd, cwd = cpp_cmd(hid / "NativeHidInput.cpp", hid_obj, expert, inc)
    run(cmd, cwd=cwd)
    objs.append(hid_obj)

c_sources = [KIT / "FocalTouchProtocol.c", KIT / "GeniSpi.c"]
for src in c_sources:
    obj = OUT / (src.stem + ".o")
    objs.append(obj)
    ccmd, ccwd = c_cmd(src, obj, startup, inc)
    run(ccmd, cwd=ccwd)

archive = OUT / "libNativeAliothTouch.a"
archive.unlink(missing_ok=True)
run(["xcrun", "ar", "rcs", archive, *objs])

(OUT / "build-report.json").write_text(json.dumps({
    "archive": str(archive),
    "objects": [str(o) for o in objs],
    "source_sha256": {str(p.relative_to(ROOT.parent)) if p.is_relative_to(ROOT.parent) else str(p): sha(p)
                      for p in [touch_cpp, hid / "NativeHidInput.cpp", *c_sources]},
}, indent=2) + "\n")
print(f"Built {archive} ({len(objs)} objects)")

# Also add the objects to libLinkStubs.a. The kernel link takes a single
# CONFIG_XNU_EXTRA_LD_ARCHIVES path here, and a second, space-separated path does not reach
# the link (measured 2026-10-27: the archive stayed out of the commands entirely).
# build-link-stubs.py runs earlier, so its archive already exists; adding members to an
# archive in place is what `ar r` does.
stub_archive = ROOT / "build" / "link-stubs" / "libLinkStubs.a"
if stub_archive.exists():
    run(["xcrun", "ar", "rcs", str(stub_archive), *[str(o) for o in objs]])
    print(f"Added {len(objs)} objects to {stub_archive}")
