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
GPU address. The mount that fails with NoSpace is a later client: four windows
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

## The screenshot channel

The USB console's `SCREENSHOT`/`SHOTNEXT` pair answers with a ready frame
(1080x2400, RLE) and then loses the device with ENXIO about a second into the
transfer. The same happens on an image without the plugin, so it is
independent of the graphics stack. Until it is fixed, the LIVE and FAULTS
rows are the only machine-readable evidence, and the picture itself needs a
person or a different read-back path.

## Open

`cb0 fsr 00000400` on the GPU's context bank, next to a successful write and a
clean CP. The FSR was read after the write completed, so either a benign
earlier access left it set or a speculative access was reported. Recorded, not
explained; it does not block the next mode.

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
