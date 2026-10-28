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
