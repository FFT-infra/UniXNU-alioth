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

## Direct scanout arms and flips

`dsarg` (`tmp/boot-dsarg.img`, sha256
`ea7e0bf0f63e21fe7d1aa20ea2fa1a69ad8919989dcf9c50142fdf8d800eecec`) added the missing
display switch to the UFS window builder. The window then installed:

```
VSYNC ... dpu=80000004 ds=0
```

`native_ufs_dpu_state` bit 31 is set and bank 4 is installed. The display still did not flip:
its next swap mapped one surface and then stopped, because `flip()` wrote CTL_FLUSH at
`0x15018`. That is sheng's SM8550 offset; on SM8250 the two CTLs are at `0x1000` and `0x1200`
from `0xae01000` (`kona-sde.dtsi`, `DpuProbe.cpp`), so the store went to a hole and hung the MDP
bus. `dsflip` (`tmp/boot-dsflip.img`, sha256
`298deba75b9cc81c9859d0c8ae7475afc856b37a27c01844274d11b6cfbcb067`) read
`flips=0 mapped=1 wait=1` with no progress.

`dsctl` fixes the CTL offsets to `0x1018`/`0x1218` and takes the boot scanout address and stride
from `PE_Video`. Source revisions: `NativeUfs-alioth` `9a1f75bec3d8937736eecd96cbef19cc81265f24`
(accepts `dpu-scanout`) and `GopFramebuffer` `6dc063c1da59376a7acb1d06a824fcac1a79c86c`.
Image `tmp/boot-dsctl.img`, sha256
`1496c71bd95453b41b95379ebf7ac0dd7229a3129cd66eaee2f23fc162fbe86c`; kernel
`433e899a7ed47d08b1e53de7e9775d0c44c3cc4fbfced0012fffdaf75232276f`. The same boot reads:

```
DIRECT step=5 flips=10 mapped=3 refused=0 noslot=0 boot=0 wait=3 gaveup=0 latch=10/50565us | q=10/149/4/0/6/10/10
SUPER t=210s ... sw=10/10/0 own=1 ...
SURFACEL n=13/14 nonblack=918/1000 luma=110 mx=250/3/113 stride=4352
PANEL nonblack=909/1000 ... (the firmware buffer; the DPU now scans the surface, not this buffer)
```

`flips=10` and `latch=10` mean the display fetched ten compositor surfaces and every flush
latched. `PANEL` remains at the firmware image because it samples `0x9c000000`, which the direct
path no longer scans; `SURFACEL` is the surface the DPU fetches. The widget probe follows.

## Widget probe

The surface probe now prints `white`, the fraction of samples in a narrow `(242,242,247)` range.
The first version used a broad bright test and counted J120 highlights: `white=412` on the lock
screen (`wpwhite`, `SURFACEL nonblack=984/1000`). That is not a widget result.

The narrow probe (`tmp/boot-wpwhite2.img`, sha256
`fecd14060daa8438672d63ccc490d57ac7b3092cdece9257d28187feb490f1e1`) did not reach the widget
renderer. Its capture reads `GPUCLI clients=0` for the first minute and then `clients=1` with only
`mediaserverd` and `backboardd`; `WidgetRenderer_Defa` is absent, and no surface-import line is
present. The `problem surface import ref~command_buffer/mod.rs:599` text quoted in earlier notes
belongs to `fix109`/`fix136` (SpringBoard, `no-copy buffer: DeviceLost`), not to this round. The
widget blocker remains the empty `ATXWidgetDescriptorCache`, not a renderer that ran and failed.

## Data migration attempt

The seed's `com.apple.migration.plist` records `LastSystemVersion = 21H461` and
`SyntheticDidUpgrade = false`, so DataMigration has no reason to run the DataClassMigrators.
A modified seed changes them to `21A000` and `true`, and puts
`_LSPrivateDatabaseNeedsRebuild = true` in the mobile and root `com.apple.datamigrator`
defaults. `tmp/boot-lsmig.img` (sha256
`3c5b886319821ade17d38975031bfcef190c5d72995bc45fcfdd6c4a8420a652`) carries that seed and
`sheng-data-reset=1`.

The first boot reaches the desktop at t=120 s (`sw=13`, `SURFACEL 901/1000`,
`WidgetRenderer_Defa:device-ok`, `SpringBoard:device-ok`) and `com.apple.datamigrator`
exits 0 twice. It is not a widget result: the serial capture has no screenshot, and the
surface is the wallpaper.

A screenshot image with `offline-screenshot=1` (`tmp/boot-shot2.img`, sha256
`4240e43ed4d9392dc92a0277f9ceddc0655e1a2462125c3a9bc8de92f250486f`) boots, but SpringBoard
loses its Metal device (`SpringBoard:no-device L+2X0X1E ... S~acheError`) and the surface stays
black (`SURFACEL 21/1000`). The screenshot provider and the migrated cache tree cannot be
used together in this round. The widget line still needs a screenshot path that does not
change the display process, or a read of `ATXWidgetDescriptorCache` from the exported log.

## Next checks

1. Keep `DIRECT flips` growing when the compositor is active; the CTL offset is fixed, so the next
   question is frame pacing, not whether the path arms.
2. Read `ATXWidgetDescriptorCache` from an exported unified log after a DataMigration run; the
   widget renderer got a Metal device in healthy boots, so the missing piece is the descriptor
   database (LaunchServices/PlugInKit registration), not `command_buffer/mod.rs`.
3. Calibrate the `white` probe against a known home-screen frame; do not use the broad count.
