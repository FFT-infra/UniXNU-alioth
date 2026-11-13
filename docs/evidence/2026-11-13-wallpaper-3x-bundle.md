# 2026-11-13 @3x wallpaper bundle overlay

This record covers the wp3xbundle board run. It shows the J120 wallpaper on the panel.

## Revisions and inputs

| Item | Value |
| --- | --- |
| Device repository | `0373d0000dea8f496e4cb674cfc653345aba0992` |
| Base repository | `e0ee2ad9a2654309480c8b0cc725d0dc09186cd4` (rebased from `88fa155` after the run) |
| Kernel | `683a17a8cd3d9a705f1e23f11ac31a98ea6479db59861c028d8e2fde723a3474`, 126,443,520 bytes |
| Boot image `wp3xbundle` | `bfabc684f36d5d7016dafa3640038b8d758408378d0e4d22797341bf78e5889f`, 191,277,056 bytes |
| GPU addon | `fe1f4824d61f4c2e2d6f48952c8bd36772aac642a17d504f93e561755fdc74f8`, 24,236,624 bytes |
| Capture | `tmp/console-wp3xbundleb1232521.txt` |

The image carries `sheng-build-tag=wp3xbundle` and `sheng-autoreset=240`. It uses the default board arguments.

## Why the sealed bundle does not work

The device reports logical screen class `1024w-1366h@3x~ipad`. Every sealed wallpaper bundle
declares `1024w-1366h@2x~ipad`. WallpaperKit refuses the sealed bundles and synthesizes a black
wallpaper.

Two intermediate readings bounded the fix:

- The Flat bundle with `logicalScreenClass = "any"` was accepted, but it used the generic square
  view. `wpclass1b` read `SURFACEL nonblack=503/1000`; the lower `SURFACEG` rows 07 to 10 were
  blank. The blue top half was not J120.
- The Flat bundle with `logicalScreenClass = "1024w-1366h@3x~ipad"` but the sealed `@2x`
  directory and `@2x` animation did not load. `wpclass3xb` read `SURFACEL nonblack=18/1000`.

The fix is an owner bundle with the class and the animation name in `@3x`:

- `configs/wallpaper-bundle/7570.iPadOS_17_Flat-1024w-1366h@3x~ipad.wallpaper/Wallpaper.plist`
  declares `1024w-1366h@3x~ipad`, keeps `productTypes = [iPad7,1, iPad7,2]`, and points
  `backgroundAnimationFileName` at the `@3x` animation.
- The same bundle carries an empty `7570.iPadOS_17_Flat_Background-1024w-1366h@3x~ipad.ca`
  directory. The kernel unions this bundle over the sealed collection, then mounts the PNG
  animation over the new `.ca`.

`patches/xnu/zzz-native-wallpaper-bundle.patch` adds both mounts to
`native_gpu_addon_prepare`. `scripts/make-gpu-addon-alioth.sh` packs the tree with
`configs/wallpaper-bundle`.

## Board readings

All three addon mounts reached stage 5:

```
[addon] 1=5/0 2=5/0 3=5/0
WALLPAPER stage=5 status=0
```

`CollectionsPoster` held a GPU client and read the device:

```
GPUCLI clients=4 ... askers=5[mediaserverd,backboardd,SpringBoard,WidgetRenderer_Defa,CollectionsPoster]
txt=... SpringBoard:device-ok L+2X0X1O | WidgetRenderer_Defa:device-ok L+2X0X1O | CollectionsPoster:device-ok L+2X0X1O |
```

The surface is full and is J120:

```
SUPER t=180s runs=180 calls=181 mon=181 sw=68/68/0 own=1 prep=68 data=2 ...
SURFACEL n=8..12 nonblack=984/1000 luma=181 mx=253/3/117 sp=250
PANEL nonblack=1000/1000 luma=139 rgb=168/126/129
```

The last row reads `SUPER t=190s ... sw=69/69/0 own=1`, `SURFACEL n=13 nonblack=998/1000
luma=126 mx=253/3/116`, and the round judge reads
`RESULT t=190 own=1 sw=69 data=2 lit=998 pa=116d10000 ds=0 panel=1000 frame=69 askers=5 -> desktop`.
`mx=253/3/117` is the most saturated J120 pixel. The twelve-row dump has content in every row:

```
SURFACEG 00 |          ........::::::|
SURFACEG 01 | @@@@@@@@@@@@@@@@@@@@@@@|
SURFACEG 02 |@@@@@@@@@@@@@@@@@@@@@@@@|
SURFACEG 03 |@@@@@@@@@@@@@@@@@@@@@@@@|
SURFACEG 04 |@@@@@@@@@@@@=====@@@@@@@|
SURFACEG 05 |@@@@@@@@@@@@@@@***#%%%%%|
SURFACEG 06 |@@@@@@@@@@@@@@@%%%%%%%--|
SURFACEG 07 |*******####%%%%%%%%---==|
SURFACEG 08 |+++++++===============++|
SURFACEG 09 |=============+++++++++++|
SURFACEG 10 |=========+++++++++++++++|
SURFACEG 11 |  +*******************  |
```

The first boot of the same image (`wp3xbundle1228231`) lost the GPU path: `own=0 sw=0`,
`GPUCLI clients=0`, `PANEL 914`. The second boot is the reading above. This is the known
one-bad-boot form of this board; it is not a wallpaper result.

## Rebased base

After the Rebase-First sync, the same addon was built on base `e0ee2ad`. The image is
`tmp/boot-wp3xbase.img`, sha256
`e2962e142b60b8d0191e434887297129bc615eb69bf7d4b5da0f21e0ebb02589`; its kernel is
`ba8496cc89dabbc315b16bdaab579a54e7c81f53b4e8b01e748e0fe04e8fec7c`.

The first boot reached the wallpaper:

```
[addon] 1=5/0 2=5/0 3=5/0
WALLPAPER stage=5 status=0
SUPER t=190s runs=190 calls=191 mon=191 sw=8/8/0 own=1 prep=8 data=2 ...
SURFACEL n=12..13 nonblack=918/1000 luma=111/110 mx=250/3/113 sp=247
PANEL nonblack=916/1000 luma=112 rgb=147/99/93
GPUCLI clients=5 askers=7[mediaserverd,backboardd,SpringBoard,WidgetRenderer_Defa,
  CollectionsPoster,PhotosPosterProvide,PosterBoard]
txt=... SpringBoard:device-ok | WidgetRenderer_Defa:device-ok |
  PhotosPosterProvide:device-ok | PosterBoard:device-ok
RESULT t=190 own=1 sw=8 data=2 lit=918 pa=1837c3000 ds=0 panel=916 frame=8 askers=7 -> desktop
```

`mx=250/3/113` is again the J120 pink maximum, and the twelve-row dump is full:

```
SURFACEG 00 |         ..........     |
SURFACEG 01 |    *#@:=  ...%@%%:::::-|
SURFACEG 02 |             .....::::--|
SURFACEG 03 |.................:--====|
SURFACEG 04 |:***********************|
SURFACEG 05 |*########*********######|
SURFACEG 06 |#######*******+#%%%%%%--|
SURFACEG 07 |+*******######%%%%%--===|
SURFACEG 08 |++++++++=============+++|
SURFACEG 09 |=============+++++++++++|
SURFACEG 10 |=========+++++++++++++++|
SURFACEG 11 |     =======+++++++     |
```

This image is built from the rebased base; the wallpaper fix did not regress.
