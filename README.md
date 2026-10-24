# UniXNU-alioth

Device repository for the Xiaomi Redmi K40 (`alioth`, `M2012K11AC`, SM8250-AC).

| Repository | Contents |
| --- | --- |
| [UniXNU](https://github.com/FFT-infra/UniXNU) | Build tools, component catalog, ARM64 and OS profiles |
| [UniXNU-sm8250-common](https://github.com/FFT-infra/UniXNU-sm8250-common) | SM8250 resources, DTSI, config fragment, patches |
| [uefi-alioth](https://github.com/FFT-infra/uefi-alioth) | Mu-Silicium UEFI port |

## State

Bring-up. The firmware initializes the display, the buttons, the UFS storage
and USB. The kernel build and the boot image are not yet produced from this
repository. No stage is verified on hardware from this layout.

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
```

Read [porting](https://unixnu.com/porting/) and the
[bring-up order](https://unixnu.com/porting/bring-up) before the first build.

## Layout

| Path | Contents |
| --- | --- |
| `board.toml` | Board selection, userdata geometry, payload and ADT inputs |
| `feeds.toml` | SoC source pin and the UFS driver fork |
| `dts/alioth.dts` | Board memory, reserved regions and enabled peripherals |
| `configs/alioth_defconfig` | Device kernel configuration |
| `patches/xnu/series` | Board XNU patches in application order |
| `porting.toml` | Porting record rendered by the documentation site |
| `package/` | Board packaging, appears as `kits/alioth` |
| `proprietary-files.txt` | Firmware inputs and hashes |
