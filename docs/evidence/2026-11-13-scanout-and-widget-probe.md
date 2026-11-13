# 2026-11-13 direct scanout and widget probe

This record covers the ds1 and wpwhite board runs. It does not claim that direct scanout or the
widget pixels work.

## Direct scanout does not arm

`tmp/boot-ds1.img` (sha256 `70f44ef0b85b3d1652e928776d90abe5decd51c80ed8d4077940881c1c583e94`)
carries `dpu-scanout=2`, `sheng-touch-synth=4000` and `sheng-touch-synth-swipe=1`. The first boot
reached the desktop (`own=1 sw=74/74/0`) but the display row never left the CPU path:

```
VSYNC arm=0 tick=0 idle=0 late=0 start=1 pub=1 hz=120 dpu=00000000 ds=0
SURFACEL n=13 nonblack=984/1000 luma=180 mx=253/3/117 white=412
PANEL nonblack=987/1000 luma=181
```

`NativeLegacyDisplay.cpp:306` is the gate:

```c
if(mode<2 || !&native_ufs_dpu_map || !&native_ufs_dpu_state || !(native_ufs_dpu_state>>31) || !primary)return;
```

The argument is present, so the return is the missing display bank: `native_ufs_dpu_state` reads
`00000000`. The direct path never installs its VIG window; `ds` stays 0. The next check belongs in
the ShengUfs display-bank setup, not in GopFramebuffer.

## Widget probe

The surface probe now prints `white`, the fraction of samples in a narrow `(242,242,247)` range.
The first version used a broad bright test and counted J120 highlights: `white=412` on the lock
screen (`wpwhite`, `SURFACEL nonblack=984/1000`). That is not a widget result.

The narrow probe (`tmp/boot-wpwhite2.img`, sha256
`fecd14060daa8438672d63ccc490d57ac7b3092cdece9257d28187feb490f1e1`) read:

```
SURFACEL n=11 nonblack=940/1000 luma=237 mx=5/123/251 white=0
PANEL nonblack=988/1000 luma=250 rgb=250/250/250
GPUCLI clients=5 askers=6[...,WidgetRenderer_Defa,...]
txt=... WidgetRenderer_Defa:problem surface import ref~command_buffer/mod.rs:599
```

The repeated synthetic swipe left a white surface and opened `MobileSlideShow`; the capture does
not prove it is the home screen. The useful reading is the renderer failure, not a widget pixel
count: `WidgetRenderer_Defa` reports a surface import problem from the Turnip command buffer.
`white=0` also says the placeholder is not exactly `(242,242,247)` in this frame, so the counter
needs calibration before it can judge the three widget slots.

## Next checks

1. Find why `native_ufs_dpu_state` bit 31 stays zero after `dpu-scanout=2`; the direct path cannot
   start without the display bank.
2. Read `command_buffer/mod.rs:599` and the bridge surface import path for
   `WidgetRenderer_Defa:problem surface import ref`; the widget renderer fails before pixels reach
   the surface.
3. Calibrate the `white` probe against a known home-screen frame; do not use the broad count.
