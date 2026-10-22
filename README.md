# UniXNU-alioth

Device repository for the Redmi K40 (`alioth`, `M2012K11AC`, SM8250-AC), laid out like
[UniXNU-sheng](https://github.com/UniXNU/UniXNU-sheng). The matching firmware checkout is
[uefi-alioth](https://github.com/FFT-infra/uefi-alioth), branch `feat/alioth-unixnu`.

**State: bring-up, not a daily driver.** The first milestone is deliberately narrow: boot the
unchanged iPad7,1 iPadOS 17.7.11 target image through UEFI, UniXNU and DarwinBoot, then reach the
XNU `Failed to mount root device` panic. This repository does not describe or build `userdata`.

The current execution order, latest early-XNU blocker and disposition of
community recommendations are tracked in [PLAN.md](PLAN.md).

## What is here

| File | Contents |
| --- | --- |
| `board.toml` | Board, IPSW, firmware, payload and ADT settings |
| `configs/alioth_defconfig` | Minimal first-boot kernel configuration, including board archive builders |
| `scripts/build-arm-driver.py` | Generic XNU driver archive builder copied from UniXNU-sheng |
| `scripts/build-platform-power.py`, `package/ShengPlatformPower.cpp` | Original-PMGR bridge; it accesses no hardware |
| `scripts/make-alioth-adt.py` | Apple-format device tree for SM8250: GICv3 `0x17a00000` / `0x17a60000`, stride `0x20000`, Cortex-A55/A77 cores and 19.2 MHz timebase |
| `scripts/check-alioth.py` | Firmware preflight: UniXNU integration, FD/stack, vendor binaries, DTB splash region and Android boot image parameters |
| `scripts/patch-xnu-entry-probe.py`, `scripts/xnu-panic-screen.c` | Entry probe and on-screen panic support copied from UniXNU-sheng |

`uefi-alioth` supplies `aliothPkg`, the `ShengBootMemoryDxe` ramdisk block device, UniXNU,
UniXNUDxe and DarwinBoot. Its existing alioth vendor drivers, display path and storage stack are
not replaced by this integration.

## Shared workspace

The orb and Mac share the same checkout at two mount paths:

- orb: `/home/xinyang/Lab/Bridge/WorkSpace/alioth`
- Mac: `/Users/xinyang/Lab/Bridge/WorkSpace/alioth`

All source edits, Git operations, static checks, EDK2 builds, payloads, boot images and QEMU runs
belong in orb. Only XNU's Xcode build and the physical USB `adb` / `fastboot` steps run on the Mac.
The portable relative tree links keep `out/alioth/tree` usable from both mount paths.

## Firmware and payload

From the shared workspace on orb:

```sh
export UNIXNU_UEFI=$PWD/uefi-alioth
cd UniXNU

./unixnu defconfig alioth_defconfig -D ../UniXNU-alioth
./unixnu fetch -D ../UniXNU-alioth
./unixnu ipsw fetch -D ../UniXNU-alioth
./unixnu build package -D ../UniXNU-alioth
./unixnu build firmware --uefi "$UNIXNU_UEFI" -D ../UniXNU-alioth
./unixnu bootimg -D ../UniXNU-alioth \
  --dtb "$UNIXNU_UEFI/Resources/DTBs/alioth.dtb" --ramdisk none
```

Expected during the first `fastboot boot`: the Mu-Silicium firmware initializes the display and
buttons, UniXNU shows its menu, and the `ALIOTHESP` smoke payload is readable. `fastboot boot`
does not write a partition.

## Kernel and full payload

The kernel is the one step that must run on the Mac with a complete Xcode installation. The
current shared `out` tree already contains a kernel and its PMGR reference. Rebuild once after a
source change:

```sh
export DEVELOPER_DIR=/Volumes/Disk/Applications/Xcode-27.1.0.app/Contents/Developer
export UNIXNU_UEFI=/Users/xinyang/Lab/Bridge/WorkSpace/alioth/uefi-alioth
cd /Users/xinyang/Lab/Bridge/WorkSpace/alioth/UniXNU

./unixnu build kernel -D ../UniXNU-alioth
```

A completely fresh `out` tree still needs two passes: the first creates the compile records and
temporarily links a no-op PMGR bridge; the second uses that kernel as the reference and links the
real `ShengPlatformPower` archive. Do not repeat the bootstrap pass when the reference already
exists.

Return to orb for every remaining step:

```sh
cd /home/xinyang/Lab/Bridge/WorkSpace/alioth/UniXNU
./unixnu build payload -D ../UniXNU-alioth --unique-chip-id <16-hex-digits>
./unixnu bootimg -D ../UniXNU-alioth \
  --dtb ../uefi-alioth/Resources/DTBs/alioth.dtb
```

The platform seed is generated once, kept in the ignored `.cache/platform-seed`, and reused. It is
not committed. Expected on hardware: DarwinBoot handoff, the entry probe colour bars, then the XNU
`Failed to mount root device` panic. No earlier provider or PMGR panic is acceptable.

## Hardware facts

The alioth DTB has two memory banks:

- `0x80000000 + 0x3bb00000`
- `0xc0000000 + 0x1c0000000`

XNU receives one `dram-base` / `dram-size` span, so `board.toml` uses `0x200000000`, the distance
from the first bank to the end of the second. DarwinBoot requests a 4 GiB owned-memory window in
the large second bank. The CPU affinity set is `0x000..0x700`, the GIC and timebase values are the
SM8250 values, and the device identity uses the unchanged iPad7,1 target image.

The repository intentionally has no `[userdata]` section. No build or flash step writes `userdata`,
`metadata`, `super` or GPT. `boot_a` remains unchanged.

## Checks and expected limits

`scripts/check-alioth.py` is the firmware preflight. In the assembled tree it writes
`out/alioth/check.json` and verifies the required INFs, `Resources/Configs/alioth.toml`,
`MemoryMapLib`, `Binaries/alioth`, the splash region and the boot image contract:

```text
header_version=1 base=0x10000000 kernel_offset=0x8000 ramdisk_offset=0x16000000 tags_offset=0x100 pagesize=2048
```

The 176 MiB payload is loaded at `0x26000000..0x31000000`. The bootloader DTB
advertises that range through `/chosen`; `ShengBootMemoryDxe` pins it as
unowned reserved RAM before publishing the rest of DRAM as SystemMemory, then
publishes it as `BlockIo`. This ordering prevents the DXE allocator from taking
the payload pages before FatDxe can mount them.

The current FD is 3 MiB with base `0x9FC00000` and stack at `0x9FF90000`. If it does not fit, the
maximum pre-planned growth is `0x390000`; both `alioth.toml` and `MemoryMapLib.c` must change
together. Vendor drivers are not removed automatically.

Persistent testing is limited to `boot_b` after every read-only check passes. The exact write path
is `fastboot flash boot_b out/alioth/boot/alioth-full-reserved.img` followed by
`fastboot --set-active=b`. Substitute a newly accepted build after rebuilding. Rollback is
`fastboot --set-active=a`. No step formats or resizes a partition.

## License

BSD-2-Clause-Patent. See [LICENSE](LICENSE). The generic archive builder and panic-screen files are
derived from UniXNU-sheng; `ShengPlatformPower.cpp` remains under its MIT SPDX hea