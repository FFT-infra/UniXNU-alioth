# GX power and the GPU's first command on alioth

2026-10-29, alioth (SM8250-AC, Adreno 650). Modes 8 and 9 of the GPU probe,
images usb191 and usb192.

## Mode 8 — GX power through the GMU

Mode 7 ends with a complete GMU cold start, so mode 8 adds exactly one new
write path: `A650GmuPowerGx` sends the GX_BW_PERF_VOTE for the requested GPU
level (7 = 670 MHz, the top one, where Linux starts) over HFI, then the OOB
GPU_SET request, and reads GX back.

```
GPUROW0  GPU m8 gcc=ok cc=ok smmu=ok gpu=skip io=0 cx1 clocks1 ahb0
GPUROW17 GX on perf 7 vote 0 oob 2 gx 000000c0>00000000 gdscr f8222800 rbbm 00000001 gbif halt 0/3 rbbm halt 0/0
```

| Field | Value | Meaning |
| --- | --- | --- |
| `vote 0` | HFI success | The firmware accepted the vote; no protocol error. |
| `oob 2` | 2 polls | The OOB GPU_SET request was acknowledged on the second 100 us poll. |
| `gx 000000c0>00000000` | GX off to on | `SPTPRAC_PWR_CLK_STATUS` lost every `GX_OFF_BITS` bit. |
| `gdscr f8222800` | bit 31 set | The GX GDSC reports power on after the vote. |
| `rbbm 00000001` | non-zero | The GPU register window answers. |

`GPUROW13/14/15` were identical to mode 7, so the cold start was not disturbed.

## Mode 9 — the GPU's own context and its first command

```
GPUROW18 GPUCTX step 7 status 0 kit 0/0 routes 4 program 5/1/1 ttbr0 0 smr 00000000 s2cr 00000000 actlr 3 pool 768
GPUROW19 CP ok step 7 sqe 31628/0 ring 0 init 0 fence 0/1 write 0 marker a6500c0d | hwcg 0>8aa8aa82 sptp 0>0
GPUROW20 CPSTATE rbbm 00000001 rptr 1b wptr 1b hwfault 0 cpint 0 int0 00100000 cb0 fsr 00000400 far 00005e09e55794f5
```

- `GPUCTX status 0`, `routes 4`: bank 0 / ASID 2 took the four streams of the
  DT's `iommus <0 0x401>` (0, 1, 0x400, 0x401) on SMR slots 0, 1, 4 and 5. The
  slot normalisation that fixed the GMU attach is applied here first, on the
  GPU's own slot list.
- `CP ok`, `init 0`, `ring 0`, `fence 0/1`: `CP_ME_INIT` completed and its
  fence advanced on the first poll.
- `marker a6500c0d`: the first PM4 command the GPU executed wrote `0xA6500C0D`
  through `CP_MEM_WRITE` into the scratch page, and the CPU read it back.
- `rptr 1b wptr 1b`: both ring pointers advanced to the same value.
- `hwfault 0`, `cpint 0`: no CP hardware fault.
- `hwcg 0>8aa8aa82`: the clock-gating table was written and reads back.

## Mode 10 — the zap shader, the secure-mode exit and the GPU interrupt

The first attempt (usb193) stopped at `ZAP stopped step -6`. The vendor
`a650_zap.mdt` is packed as `[ELF header 148 B][hash 6712 B]`, but its own
program headers address the hash at `0x1000` and the `a650_zap.b02` segment at
`0x3000`. Concatenating the two files into 8536 bytes put `hashOff + hashBytes`
at `0x2a38`, past the end of the image, and `zapLoad` stops when that happens.

The embedded image is now 13964 bytes in the addressed layout (header at 0,
hash at `0x1000`, segment at `0x3000`); each range was compared byte for byte
with the vendor files.

```
GPUROW21 ZAP ok step 7 init 0/0 mem 0/0 auth 0/0 resumes 0 ms 7/0/0 phdrs 3 reloc 1 span 1000 secure 0 meta 13ed13000
GPUROW22 IRQ attach 0 readback a002 router 0 count 1>2 nop fence 1 seen 00100000 status now 00000000 masked 0
```

- `init 0/0`, `mem 0/0`, `auth 0/0`: `qcom_scm_pas_init_image` (7 ms),
  `pas_mem_setup` and `pas_auth_and_reset` all returned success, so TrustZone
  accepted this zap image for PAS 13.
- `secure 0`: the `CP_SET_SECURE_MODE(0)` packet's fence completed.
- `attach 0`, `readback a002`: SPI 300 attached as level high through ArmCpu.
- `count 1>2`, `nop fence 1`: a `CP_NOP` fence raised the interrupt.
- `seen 00100000`, `masked 0`: the fence status bit arrived and no storm built.

The board inputs were confirmed against the K40 vendor tree
(`reference/k40-kernel/arch/arm64/boot/dts/vendor/qcom/`): `qcom,pas-id = <13>`,
`qcom,firmware-name = "a650_zap"`, `pil_gpu_region@8681a000` (size 0x2000,
no-map) and `interrupts = <0 300 IRQ_TYPE_LEVEL_HIGH>`.

## Mode 11 — the published device

```
GPUROW23 PUBLISH ok submit 0 timer 0 publish 0 | uc 36/36 first 0 at 0 cpumap 0 | rounds 4 4
```

`submit 0` and `timer 0` are the attach results, `publish 0` is
`native_adreno_publish`, and `uc 36/36` means every step of two kernel-side
user-client rounds passed: open, two BOs, an IB of three CP_MEM_WRITEs, the
fence and close. No process outside the kernel had opened the service.

## Mode 12 and the Metal staging

