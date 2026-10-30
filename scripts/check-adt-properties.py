#!/usr/bin/env python3
"""Refuse a device tree whose ADT-exported node carries a property the ADT writer drops.

`unixnu.devicetree.hardware_properties()` copies only keys named in five fixed sets
(STRINGS, U32, U64, FLAGS, REFERENCES). Its if/elif chain has no final else, so any
other key is silently omitted: the node reaches the ADT without it, and nothing is
logged. `AppleNubSynopsysOTGDevice` needs `fifo-depth`, `phy-id` and `clock-mask`,
none of which are in those sets, so a USB node that looks complete in the DTS arrives
incomplete in the ADT.

This check turns that silent omission into a build failure. It does not fix the
omission -- the properties still cannot reach the ADT -- it only makes the gap
visible at the point where the device tree is written.

usage: check-adt-properties.py <dtb>

Exit 0 when every exported node's properties are all writable, 1 otherwise, 2 on a
usage or read error.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'UniXNU' / 'scripts'))

from unixnu import devicetree  # noqa: E402

# The keys hardware_properties() actually writes. `reg` and `interrupts` have their own
# elif arms, and the export/selection keys (`unixnu,adt-name`, `status`) are consumed by
# the walk itself rather than copied into the node, so all of them are writable by
# definition and must not be reported.
WRITABLE = (devicetree.STRINGS | devicetree.U32 | devicetree.U64
            | devicetree.FLAGS | set(devicetree.REFERENCES)
            | {'interrupts', 'reg', 'status', 'unixnu,adt-name',
               '#address-cells', '#size-cells'})


def main():
    if len(sys.argv) != 2:
        sys.stderr.write(__doc__)
        return 2
    dtb = Path(sys.argv[1])
    if not dtb.is_file():
        sys.stderr.write(f'no such dtb: {dtb}\n')
        return 2
    root = devicetree.read_fdt(dtb.read_bytes())
    failures = []
    for entry in root.walk():
        if 'unixnu,adt-name' not in entry.properties or not entry.enabled():
            continue
        exported = entry.strings('unixnu,adt-name')[0]
        dropped = sorted(key for key in entry.properties if key not in WRITABLE)
        if dropped:
            failures.append((exported, entry.name, dropped))
    if not failures:
        print(f'{dtb}: every exported ADT node carries only writable properties')
        return 0
    sys.stderr.write(
        'The ADT writer would silently drop these properties. The node reaches the\n'
        'ADT without them and no message is logged, so a driver that reads one sees\n'
        'it missing at runtime. Fix by adding the keys to devicetree.py\'s STRINGS/\n'
        'U32/U64 sets in the base repository, or by not declaring them here.\n\n')
    for exported, node_name, dropped in failures:
        sys.stderr.write(f'  ADT node "{exported}" (from {node_name}) drops: '
                         f'{", ".join(dropped)}\n')
    return 1


if __name__ == '__main__':
    raise SystemExit(main())
