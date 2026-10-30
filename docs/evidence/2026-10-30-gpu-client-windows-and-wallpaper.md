# GPU client windows, slot refusals and the wallpaper on alioth

2026-10-30, alioth (SM8250-AC, Adreno 650), iPadOS 17.7.11 (21H461), Darwin 23.6.0,
4 KiB pages. Device revision `2af006db44f11e10ebdff213e7017a3ea02c3887`.

Every reading below is from a captured boot and is reproducible with
`scripts/check-gpu-client-windows.py`. No reading in this document is a
prediction, and the places where the chain of evidence stops are marked.

## 1. Two client rows report different quantities

The GPU boot log prints two numbers per process that look comparable and are
not.

| row | field | source | quantity |
| --- | --- | --- | --- |
| `GPUCCLI <name> ... peak N` | `peak_mib` | `NativeAdrenoPublish.cpp:303` takes `core_->windowPeak()`, which `NativeAdrenoUserClient.cpp:289` sets to the mapping's end iova; `:312` stores `peak >> 20` | **absolute** GPU address |
| `GPUSLOT s N ... hi M` | `clientStats(...)[5]` | `NativeAdrenoUserClient.cpp:291` stores `(window_peak_ - window_start_) >> 20` | **window-relative** offset |

The two agree on every boot that carries both, which is what establishes the
reading rather than the source comments alone:

| boot | process | `GPUCCLI peak` | slot start | difference | `GPUSLOT hi` |
| --- | --- | --- | --- | --- | --- |
| u266 | backboardd | 754 | 512 | **242** | **242** |
| u258 | backboardd | 1048 | 512 | **536** | **536** |
| u260 | backboardd | 1479 | 512 | **967** | **967** |
| u266 | SpringBoard | 1727 | 1706 | **21** | **21** |
| u258 | SpringBoard | 1727 | 1706 | **21** | **21** |

Use the absolute value for coverage ("is this address inside the aperture") and
the relative value for capacity ("does this fit the slot's size").

## 2. SpringBoard's 1727 MiB is not a window overflow

```
GPUCCLI SpringBoard opens 4 refused 0 closes 3 slot 255 peak 1727 last 0
GPUSLOT s 2 live 22 peak 22 refused 0 last 0 bytes 0 hi 21
GPUSWAP validate 7 ok 1 reason 0 | copied 7 status 0 bytes 10368000 | retired 7 gen 7
GPUREASONS accepted 7 rejected 0 | r0 7 r1 0 r2 0 r3 0 r4 0 r5 0 r6 0 r7 0
```

SpringBoard held slot 2, whose window is `0x6AAA9000..0x75552000` (1706..1877 MiB,
170 MiB). Its offset is 21 MiB. Every `GPUSLOT` row in that snapshot reports
`refused 0`, and `GPUSWAP` reports no rejection. No mapping was refused for
leaving a window, and no swap was refused.

## 3. The binding constraint is slot 0, not slot 2

| boot | `GPUSLOT s 0 hi` (offset MiB) | `GPUCCLI backboardd peak` | that boot's panel |
| --- | --- | --- | --- |
| u250 | 356 | 868 | — |
| u251 | 520 | 1032 | — |
| u256 | 515 | 1027 | — |
| u258 | 536 | 1048 | luma 236 |
| **u260** | **967** | **1479** | `GPUFB nonblack 990/1000 luma 246`, `GPUFRAME gpu offered 830 done 830 fail 0`, zero AegirPoster exits |
| u266 | 242 | 754 | luma 29 |

u260 is the best boot on record and the render server used **967 MiB of slot 0**
there. A table that gives slot 0 less than that risks taking backboardd's device
partway through a boot that would otherwise draw fully.

## 4. `slot 255` and the refusal codes

`NativeAdrenoPublish.cpp:311` sets `record->slot = 0xff` on the **successful**
`clientClose` path. A row's `slot 255` therefore means "no live connection when
this row printed", not "never obtained a slot". The counter-example is
`docs/evidence/2026-10-29-gpu-gx-and-context.md`:
`SpringBoard opens 2 refused 0 closes 1 slot 1 peak 1674` — the same code, live
in slot 1.

