# Alioth Bring-up Execution Plan

Last reviewed: 2026-10-23

The first milestone is complete on hardware: UEFI, UniXNU, `ALIOTHESP`,
DarwinBoot and the 4 KiB XNU entry reach `Failed to mount root device` on
alioth. The current milestone is the NativeUfs storage gate: prove the UFS
stream, SMMU mapping, GPT geometry and raw first-block state before building
the reviewed userdata artifact. Probe 10 has now cleared that gate; the current
milestone is the reviewed userdata artifact, a narrow post-flash manifest probe
and the owner-verified root path. The selected root-image route is upstream
UniXNU's `SHNGROOT` manifest plus the IPSW `OS` and `Cryptex1,SystemOS` windows.
Do not pursue Android F2FS metadata decryption or reconstruct an F2FS root as a
substitute for that route. Owner-root, Data and userland remain later work.

## Invariants

- The user has explicitly authorized overwriting `userdata` after confirming a
  backup. Do not write, erase, format or resize it before the generated image,
  manifest and preflight record have been reviewed. Never write, erase, format
  or resize `metadata`, `super` or GPT.
- Do not use `fastboot erase userdata` as a diagnostic. An empty partition has
  no root image; the accepted route is an explicitly reviewed `SHNGROOT`
  overwrite.
- Probe 10 is a non-data-mutating probe, not an absolute read-only operation.
  It may alter controller, SMMU and possible device state while establishing
  transport.
- Do not change `boot_a`.
- `fastboot` returning `OKAY` only proves transport. Acceptance requires the
  next-stage screen, log or probe output. `fastboot boot` remains the default
  hardware path until the storage gate passes.
- A/B slot state may only be changed through bootloader commands. Never edit
  GPT attribute bits and never use `--set-active` as a diagnostic when state is
  unknown. Before any persistent write, re-read slot, unlock state, partition
  size and image size.
- Change one variable at a time. If the same failure occurs twice, stop and
  record the hypotheses already excluded before making another change.
- Every stage needs explicit pass evidence, a rollback path and a cold-boot
  check where persistence is involved. Do not treat "driver loaded", "service
  started", "fastboot OKAY" or "QEMU passed" as functional acceptance.
- Record each artifact's SHA-256, build inputs and verification state.
- Run source edits, Git operations, static checks, EDK2, payload, boot image
  and QEMU work on orb.
- Run only the Xcode kernel build and physical USB `adb` / `fastboot` steps on
  the Mac.
- Use `phxinyang <phxinyang@users.noreply.github.com>` for commits.

## Current State

| Stage | State | Evidence |
| --- | --- | --- |
| 1. UEFI | Passed on hardware | Project Silicium and the UniXNU menu appear on alioth |
| 2. UniXNU | Passed on hardware | Menu, timer, side buttons, payload and firmware return work |
| 3. Payload and DarwinBoot | Passed on hardware | `ALIOTHESP`, DarwinBoot handoff and entry probe work |
| 4. XNU entry | Passed on hardware | The 4 KiB kernel reaches `Failed to mount root device` |
| 5. NativeUfs storage evidence | Passed on hardware | Probe 7 verified reusable `sid00e0` and `sid04e0`; probe 10 verified GPT, capacity, three raw windows, repeated-read equality and unchanged SMMU state |
| 6. Root and userdata | Current gate | GPT geometry is pinned in `board.toml`; build and audit `SHNGROOT`, preflight and write `userdata`, then require probe 11 and an owner-verified root mount |
| 7. Userland | Out of scope | No Linux userland or partition scheme is adopted |

UEFI, UniXNU, DarwinBoot and XNU entry are closed stages. Do not reopen them
merely because a Linux porting note mentions DTS identity, overlays or
`__symbols__`. Reopen those checks only after a new, reproducible early-boot
failure.

## Passed-Stage Evidence

- The original hardware exception had `ESR_EL1=0x96000006`,
  `FAR_EL1=0xa4dff000`, and a stack carrying `ShengBootMemoryDxe.pdb`.
  It was a DXE second-level translation fault while reading the bootloader DTB,
  not an XNU or AppleImage4 failure.
- `ShengBootMemoryDxe` now maps the DTB header page, validates the FDT, maps the
  remaining pages and uses a copied DTB. Hardware subsequently reached the
  Project Silicium logo and the UniXNU menu.
- The payload initrd is pinned as reserved RAM before general DRAM is published.
  Hardware mounted `ALIOTHESP`, reached DarwinBoot and produced the 4 KiB XNU
  entry bars.
