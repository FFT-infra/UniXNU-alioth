# 2026-11-17 c1 combined short round

## Image identity

- Exact build command: `sh tmp/build-c1-180-exact.sh` on orb. It splices `tmp/args-c1-180.txt` temporarily into `src/UniXNU-alioth/board.toml`, restores the original board file, and builds only the payload and boot image over the `comb2` kernel.
- Argument line: 52 arguments, 959 bytes, 40 bytes below the 999-byte effective joined-line limit. Device repository HEAD: `930207661ede3033603b064c2c062574c322946c`. The exact script SHA-256 is `995accbfcecd09227e3c57beef71a17b7d5c5bd7cc57ebf82579a428a8c48a93`; the exact argument file SHA-256 is `fd6c55243f061b0c1d10832a1fd0a357ee2b25b82ecf896424ac423f826ff7ca`.
- `tmp/build-c1-180-exact.local.log`: `BUILD_EXIT=0`, `PAYLOAD_ARGS_OK 959 40`, `CHECK_OK`, `LINK_OK`, `EXACT_BUILD_OK`, `HOST_EXIT=0`, board restored to `9525a29068da66a72f0d8dfcb1c7f45d4012400cb29930b3507b9cd725c0c628`.
- `payload.json`: kernel `3925717d3fdb6ae9eaad7c12d219753359cafd666462e58beb5d68d321bb000b`, payload kernel `4d26bac90245a64ab3989ed7dd30d19e279b19114e16473b231b4efb75cf0d61`, trust cache `cc4a45ee85d3757bab71d9715ae7aea6cf01e2f0b79cfb7f1d62e68fb6f9a9f9`, GPU addon `7e10819ce70d6294c841b8cd2ebe976a24121bf181ae451f2ffa46cc6f44283b`, `hardware_tested: false`. The payload record is a static build record, not a device-verification record.
- `tmp/boot-c1.img`: SHA-256 `2ed0bff0ee969fdf1f3d1dbcf2b3ee2dde1c560ffea4570771d0a54118f02e08`, 191,277,056 bytes. Image strings carry `sheng-build-tag=c1` and `sheng-autoreset=180`.
- Key arguments: `alioth-bl=5`, `sheng-init=2`, `pcie=19`, `alioth-wifi=2`, `lsfix=120`, `sheng-diag-export=130`, `sheng-autoreset=180`, `serial=3`, `debug=0x14e`, `sheng-touch-synth=400`, `sheng-touch-synth-swipe=1`, `vsync=120`, `vsync-fixed=1`, `gpu-addon=1`.

## Device command and result

```sh
sh tmp/rounds-count2.sh tmp/boot-c1.img c1comb 1 200 > tmp/rounds-c1comb-host.log 2>&1
```

- Host log: one boot at 01:14:23; capture window `SECS=200`; `AUTORESET deadline 180s reached at 180s`; final marker `c1comb0114181: RESULT t=180 own=0 sw=0 data=2 lit=? pa=? ds=0 panel=913 frame=0 askers=2 witness=swap -> no-desktop`; `ROUND_EXIT=0`.
- Console: `tmp/console-c1comb0114181.txt`. At t=180 s, `SUPER runs=180 calls=180 mon=180 sw=0/0/0 own=0 prep=0 data=2 tf=24738`, `CORES run=7 deaf=00`, `VSYNC start=1 pub=1 hz=120 ds=0`, and `PANEL nonblack=913/1000 luma=44`. At t=20 s one core was transiently marked `deaf=08`; by t=180 s the mask was `00`.
- This is a `no-desktop` device result. It is not a build failure. It does not establish a root cause for the missing desktop, wallpaper, or widgets.

## Sources read in one round

