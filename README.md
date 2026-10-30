# UniXNU-alioth

Device repository for the Xiaomi Redmi K40 (`alioth`, `M2012K11AC`, SM8250-AC).

| Repository | Contents |
| --- | --- |
| [UniXNU](https://github.com/FFT-infra/UniXNU) | Build tools, component catalog, ARM64 and OS profiles |
| [UniXNU-sm8250-common](https://github.com/FFT-infra/UniXNU-sm8250-common) | SM8250 resources, DTSI, config fragment, patches |
| [uefi-alioth](https://github.com/FFT-infra/uefi-alioth) | Mu-Silicium UEFI port |
| [NativeUfs-alioth](https://github.com/FFT-infra/NativeUfs-alioth) | UFS driver fork carrying this board's stream configuration |

Device page: <https://unixnu.com/zh/devices/alioth> (generated from `porting.toml`).
Porting record: [`porting.toml`](porting.toml) — the authoritative statement of what
is verified; read <https://unixnu.com/zh/porting/records> for its levels.

## State

UFS root with `launchd` as PID 1, a stable lock screen, touch input and a published
GPU device driving the staged Metal accelerator. The desktop draws on the
`ipados27-gpu-parity` pin but stops at the client IOVA window: SpringBoard peaks at
1727 MiB and slot 1 ends at 1706 MiB, so it loses its Metal device. No Wi-Fi: the
QCA6390 is ath11k and the catalog's `Ath12kWifi` is ath12k, and PCIe step 10 takes
the machine down at its first DBI access.

`porting.toml` carries the per-area status and evidence; this paragraph is a
summary of it, not a second source.

## Prerequisites

- A UniXNU checkout beside this repository (`../UniXNU`), and `../uefi-alioth` for
  the firmware step.
- The donor IPSW components under `../UniXNU/out/ipsw/21H461/`, including
  `kernelcache.decompressed` (sha256 `f18b4df8…`). Set `UNIXNU_KERNELCACHE` to it
  to build without network access.
- A `fastboot`-capable host for the device. The kernel lives in the payload
  ramdisk, so `build kernel` alone does not change what the device runs: run
  `build payload` and then `bootimg`, and confirm with
  `scripts/check-payload-kernel.py`.
- `dtc`, and the EDK2 tree that `firmware.py` resolves from `feeds.toml`.

## Inputs

| Item | Value |
| --- | --- |
| SoC | SM8250-AC (kona) |
| Donor | `iPad7,1`, iPadOS `17.7.11`, build `21H461`, `j120ap` |
| Donor chip / board ID | `0x8011` / `12` |
| Userdata | first LBA `0x2a8000`, last LBA `0x3b037fa`, 4096-byte blocks |
| Firmware config / DSC | `alioth` / `Platforms/Xiaomi/aliothPkg/alioth.dsc` |
| Firmware volume | `Build/aliothPkg/DEBUG_CLANGPDB/FV/SILICIUM_UEFI.fd` |

The partition geometry is an observation from one handset. Installing replaces
Android's userdata.

## Build

Run from the UniXNU checkout:

```sh
./unixnu defconfig alioth_defconfig -D ../UniXNU-alioth
./unixnu fetch -D ../UniXNU-alioth
./unixnu status --json -D ../UniXNU-alioth
./unixnu tree -D ../UniXNU-alioth
./unixnu build kernel -D ../UniXNU-alioth
./unixnu build payload -D ../UniXNU-alioth --uefi ../uefi-alioth \
  --unique-chip-id <hex> --trust-cache <bundle> --data-ramdisk <seed>
./unixnu bootimg -D ../UniXNU-alioth --uefi ../uefi-alioth \
  --dtb ../uefi-alioth/Resources/DTBs/alioth.dtb -o boot.img
```

Read [porting](https://unixnu.com/porting/) and the
[bring-up order](https://unixnu.com/porting/bring-up) before the first build.

## Layout

| Path | Contents |
| --- | --- |
| `board.toml` | Board selection, userdata geometry, payload and ADT inputs |
| `feeds.toml` | SoC source pin and the board driver forks |
| `unixnu.lock` | Resolved source commits from the last `unixnu fetch` |
| `dts/alioth.dts` | Board memory, reserved regions and enabled peripherals |
| `configs/alioth_defconfig` | Device kernel configuration |
| `patches/xnu/series` | Board XNU patches in application order |
| `porting.toml` | Porting record rendered by the documentation site |
| `package/` | Board packaging, appears as `kits/alioth` |
| `docs/evidence/` | Per-boot readings cited by `porting.toml` |
| `proprietary-files.txt` | Firmware inputs and hashes |
