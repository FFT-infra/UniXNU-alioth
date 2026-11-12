# 2026-11-13 shader prime and metal telemetry

This record covers the shdr3i board run. It does not claim that the wallpaper draws.

## Revisions and inputs

| Item | Value |
| --- | --- |
| Device repository | `bd1904230b4370a8ed7e14c926da6514a7eee17b` |
| Base repository | `fe14b996a4fd6e44a1b19f452ea8d2e48fcc6099` |
| AdrenoMetal branch `fft` | `3b84be5e81251ecba8c32b8928899f3c56a83dd0` |
| Kernel | `9e91fa502f317621ddf9f349e6a791ae7ce665162e6c29d5b9e0cd6748549014`, 126,443,520 bytes |
| Boot image `shdr3i` | `b51583cbb1684fd46e008fb10a245f2f5887524624e68db3b83e48d0d19e56e1`, 191,277,056 bytes |
| GPU addon | `becec049ee0cfcf7899f7de32df4a81565f4c021ff2ab7e138b3ba7742c61c96`, 24,235,240 bytes |

Boot arguments on the image: `gpu-addon=1 diag=1 log-gpu=1
sheng-autoreset=240 vsync=120 fb-bgra=1 fb-gpu=1 pmgr=2`.

## Two defects fixed before the run

**The prime list named the build host.** `make-gpu-addon.py --prime-list`
packed `build/prime-list.txt`, a macOS list of 159 paths under `/Applications`
and `/System/Library`. Most of those paths also exist on iPadOS, so the pass
reported progress while the libraries it never reached stayed uncompiled.
`configs/prime-list-ios.txt` now holds the 168 `.metallib` paths of this
board's 21H461 OS and SystemOS DMGs, with the cryptex paths under
`/System/Cryptexes/OS`. The board keeps 159 of the 168 paths.

**BootLog printed six table rows.** The AdrenoMetal table has 48 rows, and the
first six are memory counters. The compile, prime, cache and pass rows follow
them, so the console never showed a shader reading. `package/BootLog.c` now
prints every non-empty row each tenth second.

## shdr3i readings

The run started at 05:02:14 and the capture is
`tmp/console-shdr3i0502091.txt`. The render server is `backboardd`; the Metal
telemetry table is its table.

At t=230 s:

```
MT18 T pipeline n=55 total=175ms max=39ms
MT19 T library n=4 total=81ms max=31ms
MT20 T compile n=0 total=0ms max=0ms cached=0
MT21 T function n=727 total=53ms max=0ms
MT22 VP made 137 create 168/33 step 0 | drv gfx 545ms cmp 0ms | fn 411 mod 501
MT23 V cache hit650 miss910 wr765 | msg i0 w64
MT24 SC fn 367/422 cp 283/354 rp 0/134 rej 0 rd-fail 0
MT25 SCb wr 765 q 938 thr 0 23583K wfail 0 evict 0 qhit 0 | drv load 118 save 56 hit 156 miss 7
MT26 PR compiling=0 libs=39/159 made=385 rate=0 resumed=6
MT27 PRb scope=2 primed=4 left=0 | system 0 pub 0/0K
MT38 S c32/0 d32 p465 x112 R116 D1128 F42 M2531k C10 B2
MT39 SA big 42 whole 41 empty 1 | target 108M scissors 106M | tile pipelines 0 dispatches 0
```

- The prime pass reads the iPadOS list: `libs=39/159`. It finished 35
  libraries on an earlier boot and 4 on this one, so progress persists in the
  shared cache.
- The pass compiled 385 pipelines and wrote 23.5 MiB of cache. The render
  server compiled 134 render pipelines and 354 compute pipelines in the same
  run.
- `PRb system 0` is expected on iPadOS: the macOS system-cache writer is not
  selected. The clients use the shared cache directory, shown by their
  `L+2X0X1O` trace: candidate 2 opened.
- `S` reports 32 commits, 32 draws, 116 render encoders and 42 frames, so the
  render server is submitting work, not idle.