- Wi-Fi: `PCIE 19: step 19: done`, `WIFI windows open ... rails voted, SID claimed, arena up`, and `WIFI setup only (alioth-wifi=2); the runtime is not started`. This verifies the setup-only endpoint path. It does not start or verify the full QCA6390 MHI runtime.
- GPU: final `GPUCLI clients=1 mountfail=1/e00002d5/2 askers=2[mediaserverd,backboardd] disp=9/165/0 ... txt=mediaserverd:problem1 Values:pipelineLibrary:error: (answered nil)`. `GPUHANG h=0 r=0 f=0 fence=0 st=0`, and `GPUROW25 sub 977/977` show a live ring, not a desktop frame. No `WALLPAPER`, `FRAME`, or `SURFACEL` row appeared on the serial console.
- Widget and LaunchServices: eight `LSINITLOG` rows show the trampoline, `pkd copy 0`, the libexec view mount, both migration directories, both 233-byte migration flags, and one composite `WIDGET` row (`mig=233/233 ... ok=0 ... pk=0 ... done=-1`). `diag/files/private/var/ngpu/lsinit.log` contains the same eight rows. There is no `PKDSHIM` row and no widget pixel.
- Thirty-nine `LSPKD` rows were dumped from the launchd log by `BootDiagExport.c`. They contain two spawn cycles of `/usr/libexec/pkd` ending in `OS_REASON_CODESIGNING | Launch Constraint Violation`, `Constraint not matched`, service removal, and ten-second respawn throttling. These rows carry a 2026-11-15 wall-clock timestamp. They are the exported launchd-log tail, not proof that either spawn came from the c1 trampoline and not proof of a single root cause for widget failure.
- Brightness: `alioth-bl=5` reached the board. The serial console has no `L st`, `L path`, `L2`, or `DCS` rows. The persistent UFS log has all three rows in each of 66 slots. The first slot reads `L st=3 req=0 lvl=0 ... a=34 b=-1 ... err=1280 goal=0`, `L path runs=0 fades=0 ...`, `L2 ia=2 ib=0 ...`; the latest reads `L st=3 req=1 lvl=1024 ... a=34 b=-1 ... err=1280 goal=1024`, `L path runs=1 fades=1 ...`, `L2 ia=3 ib=0 ...`. Stage 3 is service registered, `req=1` is one OS request, and `a=34 b=-1` is not a successful DCS 0x52 read-back. No per-level visible brightness change, fade completion, or valid 0x52 read-back is claimed.
- Prime list: `diag/files/private/var/mobile/Library/Caches/com.apple.backboardd/m2v/prime.log` starts with `list ... lines=168 kept=159 stat-failed=7 errno=2 build=b126f0c9677c1e3c5237c0bc0d3ed3a9-46771b3458313ddebc7782f3f216050b`; it records `.metallib` compilations, refusals and cache work. It is telemetry, not a wallpaper or desktop pixel result.

## Persistent export and host decode

```sh
sh tmp/read-log.sh --diag > tmp/read-log-c1comb.log 2>&1
sh tmp/decode-oslog.sh build/sheng-logs/c1-00000153fbf21b88 > tmp/decode-oslog-c1.log 2>&1
```

- `tmp/read-log-c1comb.log`: 32 MiB log pull and 128 MiB diagnostic pull; `diag 156 files in diag/files, 0 dropped`; screen exported to `build/sheng-logs/c1-00000153fbf21b88/diag/screen.bmp`; `boot 00000153fbf21b88 uptime 178s rounds 66 slots 66/507 errors 0 state 2`; slot restored to b; `READ_LOG_EXIT=0`. The script set `tmp/needs-power-cycle`. A complete manual power cycle and `sh tmp/power-cycle-done.sh` are required before another boot.
- `header.txt`: `SHLOG001`, version 5, `boot_id 0x153fbf21b88`, `uptime_s 178`, `rounds 66`, `slots_written 66`, `write_errors 0`, `state 2`, `tag c1`, `diag_bytes 134217726`, `frozen_first 33`, `frozen_round 27`.
- `diag.bin` is 134,217,728 bytes. Its first eight bytes are `SHEXR001`, its second magic at offset 16 is `M2VCACH1`, and its stream length field is 134,217,710. The parsed stream contains 156 entries, including `screen.bgrx` of 2,592,016 bytes; the parsed end offset is 134,217,726, two bytes before the 128 MiB blob boundary. `read-sheng-log.py` extracted 154 regular files plus the converted screen; 156 stream entries and 154 regular files are separate counts.
- `diag/screen.bmp`: `BM`, 1,944,054 bytes, 540 by 1200, 24 bits per pixel, pixel offset 54. Direct pixel statistics are 594,058 of 648,000 non-black pixels (916 per 1000), mean luma 41, and maximum channel values 255/255/255. These are pixel statistics, not a desktop or wallpaper verdict.
- Raw byte hits in the extracted files: `ATXWidgetDescriptorCache` 19 hits in one dsc file, `pkd Candidate plugin count` 0, `com.apple.backboardd` 84 hits in five files, `BatteriesAvocadoWidgetExtension` 7 hits in two files, `SHEXR001` 0, `M2VCACH1` 0. The last two magics are in the `diag.bin` container header, not the extracted files. Raw byte hits are not decoded unified-log events and do not prove widget descriptor registration, success, or failure.
- `tmp/decode-oslog-c1.log`: `build/oslog-c1-00000153fbf21b88.logarchive.txt (1 lines)`, `compose failures: 0`, `DECODE_EXIT=0`. The text file contains only `Timestamp               Ty Process[PID:TID]`; `.err` is empty. The host decode command succeeded but produced no event rows. Do not infer `ATXWidgetDescriptorCache` or `pkd Candidate plugin count` results from it.