- The accepted hardware terminal state is the XNU
  `Failed to mount root device` panic. Any earlier provider or platform panic
  blocks storage work.
- Probe 7 on alioth observed the Linux SM8250 stream pair. `sid00e0` and
  `sid04e0` each match one exact SMR with mask zero, use a distinct context
  (`cb3` and `cb2`), and each context has one reference. Their `CBAR` is
  `0x1f000` (`TYPE=1`, stage-2 bypass), `CBA2R=1` (VA64), and the context
  `SCTLR` is `0x9f00e0` with `M=0`. This is the safe bypass shape; both
  firmware mappings can be reused without takeover. The earlier `0x60` and
  `0x10` values were board hypotheses, not alioth stream facts.
- Probe 10 on alioth found exactly one `userdata` GPT entry with:
  `first=0x2a8000`, `last=0x3b037fa`, `lba_bytes=4096`, capacity
  `242053263360` bytes, type GUID `e6e7811b-0df5-9b41-a739-2aeef8da3335`
  and unique GUID `cbaca77f-dea7-c784-cd96-20bc64379313`.
- The first-block read returned `d1da67fa 521637a0`, `ext4magic=55f8`,
  `f2fsmagic=1127c75f`, `manifest status=1`, `windows=0`. The values are not
  a valid Android F2FS signature and are expected to be encrypted or otherwise
  transformed user data. `SHNGROOT` absence is not an error for an untouched
  Android partition; the raw hex probe must distinguish ciphertext from a bad
  read projection before any persistent overwrite.
- The hardware raw probe showed the first-block windows
  `0x000..0x07f`, `0x3f0..0x43f` and the second-block window
  `0x13f0..0x143f`, with `repeat status=0` and `match=1`. The device reported
  `last=61880319`, `bytes=4096`, `total=253461790720`.
- Probe 10 before, after and final snapshots preserved
  `sid00e0=800000e0/00000003` and `sid04e0=800004e0/00000002`,
  `gfsr=0`, and UFS `hcs=0x101f`; only the expected `rsr` transition was
  observed.

## Supplemental Material Decisions

| Item | Decision | Reason |
| --- | --- | --- |
| `https://github.com/yzddmr6/android-mainline-port` | Read-only reference; selective adoption | It is an MIT Agent Skill/experience library for Qualcomm Android to mainline Linux and Ubuntu. It has no alioth, SM8250, UniXNU, NativeUfs, DarwinBoot or XNU implementation. Do not install it as a global skill and do not apply its Linux boot path to this port |
| `https://github.com/torvalds/linux/blob/master/arch/arm64/boot/dts/qcom/sm8250.dtsi` and `https://github.com/UniXNU/NativeUfs/issues/1` | Adopt as stream-identity evidence only | Linux names UFS SMMU streams `0x0e0` and `0x4e0` for SM8250. The NativeUfs issue confirms stream IDs are board parameters. Neither source authorizes a Linux partition or userland path |
| `FFT-infra/NativeUfs-alioth` | Keep as the writable alioth fork | The upstream `UniXNU/NativeUfs` remote remains read-only. The core submodule pins the alioth commit through this fork so a clean clone can reproduce the tested driver |
| Seven-stage bring-up order | Adopt | It keeps acceptance local and evidence-driven |
| Official QEMU runner and assertions | Adopt | It provides repeatable menu, payload and DarwinBoot checks |
| `probe-darwinboot-xnu.py` real-entry fixture | Keep as an XNU regression gate | It observes the actual 4K MMU and kernel globals |
| Community fork progress notes | Read-only reference | Their configurations contain board-specific and destructive assumptions |
| `sheng-log` and `sheng-autoreset` | Reject | The log path writes the end of `userdata` |
| `boot-progress=1/2` | Defer | The current kernel has no `_boot_progress_*` symbols and this does not replace the storage gate |
| `sheng-build-tag` | Do not rely on it yet | It is documented but not implemented in the current source tree |
| `amfi_get_out_of_my_way=1` | Do not add | It changes security policy and is not the current blocker |
| `uefi-pipa` as the ADT data source | Correct the scope | It contains pipa UEFI integration; GIC and ADT values live in `UniXNU-pipa` |
| `cust` as a persistent payload volume | Defer; candidate only | The `nabu` documentation reads a payload from `cust`, but alioth has not confirmed its capacity, GPT identity, filesystem or UEFI enumeration. `fastboot flash cust` overwrites existing content, so it requires a backup, an Android dependency check and explicit write approval. It only relocates the payload and does not replace the NativeUfs/root-device gate |

