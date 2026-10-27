#!/usr/bin/env python3
"""Refuse a board whose device tree declares E/P clusters without the kernel side of them.

A cpu node carrying `unixnu,cluster-type` is only meaningful together with
native-smp-type-clusters.patch, which raises MAX_CPU_CLUSTERS above one and defines
__ARM_AMP__. Without it the topology parser takes the `phys_cluster_id = (cluster_type == P)`
branch while MAX_CPU_CLUSTERS is still 1, and writes cluster_phys_to_logical[1] past the end
of a one-element stack array. Release builds do not assert on that: the kernel simply dies
before the console comes up.

That cost three flash cycles on 2026-10-28 — the silent death was read first as a loader hang
and then as an entry-probe conflict, because nothing on screen or on the wire names the cause.
This check is the invariant that would have named it in one line.

  check-smp-topology.py                      # run from the device repository
  check-smp-topology.py --device DIR --tree TREE

Exit 0 when the board is consistent, 1 when it is not, 2 when the inputs cannot be read.
"""
import argparse
import re
import sys
from pathlib import Path

CLUSTER_PATCH = 'native-smp-type-clusters.patch'
CLUSTER_PROPERTY = 'unixnu,cluster-type'


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--device', default='.', help='device repository root (default: here)')
    parser.add_argument('--tree', help='build tree holding .cache/xnu-work (default: none, skip that half)')
    args = parser.parse_args()

    device = Path(args.device).resolve()
    dts_dir = device / 'dts'
    if not dts_dir.is_dir():
        print(f'check-smp-topology: {dts_dir} not found', file=sys.stderr)
        return 2

    declaring = []
    for path in sorted(dts_dir.glob('*.dts*')):
        text = path.read_text(errors='replace')
        count = len(re.findall(CLUSTER_PROPERTY, text))
        if count:
            declaring.append((path, count))

    if not declaring:
        print('check-smp-topology: no cluster-type in the device tree; nothing to check')
        return 0

    failures = []
    for path, count in declaring:
        print(f'check-smp-topology: {path.name} declares {count} {CLUSTER_PROPERTY} node(s)')

    # The kernel half lives in the SoC patches the build actually applied, so look at the tree
    # rather than at any repository checkout: a patch can sit in a checkout and never be in the
    # series the build reads, which is exactly how this was missed.
    if args.tree:
        tree = Path(args.tree)
        series = list(tree.glob('patches/xnu/series')) + list((tree / 'drivers').glob('*/patches/xnu/series'))
        soc_series = [p for p in tree.parents[3].glob('feeds/*/patches/xnu/series')]
        applied = []
        for path in series + soc_series:
            try:
                applied.append((path, path.read_text(errors='replace')))
            except OSError:
                continue
        if not any(CLUSTER_PATCH in text for _, text in applied):
            failures.append(f'{CLUSTER_PATCH} is not in any applied patch series under {tree}')

        config = tree / '.cache/xnu-work/pexpert/pexpert/arm64/board_config.h'
        if config.is_file():
            text = config.read_text(errors='replace')
            # Match the block the patch inserts rather than parsing the file's nesting: a
            # non-greedy scan to the next #endif stops at the wrong one and reads another
            # board's value, which is how this check first reported a false failure.
            if not ('#undef MAX_CPU_CLUSTERS' in text and re.search(r'#define\s+MAX_CPU_CLUSTERS\s+(\d+)', text)):
                failures.append('MAX_CPU_CLUSTERS is not redefined for this build')
            else:
                highest = max(int(m) for m in re.findall(r'#define\s+MAX_CPU_CLUSTERS\s+(\d+)', text))
                if highest < 2:
                    failures.append(f'MAX_CPU_CLUSTERS never reaches 2 (highest {highest})')
            if '__ARM_AMP__' not in text:
                failures.append('__ARM_AMP__ is not defined anywhere in board_config.h')
            if not re.search(r'#define\s+MAX_CPUS\s+(\d+)', text):
                failures.append('MAX_CPUS is not defined')
        else:
            failures.append(f'{config} not found; build the kernel first')
    else:
        print('check-smp-topology: no --tree given, so the kernel half was not checked')

    if failures:
        print('check-smp-topology: REFUSING this configuration', file=sys.stderr)
        for line in failures:
            print(f'  {line}', file=sys.stderr)
        print('  a board with cluster-type needs the cluster patch applied as one change', file=sys.stderr)
        return 1

    print('check-smp-topology: cluster-type is backed by the cluster patch and MAX_CPU_CLUSTERS >= 2')
    return 0


if __name__ == '__main__':
    sys.exit(main())
