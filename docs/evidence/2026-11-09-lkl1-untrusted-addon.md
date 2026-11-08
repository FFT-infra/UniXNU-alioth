# lkl1: a desktop that renders nothing — the addon's Metal plugin is not in the trust bundle

## Round

- Image: `tmp/boot-lkl1.img`, sha256
  `36e15cc361de4bd77762348fd917d389b358a03c2e44f2bd46a8a82d82981751`.
- Build: `TAG=lkl1 scripts/build-round.sh` (default `ADDON=gpuaddon-align.gpuaddon`), the
  `lkl1` kernel adds the `GICRL`/`CNTK` rows and the per-core `CNTKCTL_EL1` reading.
- Console: `tmp/console-lkl10026381.txt`.
- One round: `sh tmp/rounds-count2.sh tmp/boot-lkl1.img lkl1 1 300`;
  `lkl10026381: RESULT t=290 own=1 sw=14 data=2 lit=25 pa=10ad15000 ds=0 panel=25 frame=14 askers=12 -> desktop`.

## What the desktop looks like

```
SUPER t=180s runs=180 calls=181 mon=181 sw=12/12/0 own=1 prep=24 data=2
GPUCLI clients=0 mountfail=1/e00002d5/2 … askers=12[backboardd,mediaserverd,SpringBoard,
  WidgetRenderer_Defa,CollectionsPoster,PhotosPosterProvide,PosterBoard,UnityPosterExtensio,
  AegirPoster,KaleidoscopePoster] disp=12/255/0 marks=0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 txt=-:-
PANEL nonblack=25/1000 luma=5 rgb=5/5/5
WALLPAPER stage=5 status=0
```

The swap path runs (`own=1`, `sw=14`) but the panel is black. `marks` is the plugin's own
progress ring: zero with askers > 0 means, in `BootLog.c`'s words, the system found the plugin
and never got an entry point out of it. No client text was recorded at all (`txt=-:-`).

Compare the last rounds whose plugin did run:

```
wl    GPUCLI clients=7 … askers=8[…] disp=12/237/0 marks=304 6730 6731 6730 … txt=mediaserverd:problem1 …
w4164 GPUCLI clients=4 … askers=5[…] disp=8/218/0  marks=120 6730 6731 6730 … txt=…CollectionsPoster:device-ok L+2X0X1O |
```

`0x6730/0x6731` are the plugin's "Turnip found / device made" markers; wl's panel was
`nonblack=513/1000 luma=119`. So the difference between a lit desktop and this black one is
whether the userland Metal plugin loads, not the display or compositor path.

## Why it does not load

The plugin binary inside the addon is not the one the owner trust bundle covers:

```
tmp/gpuaddon-align.gpuaddon  NativeAdrenoMetal  CDHash 8db4e5fb73fc86400d0fd814250a7908350ed009  (2026-11-07 21:27)
tmp/gpuaddon-wl2.gpuaddon    NativeAdrenoMetal  CDHash cddc57051b5bc3db1a19daebd2654a235a22cc78  (2026-11-08 16:21)
src/UniXNU/build/adreno-metal-plugin/owner-trust.bundle (2026-11-08 15:52):
  20-byte cddc5705… present; 8db4e5fb… absent.
```

`build-round.sh` defaults to the align addon, so every image built with the default after
15:52 (norm1, norm2, pc4, lkl1) ships a plugin the kernel will not execute. The align addon
also carries no wallpaper files at all; only the wl2 addon has `wallpaper/main.caml` (with the
leading-slash form, 25 references).

## Boundary

This round does not measure GPU performance, wallpaper drawing or widgets: no userland Metal
code ran. The next image (`wall1`, `ca7e9eea…`) uses `ADDON=gpuaddon-wl2.gpuaddon`, whose CDHash
is in the bundle and which carries the wallpaper overlay.