`NativeAdrenoPublish.cpp:689` caps every non-backboardd process at 8 opens per
boot; `:692` refuses with `kIOReturnBusy` once `record->opens` reaches it.
AegirPoster's final row is `opens 8 refused 1 closes 8 slot 255 peak 0 last
e00002d5`: `e00002d5` is `kIOReturnBusy` at the cap, not `kIOReturnNoSpace`.
The only `NoSpace` row is `GPUMOUNT fail 1/e00002db slot 4`, the kernel client's
own second mount, which is not a fault. A fifth process does not currently meet
`NoSpace`, so the four slots are not what limits the posters.

## 5. The posters open the GPU and map nothing

```
GPUCCLI PosterBoard opens 1 refused 0 closes 1 slot 255 peak 0 last 0
GPUCCLI AegirPoster opens 8 refused 1 closes 8 slot 255 peak 0 last e00002d5
EXITLOG pid=238 name=AegirPoster status=6 ns=2 code=6
EXITCRASH pid=238 name=AegirPosterp238 AegirPoster sig6 PC1e9af2f74 LR1fb98cfa4 E0 F0 +1e368000 bt ...
```

`peak_mib` advances only on a successful `BoMap` (`:453`) or a successful
`clientClose` (`:312`), so `closes 8` with `peak 0` means eight successful mounts,
eight successful teardowns, and not one byte of GPU address space mapped. All
fifteen AegirPoster records carry the same PC.

A client-visible `OutOfMemory(Device)` in the same boot cannot come from this
driver: every `GPUSLOT` row reports `refused 0`, so no `BoAlloc` was refused.

**Where the chain stops.** The abort site is not symbolicated, and the reason is
a missing file rather than a missing address. Applying the row's slide
(`+1e368000`, `BootFaultProbe.c:55-60`) moves the PC to `0x1cb78af74`, which
**is** inside the readable `dyld_shared_cache_arm64.23` mapping
(`0x1c79c4000..0x1ce040000`, at `+0x3dc6f74`), so the address resolves to that
subcache. What cannot be recovered offline is the name of the image at that
offset and the symbol inside it: the extracted SystemOS volume carries
`dyld_shared_cache_arm64.34.dyldlinkedit` and `…52.dyldlinkedit` as **16-byte
stubs**, and the image table in the base cache header is empty
(`imagesOffset 0`, `imagesCount 0`), so neither the dylib list nor the symbol
table is present.

The frames at `0x21caf6b1c` and `0x1003924d0` resolve to neither the raw nor the
slid address, so they are the appex's own text, which needs that binary and its
load base. `EXITREASON`/`EXITTHROW` rows exist for other processes in other
captures, so the reader works, but no capture holds one for AegirPoster.

A fully symbolised crash needs one of: the linkedit subcache extracted intact
from the IPSW, or a kernel record of each process's load base.

## 6. The wallpaper overlay is mounted and its PNG is visible

Reading `patches/xnu/zzy-native-gpu-addon-mount.patch:22-66`, the stage sequence
is: source exists (1), `mount("bindfs", MNT_RDONLY|MNT_UNION)` succeeds (2),
the sealed lower entry is still visible (4), and the mount's own entry is
visible through the union (5). For the wallpaper mount the "own" entry is
`...ca/J120.png` (`:319-320`).

So `WALLPAPER stage=5 status=0` means the overlay is mounted **and** the
converted PNG is visible. The HEIC problem the overlay exists to solve is solved
on the device side.

Independently, `BootLog.c:365` writes the whole mount set to the persistent log:

```
A files=11 wr=3 same=8 fail=0 err=0 mount=5/0 mount2=5/0 mount3=5/0
```

plugin bundle, launchd daemons and wallpaper overlay all reach stage 5 with
status 0 on u266. Boots usb177 to usb191 read `mount=1/2 mount2=1/2` with
`files=5` — no plugin bundle in the addon at all.

## 7. The os_log export never runs

`NativeOsLogExport.mm` is compiled into `adrenoprobed`
(`scripts/build-adreno-metal-plugin.py:109`) and `NativeAdrenoProbeDaemon.cpp:27`
calls it. Its job is packed and mounted (`mount2=5/0`). Measured: `adrenoprobed`
appears **zero** times in every persistent log under `build/sheng-logs/` and in
`tmp/console-u266.log`, and no `oslog*.gz` exists anywhere in the workspace.

The export's mechanism is therefore untested rather than broken, and any
diagnosis that needs a process's own `os_log` lines is blocked on the daemon
starting, not on the format.

## 8. The client allow-list is dead code

`drivers/AdrenoMetal/NativeAdrenoMetalPlugin.mm` has no `listedClient` and no
reference to `NativeAdrenoMetal.clients`; `:279` reads
`if (strcmp(getprogname(), "backboardd") != 0) return device = clientDevice(port);`,
so every non-backboardd process gets a device unconditionally.
`scripts/make-gpu-addon-alioth.sh` still packs that file, so the earlier
old-list versus new-list rounds compared two identical behaviours.

## 9. The window table, checked offline

`scripts/check-gpu-client-windows.py` replays all of the above without a device:

- geometry: page alignment, disjointness, containment in
  `0x20000000..0x80000000` (`GpuProbe.cpp:1143` kDeviceInfo `va_start`,
  `va_size`), and total against `va_size`
- every measured offset against each slot's size
- `peak - slot_start == GPUSLOT hi` for every boot carrying both

Results:

| table | geometry | total | slot 0 vs u260's 967 MiB |
| --- | --- | --- | --- |
| current | OK | **1535 MiB** (20480 B of the aperture unused) | 1024 MiB, fits |
| 768+320+224+224 | OK | 1536 MiB | **768 MiB, does not fit** |
| 969+189+189+189 | OK | 1536 MiB | 969 MiB, fits |

The 768 MiB proposal is arithmetically correct in its total and geometry but
unsafe for slot 0. The 969/189/189/189 variant satisfies both constraints
(slot 0 above the measured 967; slots 1–3 above the 128 MiB that
`NativeAdrenoPublish.cpp:114` records for iconservicesagent in int-198) and has
**not been applied or run on hardware**.

## Revision

Device repository `2af006db44f11e10ebdff213e7017a3ea02c3887`. The driver
readings cite `reference/repo/Adreno650` at `bef707a`; its window table at
`NativeAdrenoPublish.cpp:118` is unchanged by this document.