From `android-mainline-port`, adopt only these engineering practices:

- Qualcomm ABL debugging discipline: separate bootloader transport, handoff and
  the next-stage observer.
- A/B slot risk control: bootloader commands are the only authority for slot
  state; GPT attribute bits are never edited by hand.
- UFS and partition forensics: identify partitions by GPT name and GUID, record
  geometry and verify the first block before proposing a write.
- Explicit validation criteria, rollback paths and artifact hashes.

Do not adopt its Linux-specific solutions: Linux root partitioning, ext4/tar
images, systemd, Ubuntu, simpledrm, `iommu.passthrough`, Linux userland or
`sdX`/Linux device-name based partition selection.

Other community repositories remain read-only:

- `UniXNU-uke` is useful for later stages, but it flashed `userdata`, uses
  unverified storage bindings and carries a large device patch stack.
- `UniXNU-guacamole` is a long-term userland reference.
- `uefi-pipa` is useful for firmware structure and boot image parameters. Its
  local checkout is dirty; use `git show HEAD:path` and never reset it.
- `UniXNU-qemu` is useful for QEMU fixture expectations.

## Engineering Discipline

1. State the single variable changed and the expected observable difference.
2. Capture the before and after controller/SMMU state for every hardware probe
   that can alter either one.
3. Require a next-stage screen, log or probe result as pass evidence.
4. Stop after two identical failures and write down the hypotheses already
   excluded.
5. Record the artifact hash, build inputs, device slot, boot command and result.
6. For persistent work, define the rollback command before writing anything.

## Active Sequence

### 1. Complete the NativeUfs Probe Implementation

1. Keep the driver's default stream at `0x60` for the existing sheng board.
2. Keep the stream configurable with `native-ufs-stream=<id>` and
   `native-ufs-stream2=<id>`. Alioth uses `native-ufs-stream=0xe0` and
   `native-ufs-stream2=0x4e0`; the two masks are zero and the SIDs are
   independent.
3. Match both `qcom,sm8550-ufshc` and the generic `qcom,ufshc` binding.
4. Probe 7 is a non-mutating SMMU survey. For both configured SIDs it prints
   the match count, SMR index/mask/raw value, S2CR raw/type/CBNDX, context
   reference count, CBAR, CBA2R and context `SCTLR`.
5. A reusable stream requires an exact unique match, `S2CR.TYPE=TRANSLATE`,
   one reference to its context, `CBAR.TYPE=1` (stage-1 translate with
   stage-2 bypass), `SCTLR.M=0`, and `CBA2R.VA64=1`. Other `SCTLR` bits are
   implementation state and do not make the bypass unsafe.
6. Alioth probe 7 passed this reuse criterion for both `0xe0` and `0x4e0`.
   The driver now validates both mappings before transport access and reuses
   them. A two-stream takeover remains a fallback for future boards; it is not
   needed for alioth and must not be inferred as complete from this path.
7. Probe 10 is a non-data-mutating probe. It may change UFS controller, SMMU
   and possible device attributes while establishing transport, so it must
   never be described as absolutely read-only.
8. Probe 10 must capture before and after:
   - UFS `cap`, `hcs`, `hce`, `UTRLBA`, `UTRLBAU`, doorbell and `RSR`;
   - SMMU `SMR`, `S2CR` and `sGFSR` for both configured streams.
9. Probe 10 must locate `userdata` by GPT partition name and GUID, never by
   `sdX` or a Linux block-device name. Record its `first_lba`, `last_lba`,
   `lba_bytes`, partition capacity, first-block magic and the F2FS magic
   candidate at byte 1024.
10. Treat `f2fsmagic` as observational evidence, not a pass condition. An
    untouched Android `userdata` is normally metadata-encrypted, so an
    unrecognized F2FS magic or missing `SHNGROOT` is not evidence of a bad
    read. Capture raw bytes from the first block at `0x000..0x07f` and
    `0x3f0..0x43f`, plus the second logical block at `0x13f0..0x143f`.
11. Re-read the first logical block and require it to match the first capture
    byte-for-byte before treating the raw projection as usable evidence.
12. The probe pass uses an explicit `NATIVE_UFS_PROBE_ONLY` compile boundary.
    It keeps the UFS transport, probe 7 and probe 10 paths, but omits
    `IOBlockStorageDevice`, VerifiedRoot, root-window publication and
    boot-progress dependencies. This is a dependency boundary for the probe
    image, not a replacement for the complete root storage driver.

