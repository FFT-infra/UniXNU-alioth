#!/usr/bin/env python3
"""Host test of scripts/check-adt-properties.py against synthesised device trees.

The guard exists because `unixnu.devicetree.hardware_properties()` copies only keys named
in five fixed sets and its if/elif chain has no final else, so any other key is silently
omitted: the node reaches the ADT without it and nothing is logged. The guard's whole
value is that it fires on exactly the dropped keys and on nothing else, so both halves
are tested here -- a tree that must pass, and a tree that must fail naming precisely the
three keys `AppleNubSynopsysOTGDevice` requires.

Build a tree with `dtc`, run the guard, check its exit code and its message. Run:

    python3 package/tests/adt_properties_host.py

Needs `dtc` on PATH (`brew install dtc`); the base repository already requires it.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GUARD = ROOT / 'scripts' / 'check-adt-properties.py'
DTC = 'dtc'

HEADER = '/dts-v1/;\n/ {\n'

# A node the ADT writer can export completely: every key is in its writable set.
CLEAN_NODE = '''
    good@0 {
        compatible = "test,good";
        unixnu,adt-name = "good";
        reg = <0 0x1000 0 0x100>;
        status = "okay";
    };
'''

# The three keys AppleNubSynopsysOTGDevice reads, none of which the writer copies.
DROPPED_NODE = '''
    usbdev@a600000 {
        compatible = "snps,dwc3";
        unixnu,adt-name = "usb-device,t7000";
        reg = <0 0xa600000 0 0xd93c>;
        fifo-depth = <27696>;
        phy-id = <0>;
        clock-mask = <0x7>;
        status = "okay";
    };
'''

# Same node, disabled: devicetree.py skips it (entry.enabled()), so nothing is exported
# and the guard must stay quiet.
DISABLED_NODE = DROPPED_NODE.replace('status = "okay";', 'status = "disabled";')

FOOTER = '};\n'


def build(tmp, body):
    src = tmp / 'tree.dts'
    dtb = tmp / 'tree.dtb'
    src.write_text(HEADER + body + FOOTER)
    result = subprocess.run([DTC, '-I', 'dts', '-O', 'dtb', '-o', str(dtb), str(src)],
                            capture_output=True, text=True)
    if result.returncode:
        raise SystemExit(f'dtc failed: {result.stderr.strip()}')
    return dtb


def run_guard(dtb):
    result = subprocess.run([sys.executable, str(GUARD), str(dtb)],
                            capture_output=True, text=True)
    return result.returncode, result.stdout + result.stderr


def main():
    if not GUARD.is_file():
        raise SystemExit(f'guard missing: {GUARD}')
    cases = []

    with tempfile.TemporaryDirectory() as name:
        tmp = Path(name)

        # 1. Everything writable: the guard must pass.
        code, out = run_guard(build(tmp, CLEAN_NODE))
        cases.append(('clean tree passes', code, 0, out, None))

        # 2. A dropped-key node that is enabled: the guard must fail and name all three.
        code, out = run_guard(build(tmp, DROPPED_NODE))
        cases.append(('dropped keys fail', code, 1, out,
                      ['clock-mask', 'fifo-depth', 'phy-id']))

        # 3. The same node disabled: not exported, so the guard must pass. This is the case
        #    that makes the nodes in sm8250.dtsi safe to carry before they are switched on.
        code, out = run_guard(build(tmp, DISABLED_NODE))
        cases.append(('disabled node passes', code, 0, out, None))

        # 4. A missing path: usage error, distinct from a finding.
        result = subprocess.run([sys.executable, str(GUARD), str(tmp / 'nope.dtb')],
                                capture_output=True, text=True)
        cases.append(('missing dtb is a usage error', result.returncode, 2,
                      result.stdout + result.stderr, None))

    failures = 0
    for label, got, want, out, must_name in cases:
        ok = got == want
        if ok and must_name is not None:
            missing = [k for k in must_name if k not in out]
            extra = [k for k in ('compatible', 'reg', 'status', 'unixnu,adt-name')
                     if k in out]
            if missing:
                print(f'FAIL {label}: exit {got} as expected, but did not name {missing}')
                ok = False
            elif extra:
                print(f'FAIL {label}: named writable keys it must ignore: {extra}')
                ok = False
        if ok:
            print(f'pass {label} (exit {got})')
        else:
            print(f'FAIL {label}: exit {got}, wanted {want}')
            print(out.rstrip())
            failures += 1
    return 1 if failures else 0


if __name__ == '__main__':
    raise SystemExit(main())
