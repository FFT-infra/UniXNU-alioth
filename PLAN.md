# Alioth Bring-up Execution Plan

Last reviewed: 2026-10-22

This plan narrows the work to one milestone: reach the XNU
`Failed to mount root device` panic through UEFI, UniXNU and DarwinBoot.
Storage, owner-root, Data and userland work are out of scope.

## Invariants

- Do not write, erase, format or resize `userdata`, `metadata`, `super` or GPT.
- Do not change `boot_a`.
- On hardware, use `fastboot boot` until the full payload reaches the expected
  XNU state. Persistent writes require a separate, explicit approval.
- Run source edits, Git operations, static checks, EDK2, payload, boot image
  and QEMU work on orb.
- Run only the Xcode kernel build and physical USB `adb` / `fastboot` steps on
  the Mac.
- Use `phxinyang <phxinyang@users.noreply.github.com>` for commits.

## Current State

| Stage | State | Evidence |
| --- | --- | --- |
| 1. UEFI | Passed on hardware | The bootloader DTB mapping no longer faults; Project Silicium and the UniXNU menu appear on alioth |
| 2. UniXNU | Passed on hardware | The menu, timer, side buttons and firmware return work on alioth |
| 3. Payload | In progress; initrd ownership fix built, hardware retest pending | Firmware reached the menu but reported `initrd=Access Denied`, so no `ALIOTHESP` BlockIo reached UniXNU |
| 4. DarwinBoot | Passed in QEMU | EL1 handoff and active native 4K XNU translation |
| 5. Kernel entry | Deferred behind the Stage 3 retest | XNU starts in QEMU and panics in early service setup |
| 6. Storage and root | Not started | Requires a later decision about `userdata` |
| 7. Userland | Out of scope | Nothing in this plan may write `userdata` |

The current kernel and payload still exist. The UEFI-only smoke image is
deliberately separate from the full payload so a firmware regression cannot be
confused with the known kernel service-ordering problem.

Latest Stage 1 and Stage 2 evidence:

- The hardware exception had `ESR_EL1=0x96000006`,
  `FAR_EL1=0xa4dff000`, and a stack carrying
  `ShengBootMemoryDxe.pdb`. It was a second-level translation fault, not an
  XNU or AppleImage4 failure.
- SEC can read the bootloader DTB under the identity map, but the DXE page
  tables do not initially map the DTB's physical pages. The first libfdt
  header read therefore faulted before any memory or initrd processing.
- `ShengBootMemoryDxe` now maps the DTB's first page as temporary MMIO,
  validates the header, maps the remaining FDT pages in GCD descriptor-sized
  pieces, and releases only the pieces it added. The driver now depends on
  `gEfiCpuArchProtocolGuid`.
- Hardware then displayed the Project Silicium logo, loaded the firmware
  drivers and reached the UniXNU menu. The side buttons moved the selection and
  the firmware-return action worked.

Latest Stage 3 evidence:

- A full payload boot reached firmware and UniXNU, but the menu reported
  `fdt=Success initrd=Access Denied`.
- The initrd is loaded at `0x26000000..0x31000000`. `AddMemory()` previously
  ran first and handed the DRAM hole containing it to the DXE allocator as
  SystemMemory. A later `AddMemorySpace(MemoryMappedIo)` cannot own an already
  allocated descriptor; `CoreConvertSpace()` returns `EFI_ACCESS_DENIED`
  whenever any entry has a non-Nonexistent type or a non-NULL `ImageHandle`.
- The driver now parses and pins initrd pages as `EfiGcdMemoryTypeReserved`
  before publishing general DRAM. It also subtracts the page-aligned initrd
  extent from every SystemMemory hole as a failure-safe guard. Reserved RAM is
  visible to the CPU but cannot be allocated by DXE.
- The latest firmware FD has SHA-256
  `e906a0cd50d899dcc014ecb97d574239cbf1022d510a2c90611f52c00a5fdb63`.
  `alioth-full-reserved.img` is 187,068,416 bytes with SHA-256
  `03bcde44a88d97ee1aa0467d2a85e84dd2299c40b88f18cdfa1926a35e35f76a`.
  It still requires a hardware screen check for `i=Success m=Success b=Success`
  and a mounted `ALIOTHESP`.

Existing Stage 5 evidence (do not work on it until Stage 1 passes):

- QEMU reaches real 4 KiB XNU translation.
- The formal `--armvirt --armcpu` fixture includes `/product`; it still reaches
  the same AppleImage4 failure. The older synthetic AES trace omitted
  `--armvirt`, so its missing `/product` node cannot explain this panic.
- XNU faults at `0xfffffff004a92828` with `ESR_EL1=0x96000005`,
  `FAR_EL1=0`, and saved `x0=0`. The instruction sequence dereferences a null
  service object before the expected provider is registered.
- `NativeAES` globals remain zero:
  `_native_aes_registered=0`, `_native_aes_kat_status=0`.