The original Metal discovery starts at `IOAcceleratorES`, so the published
device alone is not enough: something has to register a service that names
the plugin. The staged accelerator does that. It compiles from the A650
checkout into the same `libAdreno740.a` the kernel links, with three
`IOAcceleratorES` imports bound to the audited addresses
(`0xfffffff004bd3b20` constructor alias, `0xfffffff004bd3b64` destructor,
`0xfffffff006aeb688` metaclass), its `__mod_init_func` registration removed
and its initializer globalised so the probe runs it after `IOGPUFamily`.

Turnip, the plugin bundle and the owner trust module are staged by
`build-turnip-darwin.py`, `build-adreno-metal-plugin.py` and
`add-gpu-plugin-data.py --publish`; the staged seed is the payload's
`--data-ramdisk`.

```
GPUROW24 METAL ok: accelerator registered (IOGPUFamily loaded, class size 136), Turnip options nocb
GPUROW25 LIVE irq9 ... | sub 7/7 t2 p0 | uc 4/4 | pool 128/128/768 ... | busy 0
```

The live row is the proof that the path is alive: it moved from
`sub 2/2 | uc 2/2` (the probe's own rounds) to `sub 7/7 | uc 4/4` with the
128 MiB pool taken, so two processes outside the kernel opened the GPU
service and completed five more submissions.

## Who opens the GPU service, and why it leaves

Rows 25 and 28 count clients but never named them, so the driver's own records
were printed instead (`GPUOPEN`, `GPUCCLI`, `GPUCLIENT`, `GPUMOUNT`, usb201):

```
GPUOPEN  backboardd type 1    macf 1 res e00002e2 attach 1 foreign 1
GPUOPEN  backboardd type 4147 macf 1 res e00002e2 attach 1 foreign 0
GPUOPEN  backboardd type 4148 macf 1 res e00002e2 attach 1 foreign 1
GPUCCLI  backboardd opens 2 refused 0 closes 2 slot 0 peak 650 last 0
GPUMOUNT fail 1/e00002db slot 4 count 2
```

`0xe00002e2` is `kIOReturnNotPermitted` (the sandbox gate's answer) and
`0xe00002db` is `kIOReturnNoSpace`, both read from this tree's IOReturn.h. So
backboardd does reach the accelerator (type 1) and the plugin types (0x4147,
0x4148), the grant admits it anyway, it takes slot 0 and maps up to 650 MiB of
GPU address. A later correction: every user process shows `refused 0`, so the two
NoSpace entries come from the probe's own kernel-client rounds colliding (the
kernel mounts to slot 0 or, holding one already, to the sentinel 4), not from
four processes competing. Original reading follows: four windows
and four processes that each want one (the render server, PosterBoard,
iconservicesagent, WidgetRenderer).

Then backboardd dies:

```
EXITCRASH pid=54 name=backboardd sig10 PC106da915c LR106dbbd24 E82000007 bt 106cf852c 106d60780 ...
```

`0x82000007` is a data abort that translates as a level-3 translation fault,
so a userspace pointer in backboardd is bad; the PC and the backtrace are the
same on every respawn, SpringBoard, PosterBoard and WidgetRenderer are
terminated with it, and ReportCrash dies the same way, which is why no crash
report is written.

## The context-bank FSR is not a fault

`cb0 fsr 400` looked like a translation fault. The vendor kernel's
`arm-smmu-regs.h` settles it: `FSR_TF` is bit 1, and the fault mask is
`0xc00001fe` (bits 31, 30, 8, 7, 6, 5, 4, 3, 2, 1). `0x400` is bit 10, outside
that mask, which is exactly why the kit's own fault test never fired. Bit 10 is
either implementation-defined on this part or the readback is not the register
it is taken for; it stays recorded and unexplained, and it is not the reason
the desktop does not draw.

The base arithmetic checks out with the values already on the row: `GPUROW3`
reports `id 4c017e06 20000005 00005511`, so IDR1 = 0x20000005, bit 31 clear
(4 KiB pages) and bits 30:28 = 2, giving context banks at 0x1000 << 3 = 0x8000
and CB0_FSR at 0x8058, which is the standard SMMU-500 placement. The read is
of the register it claims to be.

## A/B: with and without the accelerator

The plan's control is "remove the publish file and the desktop falls back to
software rendering". The APFS staging that would rebuild that seed needs the
host's disk-image tools, which wedged, so the nearest available control is the
same staged seed on an image that does not register the accelerator (mode 11
against mode 12):

| Image | Accelerator | backboardd | SpringBoard |
| --- | --- | --- | --- |
| usb196, mode 11 | none | no crash in 170 s | 4 crashes, SIGTRAP at 0x1ae4406b4 (ESR 0xf2000000, a BRK) |
| usb201, mode 12 | registered | crash loop, 39 crashes in 120 s | terminated with backboardd each time |

So the SIGBUS in backboardd appears only once the plugin is loaded, and the
SIGTRAP in SpringBoard is a different failure that is there without it. The
plugin is the first suspect for backboardd; nothing yet explains the trap.

A third image asked Turnip for system memory everywhere (`pipa-tu=6`). In its
170 s window backboardd opened the accelerator (`type 1`) but never the plugin
types, and it did not crash; six SpringBoard traps were all that was left. That
is avoidance rather than a cure: the desktop still does not draw, and the
plugin is not exercised at all.

The plan's control is the same experiment done properly: usb204 registers the
accelerator but carries the original Data seed, so no plugin bundle exists.

| Image | Accelerator | Plugin | backboardd |
| --- | --- | --- | --- |
| usb204 | registered | absent | **no crash in 170 s**, opens only type 1 |
| usb201 | registered | present | 39 crashes in 120 s, opens type 1, 0x4147 and 0x4148 |

With the plugin absent the whole crash loop disappears and the desktop stays
on the software renderer, which is what the plan's control predicts. The
plugin is therefore the cause of backboardd's SIGBUS, not the accelerator, the
driver or the SMMU.

## What the plugin does before it dies

The driver counts every ABI call it serves. In usb205, with the plugin in the
seed:

```
GPUMETH m 2  calls 50 fail 0      BoAlloc
GPUMETH m 3  calls 50 fail 0      BoMap
GPUMETH m 13 calls 48 fail 0      BoCpuMap
GPUMETH m 5  calls 7  fail 0      Submit
GPUMETH m 6  calls 29 fail 2      WaitFence, two timeouts
GPUMETH m 8  calls 8  fail 8 last e00002c7   GetStatus, always kIOReturnUnsupported
GPUSLOT s 0 live 32 peak 32 refused 0 hi 128
GPUMOUNT fail 1/e00002db slot 4 count 2      a later client: kIOReturnNoSpace
```

So the plugin is not failing at the connection: it allocates 50 buffers, maps
all 50 to GPU addresses, CPU-maps 48, submits seven times and waits on fences.
Slot 0 (backboardd) peaks at 32 live objects and 128 MiB of its window, with
no refused allocation. `GetStatus` is unsupported and always was; the plugin
calls it eight times and carries on.

Then it reads through an address that has no mapping: `ESR 0x82000007` is a
data abort from a lower exception level whose fault status decodes as a level-3
translation fault and whose WnR bit is clear, so it is a **read**. The PC moves
between boots (0x106da915c, 0x105ff3460) but is fixed within one, which is what
a fixed code site under ASLR looks like.

That matches the note the driver itself carries next to the BO counters:
Turnip turns a refusal into `VK_ERROR_OUT_OF_DEVICE_MEMORY` and its command
streams do not check it, "int-237: backboardd wrote through a NULL stream
pointer". Here the plugin's reads are what fault, and the two buffers that were
allocated but never CPU-mapped are the first place to look.

## No kernel counter sees the frames, but the GPU is doing the work

With the fixed diagnostics (u215) the client rows print every round, and
GPUSWAP answers the display question:

```
GPUSWAP validate 0 ok 0 reason 0 | copied 0 status -1 bytes 0 | retired 0 gen 0
```

Not one swap was ever *asked for* — so the phase-4 swap path is not refused,
it is never called. The same boot shows the GPU working:

```
GPUROW25 LIVE irq159 ... | sub 157/157 t3370 | uc 4/3 | pool 131/131/768 | busy 42
```

157 submissions, 159 interrupts and 42 per mille of GMU busy while the UI is
up A later sample of the same boot shows the rate rather than a total: sub 266, 339, 412 on consecutive reports with GMU busy between 18 and 60 per mille, about 2 to 4 submissions a second with the desktop idle. That means the desktop is not being drawn by the CPU; it is drawn through the
GPU path straight into the buffer the display already scans out, which is why
the legacy swap API never sees a frame. That also means no kernel counter on
this board measures the panel: presenter counters need phase 3, the legacy
swap counters need a caller that does not exist, and the direct-scanout flips
need sheng-dpu-scanout=2.

## Every swap is refused once the apps have a device

Giving SpringBoard, PosterBoard and the rest a native device (the addon's
`NativeAdrenoMetal.clients` list) changed one thing immediately: the compositor
started asking to swap.

```
GPUSWAP validate 311 ok 0 reason 4 | copied 0 status -1 bytes 0 | retired 0 gen 0
```

311 requests, none accepted, reason 4. In NativeLegacyDisplay.cpp reason 4 is
the surface rule set: the surface's format and plane count must pass
`nativeBgra()` — `0x42475241` with no planes or `0x26424741` with at most one —
and its geometry must pass `native_legacy_surface_bounds()` against the
display. Without a device no swap is even asked for; with one, every swap is
refused, which is exactly the black screen that stays after the boot logo.

GPUREJ now prints the refused surface's width, height, format, planes, stride,
allocation and descriptor length, so the failing field is named rather than
guessed. Hypothesis, not yet measured: the surface the compositor produces
once it has a GPU device is not one of the two formats the legacy driver
accepts.

## The frames reach the panel

The refusal was a unit mismatch. GPUREJ named it:

```
GPUREJ surface 1080x2400 fmt 26424741 planes 1 stride 1088 alloc 10649600 bytes 10649600
       | stride4 4352 need4 4320 alloc/height 4437
```

The IOSurface field the display reads holds 1088 for a 1080-wide window whose
allocation is four bytes a pixel a row, so `native_legacy_surface_bounds`
compared 1088 with `width*4 = 4320` and refused every swap. The board now
binds the original getter as `native_legacy_surface_stride_raw` and wraps it:
the field is taken as pixels when the allocation and the row count prove it,
and a getter that returns bytes is left alone.

With that, the same boot reads

```
GPUSWAP validate 22 ok 1 reason 5 | copied 22 status 0 bytes 10368000 | retired 22 gen 22
```

22 copies of 1080x2400x4 bytes, all retired: the GPU renders into the
compositor's surface and the display copies it to the panel. A minute later
the counters are unchanged because the screen is static and nothing asks for a
new frame, which is what an idle desktop does.

## Reporting the GPU families stops the trap and stops the surfaces

Two runs, one argument apart.

| `pipa-metal-withhold` | SpringBoard | swaps |
| --- | --- | --- |
| `apple-families` (default) | traps, SIGTRAP at the same PC, up to nine a minute | 74 real surfaces, desktop on the panel |
| `none` | **no crash in 200 s** | 952 requests, **all reason 1** (no surface in the request), none accepted |

A third boot with the same `apple-families` default as the first produced
1270 requests that all carry no surface and no crash at all, so the argument is
not what decides it: the two outcomes alternate between boots of the same
image. The desktop appearing is therefore a race, and the first thing to
establish is what the two outcomes correlate with, not which argument was set.

## offline-legacy-validate is the swap gate, and removing it cost a round

`NativeLegacyDisplay.cpp:555` guards every swap validation with
`enabled==1 && logicalPM && primary && bootMapping && request`, and `enabled`
comes only from `offline-legacy-validate=1`; without it the reason never leaves
its initial 1, so every swap is refused before the surface is even looked at.
That argument was removed once as "unused" for command-line room, which is why
one round went from 74 real surfaces to 1270 requests that carried none. The
grep then covered only the board package; the parse lives in the driver
component. Restoring it brings `r0` back, so the "boot-to-boot race" of the
previous rounds was this argument, not timing.

## How much of the panel is lit

GPUFB also reports the share of sampled pixels that are not black and their
mean luminance, which is the only way to judge the picture on this board
without a person at the panel:

```
boot, 4170cc9c  nonblack 492/1000  luma 67     (setup or lock screen)
settled, 0b2ad67c nonblack 148/1000 luma 29    (home screen)
settled, df10c016 nonblack 88/1000  luma 21
```

The settled desktop is between nine and fifteen percent lit: real surfaces are
being accepted (`r0` grows) but almost nothing is drawn on them, and a photo
wallpaper would put this near ninety percent. So "the wallpaper is not shown"
is now a number, not an impression.

## The wallpaper overlay has no effect on what is drawn

Same board, same clients list, one difference: the GPU addon carries the
wallpaper overlay or it does not.

| Image | `WALLPAPER` | settled frame | lit |
| --- | --- | --- | --- |
| with the overlay | `stage=5 status=0` | `df10c016` / `0b2ad67c` | 88 / 148 per mille |
| without it | `stage=1 status=2` | `df10c016` / `0b2ad67c` | 88 / 148 per mille |

The framebuffer checksums are **identical** in both rounds, so the mounted
overlay changes nothing about what reaches the panel. The wallpaper question is
therefore not about the assets, the collection or the mount; nothing renders a
wallpaper at all on this image.

## The screen update rate depends on what the desktop is doing

The framebuffer is sampled once a second and the report says how many of the
last interval's seconds moved:

```
GPUFB checksum e9d71100 changes 12 (+12 since the last report)
GPUFB checksum 2245e244 changes 47 (+35 since the last report)
GPUFB checksum a70b9bc8 changes 80 (+33 since the last report)
```

35 and 33 changes per fifteen-second report is a little over two a second while
the UI is starting up; on a settled desktop the same row reads +3, a fifth of a
secondly change. The number is a screen-activity measure, not a frame rate, and
it must be quoted with the interval it was taken over.

## The framebuffer changes whether or not a swap carries a surface

A checksum over every 4096th dword of the mapped framebuffer (GPUFB) settles
what no swap counter can:

```
GPUFB checksum 33a6969c width 1080 height 2400 bytes 10368000
GPUFB checksum 6599802e ...
GPUFB checksum dc1c2bfb ...
```

In a boot where every swap was refused for carrying no surface, the panel
memory still changed between reports, so something writes pixels by a route
the legacy swap API does not see. That is why "no accepted surfaces" and "the
desktop appeared" can both be true in the same minute.

## The desktop draws

Splitting the accepted swaps by validation reason shows what the frames
really are (u223):

```
GPUREASONS accepted 92 rejected 0 | r0 74 r1 0 r2 0 r3 0 r4 0 r5 18 r6 0 r7 0
GPUSWAP validate 92 ok 1 reason 0 | copied 92 status 0 bytes 10368000 | retired 92 gen 92
```

Reason 0 is a real surface and reason 5 is the blank-screen fill, so 74 of the
92 frames carry the compositor's own content, none were refused, and the
picture on the panel is the desktop. Two things had to be right for that: the
stride unit, and the clients list that gives the UI processes a Metal device.

## Earlier in the same day: the UI behind the frames trapped

With the stride fixed, frames keep reaching the panel: `copied`/`retired` read
38, 46 and 50 on consecutive reports, about eight frames in thirty seconds,
which is what an almost static screen asks for. The picture is black with one
grey and one white bar, and the same window shows SpringBoard dying six times:

```
EXITCRASH SpringBoard sig5 (SIGTRAP) at the same PC each time
```

That trap is not the GPU's: it happens on the image without a Metal device as
well, so the black screen is the UI being torn down and rebuilt over a black
background, not a frame that failed to arrive. Metal, the driver and the
display path are each demonstrably working now; what is left is why
SpringBoard traps.

## Baseline before any component moves

u232, one minute of a settled desktop. This is the reference every component
move is judged against:

```
GPUROW0    GPU m12 gcc=ok cc=ok smmu=ok gpu=skip io=0 cx1 clocks1 ahb0
GPUREASONS accepted 8 rejected 0 | r0 8 r1 0 r2 0 r3 0 r4 0 r5 0 r6 0 r7 0
GPUFB      checksum 7095d6c3 changes 77 (+34) / f1c876e4 changes 87 (+3)
GPUMETAL   backboardd, SpringBoard, WidgetRenderer_Defa, mediaserverd each read 1
GPUMOUNT   fail 1/e00002db slot 4 count 2   (the kernel client's second mount)
```

Eight real surfaces, no blank fills, no refusals: the compositor is sending
content and none of it is being turned away.

## The upstream parity branches already answer three of these

`feat/ipados27-gpu-parity` exists in all three components and is ahead of the
pins this port carries:

- **Adreno740**: the fixed four-client design is gone. There is no
  `kClientWindows` table and no "no free slot" any more; a client that cannot
  be given address space is refused instead, clients survive a GPU restart, and
  a stalled SMMU fault is ended before restart. That is the slot and stall
  picture this port spent rounds on.
- **GopFramebuffer**: the render server on iPadOS 27 picks a **64-bit RGBA half
  float** surface (`0x52476841`, 'RGhA', one plane) once a Metal device renders,
  and the component converts its rows to the scanout's BGRA, either on the CPU
  or by offering it to the GPU client (`sheng-fb-half`). It also keeps each
  swapped surface's client mapping.
- **AdrenoMetal**: Metal semantics for waits, imports and heaps, Core Image
  pipelines and naming the cause at the tail of a refusal.

None of this is in the pinned commits. Moving the pin is not a one-line change
though: the parity GopFramebuffer builds through `build-arm-driver.py
NativeLegacyDisplay`, while this board's `build-gop-legacy-display.py` compiles
the component directly from a recorded compile line, so the pin was put back and
the migration of that recipe is the real next task.

The recipe now supplies the six swap-request offsets the parity component reads
(`kRequestSurfaces` 2624, `kRequestMasks` 792, `kRequestRect0` 17,
`kRequestRect1` 81, `kRequestOptions` 804, `kRequestDims` 816) in the generated
ABI header, which is what the pin switch was missing. With them the build
reaches the component and then **clang-24 crashes in the preprocessor**
(`Preprocessor::EnterMainSourceFile`, no diagnostic), so the pin stays where it
is. The recipe writes its argv to `build/native-legacy-display/compile-command.json`,
so the crash reproduces from that file. `-fsyntax-only` crashes the same way,
which puts it in parsing and semantics rather than in the preprocessor, and
removing the enum this board adds does not change it, so the six offsets are
not the cause. That assertion (`Cannot get layout of invalid decl`) hides a real compile error:
a declaration was already invalid when its layout was asked for. The errors
behind it name the whole contract the parity component expects from the
generated ABI header, and only the first two are supplied so far:

| Input | State |
| --- | --- |
| `kRequestSurfaces`, `kRequestMasks`, `kRequestRect0/1`, `kRequestOptions`, `kRequestDims` | supplied |
| `NATIVE_LEGACY_LEAVES(LEAF)`, over the guarded leaves (not 213 and 337) | supplied |
| `OriginalLegacySwapRequest` | **missing** |
| `OriginalLegacyStateRequest` | **missing** |
| `kLegacyVsyncClients` (the +5592 the recipe already audits) | **missing** |

So the migration is a matter of growing the generated header to the component's
whole request ABI, not of switching a pin.

The method that produced that list generalises: grep the target component for
identifiers it uses and never defines, and each one is an input the device
recipe has to generate. For this component the two supplied so far are the
request offsets (literals from the previous component's source) and
`NATIVE_LEGACY_LEAVES`, which must list the abstract slots and nothing else
(213 and 337 are declared differently, and including them turns every leaf into
an `override` of a non-virtual slot). Expect the same shape of gap when
Adreno740 and AdrenoMetal are moved.

What the two missing structures need, from the parity source:

- `OriginalLegacySwapRequest` is only ever used through a pointer, and its
  contents are read as bytes at the offsets listed above (the highest is 816/820,
  so at least 824 bytes). An opaque byte array of that size satisfies the
  component; a field-based definition would be decoration.
- `OriginalLegacyStateRequest` is built by value as `{1,0,0,1,1,{}}` and handed
  to the original class's state setter, so its layout is the *original* one and
  cannot be invented: five scalars and one aggregate, with the meaning the
  original virtual expects. That one needs the original request layout, and
  getting it wrong would fail silently rather than loudly.

`kLegacyVsyncClients` is 5592; the audit JSON does not carry it, but the recipe
already asserts the same 5592 when it validates the vsync sender.

With those supplied the parity source compiles down to one error:
`OriginalLegacyStateRequest` is undefined at line 333. That is the last input,
and it is the one that cannot be a blob: the object is built by value and handed
to the original class's state setter, so its field layout has to be the original
one.

One more thing was fixed on the way: the recipe wrote the generated header with
a plain `write_text` and compiled immediately, and on this shared mount clang
sometimes read the file while that write was still in flight, which surfaces as
clang's `MemoryBuffer` assertion rather than as a compile error. The header is
now written through a flushed, `fsync`ed, closed stream, and the crash stopped
reproducing. The stride wrapper is not part
of that question: the parity `NativeLegacyRequest.h` is unchanged and still
compares the stride as bytes, so the board-side normalisation stays either way.

Dropping the board-side stride wrapper is a step of its own, not a side effect
of the migration, because two changes at once cannot be attributed:

1. migrate the recipe and keep the wrapper: `GPUREJ` must not appear and `r0`
   must keep growing, which says the wrapper is harmless on the new component;
2. then remove the wrapper alone: `r0` must still grow, which says the new
   component handles the format itself.

If `r0` falls at either step, that step is reverted.

## The screenshot channel

The USB console's `SCREENSHOT`/`SHOTNEXT` pair answers with a ready frame
(1080x2400, RLE) and then loses the device with ENXIO about a second into the
transfer. The same happens on an image without the plugin, so it is
independent of the graphics stack. Until it is fixed, the LIVE and FAULTS
rows are the only machine-readable evidence, and the picture itself needs a
person or a different read-back path.

## Open

**The SpringBoard trap is an environment assertion, not a graphics one.** Its
addresses land in IMDPersistence with frames in WebCore and TextInputCore, and
healthappd and ReportCrash trap in Espresso and DocumentManager the same way.
The fix belongs on the environment side (the storage or service those
frameworks expect, or holding them back through a job override); it is not on
the GPU main line and the desktop draws between the traps.


`cb0 fsr 00000400` on the GPU's context bank, next to a successful write and a
clean CP. The FSR was read after the write completed, so either a benign
earlier access left it set or a speculative access was reported. Recorded, not
explained; it does not block the next mode.

## The traps resolve into the frameworks, and none of them is graphics

The SystemOS volume in `out/ipsw/21H461/044-76034-038.dmg` carries no UDIF
trailer, and its first `NXSB` sits at **offset 32**, not 0; `apfs-fuse` on the
build host mounts it as it is (`apfs-fuse -o ro <dmg> <mount>`) without
`hdiutil`, which DiskArbitration wedges on this machine. Quote the offset if
this is ever scripted: a check of `head -c 4` for `NXSB` fails on this file.
With the cache mounted, `scripts/find-cache-image.py <cache-dir> <pc>…` names them (offsets are image-relative):

| Address | Image |
| --- | --- |
| `0x1ae4406b4` (SpringBoard PC) | `PrivateFrameworks/IMDPersistence` +0x586b4 |
| `0x1ae442310` (its first frame) | `IMDPersistence` +0x586b4 (its first frame, image-relative) |
| `0x1d9483e20` | `DataDeliveryServices` |
| `0x1997e2cc0`, `0x1997b40e4` | `WebCore` |
| `0x1d9445fec` | `TextInputCore` |
| `0x1967a92f0` (healthappd) | `Espresso` +0x6212f0 |
| `0x1e1adf2a0` (ReportCrash) | `DocumentManager` +0x3e2a0 |

SpringBoard's trap is in the Messages persistence layer, and its backtrace runs
through WebCore and TextInputCore — the frameworks a home screen loads for
widgets, keyboard and notification previews. Nothing in the graphics stack
appears in any of them.

## The traps are a pattern, not one process

A minute of u232 with the desktop drawing (`r0 10`, no refusals) shows eight
`EXITCRASH` lines for **healthappd**, all at the same PC with `sig5` and
`ESR 0xf2000001`, and **no SpringBoard crash in that window**. Earlier windows
trapped SpringBoard and even ReportCrash the same way. Different binaries,
different fixed PCs, one signal: a BRK in a shared framework, which reads as an
assertion about something the environment lacks rather than a GPU fault.

That matters for the plan: SpringBoard's trap is not a separate disease from
the daemons', and the desktop does draw while daemons trap around it. What is
still unproven is whether SpringBoard's own trap is on the same assertion.

## Two counters are not one path

Six numbers in this port were read as something other than what they measure.
The rule that came out of it: when two counters describe the same phenomenon,
prove they count the same path before using one to confirm the other.

| Read as | Actually |
| --- | --- |
| `fence 0/1` = fences completed | wait status 0, one poll |
| `write 0` = the value written | the fence wait's status |
| `t` = failed submissions | timed-out polls; every submission completed |
| `copied` rising = the desktop is presented | includes the blank-screen fill (reason 5) |
| `mount_fail` = a process refused | the kernel client's own second mount |
| `GPUFB` changing = frames presented | the diagnostic page paints the framebuffer too |

The last one has a corollary for instruments: the page-off boot loop did not
sample the framebuffer, so with the page off GPUFB would freeze, and a frozen
instrument reads exactly like a screen that stopped changing. The sample call
now sits in both loops.

## Where the GPU rows appear

The GPU rows go to the serial console only. The persistent log read back
through TWRP carries no `GPUROW` line at all (0 in `slots.txt` for both usb188
and usb191), so a round has to capture `/dev/tty.usbmodem14101`. The console's
CDC ACM node appears a few seconds after the kernel binds USB; a capture that
opens it before that has to retry.

## Reading the board back after a round

The unattended script's last stage sets slot b active and reboots the reader.
On this board the reader comes back into TWRP instead of the bootloader, so
that stage reports a failure. `adb reboot bootloader` from TWRP reaches a
fastboot that accepts `fastboot boot`, which is all the next round needs.

## The original state request, read out of the original body

The parity component builds `OriginalLegacyStateRequest` by value and hands it
to the original class's display state setter, so the layout had to come from the
original code rather than be declared. The setter is vtable slot 354, and the
slot's entry in the prelinked candidate equals `methods[354]` in the audit:
`0xfffffff0057970cc`. Its body reads, in order:

| Instruction | Meaning |
| --- | --- |
| `ldr w8,[request]` then `cset ne` | a word at 0 that suppresses the first store |
| `str w8,[self,#0x16cc]` | the word at 4, written to display +5836 |
| `ldr w8,[request,#8]` then `cset ne` | a word at 8 that suppresses the second store |
| `ldr w9,[self,#0x16d0]`, `cmp`, `cset eq` | the old value is compared before writing |
| `str w8,[self,#0x16d0]` | the word at 12, written to display +5840 |
| `ldrb w8,[request,#0x10]`, `eor w8,w8,#1` | the byte at 16, read as a boolean |

Four words, that byte and eleven bytes of padding: 28 bytes, which is what the
component's `{1,0,0,1,1,{}}` initialises. (`ldr`/`str` with an unsigned offset
scale by four, so `0x6cc` in the encoding is +5836 and `0x6d0` is +5840.)

The recipe now emits that struct and names slot 354 `applyStateRequest`, which
binds it to the original address through the same mechanism as `validateSwap`
and the other qualified slots; the component defines nothing.

## Two ways a changed recipe silently fails to reach the build

Both cost a round here, and both read as a compiler problem rather than as a
stale input.

1. **The generated header is read while it is being written.** The recipe wrote
   `OriginalLegacyFramebufferABI.h` with `write_text` and compiled in the next
   statement. clang caches a length and then aborts in `MemoryBuffer::init`
   ("Buffer is not null terminated"). `flush` is not enough on a shared mount;
   the write now goes through one `write_synced` helper that flushes, `fsync`s
   and closes before the compiler opens the file. Three consecutive builds
   passed on the first attempt afterwards.

2. **`unixnu` copies the tree's scripts by size and mtime, and the VM's
   virtiofs answers stat from before the edit.** Two builds compiled the old
   recipe while the new one sat in the device repository: the Mac reported
   30065 bytes and mtime 1793245792, the VM reported 29203 and 1793245546, and
   `md5sum` on the VM read the *new* content. Content was right, metadata was
   stale, so `_copy_scripts` skipped the copy. The build wrapper now runs
   `tmp/sync-tree-scripts.py` first, which compares content and deletes a stale
   copy so the next step must recopy it.

A third symptom belongs to the same family: a component source that `git
checkout` has just replaced can crash clang the same way once, because the
length the VM cached belongs to the previous revision. A second run clears it.

## What the four inputs were enough for

With the six request offsets, `NATIVE_LEGACY_LEAVES`, the vsync client offset
and the two request structures supplied — and nothing else — the parity
component builds under `-Werror` and passes its object audit:

```
NativeLegacyDisplay: -Werror build and actual ARM64/iOS 17 object audit passed
```

That is the positive answer to the migration question the earlier section could
only pose. The remaining inputs were never a matter of judgement: each is an
offset or a size the original code fixes, and the recipe now derives or asserts
each one.

One size in that set is not from the original. `OriginalLegacyStateRequest` is
28 bytes because 17 bytes of read fields round up to the struct's alignment;
the original body never indexes past 16. The assert pins that choice against a
later edit. It is not evidence about the original ABI and should not be cited
as such.

## The parity component on the board (u237)

GopFramebuffer at `90b64ef` with the board supplying the state request boots to
the same GPU ladder as before — `GPUROW21` ZAP ok, `GPUROW23` PUBLISH ok,
`GPUROW24` METAL ok, `GPUROW25` busy 0 — and the swap path is healthy:

```
GPUSWAP validate 7 ok 1 reason 0 | copied 7 status 0 bytes 10368000 | retired 7 gen 7
GPUREASONS accepted 7 rejected 0 | r0 7 r1 0 r2 0 r3 0 r4 0 r5 0 r6 0 r7 0
```

`ok` is the last validation result, not a count; `bytes` is one full
1080x2400 frame per swap, so every accepted swap was copied and retired.
No `GPUREJ`, no reason-1 refusals, `r5` (the blank fill) 0.

That is step 1 of the two-step migration check: the board-side stride wrapper
is harmless under the parity component, so removing it is now a separate,
attributable experiment rather than a change mixed into the pin move.

The picture is not comparable yet, and an earlier version of this section said
it was. Laid out by sample order, both components do the same thing:

| Round | Component | `nonblack` / `luma` per sample |
| --- | --- | --- |
| u235 | 33e941a | 494/67 → 436/58 |
| u237 | 90b64ef | 492/67 → 429/57 → 428/57 → 434/58 |
| u235b | 33e941a | 88/21 → 148/29 (the log ends here) |
| u237, later sample | 90b64ef | 436/59 → 90/22 → 86/21 → 86/21 |

Both brighten to 430-490 permille during boot and then fall to the 90-148
band, so "bright then dark" is common to both and is not something the pin
introduced. The 86 against 148 comparison is not a difference between the
components either: u235b's 148 is its *last* sample, with no evidence that it
would not have fallen further, while u237 has two consecutive 86/21 rows.

What the drop does line up with is the Data volume. On u237 it happens around
300 s of uptime, and the u238 read-back puts the first `disk1` checksum
failures at about 334 s. A volume that stops answering is a candidate
explanation for both, and it is still unresolved.

One round-keeping detail: the round ended `rc=1` at its 200 s budget because
`sheng-autoreset=3600` returns the board to the bootloader long after that.
The console capture is complete and the rows above come from a later
45-second sample on the same boot, so nothing was lost — but an unattended
round needs the autoreset at or below its own budget.

## What the TWRP read-back has that the console does not, and vice versa

The two channels carry different rows and neither replaces the other:

- The console (`/dev/tty.usbmodem14101`) carries every instrument row —
  `GPUROW*`, `GPUFB`, `GPUSWAP`, `GPUREASONS`. The persistent log carries none
  of them.
- The persistent log carries timestamps and the kernel's own messages, which
  the console shows only while it is attached, and it survives the run. Its
  `latest.txt` also carries the recorder's own health line:
  `UFS requests=328563 verified4K=1154616 failures=0 integrity=0` and, on this
  boot, `LIVE: DEADLINE EXCEEDED ... lost=66719`.

u238's read-back (`build/sheng-logs/u238-00000000d34ec5ed`) was the first one
in many rounds, and it was first because round stage 4 waits for the bootloader
inside a fixed budget: with `sheng-autoreset=3600` the board never came back
inside that budget, the round failed there, and stages 5 and 6 — which is where
the reader is booted and the log is read — never ran. The autoreset has to fire
inside the round's budget (u238 sets 150 s); otherwise the most complete
evidence channel is silently skipped.

## The Data volume reports checksum corruption, and the resets are a candidate

The u238 window holds 314 `nx_corruption_detected_int` lines on `disk1`
(the Data volume), all at uptime ~334 s, followed by
`IOBlockStorageDriver ... request failed to start!`, `disk1: device is not
ready` and APFS pageouts failing with error 16:

```
[  334.822618]: nx_corruption_detected_int:37: disk1 Container corruption detected by obj_checksum_verify:6629!
[  334.822621]: obj_read:6207: disk1s1 oid 0x446 ... paddr 0x56df error verifying checksum
[  334.823423]: disk1: device is not ready.
```

The older usb191 log has zero such lines. Two readings are open, and the
evidence does not yet choose between them:

1. The corruption is caused by the reset. Every round ends by resetting the
   board on a timer while APFS may have writes in flight, and the next boot
   finds the container inconsistent. The usb191 rounds that show no corruption
   also ran without that timer.
2. The corruption is independent and older than the window, and the 334 s
   lines are simply the first ones this boot kept.

The test that separates them is a boot with the reset disabled: if a long run
shows no `corruption detected`, the timer is the cause.

This matters beyond storage. On the u237 boot the panel fell from
`nonblack 436/1000` to `86/1000` at about the same uptime, and the Data volume
is where the desktop's assets live. A volume that stops answering is a
sufficient explanation for a desktop that loses its content, and it would mean
the mostly-black screen is not a GPU question at all.

## The Data volume, not the GPU, decides whether a boot is measurable

u239 differs from u237 in one boot argument (`sheng-autoreset` 3600 → 600), and
that argument cannot affect a boot before 600 s. It still produced a completely
different boot:

| Round | Data volume first `device is not ready` | `corruption detected` | GPU rows on the console |
| --- | --- | --- | --- |
| u237 | none before the desktop | 314 (in the window that reached 334 s) | 471 |
| u239 | **136 s** | 0 | **0** |

On u239 the volume reported not ready at 136 s and never recovered: 854 such
lines by 600 s, and the console has no `GPU` line at all. The display probe
never ran, so there was nothing to measure — not a GPU failure, an earlier
one.

Two things follow.

1. **Every frame-rate or brightness reading is conditional on that volume.**
   A round that lands on a boot like u239 measures nothing, and a round like
   u237 measures a desktop that loses its assets partway through. Before the
   panel numbers mean anything, the boot has to be one where `disk1` stays
   ready.

2. **The corruption lines and the not-ready lines are not the same event.**
   u238's window had both at ~334 s; u239 has not-ready alone from 136 s with
   zero corruption. So corruption is not the cause of the stall — at most it is
   one of its consequences. The earlier section named the timed reset as a
   candidate for the corruption; nothing here supports that as the cause of the
   stall.

The next useful measurement is not on the panel at all: it is whether the
block device goes not-ready because the UFS link drops, or because the driver
stops being asked. `UFS requests=328563 ... failures=0 integrity=0` on u238
says the driver's own counters saw no failure, which points away from the link
and towards the stack above it.

## The same image does not boot the same way twice

Five boots of one image (u240, nothing changed between them) capture the
variation that made single rounds unreadable:

| Boot | Console | Highest `GPUROW` | `GPUFB` samples | Swaps |
| --- | --- | --- | --- | --- |
| 1 | none | — | — | — |
| 2 | 45 KB | 20 (`CPSTATE`) | 0 | none |
| 3 | none | — | — | — |
| 4 | 79 KB | 28 (`FAULTS`) | 4 | 52 accepted, 0 rejected, `r0 5` |
| 5 | none | — | — | — |

Two things matter here.

1. **One boot in five reached the desktop, and one more reached the GPU
   ladder and stopped at `CPSTATE`.** `GPUROW20` is the last row before the
   zap: the CP start completed and the probe never got to `GPUROW21` (zap),
   `GPUROW22` (interrupt) or `GPUROW23` (publish). That is a second stall
   point, earlier than the display, and it is new in this table.
2. **Three boots produced no console at all**, which is not yet a measurement:
   the capture waits a fixed 120 s for the CDC ACM node and gives up silently,
   so a boot whose USB binds late is recorded as an empty log. The three are
   being re-run with the wait raised to 300 s and with the capture's own stderr
   kept.

Until the empty-log case is separated from the dead-boot case, no rate can be
quoted. What can be said is that a single round proves nothing about the
component: u237 and u239 differ by one argument that cannot act inside either
window, and they landed on different outcomes.

## The Data volume was never able to take a write (u241)

The `disk1: device is not ready` cascade is not the hardware failing. The 3 GiB
seed is a *big* volume for the ram-block driver, and that driver has no volatile
mode for one:

```
NativeRamBlock.cpp:86   if (mode && (sparse.pages || isBig) && &native_ufs_store_open && ...)
NativeRamBlock.cpp:111  direct = mode == 2;
NativeRamBlock.cpp:261  if(isBig && !direct) { ... reads from the seed, writes wait ... }
```

`mode` is `sheng-data-persist`, and the board never set it, so the store never
opened: APFS could read through the seed and could not complete a write. The
kernel said so itself — `apfs_vnop_pageout ... returned 16`, then
`disk1: device is not ready` for the rest of the boot, on some boots from 136 s.

With `sheng-data-persist=2` (mode 2, the store-backed direct path) the u241 boot
has none of it:

| Round | `device is not ready` | APFS `returned 16` | `corruption detected` |
| --- | --- | --- | --- |
| u239 | 854 | present | 0 |
| u241 | **0** | **0** | **0** |

and the boot carried on to CoreAnalytics/CoreParsec at 280 s, well past both
earlier failure points.

That boot did not submit a single swap (`GPUSWAP validate 0`, `r0 0`) while the
display probe ran normally (21 `GPUROW25` rows) and the panel sat at
`nonblack 428-432 / luma 57-58`. So the storage fault and the "compositor does
not submit" state are separate: fixing the first did not fix the second, and
neither did it make the picture worse.

An earlier section named the timed reset as a candidate cause of the checksum
corruption. That is now the less likely reading: the corruption was a
consequence of writing into a disk that never accepted the write, not of the
reset.