### 2. Static and Host Validation on Orb

1. Run `git diff --check` and Python syntax checks.
2. Run the NativeUfs host tests, including the configurable-stream SMMU test
   and the RootLayout F2FS/GPT fixture.
3. Build the NativeUfs archive only after the host tests pass.
4. Rebuild payload and boot image on orb. The XNU kernel itself is built only on
   the Mac with the pinned Xcode `DEVELOPER_DIR`.
5. Record the resulting kernel, payload and boot image hashes in the execution
   record.

### 3. Hardware Probe 7

1. Boot with `fastboot boot`; do not flash a partition.
2. Require the next-stage screen and logs for both `sid00e0` and `sid04e0`.
   Each line must include the full SMR/S2CR/context state and the global
   `sGFSR`.
3. If either stream has no match, a nonzero mask, a conflicting mapping or a
   non-bypass context, stop and record the observed values. Do not run probe
   10 and do not guess another stream in the same run.
4. Both alioth streams are already safe and are reused. If either stops
   matching this shape, implement the atomic two-stream takeover described
   above before the next hardware attempt.
5. `fastboot OKAY` alone is not a pass. The UniXNU/XNU screen or probe output
   is the evidence.

### 4. Hardware Probe 10

1. Run only after probe 7 identifies the stream.
2. This probe does not issue a storage write and does not modify user data, but
   it is not absolutely read-only: it can alter controller, SMMU and device
   state.
3. Record the before, after and final UFS/SMMU snapshots for both `0xe0` and
   `0x4e0` from the probe output. The UFS queue transition is expected; the
   SMMU stream ownership and bypass context shape must remain unchanged.
4. Pass only when the GPT is valid, exactly one `userdata` entry is found, its
   type and unique GUID are recorded, and its geometry and capacity are
   internally consistent.
5. Require the two raw windows and the repeated first-block read to agree
   byte-for-byte. Record `f2fsmagic` and `manifest` but do not require either
   to have a recognized value on an untouched Android partition.
6. If the probe fails or the repeated reads disagree, do not write
   `[userdata]` and do not build or flash a
   userdata image.

### 5. Userdata Build and Flash Gate

1. Only after probe 10 passes, add the real `first_lba`, `last_lba` and
   `lba_bytes` to `board.toml`.
2. Review the generated manifest, window offsets, sizes, hashes and partition
   capacity before building.
3. Before any flash, re-read `current-slot`, unlock state,
   `partition-size:userdata`, `partition-size:boot_b` and the actual image size.
4. Keep `boot_a` unchanged. A/B changes, if required, use bootloader commands
   only.
5. The default hardware test remains `fastboot boot`; persistent writes require
   explicit approval and a separately recorded rollback plan.
6. A cold boot must reproduce the accepted result. Rollback must restore the
   Android slot and verify that Android still mounts `/data`.

## Validation Checklist

- [ ] `fastboot OKAY` is backed by a next-stage screen, log or probe output.
- [x] Probe 7 reports `sid00e0` and `sid04e0` with exact SMR/S2CR/context
      evidence and both existing mappings satisfy the reuse criterion.
- [x] Probe 10 reports before/after UFS and SMMU state for both streams.
- [x] GPT `userdata` is identified by name and GUID, not `sdX`.
- [x] `first_lba`, `last_lba`, `lba_bytes`, capacity and first-block magic are
      recorded.
- [x] The requested raw windows are captured and two first-block reads agree.
- [x] F2FS magic and manifest status are recorded as diagnostics, not treated
      as required signatures for untouched Android data.
- [x] `[userdata]` was added only after real GPT evidence was available.
- [ ] No write to `userdata`, `metadata`, `super`, GPT or `boot_a` occurred.
- [ ] Every artifact has SHA-256, inputs and a verification result.
- [x] The next hardware command is recorded before it is run:
      `fastboot -s 9c6a8eb6 boot out/alioth/boot/alioth-probe10-readcap.img`.
- [ ] UEFI, UniXNU, DarwinBoot and XNU entry are not reopened without a new
      reproducible early-boot failure.

## Execution Record