- The previous bounded trace records no `NativeAES::start` event, but did not
  explicitly instrument that function. Service registration order still needs
  one direct trace before changing the personality.

The leading hypothesis is that the NativeAES personality waits for
`IOResourceMatch = IOBSD`, while AppleImage4 needs an AES service before BSD is
available. Do not change the personality until the service trace confirms the
registration order.

## Supplemental Material Decisions

| Item | Decision | Reason |
| --- | --- | --- |
| Seven-stage bring-up order | Adopt | It matches the official porting guide and keeps acceptance local |
| Official QEMU runner and assertions | Adopt | It provides repeatable menu, payload and DarwinBoot checks |
| `probe-darwinboot-xnu.py` real-entry fixture | Keep as the Stage 5 gate | It observes the actual 4K MMU and kernel globals |
| macOS GitHub Actions workflow | Keep as fallback only | The local Mac has already produced a kernel; CI is for a local toolchain regression |
| Community fork progress notes | Read-only reference | Their configurations contain board-specific and destructive assumptions |
| `sheng-log` and `sheng-autoreset` | Reject | The log path writes the end of `userdata` |
| `boot-progress=1/2` | Defer for this gate | The current kernel has no `_boot_progress_*` symbols; it does not replace the service-order trace and would require another kernel configuration |
| `sheng-build-tag` | Do not rely on it yet | It is documented, but is not implemented in the current source tree |
| `amfi_get_out_of_my_way=1` | Do not add | It changes security policy and is not the current blocker |
| "Second kernel build still pending" | Correct as stale | Current outputs were built; only rebuild after a source change |
| `UNIXXNU` in the sample assertion | Correct as a typo | The real marker is `UNIXNU: menu: shown` |
| `uefi-pipa` as the ADT data source | Correct the scope | It contains pipa UEFI integration; GIC and ADT values live in `UniXNU-pipa` |

Community repositories remain read-only:

- `UniXNU-uke` has progressed beyond this milestone, but it flashed
  `userdata`, uses unverified storage bindings and carries a large device patch
  stack. Use it to understand later stages, not to replace this narrow plan.
- `UniXNU-guacamole` is a long-term userland reference.
- `uefi-pipa` is useful for firmware structure and boot image parameters. Its
  local checkout is dirty; use `git show HEAD:path` and never reset it.
- `UniXNU-qemu` is useful for QEMU fixture expectations.
- A black screen can occur after a successful UEFI handoff; it only reopens
  Stage 1 if it also occurs before the UniXNU menu. Record which side of the
  handoff it occurred on before changing firmware or kernel code.

## Active Sequence

### 1. Verify the initrd Reservation and BlockIo Publication

1. Boot `alioth-full-reserved.img` with `fastboot boot`; never flash it during
   this check.
2. Pass the first gate when the UniXNU notice reports success for initrd
   reservation, GCD mapping and BlockIo installation, and at least one FAT
   filesystem volume is found.
3. If mapping still fails, record the compact `i= m= b=` status, its address
   range and piece count. Inspect the conflicting GCD descriptor type and
   owner before changing the payload address or driver order again.
4. If BlockIo exists but FAT does not mount, capture the block/filesystem
   counts. Change `LogicalPartition` only after confirming that BlockIo reached
   the device path.

### 2. Confirm Payload, DarwinBoot and Early XNU Handoff

1. Require `ALIOTHESP`, `unixnu.toml`, the kernel and the ADT to be consumed
   from the ramdisk volume.
2. Require DarwinBoot handoff and the native 4K XNU entry probe.
3. Then continue with the early service-ordering work below.

### 3. Confirm and Fix Early XNU Service Ordering

1. Trace `NativeAES::start`, `IOService::registerService`, the matching wait
   and the AppleImage4 call site in one bounded QEMU run.
2. Confirm whether the NativeAES personality is still waiting for `IOBSD`.
3. If confirmed, make the smallest change that allows NativeAES to start at the
   point AppleImage4 requests it. Do not move unrelated storage or owner-root
   features forward.
4. Add a regression check that fails if `NativeAES::start` is not reached or if
   `_native_aes_registered` is not 1 before the AppleImage4 path.
5. Rebuild the kernel on the Mac. A fresh `out` tree may need the documented
   bootstrap plus reference pass; the current tree needs only the normal rebuild.
6. Rebuild payload and boot image on orb.

### 4. QEMU Acceptance

Run both layers:

1. The official `qemu-run.py` fixture must match
   `UNIXNU: menu: shown` and the DarwinBoot markers, and must reject early
   AppleImage4 or provider panics.
2. `probe-darwinboot-xnu.py` must record a real 4K XNU entry,
   `NativeAES::start`, `_native_aes_registered=1` and
   `_native_aes_kat_status=1`.
3. The run is accepted only when no provider panic occurs before the intended
   root-device failure. The target terminal state is the XNU
