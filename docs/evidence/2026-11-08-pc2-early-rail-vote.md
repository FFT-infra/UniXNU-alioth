# pc2: the three-rail vote completes before the controller, but step 11 still has no first access

## Round

- Image: `tmp/boot-pc2.img`, sha256 `1d3b01eb75d27ba1915b28307660f86728864ccef91c803ab10dc4df9a015f78`.
- Kernel: `d49e57d555b2e2ef85bc4d3eee43093574db08dca402baee7f2ddd227d9801d4`, 125,861,888 bytes.
- Build: `TAG=pc2 scripts/build-round.sh --kernel-arg pcie=11 --kernel-arg pp=1 --kernel-arg pt=1`; `LINK_OK`.
- Source revision for this image: `1ba611e` (`wifi: vote the controller rails early and persist the last PCIe step`).
- Console: `tmp/console-pc22308321.txt`, sha256
  `15037f41e8759450cc39b92616f8210b0301f5d1ae293283f43fb6bf1450583a`.
- One round: `sh tmp/rounds-count2.sh tmp/boot-pc2.img pc2 1 300`; `RESULT t=290 own=0 sw=0 data=2 frame=0 askers=0 -> no-desktop`.

## What the vote did

`step_gdsc` now calls the gated early vote before it writes the controller's power switch. The
seven commands were:

```
0x41b00 = 1200   ldoa9 voltage
0x41b04 = 1      ldoa9 enable
0x41b08 = 7      ldoa9 mode
0x41800 = 880    ldoa5 voltage
0x41804 = 1      ldoa5 enable
0x41808 = 7      ldoa5 mode
0x30000 = 256    cx.lvl level (NOM)
```

The command-database lookup resolved `cx.lvl` to `0x30000`; the values match
`kona-pcie.dtsi:47-53` and the vendor `rpmh-regulator` map. The RSC DRV 2 config read
`0x800c8104` (8 TCS, 16 commands each), TCS 0 was idle (`status 0x1`), and the vote completed
after one poll: `command status 0x10101, irq 0x1`.

This corrects the earlier `rail2` evidence. `0x10101` has `CMD_STATUS_ISSUED` (bit 8) and
`CMD_STATUS_COMPL` (bit 16) set; the vendor ISR judges those two bits (`rpmh-rsc.c:283-290`).
The old `DID NOT COMPLETE` line used the TCS IRQ status, not the command status, and must not be
read as an RPMh rejection. The old round's three ldoa5 values were accepted; what was missing was
the completion criterion, not necessarily the vote.

## What it did not do

The link still did not train:

- `PCIE 10: parf parf_ltssm after the endpoint enable ... = 0`.
- `PCIE 11: write the vendor's DBI training words...` is the last PCIe line.
- There is no `PCIE 11: about to read dbi...`, no `THE LINK IS UP`, and no `link after` row.
- `BOOTLOG beat 2 phase 7` is the last writer beat; no later beat appears, so the persistent
  writer was still inside its sync phase when the PCIe thread reached step 11.
- At `t=290s`: `SUPER ... own=0 sw=0/0/0`, `PROC a=0 p=0 s=0 b=0`, `CORES ... deaf=9e`.

The missing first DBI line does not prove the DBI read hung. This image's `line_persist()` waited
for the writer **before** its USB write, and the wait used `delay_for_interval()`; with the writer
stuck at `phase 7` and a deaf-core boot, the wait itself can be where the thread stops. The next
image (`pc3`, commit `822f7a4`) writes the USB line first and spins on `cntpct_el0` for the
persistent wait, so that distinction is measured rather than assumed.

## Boundary

The early vote is a verified hardware action and a completed RPMh request. It is not a link
result, not a Wi-Fi result, and not a promotion of the network completion level.
