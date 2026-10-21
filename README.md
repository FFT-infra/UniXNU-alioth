# UniXNU-pipa

Device repository for the Xiaomi Pad 6 (pipa, SM8250), laid out like [UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng). Firmware: `uefi-pipa`, branch `pipa`, a fork of [uefi-sheng](https://github.com/UniXNU/uefi-sheng) whose upstream Mu-Silicium tree already carries `pipaPkg`.

**State: prepared without the tablet. Nothing here has run on a pipa.** The static checks pass (see [Checks](#checks)); every hardware value names its public source.

## What is here

| File | Contents |
| --- | --- |
| `board.toml` | Board, IPSW, firmware, payload and ADT settings |
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
export UNIXNU_UEFI=~/agents-home/uefi-pipa      # the preflight reads the checkout from here
cd ~/agents-home/UniXNU
git submodule update --init --recursive

./unixnu fetch -D ../UniXNU-pipa
./unixnu build package -D ../UniXNU-pipa
./unixnu build firmware -D ../UniXNU-pipa
./unixnu bootimg -D ../UniXNU-pipa --dtb "$UNIXNU_UEFI/Resources/DTBs/pipa.dtb" --ramdisk none

fastboot boot out/pipa/boot/boot.img
```

`fastboot boot` writes nothing to the tablet. Expected: the Mu-Silicium boot log, then the UniXNU menu with its built-in configuration. A firmware stall leaves the tablet in the bootloader; start the same image again.

## Stage 4 and 5: kernel entry

The first kernel is the one the UniXNU-sheng configuration builds. Its board drivers start only on `sheng-*` boot arguments, and `board.toml` passes none of them; the pipa ADT names `qcom,sm8250-ufshc`, which the SM8550 UFS driver does not match.

```sh
git clone --recurse-submodules https://github.com/UniXNU/UniXNU-sheng ../UniXNU-sheng
./unixnu defconfig sheng_defconfig -D ../UniXNU-sheng
./unixnu fetch -D ../UniXNU-sheng
(cd ../UniXNU-sheng && python3 scripts/extract-files.py)   # sheng's GPU and touch firmware from its public mirror
./unixnu ipsw fetch -D ../UniXNU-sheng
./unixnu build kernel -D ../UniXNU-sheng

./unixnu build payload -D ../UniXNU-pipa --kernel out/sheng/kernel/kernel.macho --unique-chip-id <16 hex digits>
./unixnu bootimg -D ../UniXNU-pipa --dtb "$UNIXNU_UEFI/Resources/DTBs/pipa.dtb"
fastboot boot out/pipa/boot/boot.img
```

The platform seed is any nonzero 64-bit value kept for this tablet, for example from `openssl rand -hex 8`. Expected: DarwinBoot hands over, the kernel prints on the frame buffer and stops at the root mount, with the panic text on screen.

## Values to take from the tablet

Run `android-devinfo` (`python3 devinfo.py --firmware`) on the tablet and compare:

| Value | Where | Now |
| --- | --- | --- |
| DRAM banks | `summary.md` Memory | `dram_size = 0x3fbb00000`, inferred for this 16 GB tablet from the 8 GB tree in `pipa.dtb` |
| Second bank | `summary.md` Memory | `owned_memory_size = 0x100000000` assumes 4 GiB of contiguous free memory there |
| userdata first and last LBA | `summary.md` Storage | `[userdata]` left out |
| Boot image header of the stock `boot` | `summary.md` Boot images | the firmware uses header v1, page 2048 |
| Touch controller, SPI bus and GPIOs | `summary.md` Touch | NT36532 host download over SPI, from Xiaomi's kernel source |
| Panel name | `summary.md` Display | unknown |

## Known risks

- **Firmware volume size.** The vendor drivers are 4.8 MiB before compression and the FD is 3 MiB. If UniXNU does not fit, the EDK II build stops. Then either drop vendor drivers that UniXNU does not need (USB, charger) the way sheng's `SHENG_VENDOR_HW = 0` does, or grow the FD into the 576 KiB gap below the UEFI stack (`pipa.toml` and the "UEFI FD" descriptor together).
- **Vendor drivers and the panel.** On sheng the vendor clock stack blanked the panel; upstream reports a working display on pipa, but not together with UniXNU.
- **ABL acceptance** of a 192 MiB ramdisk in a header v1 image is unverified on pipa.
- **userdata.** A later root image is written with `fastboot flash userdata`, which erases Android's data partition.

## Next steps

1. NativeUfs for SM8250: the compatible, GCC offsets from `gcc-sm8250.c`, SMMU stream 0x0e0, the DMA window; `[userdata]` from the tablet's GPT.
2. Touch: NT36532 over GENI SPI with SM8250 QUP, GCC and TLMM constants (`TouchBoard.h` of sheng is the template).
3. A pipa kernel configuration of its own once the shared scripts of UniXNU-sheng are generalized.
4. GPU: Adreno 650 needs its own GMU, HFI and PM4 work.

## License

BSD-2-Clause-Patent. See [LICENSE](LICENSE). `make-pipa-adt.py` is derived from `make-sheng-adt.py` of UniXNU-sheng, and the two panic-screen files are copies of it.