## Independent host-side recount

```sh
/opt/homebrew/bin/python3.13 -I tmp/verify-c1-log-agent.py > tmp/verify-c1-log-agent.log 2>&1
```

- `tmp/verify-c1-log-agent.py` SHA-256 is `e79cd9bab2bee24c6213aaa35161783a57c7c924eab9d87ee1f07416f6516f74`; `tmp/verify-c1-log-agent.log` SHA-256 is `f978abe7065bc4269ccecc8f52a6c08b3cba1d979c9e4fddb4993369788f50d9`. The script is read-only. `VERIFY_EXIT=0`.
- The decoded text is 44 bytes and one line. `.err` is 0 bytes. `DECODE_EXIT=0` means the `log show` command exited successfully. It does not mean an event was decoded. `compose failures: 0` is the count of the phrase `compose failure` in the output, not an event count. The assembled `.logarchive` has two `.tracev3` files, one `dsc` file, one `timesync` file and `Info.plist`. Both `.tracev3` files have SHA-256 values equal to the exported source files. The archive has no two-hex `uuidtext` directories at its root. The cause of the header-only output is not established.
- The `dsc` file has one static format string `ATXWidgetDescriptorCache init - descriptor count: %ld` and zero numeric descriptor counts. Raw `diag.bin` byte hits are `ATXWidgetDescriptorCache` 19, `descriptor count` 12, `codesigning` 12, `WidgetRenderer` 81, `SpringBoard` 1256, `WIDGET` 319, `backlight` 401, `Brightness` 959, `desktop` 121, `FRAME` 508 and `PANEL` 10. These are static strings or raw bytes, not decoded events.
- In the persistent text set without the serial console, `pkd` occurs 75 times across 39 lines (`msgbuf.txt` 53, `frozen.txt` 21, `lsinit.log` 1), `Launch Constraint Violation` 18 times, `L st=` and `L path` 67 times each, `[diag] status` 25 times, `IOMFBBrightnessLevelProto` twice, and `SpringBoard` twice as container-path strings. `ATXWidgetDescriptorCache`, `descriptor count`, `pkd Candidate plugin count`, `WidgetRenderer`, `PKDSHIM`, `DCS 0x51`, `DCS 0x52`, `desktop`, exact uppercase `FRAME` and `SURFACEL` are all zero. The two `IOMFBBrightnessLevelProto` rows are backboardd sandbox denials, not brightness successes. Case-insensitive `FRAME` 698 comes from words such as `frames` and `Framework`, not the frame marker.
- Including the serial console adds `pkd` 46, `Launch Constraint Violation` 6, `LSPKD` 39, `2026-11-15` 39 and `PREVLOG` 1. `msgbuf.txt` and `frozen.txt` share 107 lines and contain none of `PREVLOG`, `LSPKD` or `2026-11-15`. Keep the 39 historical console rows separate from c1-caused events.
- `log.bin` has 507 mixed-generation slots: `c1` 66, `wifi8s` 27, `seed501` 27 and `prime2` 387. The c1 tag has 66 `L st=` rows, 66 `L path` rows and 24 `[diag] status` rows. The prime2 tag has 387 `[diag] status` rows. `diag-skipped.txt` has 401 non-NUL records: 388 `skipped`, 10 `lookup`, 3 `long` and 43 trailing NUL bytes; it does not enumerate the 605 skipped items named in the status line.
- No decoded event row shows plugin registration, a desktop frame, brightness control success or DCS command success. This recount does not assign a root cause or change a `porting.toml` status.

## Boundaries

- This evidence file records one short board round and its host-side exports. It does not raise any `porting.toml` area to a new completion level.
- `payload.json` retains `hardware_tested: false`: that field describes the payload build record, not this separate device round.
- No desktop, wallpaper, widget pixel, working user brightness control, successful DCS 0x52 read-back, full Wi-Fi runtime, or decoded widget unified-log event is claimed.
- No further boot is authorized from this evidence file. The power-cycle flag remains in force.