| Date | Stage | Artifact / input | SHA-256 or evidence | Result |
| --- | --- | --- | --- | --- |
| 2026-10-22 | UEFI bring-up | `SILICIUM_UEFI.fd` | `12d8ffdede0a662ed124f2070515e4ca7e9e338a757b0a092fcef3d6857441b2` | Hardware display, menu and firmware return passed |
| 2026-10-22 | Full payload | `alioth-full-reserved.img` | `bdb193bab5bcd3fb858b321d22d5fb722735e039533551033e173f79f63bd479` | DarwinBoot and 4 KiB XNU root-device panic passed |
| 2026-10-22 | Kernel | `out/alioth/kernel/kernel.macho` | `4650771a0f9d9e904de234f169b04bd80f01b4c7e92b016ef1ba3fd51e9d0d6a` | Probe-only NativeUfs linked; `IOBlockStorageDevice`, VerifiedRoot and boot-progress unresolved symbols absent |
| 2026-10-22 | Probe payload | `out/alioth/payload/payload.img` | `9c419687c5b4226c6fc6128e07b9609c88f696410ce60b21dfac35649380f78a` | Payload kernel after panic-screen patch: `bfbf3aa6dc8f5adf3b8e93befb0769acecf126f61850016617b76a642f64b3aa` |
| 2026-10-22 | Probe 7 boot image | `out/alioth/boot/alioth-probe7.img` | `78a6c670b366d355f5b9b7f0ae7c8cb06fbf082fa082b83a1b2178a7b5ced2bb` | `187068416` bytes; `fastboot boot` only |
| 2026-10-22 | Probe 7 | NativeUfs dual-stream SMMU survey | `sid00e0` and `sid04e0` reuse safe bypass contexts | Persistent writes remain blocked |
| 2026-10-23 | Probe 10 | NativeUfs GPT and first-block survey | Real `userdata` geometry and GUIDs recorded; F2FS magic mismatch is non-fatal on encrypted Android data | Raw-window and repeated-read evidence passed in the final hardware run below |
| 2026-10-22 | NativeUfs host validation | `RootLayout`, `RootWindowRead`, `RootDiagnostics`, `RootPublication`, `SmmuTakeover` | All host tests pass on orb | Archive is ready to link; no hardware acceptance yet |
| 2026-10-23 | Kernel rebuild | `out/alioth/kernel/kernel.macho` | `b30958f2f130687d58867ed84c54c0ac13501b408400b3a179d82be7597f4973` | Apple clang ARM64/iOS 17 Mach-O; entry `0xfffffff0073f0488`; NativeUfs compile log has no warnings or errors |
| 2026-10-23 | Kernel record | `out/alioth/kernel/kernel.json` | `7f42cdc225238a7b6efabf53b60976d0ea38e740e7b955887d259f5d62072351` | Native candidate equals the rebuilt kernel; prelink runtime qualification remains false as expected |
| 2026-10-23 | NativeUfs archive | `out/alioth/tree/build/native-ufs/libNativeUfs.a` | `fe1a6efc05da0477e82741e12db47f4e0e90878ac9ae1cc8ddfb52910a9a97b6` | Apple clang `-Werror` archive; probe-only build contains no root-publication dependency |
| 2026-10-23 | Probe payload | `out/alioth/payload/payload.img` | `50ebca73a26e528659f461498ccd29b5716d0fecc825daca78821717542c6940` | Payload kernel `06f68dee1c2347b652e4022f4c499b47bc0af62b41e26b0f362ad09f0ba387a9`; ADT `89646ee2232c740df73442229e981fe8ef798b5c30baed3111ca59daaa3ead1f` |
| 2026-10-23 | Preserved pre-readcap payload | `out/alioth/payload/payload-20261023-pre-readcap.img` | `736e906f8f19cf2644488dfad05cf578cb3924967c2afe4baeba29287d454ffc` | Previous payload retained before the default output was replaced |
| 2026-10-23 | Probe 10 read-capacity boot image | `out/alioth/boot/alioth-probe10-readcap.img` | `5212ee6d89081fb4422968887d53df8c246742c07e71042327368beb6a7923eb` | `187068416` bytes; boot manifest `cb70e2b12e66cbd0ea0af0554e15b45447924a2c06276205142a95d39e108f51` |
| 2026-10-23 | Regression tests | Host root tests, QEMU UFS commands, ARM64 TCG | Host `PASS` 4/4; QEMU `0x40010000` and `0x8cb5e9000` pass; ARM64 TCG 5/5 | Exercised current source after the warning-only edit |
| 2026-10-23 | Hardware Probe 10 readback | `alioth-probe10-readcap.img` | GPT `0x2a8000..0x3b037fa`; three raw windows; repeat `match=1`; both SMMU streams unchanged | Passed on hardware |

## Deferred Work

- Owner-root, verified root and Data
- `boot-progress`
- Legacy display, Adreno and userland
