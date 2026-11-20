# 2026-11-20 rename1 desktop

## Image and command

- Tag: `rename1`
- Boot image: `tmp/boot-rename1.img`, sha256 `9cb9f917d89ecc0a2912775a4c195af79e4b562de95313352b1c7b9abc1ff8ae`
- Kernel: `aee321769f35f97fcf413fff1b7d3e298ebb61944152a02af0cd3440e2895204`
- Addon: `gpuaddon-comb1.gpuaddon`, sha256 `7e10819ce70d6294…`
- Kernel args: `alioth-init=1`, `lsfix=120`, `alioth-diag-export=130`, `alioth-autoreset=180`; the board baseline is 51 args / 888 bytes and the full line is below the 1000-byte budget.
- Round: `sh tmp/rounds-count2.sh tmp/boot-rename1.img rename1 1 200`; console `tmp/console-rename12238341.txt`; persistent export `build/sheng-logs/rename1-00000003c23169e3`.

## Result

This is a desktop result.

- `SUPER t=170s runs=170 calls=171 mon=171 sw=8/8/0 own=1 prep=8 data=2 tf=24299 lp=1 tc=4 tfrc=0 tfs=0 tlow=0 vr=0 wdt=1/170`
- `SURFACEL n=12 nonblack=918/1000 luma=113 pa=160c44000 seg=1000 bytes=10444800 stride=4352`
- `PANEL nonblack=916/1000 luma=113 rgb=148/100/94 gpu off=8 comp=8 fail=0 aban=0`
- `WALLPAPER stage=5 status=0` (20 rows); `FRAME 8`; `FRAME 7 gap_us=46867679`.
- `GPUCLI clients=8 mountfail=1/e00002d5/2 d=000/48/36/2 ps=0/0,0/0 rc=0/0/0/0 askers=11[mediaserverd,backboardd,SpringBoard,WidgetRenderer_Defa,CollectionsPoster,PhotosPosterProvide,PosterBoard,AegirPoster,KaleidoscopePoster,GradientPosterExten] disp=12/237/0`; the `device-ok` rows include SpringBoard, WidgetRenderer_Defa, CollectionsPoster, PosterBoard, AegirPoster and KaleidoscopePoster.
- `GPUFRAME ready 1 fail 0 copied 0 gpu offered 6 done 6 fail 0 abandon 0 | legacy surfaces 2 swaps 6`
- `GPUROW24 METAL ok`; `GPUROW25 LIVE`; `GPUHANG h=0 r=0 f=0 fence=0 st=0`; `GPUREASONS accepted 6 rejected 0`.
- `CORES t=180s runs=18 ... run=7 deaf=00 st=00 fl=00 wd=0/00`; `TOUCHSTAT t=180s frames=25581 valid=25581 contacts=0`.
- `AUTORESET deadline 180s reached at 180s`.
- `LSPKD` 13 rows; `PKDSHIM` 0; `WIDGET` 0; `LSINITLOG WIDGET s=trampoline-main r=0 i=0 ok=0 cp=0 mo=1 a=72 p=70 f=-2 l=6148K mig=233/233 tr=1 pk=0 rb=-1 rg=-1 done=-1 d=-1 c=-1`.

## c1 comparison

The 2026-11-17 c1 round used `sheng-init=2` (now `alioth-init=2`). It returned no-desktop: `GPUCLI clients=1 mountfail=1/e00002d5/2 askers=2[mediaserverd,backboardd]`, `own=0 sw=0 frame=0`, no `WALLPAPER`, `FRAME` or `SURFACEL` row.

This round uses `alioth-init=1`. It returns the desktop and the wallpaper, and the GPU client list grows from 2 to 11 askers and 1 to 8 clients. The `LSPKD` rows drop from 39 to 13 and `PKDSHIM` stays absent. The pkd shim in `alioth-init=2` is therefore the c1 no-desktop cause; the init trampoline in `alioth-init=1` is the working boot.

`mountfail=1/e00002d5/2` is unchanged and is the kernel-client two-open self-test; `e00002d5` is `kIOReturnBusy`. It is not a refused user client.

The widget is not fixed by this round. The `WIDGET` row is absent and the trampoline `LSINITLOG WIDGET` still reads `done=-1` with `pk=0` and `ok=0`. Widget repair remains open.

## Persistent export and next step

`sh tmp/read-log.sh --diag` returned `rc=0` and wrote `build/sheng-logs/rename1-00000003c23169e3`. The read-log sets the active slot to `b` and prints: power the board off and on before the next `fastboot boot`; run `sh tmp/power-cycle-done.sh` after the power cycle.

Next: power cycle, then verify the widget path (`WIDGET`, `LSINITLOG`, `LSPKD`, `alioth-touch-synth`/`alioth-init=1`) and re-read `SURFACEL`/`PANEL`/`WALLPAPER`. Do not use `alioth-init=2` as the default boot.
