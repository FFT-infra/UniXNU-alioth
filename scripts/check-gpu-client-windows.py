#!/usr/bin/env python3
"""Check a GPU client window table against the aperture and against measured peaks.

  check-gpu-client-windows.py [MiB,MiB,MiB,MiB]

With no argument it checks the table the driver carries now (NativeAdrenoPublish.cpp
kClientWindows) and the proposed 768,320,224,224. Every check is arithmetic; none needs
a device.

The two measured fields are different quantities and must not be compared directly:

  GPUCCLI peak     absolute GPU address      record->peak_mib = windowPeak() >> 20
                   (NativeAdrenoPublish.cpp:312, :454)
  GPUSLOT  hi      offset into the window    clientStats()[5] =
                   (window_peak_ - window_start_) >> 20
                   (NativeAdrenoUserClient.cpp:291)

So "peak 1727" in slot 2 (start 1706) means 21 MiB used, not 1727 MiB used. The checks
below use the offsets for capacity and the absolute values only for coverage.
"""
import sys

# GpuProbe.cpp:1143 kDeviceInfo: va_start, va_size. The client may map only this range.
VA_START = 0x20000000
VA_SIZE = 0x60000000
APERTURE_END = VA_START + VA_SIZE

MIB = 1 << 20

# The table the driver carries now (reference/repo/Adreno650/NativeAdrenoPublish.cpp:118).
CURRENT = [(0x20000000, 0x40000000), (0x60000000, 0x0AAA9000),
           (0x6AAA9000, 0x0AAA9000), (0x75552000, 0x0AAA9000)]
PROPOSED = [(0x20000000, 768 * MIB), (0x20000000 + 768 * MIB, 320 * MIB),
            (0x20000000 + 1088 * MIB, 224 * MIB), (0x20000000 + 1312 * MIB, 224 * MIB)]

# Measured readings. slot is the slot the process actually held in that boot; the offset
# is GPUSLOT hi from the same snapshot; the absolute is GPUCCLI peak. Both fields are
# cross-checked against each other for every boot that carries both (see CROSS_CHECK).
MEASURED = [
    # process, boot, slot, offset (GPUSLOT hi), absolute (GPUCCLI peak)
    ("backboardd", "u250", 0, 356, 868),
    ("backboardd", "u251", 0, 520, 1032),
    ("backboardd", "u256", 0, 515, 1027),
    ("backboardd", "u258", 0, 536, 1048),
    ("backboardd", "u260", 0, 967, 1479),
    ("backboardd", "u266", 0, 242, 754),
    ("SpringBoard", "u266", 2, 21, 1727),
    ("SpringBoard", "u258", 2, 21, 1727),
]

# The driver's own comment (NativeAdrenoPublish.cpp:112) records 934 MiB for slot 0 in
# int-196 without saying which quantity it is. It is checked both ways.
INT196_SLOT0_MIB = 934


def check_table(name, table):
    problems = []
    if len(table) != 4:
        problems.append(f"{name}: expected 4 windows, got {len(table)}")
    for i, (start, size) in enumerate(table):
        if start % 4096:
            problems.append(f"{name} slot {i}: start {start:#x} is not page aligned")
        if size % 4096:
            problems.append(f"{name} slot {i}: size {size:#x} is not a page multiple")
        if start < VA_START or start + size > APERTURE_END:
            problems.append(f"{name} slot {i}: {start:#x}..{start + size:#x} leaves the aperture "
                            f"{VA_START:#x}..{APERTURE_END:#x}")
    ordered = sorted(table)
    for i in range(len(ordered) - 1):
        if ordered[i][0] + ordered[i][1] > ordered[i + 1][0]:
            problems.append(f"{name}: slot at {ordered[i][0]:#x} overlaps the next")
    return problems


def report(name, table):
    print(f"\n=== {name} ===")
    total = sum(size for _, size in table)
    lo = min(start for start, _ in table)
    hi = max(start + size for start, size in table)
    for i, (start, size) in enumerate(table):
        print(f"  slot {i}: {start:#010x}..{start + size:#010x}  "
              f"{start // MIB:>5}..{(start + size) // MIB:<5} MiB  {size // MIB:>4} MiB")
    print(f"  total {total // MIB} MiB ({total:#x} bytes); span {lo // MIB}..{hi // MIB} MiB")
    problems = check_table(name, table)
    print("  geometry: " + ("OK" if not problems else "FAIL"))
    for problem in problems:
        print("    - " + problem)
    return total, lo, hi, problems


