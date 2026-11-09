# 2026-11-09 in22 — the GPU renders a known image with the expected byte order

Official GPU section 3 (`docs/porting/gpu.md`: "Render a known image and inspect the result before
using UI workloads") is satisfied on the device by the GMEM colour-order test, read on the USB
console in the same boot.

## Inputs

| Item | Value |
| --- | --- |
| Image | `tmp/boot-in22.img`, sha256 `6b349beb8428674339ab0ea5dd1206e4e3093b87b053a595ab3d6afae46d6da5` |
| Tag | `sheng-build-tag=in22` |
| Console | `tmp/console-in221534311.txt`, sha256 `93f21846d4c308ae5fd08c964da514c0dded8457b8a93d48379d4eb35ae594a9` |
| Boot arguments | base list plus `sheng-swaptest=1 sheng-gpu-trace=1 sheng-data-reset=1 sheng-init=1 sheng-xpccache=1` |
| Command | `sh tmp/rounds-count2.sh tmp/boot-in22.img in22 1 220` |

## Readings

```
RESULT t=210 own=1 sw=241 data=3 lit=0 pa=183349000 ds=0 panel=911 frame=84 askers=6 witness=swap -> desktop
GTPROB2 swap devs  A3366cc B0a141e C1e1e1e Dcccccc p12288 e0
GTPROB2 swap devg  A3366cc B0a141e C1e1e1e Dcccccc p12288 e0
GTPROB2 swap hosts A3366cc B0a141e C1e1e1e Dcccccc p12288 e0
GTPROB2 swap hostg A3366cc B0a141e C1e1e1e Dcccccc p12288 e0
```

The test (`drivers/AdrenoMetal/NativeTurnipSwapTest.h`) fills every pixel with B=0x0a G=0x14 R=0x1e
and declares, per 1016/1016/32/32-pixel region:

| Region | Declared | Read |
| --- | --- | --- |
| A, draw R=0.8 G=0.4 B=0.2 | stored `33 66 cc` (B G R) | `A3366cc` |
| B, load+store only | `0a 14 1e` | `B0a141e` |
| C, input red as grey | `1e 1e 1e` | `C1e1e1e` |
| D, drawn red as grey | `cc cc cc` | `Dcccccc` |

All four transport kinds — system memory and GMEM, each with device-memory and host-imported
images — produce byte-identical regions, 12288 passes, 0 errors, on a boot whose compositor
accepted 241 swaps (`own=1`) and presented 84 frames. That is a known image with correct geometry
and colours through the same Turnip render path the desktop uses, not a synthetic register check.

## How it was read

`BootDiagExport.c` snapshots the newest 4 MiB of the GPU trace at 60, 120 and 180 s and prints
every `GpuTraceProblem` (event 65) record with its text (`GTPROB1..3`). The 60 s snapshot is too
early — the test starts twenty seconds after the render server publishes — and `GTPROB2` carries
the lines above.