The client row reads `clients=8 askers=9`. `GradientPosterExten`,
`PosterBoard` and `PhotosPosterProvide` got a device through the shared cache.
`CollectionsPoster` and `EmojiPosterExtensio` read `MetalPluginName` but left
no client row.

## Open readings

The render server refuses two QuartzCore pipelines with a binary archive miss:

```
P0 render pipeline: BinaryArchiveMiss(BinaryArchiveMiss { function: "fixed_frag_lph_cph", pipeline: Render }) | frag TimgA2S1Xhf_IalpCcl
P1 native-metal: render pipeline refused: BinaryArchiveMiss(BinaryArchiveMiss { function: "fixed_frag_lph_cph", pipeline: Render }) | frag TimgA2S1Xhf_IalpCcl
```

The function is in `QuartzCore.framework/default.metallib`, which is in the
prime list. The pass had reached 39 of 159 libraries, so it had not yet reached
the `/System/Library/Frameworks/QuartzCore.framework` entry at that point. An
earlier sample in the same run also refused `p3_clamp_lph` and
`luma_boost_lph`, from the same library.

The panel stayed dark: `PANEL nonblack=31/1000 luma=6`. The desktop verdict in
the round script comes from `FRAME` rows, not from the panel.

## Next check

Run the pass long enough to reach QuartzCore and the rest of the frameworks,
then boot normally and read the same rows. `tmp/boot-prime1.img`
(sha256 `1e92b02a54a123b9755efbdeaba7019fb7781434fcb6cd6045d4664fbadfa9a8`)
raises `sheng-autoreset` to 1800 s for that run only; the board default stays
240 s.

The pass processes the list in order, and the MetalPerformanceShaders
libraries before QuartzCore are large: MPSCore alone is 31.9 MB and held the
pass at library 51 for several minutes. `configs/prime-list-ios.txt` now puts
the two QuartzCore entries and the poster extensions first, so a run reaches
the library that holds the refused function before the MPS batch.
`tmp/boot-prime2.img`
(sha256 `c2671b6b239c900fc3b5025637d53bef8425d6e3159b023364aeed349a4e0888`)
carries that order and the same 1800 s deadline.

## prime2: the pass finishes, the panel does not change

```
MT26 PR compiling=0 libs=159/159 made=638 resumed=158
MT24 SC fn 6723/641 cp 4974/637 rp 0/1401 rej 0 rd-fail 0
MT25 SCb wr 1274 ... 24609K wfail 0 evict 0 qhit 0
PANEL nonblack=28/1000 luma=6 sum=4d40fa56
```

All 159 on-board libraries finished. The panel sum is byte-identical to
prime1's last row, and the two `fixed_frag_lph_cph` refusals are still the
last problems. The shader cache was not the missing piece.

## af1: the archive fallback makes the pipeline, the wall stays black

`metal-archive-fallback=1` makes the render server retry a
`BinaryArchiveMiss` without the archive and without the fail bit. The af1
round logs:

```
MT30 P0 native-metal: archive fallback made frag TimgA2S1Xhf_IalpCcl -> ok
MT31 P1 native-metal: archive fallback made frag TmuaA2Xhf_Isrc_Isqr -> ok
PANEL nonblack=33/1000 luma=7 sum=aaf0080e
```

So the refused pipeline is now compiled, and the wallpaper still does not
draw. The layer is absent, not the shader.

## The remaining lead

The u276 sandbox log names the provider directly:

```
Sandbox: CollectionsPoster(216) deny(1) iokit-open-user-client AGXDeviceUserClient
```

No current round prints `GPUOPEN` for `CollectionsPoster`, while
`GradientPosterExten`, `PosterBoard`, `PhotosPosterProvide`, `SpringBoard`,
`WidgetRenderer_Defa`, `mediaserverd` and `backboardd` all open it. The
WallpaperKit provider reads `MetalPluginName` and never gets a GPU client, so
the default wallpaper's layer has no renderer. Changing that is a sandbox or
entitlement decision and needs a deliberate choice; it is not a shader or
cache change.
