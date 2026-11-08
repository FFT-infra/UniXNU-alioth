# norm1: a normal image without pcie= still loses c4 before the desktop

## Round

- Image: `tmp/boot-norm1.img`, sha256 `e4fa67c22845148832cb37724cb65f5452a5bdb158170b2ff844d709184cbdf0`.
- Kernel: `90ec4bbc772729fa757249cac79c963a5521b852c0b338119304485ae0f5e34b`, 125,861,888 bytes.
- Build: `TAG=norm1 scripts/build-round.sh`, no `pcie=` or probe arguments; `LINK_OK`.
- Console: `tmp/console-norm12341521.txt`, sha256
  `00689c44cd6dec896502d330f9e2f1d4d6431b6c17e121366d0dc832a5b45a7f`.
- One round: `sh tmp/rounds-count2.sh tmp/boot-norm1.img norm1 1 300`;
  `RESULT t=290 own=0 sw=0 data=2 frame=0 askers=0 -> no-desktop`.

## What it establishes

This image never calls `alioth_pcie_init()` (no `pcie=` boot argument), so the link-up probe
cannot be the cause of this stall. Yet the same failure shape appears:

```
DATAMOUNT stage=4 status=0 ro=0 store=2     (Data mounted)
[addon] 1=5/0 2=5/0 3=5/0                  (all three addon mounts landed)
[wallpaper db] c=0 r=0 n=0                  (probe ran before a container existed)
CORES t=10s  deaf=00
CORES t=30s  deaf=1e                        (c4 counters freeze)
PROC  t=90s  a=2 p=1 s=1                    (only a few userland processes)
PROC  t=290s a=9 p=9 s=1
SUPER t=290s own=0 sw=0/0/0
GPUCLI clients=0 askers=0[]
```

The good desktop round `wl1600071` instead has `DATAMOUNT stage=4`, addon `5/0`,
`SUPER own=1 sw=144/144/0`, `GPUCLI clients=7..8`, and `deaf=00`. The difference is the
c4 freeze and the missing userland GPU clients, not the Data or addon mount.

## The wallpaper probe is now a named suspect

The automatic `[wallpaper db]` probe was made to run on the first exec in `fd6d3b8`; the
`[wallpaper list]` probe runs at the same `tick % 200 == 0` point, which includes tick 0.
Both walk the Data container and read files inside the exec path. This round shows they ran
(`c=0`) while the boot still had no userland GPU clients.

Source `d37fbcc` moves both probes behind `tick > 200` (or an explicit `sheng-wp=1` /
`sheng-wallpaper-db=1`). `norm2` (`tmp/boot-norm2.img`, `3c7a0042…`) is built from that source;
it has not run yet. `pc6` (`tmp/boot-pc6.img`, `eb319e38…`) adds the same delay to the PCIe
probe plus the PARF interrupt mask; it has not run yet.

## Boundary

The c4 freeze is not proven to be caused by the wallpaper probe. norm2 is the control that
tests it. Desktop, wallpaper, widgets and Wi-Fi remain unverified in this round.
