# Boot arguments

The board follows the upstream split between shared platform arguments and
board-private arguments. Use the upstream name exactly when the argument exists
in `docs/zh/xnu/boot-arguments.md` or in a shared component. Use the `alioth-`
prefix for a board-private argument. Do not add a new `sheng-` or `pipa-`
argument: those prefixes name other boards.

The rename landed on 2026-11-20. The device branches, the board package and the
current scripts read the new names. The historical files under `docs/evidence/`
keep the names that were on the command line at the time; read them through this
table.

## Shared names

| Old | Current | Reader |
| --- | --- | --- |
| `sheng-smp`, `sheng-smp-gic` | `smp`, `smp-gic` | `drivers/ArmCpu/ArmCpu.cpp` |
| `sheng-cpufreq` | `cpufreq` | `package/CpuFreqProbe.cpp` |
| `sheng-touch-hid` | `touch-hid` | `drivers/ShengHidInput/NativeHidInput.cpp` |
| `sheng-aks`, `sheng-aks-sls` | `aks`, `aks-sls` | `package/KeyStoreProbe.cpp` |
| `sheng-lite`, `sheng-setup` | `lite`, `setup` | `package/BootWatchdogPolicy.c` |
| `sheng-disable-jobs`, `sheng-disable-watchdogd` | `disable-jobs`, `disable-watchdogd` | `package/BootWatchdogPolicy.c` |
| `sheng-data-persist`, `sheng-data-reset` | `data-persist`, `data-reset` | `NativeUfs-alioth`, `drivers/RamBlock` |
| `sheng-pmgr`, `sheng-diag`, `sheng-fb-gpu` | `pmgr`, `diag`, `fb-gpu` | base components |
| `sheng-log-reserve` | `log-reserve` | `NativeUfs-alioth/NativeUfs.cpp` |
| `sheng-gpu-asid`, `-bank`, `-bo-contig`, `-bringup`, `-cmddb-version`, `-metal-opens`, `-sid`, `-slot`, `-timeline`, `-trace`, `-ttbr` | the same name without `sheng-` | `drivers/Adreno650` |
| `sheng-gpu-power`, `-metal`, `-pool` | `gpu-power`, `gpu-metal`, `gpu-pool` | `drivers/Adreno650` |

## Board-private names

| Old | Current |
| --- | --- |
| `sheng-autoreset` | `alioth-autoreset` |
| `sheng-build-tag` | `alioth-build-tag` |
| `sheng-diag-export` | `alioth-diag-export` |
| `sheng-log` | `alioth-log` |
| `sheng-init`, `sheng-pkdshim` | `alioth-init`, `alioth-pkdshim` |
| `sheng-touch-edge`, `sheng-touch-split` | `alioth-touch-edge`, `alioth-touch-split` |
| `sheng-touch-synth`, `-at`, `-swipe`, `sheng-synth-repeat` | `alioth-touch-synth`, `-at`, `-swipe`, `alioth-synth-repeat` |
| `sheng-gpu-clients`, `-wc`, `-dynfreq`, `-norestart`, `-no-clients`, `-perf`, `-owned-coherent` | `alioth-gpu-*` |
| `sheng-dpu-scanout`, `sheng-tu-debug` | `alioth-dpu-scanout`, `alioth-tu-debug` |
| `sheng-wallpaper-db`, `-posters`, `-probe`, `-reset`, `-scan`, `sheng-wp` | `alioth-wallpaper-*`, `alioth-wp` |
| `sheng-ls-dump`, `-late`, `-report`, `-reset` | `alioth-ls-*` |
| `sheng-banner`, `sheng-cpu0-park`, `sheng-cpufreq-cap` | `alioth-banner`, `alioth-cpu0-park`, `alioth-cpufreq-cap` |
| `pipa-gpu`, `pipa-gpu-*`, `pipa-ddr-vote`, `pipa-tu` | `alioth-gpu`, `alioth-gpu-*`, `alioth-ddr-vote`, `alioth-tu` |
| `pipa-metal-withhold`, `pipa-turnip-debug`, `pipa-gpu-ro-own` | `alioth-metal-withhold`, `alioth-turnip-debug`, `alioth-gpu-ro-own` |
| `pipa-gpu-pool` | `gpu-pool` |

## Exceptions

`drivers/RamBlock` is pinned at the iPadOS 17 revision `4be2d7e`, which predates
the upstream rename. It still reads `sheng-data-reset`. The board does not pass
that argument in the current configuration. A fork commit on top of the iPadOS 17
pin, or an upstream-compatible alias, is required before that name can change.

The historical evidence files under `docs/evidence/` are not rewritten. The
`porting.toml` evidence entries keep the old command strings for the same reason.
Current configuration and `next_action` text uses the new names.

## Command line

`board.toml` has 51 arguments and 888 bytes. The c1 payload line has 52
arguments and 924 bytes. `payload.py` rejects a line above 1023 bytes; the
upstream page and `pipeline.py` use 1000 bytes. The project keeps 1000 bytes as
its budget, so both lines pass with margin.
