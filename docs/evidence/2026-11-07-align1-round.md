# align1: the aligned baseline reaches the desktop

2026-11-07. Image `tmp/boot-align1.img`, sha256
`d6f2d41a496fbcd6255132cbe84d717ee8916a1cfe0683763b486b90ebd6180e`, tag `align1`.
Base `UniXNU/UniXNU@b6de3eb` plus the board mounts; device tree `bdff7cd`.

## Command

```sh
fastboot boot tmp/boot-align1.img
sh tmp/read-log.sh --diag
```

Serial capture was empty: the round script selected the orb debug node
(`Piano_Linux`) instead of the phone. The evidence below comes from the
persistent log `build/sheng-logs/align1-0000002291370ffa` (uptime 598 s,
258 rounds, 0 write errors) and the 240 s diagnostic export.

## Result

- The desktop appeared and the owner confirmed it. The exported
  `screen.bgrx` shows the home screen with the status bar, two icon groups
  and the dock.
- GPU section 3: `GPUROW19 CP ok step 7 ... marker a6500c0d`;
  `GPUROW25 ... sub 729/729 ... bo 316/316 r0 s0 busy 0 ... roqavail 0/0/0`.
  CP initialisation, ring advance, bounded submission and fence completion
  hold. `GPUROW23` reads the publish self-test values (`badc0de feed0001
  600d0003 deadbeef`), which is a register readback, not a rendered known
  image. The known-image render is still outstanding.
- GPU section 4: `GPUROW24 METAL ok`; `[metal askers]` names ten processes;
  `[gpu clients]` shows `s0` to `s4` with `vm 9/9 last 0 at 0 nc 21`;
  `[gpu objects]` shows `s0 875/973 w485M subs=682` and `s4 43/43 w21M
  subs=19`; `prime.log` records CoreImage pipeline compilation.
- Wallpaper is absent. `UnityPosterExtension` reports
  `no-device device: Vulkan(VkResultCode(-3))` and then crashes with
  SIGSEGV in `DecodeRow` -> `Write_BGRA8_vec` at `0x150508d80`, an address
  outside any region. `PosterBoard`, `CollectionsPoster` and
  `PhotosPosterProvide` reach slot `s4` and submit, but no wallpaper is
  visible.
- The device-side USB worker reports `QCOM USB ACTIVE (CDC-ACM up)`; the
  host did not enumerate the CDC node, so no serial capture exists for this
  round.

## Next measurement

Separate the UnityPoster device-timing failure from the decode-buffer
crash, and run the known-image probe in the same round.
