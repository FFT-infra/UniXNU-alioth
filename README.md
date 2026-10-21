# UniXNU-pipa

> **Work in progress.** Prepared without the tablet; nothing here has run on a pipa yet.

Device repository for the Xiaomi Pad 6 (pipa, SM8250), laid out like [UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng). Firmware: [uefi-pipa](https://github.com/xVanTuring/uefi-pipa), branch `pipa`, a fork of [uefi-sheng](https://github.com/UniXNU/uefi-sheng) whose upstream Mu-Silicium tree already carries `pipaPkg`.

**State: prepared without the tablet. Nothing here has run on a pipa.** The static checks pass (see [Checks](#checks)); every hardware value names its public source.

## What is here

| File | Contents |
| --- | --- |
| `board.toml` | Board, IPSW, firmware, payload and ADT settings |
| `configs/pipa_defconfig` | Kernel configuration of the first image |
| `scripts/build-arm-driver.py` | The generic driver archive builder, copied unchanged from UniXNU-sheng |
| `scripts/build-platform-power.py`, `package/ShengPlatformPower.cpp` | The original-PMGR bridge of UniXNU-sheng, which accesses no hardware; the builder takes this board's previous kernel as its reference |
| `scripts/make-pipa-adt.py` | Apple device tree for SM8250: GICv3 0x17a00000/0x17a60000, stride 0x20000, Cortex-A55/A77 cores, 19.2 MHz timebase |
| `scripts/check-pipa.py` | Firmware preflight: UniXNU integration, FD and stack against the memory map, vendor inputs, splash region |
| `scripts/patch-xnu-entry-probe.py`, `scripts/xnu-panic-screen.c` | Copied unchanged from UniXNU-sheng; the payload uses them for the on-screen panic text (there is no UART) |

`uefi-pipa` changes on top of uefi-sheng:

| File | Change |
| --- | --- |
| `Platforms/Xiaomi/pipaPkg/pipa.dsc` | UniXNU, UniXNUDxe, the ramdisk block device driver, DarwinBoot, UniXNULib and FdtLib |
| `Platforms/Xiaomi/pipaPkg/pipa.fdf` | UniXNU and UniXNUDxe in the firmware volume |
| `Platforms/Xiaomi/pipaPkg/Include/DXE.inc` | `ShengBootMemoryDxe` (publishes the boot image ramdisk, the payload volume, as a block device; every range comes from the device tree, so the shengPkg driver serves pipa unchanged) and DarwinBoot |
| `Resources/Configs/pipa.toml` | The boot image base, offsets and page size written out (the mkbootimg defaults upstream packs pipa with) |

The upstream vendor driver set stays as it is; upstream Mu-Silicium lists display, internal storage and side buttons as working on pipa with it.

## Host tools

The unixnu driver needs Python 3.11 or newer and the tools of the [requirements page](https://unixnu.com/guide/requirements). On this Mac these are missing:

```sh
brew install llvm lld cmake ninja mtools dtc
```

Homebrew ships `lld-link` in `lld`, separate from `llvm`; the CLANGPDB tool chain needs both. Python 3.11 comes from pyenv: `pyenv local 3.11.15` in the UniXNU checkout covers the driver and every script it starts there.

## Stage 1 to 3: firmware, menu and payload volume

```sh
export UNIXNU_UEFI=$PWD/uefi-pipa      # the preflight reads the checkout from here
cd UniXNU                              # cloned beside this repository
git submodule update --init --recursive

./unixnu fetch -D ../UniXNU-pipa
./unixnu build package -D ../UniXNU-pipa
./unixnu build firmware -D ../UniXNU-pipa
./unixnu bootimg -D ../UniXNU-pipa --dtb "$UNIXNU_UEFI/Resources/DTBs/pipa.dtb" --ramdisk none

fastboot boot out/pipa/boot/boot.img
```

`fastboot boot` writes nothing to the tablet. Expected: the Mu-Silicium boot log, then the UniXNU menu with its built-in configuration. A firmware stall leaves the tablet in the bootloader; start the same image again.

## Stage 4 and 5: kernel entry

`configs/pipa_defconfig` builds the generic kernel path only: CPU, GIC, timer, the device-tree platform expert, the source corecrypto and pthread providers and NativeAES. Storage, Data, display and the owner root chain stay out until their inputs exist (see the file).

A first kernel build from a clean tree needs the bootstrap pass of [UniXNU#5](https://github.com/UniXNU/UniXNU/pull/5) ([UniXNU#4](https://github.com/UniXNU/UniXNU/issues/4)), which UniXNU's main has.

```sh
export UNIXNU_IPSW=$PWD/out/ipsw/iPad_Pro_HFR_17.7.11_21H461_Restore.ipsw   # a local copy of [ipsw] url
./unixnu defconfig pipa_defconfig -D ../UniXNU-pipa
./unixnu ipsw fetch -D ../UniXNU-pipa
./unixnu build kernel -D ../UniXNU-pipa

./unixnu build payload -D ../UniXNU-pipa --unique-chip-id <16 hex digits>
./unixnu bootimg -D ../UniXNU-pipa --dtb "$UNIXNU_UEFI/Resources/DTBs/pipa.dtb"
fastboot boot out/pipa/boot/boot.img
```

The platform seed is any nonzero 64-bit value kept for this tablet, for example from `openssl rand -hex 8`; `.cache/platform-seed` holds the one of this checkout.

On a new tree the PMGR bridge has no reference kernel yet: the first `build kernel` links a no-op in its place and says so, and the second links the bridge.

Expected: DarwinBoot hands over, the entry probe bars (green, blue, magenta) appear at the top of the screen, and the panic screen shows `Failed to mount root device`, as there is no storage driver yet. XNU's video console does not draw the kernel log despite `-v`. The same kernel configuration reaches that point on the QEMU board of [UniXNU-qemu](https://github.com/xVanTuring/UniXNU-qemu).

## Values taken from the tablet

Read under Android 14 (HyperOS 2.0.20) with `android-devinfo` on 2026-10-21; the firmware partitions were not copied yet.

| Value | Read | In this repository |
| --- | --- | --- |
| DRAM banks | 0x80000000+0x37700000, 0xc0000000+0x1c0000000, 0x800000000+0x200000000 (15.87 GiB): the added 8 GiB is a bank of its own at 32 GiB | `dram_size = 0x980000000`, base to the end of the last bank |
| Free memory for DarwinBoot's 4 GiB | 0xffc20000..0x27fffffff (6 GiB) in the second bank, 8 GiB in the third | `owned_memory_size = 0x100000000` |
| userdata | sda34, LBA 0x293000..0x1d387fa, 4096-byte blocks (106.6 GiB); the GPT agrees with sysfs | `[userdata]`; `unixnu build userdata --dry-run` passes |
| Storage | SK hynix HN8T05DEHKX073 (UFS 3.1), ufshc 0x1d84000, 6 LUNs | for NativeUfs |
| Boot image header of the stock `boot` | header v3, page 4096 (with `vendor_boot`, header v3); slot `_b` active | the firmware image uses header v1, page 2048, as Mu-Silicium packs pipa |
| CPUs, GIC | MPIDR 0x000..0x700 (four Cortex-A55, four Cortex-A77), GICv3 0x17a00000/0x17a60000 | as in `make-pipa-adt.py` |
| Touch | Novatek `NVT-ts-spi` on the GENI SPI at 0x990000, 9.6 MHz, interrupt TLMM 39; Android reports 0..17999 x 0..28799 | for the touch driver |
| Panel | `mdss_dsi_m82_36_02_0a_dual_dphy_video` (dual DSI, video mode), 1800x2880, 120 Hz; `cont_splash_region` 0x9c000000+0x2300000 | the frame buffer of SimpleFbDxe |
| GPU | Adreno 650v3, chip ID 0x06050002 | |

## Known risks

- **Firmware volume size.** The vendor drivers are 4.8 MiB before compression and the FD is 3 MiB. If UniXNU does not fit, the EDK II build stops. Then either drop vendor drivers that UniXNU does not need (USB, charger) the way sheng's `SHENG_VENDOR_HW = 0` does, or grow the FD into the 576 KiB gap below the UEFI stack (`pipa.toml` and the "UEFI FD" descriptor together).
- **Vendor drivers and the panel.** On sheng the vendor clock stack blanked the panel; upstream reports a working display on pipa, but not together with UniXNU.
- **ABL acceptance** of a 192 MiB ramdisk in a header v1 image is unverified on pipa; the stock boot image is header v3.
- **userdata.** A later root image is written with `fastboot flash userdata`, which erases Android's data partition.

## Next steps

1. NativeUfs for SM8250: the compatible, GCC offsets from `gcc-sm8250.c`, SMMU stream 0x0e0, the DMA window.
2. Touch: NT36532 over GENI SPI with SM8250 QUP, GCC and TLMM constants (`TouchBoard.h` of sheng is the template).
3. Display, Data and boot progress: the legacy display archive and the boot progress page live in UniXNU-sheng's package together with sheng board probes; they need separating, and the boot progress page needs the owner root chain.
4. GPU: Adreno 650 needs its own GMU, HFI and PM4 work.

## License

BSD-2-Clause-Patent. See [LICENSE](LICENSE). `make-pipa-adt.py` is derived from `make-sheng-adt.py` of UniXNU-sheng; `build-arm-driver.py` and the two panic-screen files are copies from it. `ShengPlatformPower.cpp` comes from it under MIT (its SPDX header).