def main():
    if len(sys.argv) > 1:
        parts = [int(p) for p in sys.argv[1].split(',')]
        if len(parts) != 4 or sum(parts) * MIB != VA_SIZE:
            sys.exit('four MiB sizes that sum to 1536 are required')
        start = VA_START
        proposed = []
        for size in parts:
            proposed.append((start, size * MIB))
            start += size * MIB
        report('argument', proposed)
        return

    cur_total, cur_lo, cur_hi, cur_problems = report('current (kClientWindows)', CURRENT)
    new_total, new_lo, new_hi, new_problems = report('proposed 768,320,224,224', PROPOSED)

    print("\n=== aperture coverage ===")
    print(f"  aperture {VA_START // MIB}..{APERTURE_END // MIB} MiB, {VA_SIZE // MIB} MiB")
    print(f"  current total {cur_total // MIB} MiB -> {VA_SIZE - cur_total} bytes unused")
    print(f"  proposed total {new_total // MIB} MiB -> {VA_SIZE - new_total} bytes unused")
    for label, total in (('current', cur_total), ('proposed', new_total)):
        verdict = 'equals va_size' if total == VA_SIZE else 'differs from va_size'
        print(f"  {label}: {verdict}")

    print("\n=== cross-check: peak - slot start must equal GPUSLOT hi ===")
    bad = 0
    for process, boot, slot, offset, absolute in MEASURED:
        start = CURRENT[slot][0] // MIB
        derived = absolute - start
        agrees = derived == offset
        if not agrees:
            bad += 1
        print(f"  {process:<13} {boot} slot {slot}: {absolute} - {start} = {derived} "
              f"vs GPUSLOT hi {offset} -> {'agrees' if agrees else 'DISAGREES'}")
    print(f"  {'all agree: peak is an absolute address' if not bad else 'MISMATCH: fix the model before trusting it'}")

    print("\n=== measured offsets against the window each process would get ===")
    print("  (offset is GPUSLOT hi; capacity is the slot's size)")
    for process, boot, slot, offset, absolute in MEASURED:
        for label, table in (('current', CURRENT), ('proposed', PROPOSED)):
            capacity = table[slot][1] // MIB
            fits = offset <= capacity
            print(f"  {process:<13} {boot} slot {slot} offset {offset:>4} MiB "
                  f"vs {label:<8} {capacity:>4} MiB -> {'fits' if fits else 'DOES NOT FIT'}")
    print()
    print("  absolute addresses are covered by the union of the windows:")
    for process, boot, slot, offset, absolute in MEASURED:
        for label, table in (('current', CURRENT), ('proposed', PROPOSED)):
            inside = any(s <= absolute * MIB < s + n for s, n in table)
            print(f"  {process:<13} {boot} absolute {absolute:>5} MiB "
                  f"vs {label:<8} -> {'covered' if inside else 'NOT COVERED'}")

    print("\n=== open question: int-196 ===")
    print(f"  the driver comment records {INT196_SLOT0_MIB} MiB for slot 0. Read as an offset it "
          f"needs {INT196_SLOT0_MIB} MiB of window;")
    print(f"  read as an absolute address it needs {INT196_SLOT0_MIB - VA_START // MIB} MiB. "
          "The comment does not say which.")
    for label, table in (('current', CURRENT), ('proposed', PROPOSED)):
        capacity = table[0][1] // MIB
        print(f"  {label}: slot 0 is {capacity} MiB -> "
              f"{'fits as an offset' if INT196_SLOT0_MIB <= capacity else 'DOES NOT fit as an offset'}, "
              f"{'fits as an absolute' if INT196_SLOT0_MIB - VA_START // MIB <= capacity else 'does not fit as an absolute'}")

    print("\n=== verdict ===")
    if new_problems:
        print("  proposed table: geometry FAIL")
    else:
        print("  proposed table: geometry OK (page aligned, disjoint, inside the aperture, "
              "total equals va_size)")
    worst = max((offset, process, boot) for process, boot, slot, offset, _ in MEASURED if slot == 0)
    capacity_now, capacity_new = CURRENT[0][1] // MIB, PROPOSED[0][1] // MIB
    print(f"  slot 0 capacity: the largest measured offset is {worst[0]} MiB "
          f"({worst[1]} {worst[2]})")
    if worst[0] > capacity_new:
        print(f"  -> the proposed {capacity_new} MiB slot 0 is TOO SMALL: {worst[0]} > {capacity_new}. "
              "Do not use it.")
    else:
        print(f"  -> the proposed {capacity_new} MiB slot 0 fits (current is {capacity_now} MiB)")
    spring = max((offset, process, boot) for process, boot, slot, offset, _ in MEASURED if slot != 0)
    print(f"  other slots: the largest measured offset is {spring[0]} MiB ({spring[1]} {spring[2]}), "
          f"so slot 1/2/3 need at least {spring[0]} MiB each, not more")


if __name__ == '__main__':
    main()
