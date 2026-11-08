# pc3: the first DBI read stops, and the missing pipe clock explains why

## Round

- Image: `tmp/boot-pc3.img`, sha256 `b4e5a5b9ba3fc84dd38cc9170e05f4078709bd74ae91f6cbe22dc2dfcab5d5c0`.
- Kernel: `5c42598055a97533dcec31cbdb64ed296851df1a3f0d50bfed15ce38c0efb8f0`, 125,861,888 bytes.
- Build: `TAG=pc3 scripts/build-round.sh --kernel-arg pcie=11 --kernel-arg pp=1 --kernel-arg pt=1`; `LINK_OK`.
- Source revision: `822f7a4` (`wifi: print the last PCIe step before waiting for the log`).
- Console: `tmp/console-pc32319141.txt`, sha256
  `297b34478337ad0826ec7c21a7d0b65d4f00741587e811c252e096e5a880cec9`.
- One round: `sh tmp/rounds-count2.sh tmp/boot-pc3.img pc3 1 300`;
  `RESULT t=130 own=0 sw=0 data=2 frame=0 askers=0 -> no-desktop`.

## The first DBI read

The new console-first path made the missing access visible:

```
PCIE 11: write the vendor's DBI training words, set PARF_LTSSM bit 8, and poll ELBI, DLLLA and the endpoint ID for up to 100 ms
PCIE 11: about to read dbi dbi gen3_gen2_ctrl before at 0xfffffff5f83e080c
PCIE 11: the persistent log did not complete two rounds within 6 s
```

There is no `dbi ... = ...` row, no `THE LINK IS UP`, and no ELBI/DLLLA/ID row. The `about to
read` line was printed before `rd()`; the read did not return. This is the first access that
pc1 could not distinguish from a lost log line. The persistent writer still stopped at
`BOOTLOG beat 2 phase 7`, which is why the USB-first line mattered.

The same boot had `[addon] 1=0/-1 2=0/-1 3=0/-1`, `own=0`, `deaf=9e`, and `PROC s=0`. A normal
desktop round (`wl1600071`) has `[addon] 1=5/0 2=5/0 3=5/0`, `WALLPAPER stage=5 status=0`, and
`SUPER own=1`. The probe is what changes the boot outcome: `pcie=` is absent on the desktop
rounds, and `alioth_pcie_init()` returns before touching the controller when it is absent.

The `DATAMOUNT` row shows the chain:

```
good wl1600071: DATAMOUNT stage=4 status=0 ro=0 store=2
pc3:            DATAMOUNT stage=0 status=-1 ro=-1 store=0
```

`native_owner_data_prepare()` never reached stage 4, so `native_gpu_addon_prepare()` was never
called and all three addon mounts stayed at `0/-1`. The addon and wallpaper failures are
downstream of the boot not reaching userland; they are not a separate wallpaper/widget bug in
this round. The first DBI read is the event that precedes that stall.

The TWRP reader (`read-sheng-logs/pc3-0000000025421ca8`) confirms the persistent side:
`slots_written=2`, `uptime_s=2`, `rounds=2`. Its `msgbuf.txt` has the early vote at
`pcie: 2` but no `PCIE 11` and no `about to read dbi`; the writer never reached the step 11
round. That is why the USB-first console line is the reading, not the UFS slot.

## Source cause: the pipe clock bit was not voted

Local `CLOCK_VOTES` was `0x2f` (bits 0,1,2,3,5). The external same-board successful sequence
uses `0x3f` and says why:

> bit 4 ... is the PHY's pipe clock coming back through the controller -- and with it out the
> core never answered a single register: reads of the DBI, the address translator and the link
> block sat there until the machine was reset, while every read of the wrapper's own PARF block
> came back.

That is exactly the pc3 shape: PARF reads at steps 8/9 returned; the first DBI read at step 11
did not. The vendor also enables the pipe clock in `msm_pcie_pipe_clk_init()`
(`pci-msm.c:3603-3642`) before it polls PHY ready, and the DT's `qcom,ep-latency = <10>`
(`kona-pcie.dtsi:103`) is the delay before PERST release (`pci-msm.c:4571-4583`).

## Fix built but not yet run

- `CLOCK_VOTES` changed from `0x2f` to `0x3f`.
- `step_endpoint()` now waits the DT's 10 ms endpoint latency before releasing PERST.
- Source revisions `73a0080` (`wifi: vote the pipe clock and wait the endpoint latency`) and `c93f12f` (`wifi: align the PARF writes with the same-board sequence`).
- Image `tmp/boot-pc4.img`, sha256
  `900494a914f16b1d76072fb5bb5dd82161a1692f8486d7de9bede63e657c7db7`; no board run yet.

No link, no Wi-Fi and no desktop outcome is claimed for pc4 until its one round returns.
