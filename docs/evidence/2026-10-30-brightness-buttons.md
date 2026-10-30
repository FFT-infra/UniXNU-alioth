# 2026-10-30 brightness and buttons: what is established, what is not

No device round produced a reading for either subsystem. The board was in fastboot
at 18:1x, accepted `fastboot boot tmp/boot-bl1.img` at 19:00 (sha256
`3887b7948da65f07…`), left the bootloader after 11 s, and returned at 93 s — 87 s
before `sheng-autoreset=180` could have fired, so the kernel stopped rather than the
timer resetting it. The persistent log read back afterwards is the *previous* boot:
tag `u266`, uptime 178 s, `slots_written 90`, and zero occurrences of `alioth-bl` or
`sheng-build-tag=bl1` in the message buffer or the slots. A second attempt with the
corrected image (sha256 `9c05cf41…`) never returned to any of the three states.

So: nothing below is device-verified. Everything is derived from this board's own
devicetree and the vendor kernel at `reference/k40-kernel`, then checked by two host
tests that compile with a plain C compiler.

## Panel brightness

**The interface is DCS over DSI, not an I2C controller.** The phone's own tree selects
`qcom,mdss_dsi_k11a_38_08_0a_dsc_cmd` (stock bootargs, `alioth.dts:39468`) and that
panel node declares `bl_ctrl_dcs`, `bl-min-level 1`, `bl-max-level 0x7ff`,
`brightness-init-level 0x218`, `mi,mdss-dsi-bl-dcs-big-endian-type` (`alioth.dts:27485-27489`).
There is no KTZ8866 node in this tree at all; the only WLED node
(`qcom,pm8150l-spmi-wled`, `alioth.dts:5477`) is `status = "disabled"`, as are
`pm8150a_amoled` and `pm8150l_lcdb` (`alioth-sm8250.dtsi:242-244`). sheng's
`AppleARMBacklight.cpp` is a two-part KTZ8866 I2C design and cannot be ported as-is.

**The transport** is DSI0's TPG command FIFO, written word by word, no DMA buffer and
so no display SMMU mapping — the path `dsi_ctrl_hw_cmn_kickoff_fifo_command`
(`techpack/display/msm/dsi/dsi_ctrl_hw_cmn.c:703`) takes. The controller is
`qcom,dsi-ctrl-hw-v2.4` (`alioth.dts:30915`), which `dsi_catalog.c:108-140` maps onto
the 2.2/2.3/2.4 path, the one that has that function. The panel is command mode, so
the transfer is not scheduled for a vertical-front-porch line.

**Packet words** (header bytes in order, so the data type is in bits 23:16):
`0xC0390003` for the DCS 0x51 long write, payload `0x51 | hi<<8 | lo<<16 | 0xff<<24`,
big endian per the panel's own property. The same word is what the device-verified
asteroids port sends for the same message.

**Host test found three errors in the first draft**, all now fixed:
`package/tests/backlight_host.c` asserts the packet words and level arithmetic and
passes.

## Buttons

Power and volume-down are the PM8150 PON's `RT_STS` bits 0 and 1
(`qpnp-power-on.c:51,113-114`; `alioth.dts:4193-4218`). Volume-up is PM8150 GPIO 6,
`<&pm8150_gpios 6 GPIO_ACTIVE_LOW>` (`alioth.dts:39116-39128`), active low, level in
`RT_STS` bit 0 (`pinctrl-spmi-gpio.c:38-39`).

**PPIDs are `0x008` and `0x0c6`**, from `ppid = sid << 8 | (peripheral >> 8)`
(`spmi-pmic-arb.c:977,996`). GPIO 6's own SPMI interrupt names peripheral `0xc6`
(`alioth.dts:4261`). The host test caught that my first draft used sheng's `0x18c`,
which is a SM8550 peripheral page and would have asked this phone's arbiter for a
peripheral it does not have.

**The arbiter is this board's, not sheng's**: `qcom,spmi@c440000` with
core/chnls/obsrvr/intr/cnfg = `0xc440000 / 0xc600000 / 0xe600000 / 0xe700000 / 0xc40a000`
(`alioth.dts:4172-4176`), and the 32 MiB `chnls` window is the **version-5** layout
(channels at `0x10000 * apid`, `ACC_ENABLE/IRQ_STATUS/IRQ_CLEAR` at `+0x100/+0x104/+0x108`).
The driver reads the version at run time and refuses one it does not implement.

The PDC hop matches sheng: `interrupts-extended = <&pdc 1 4>` (`alioth.dts:4177`),
pin 1, and `qcom,pdc-ranges = <0 0x1e0 0x5e …>` (`alioth.dts:3694`) maps pin 1 to
GIC SPI 481, attached as 32 + 481.

`package/tests/buttons_host.c` asserts the address arithmetic and the key-to-report
mapping (report 2 bits `0x2/0x4/0x8`, inside `NativeHidInput`'s `mask & 0xe`) and
passes.

## What round 1 exists to settle

Both drivers have a read-only first mode for exactly this, because these are the
things no amount of source reading settles:

| Question | Where the answer lands |
| --- | --- |
| Does the DSI controller accept the FIFO packets? | `native_backlight_stats[10]` (commands sent), `[11]` (timed out), `[12]` (last command µs) |
| Does the panel answer DCS 0x52? | `native_backlight_stats[3]`, and `native_backlight_request[5]` (read-back count) |
| Is the panel's brightness really the DCS register? | `[3]` returning a value in `1..0x7ff` |
| Which arbiter version is this phone? | `native_buttons_stats[7]` (PDC version), `[4]/[5]` (the two APIDs) |
| Are the PPIDs right? | `[4]` and `[5]` non-negative |

The one thing that could not be checked at all is where in `RDBK_DATA0..3` the two
read-back bytes land; `bl_decode_brightness` is written from `get_cmd_read_data`
(`dsi_ctrl_hw_cmn.c:828-860`) and is the first thing a DCS 0x52 read would confirm.

## Boot arguments

`alioth-bl=<1|2|3>` and `alioth-bt=<1|2|3>`. Mode 1 is read-only, mode 2 is the
visible/write test, mode 3 starts the service (brightness) or the interrupt path
(buttons). The names are shortened from `alioth-backlight`/`alioth-buttons` because
the command line was 5 bytes over the 1000-byte limit with the full pair; board.toml
already carries 965 bytes of arguments.

Only one of the two fits on the command line at a time, so a round carries one.

## 2026-10-31 btn1: the first round that produced readings

Image `tmp/boot-btn1.img`, sha256 `c9e031b879b9051278e25fe3948d3eed11167a4b8c5d33c74e08fdc3a924d8b6`,
kernel `5cb276025b32f3baad4f1e4cbfc1f06967f8b1364f893985e9370733662beeaa`, tag `btn1`,
carrying `alioth-bl=1` and `alioth-bt=1`. The board left the bootloader, ran, and returned to
fastboot on its own at about 200 s - not the 93 s early stop the earlier attempt showed, so
that stop did not reproduce.

The log came back through TWRP as `build/sheng-logs/btn1-0000000040d23402`: uptime 4 s,
3 slots, `write_errors 0`, `state 2`, `tag btn1`. An early snapshot, but the diagnostic rows
are complete.

```
B st=-4 irq=0 rep=0 mask=0 apid=-1/26 off=1 pdc=20000 err=-2 spur=512
L st=2 req=0 lvl=0 a=-1 b=0 xerr=1 irq=0 ea=1f5 eb=5 err=0 goal=0 fade=0
L path runs=0 fades=0 irq=1/1 mode=87396 tmo=537133056 done=335544320
```

### Buttons: the PPID was right, the ownership register was wrong

`off=1` says PPID `0x008` matched exactly one APID map entry, so the PON is present and the
`findApid` filters threw it away. The filters read the ownership table at `cnfg + 4 * apid`;
the vendor reads it at `cnfg + 0x700 + 4 * apid` (`SPMI_OWNERSHIP_TABLE_REG`,
`spmi-pmic-arb.c:50`). Those are different registers, and the value at the wrong one almost
never equals this EE.

`spur=512` confirms the scan bound now comes from the core window (512 entries from the v5
map offset 0x900 in a 0x1100-byte window), not from `core[0x04]`, which is `PMIC_ARB_CONFIG`
and read as 26 in u278.

### Brightness: the trigger source was never set

`irq=1/1` is `stats[10]=1` sent and `stats[11]=1` timed out, with `a=-1` and no read-back. At
boot `ea=1f5` already had `CMD_MODE_EN` (bit 2) set and `eb=5` already had
`CMD_MODE_MDP_BUSY` (bit 2) set, so the command engine was on and its MDP side was busy.

`TRIG_CTRL` (0x084) was never written. The device-verified asteroids port writes
`(rd(kTrigCtrl) & ~((1u << 16) | 0xfu)) | 0x4u` on every packet - DMA_TRG_MUX 0 in bits 3:0
and DMA_TRIGGER_SEL 4 (software) in bits 19:16. Without a trigger source the software trigger
has nothing to start.

`mode=87396` is `stats[12]`, the last command's time in microseconds: 87 ms, which is the
50 ms deadline plus the polling step. `done=335544320` is `stats[15]`, a `COMMAND_MODE_DMA_CTRL`
snapshot, not a count - BootLog's wording for that field is sheng's and does not describe
this board.

### The backlight read-only mode now reads

Mode 1 returned before any DCS read in the merged code, so the earlier `ea`/`eb` values were
`DSI_CTRL` and `DSI_STATUS`, never a brightness reading. btn1 makes mode 1 send the
`SET_MAXIMUM_RETURN_PACKET_SIZE` and DCS 0x52 packets, which is why this round's `L st=2` and
`a=-1` describe an actual attempt rather than a register snapshot.

## 2026-10-31 btn2: the buttons work; brightness still gets no answer

Image `tmp/boot-btn2.img`, sha256 `20b2cf30585a09dc4d504f0a8d9c97729202c92eb668aca06aadddb1e4e2cb90`,
tag `btn2`. Full round this time: `uptime 178 s`, `rounds 90`, `slots 90/509`,
`write_errors 0`, `state 2`. Log at `build/sheng-logs/btn2-000000014652207f`.

```
B st=3 irq=0 rep=1 mask=4 apid=13/26 off=65536 pdc=20000 err=131072 spur=512
L st=2 req=0 lvl=0 a=-1 b=0 xerr=1 irq=0 ea=1f5 eb=0 err=4 goal=0 fade=0
L path runs=0 fades=0 irq=1/1 mode=85949 tmo=537133056 done=335544320
```

### Buttons: armed, and a press reached the HID device

`st=3` is the armed stage, so `setup()` ran to the end. `apid=13/26` is the PON at APID 13
(it was -1 in btn1) and the GPIO at 26. `rep=1 mask=4` is one report sent carrying report 2
bit 2, volume up - the volume-up key was pressed during the round and the HID device received
it. `off=65536` is `0x10000`: the `findApid` sample for the PON, IRQ-owner bit 1 and owner
EE 0, which is why it was accepted. `err=131072` is `0x20000`: the same sample for the GPIO,
whose owner EE reads 2 - the GPIO entry is not owned by this EE, and `armPeripheral` for it
therefore may not have taken.

The fix was the ownership table's offset: `cnfg + 0x700 + 4 * apid`
(`SPMI_OWNERSHIP_TABLE_REG`, `spmi-pmic-arb.c:50`), not `cnfg + 4 * apid`.

### Brightness: the trigger was right, the done could not be seen

`irq=1/1` again - one packet sent, one timed out - but `eb=0` now, so `DSI_STATUS` was clean
and the command-mode MDP was not busy. `err=4` is the TRIG_CTRL snapshot, exactly what the
write set (bits 3:0 = 4), so the trigger source was correct.

The remaining fault is the DMA-done status: `INT_CTRL` carries a status bit per event and an
**unmask** bit one above it. `dsi_ctrl_hw_cmn.c:975-976` sets `BIT(1)` to enable
`DSI_CMD_MODE_DMA_DONE`, while `clear_interrupt_status` writes `BIT(0)`. The driver only ever
wrote bit 0, so it cleared a status that was never allowed to set.

### What this does not establish

The GPIO entry's owner EE is 2, not 0, so volume-up's interrupt was probably not armed even
though its APID was found. The `mask=4` report came from the volume-up key, which suggests the
press was read through the thread's level poll rather than the interrupt. Whether volume-down
and power produce reports, and whether the interrupt path works at all, is still open.

## 2026-10-31 btn3: unmasking the DMA-done status hangs the boot

Image `tmp/boot-btn3.img`, sha256 `a86c32c8dc156a7a0d55232bfc0dddc5dbc2048426ad078d139b156d178f2699`,
tag `btn3`. It booted, the kernel came up (`Qualcomm XNU Debug` enumerated, a fresh
`/dev/tty.usbmodem14101` at 01:05), and the panel kept the firmware frame - but the board
**never returned to the bootloader**, where btn1 and btn2 both returned at about 200 s. The
board had to be power-cycled by hand; no log was read back.

### The cause

btn3's only functional change was setting `INT_CTRL` bit 1 as well as bit 0. `INT_CTRL` pairs
a status bit with an unmask bit above it, and `dsi_ctrl_hw_cmn.c:975-976` sets `BIT(1)` when
enabling `DSI_CMD_MODE_DMA_DONE`, while `clear_interrupt_status` writes `BIT(0)`
(`dsi_ctrl_hw_cmn.c:929`). Setting only bit 0, as btn2 did, cleared a status that was never
allowed to set.

Setting bit 1 is what hung it. DSI0's interrupt is routed - `interrupt-parent = <0x24d>`,
`interrupts = <0x04 0x00>` (`alioth.dts:30921-30922`) - and nothing in this kernel attaches a
handler to that SPI. A status that re-arms with no handler holds the CPU, and the in-kernel
`autoreset` thread stops running, which is exactly the observed state: the kernel alive, the
screen frozen, the timed reset never firing.

### What this says about the transfer itself

The vendor only takes the TPG FIFO path in `secure_mode`: `dsi_message_setup_tx_mode` sets
`DSI_CTRL_CMD_FIFO_STORE` there and nowhere else (`dsi_ctrl.c:1170-1180`), while the normal
path writes a command packet into a DMA buffer. Its completion is reported through an interrupt
this kernel does not attach. So the done poll in this driver may never succeed regardless of
the packet being correct, and the packet's arrival has to be judged some other way - by the
panel's own DCS 0x52 read-back.

Bit 1 therefore stays clear. Unmasking it is only valid together with attaching a handler to
that SPI, which is a separate change.

## 2026-10-31 btn4: a level is written, and the read-back is not a brightness value

Image `tmp/boot-btn4.img`, sha256 `74461b1c76bc27ad7caca2167d908807d609457f857fa801eb1d1960336a76d4`,
tag `btn4`, carrying `alioth-bl=2` (the three-level sweep) and `alioth-bt=1`. It returned to
the bootloader on its own at about 200 s, so reverting the `INT_CTRL` unmask fixed btn3's
hang. Full round: `uptime 178 s`, `rounds 90`, `slots 90/509`, `errors 0`. Log at
`build/sheng-logs/btn4-00000001d8b52d83`.

```
B st=3 irq=0 rep=1 mask=4 apid=13/26 off=65536 pdc=20000 err=131072 spur=512
L st=2 req=0 lvl=2047 a=0 b=1 xerr=2 irq=ffffffff ea=1f5 eb=5 err=1280 goal=0 fade=0
L path runs=0 fades=0 irq=2/1 mode=84391 tmo=537133056 done=335544320 empty=0 last=0
```

### Buttons repeat

`st=3`, PON at APID 13, one report with `mask=4`. Same as btn2, so that is reproducible.

### Brightness: the write went out, the read-back is meaningless

`lvl=2047` is `stats[2]`, set by `writeBrightness` when the transfer returned 0, and 2047 is
the maximum the sweep writes last. So at least one level write reported success.

`b=1` is `stats[4]`, the read-back after the *first* level (the maximum). A brightness of 1
after setting 2047 is not a plausible answer - it is what the controller returns when nothing
valid came back. `xerr=2` (two failed transfers) and `irq=ffffffff` (the `INT_CTRL` seen at a
failure) agree that the transfers did not complete cleanly.

`stats[5]` and `stats[6]` hold the other two read-backs but BootLog's row does not print them;
the next round's row should.

### Why the read-back cannot be trusted yet

The panel answers DCS 0x52 only after the controller turns the lane around (BTA) and latches
the response into `RDBK_DATA0..3`. This kernel never attaches a handler to DSI0's SPI
(`alioth.dts:30921-30922`), and the vendor's FIFO path only runs in `secure_mode`
(`dsi_ctrl.c:1170-1180`), so completion is not observable here. A read that returns 1 is
therefore not evidence that the panel reported 1; it is evidence that the read path is not
established.

What is established: the packets are accepted by the controller (no fault, and the write
returns 0), and the sweep runs to completion without hanging. What is not: whether the panel
receives them, and what its DCS 0x52 would say.

## 2026-10-31 btn5: the read-backs hold the round, and the round is lost

Image `tmp/boot-btn5.img`, sha256 `d7339d4b650976700c96e0c94ceb6fcdc46e13b7e06585bc3bf2eed60e863a85`,
tag `btn5`, `alioth-bl=2` plus `alioth-bt=1`. It booted at 01:35 and was still running at
01:44 - nine minutes, against btn4's ~200 s - with the kernel alive (`Qualcomm XNU Debug`
enumerated, a fresh `/dev/tty.usbmodem14101`) and the screen frozen.

**It never returned on its own.** The user power-cycled the board at 02:05. The timed reset
did not fire in that state, so this is the fact to plan every later round around: once a
round reaches this condition, waiting does not recover it and only a person can. **No log was
read back** - there is no `build/sheng-logs/btn5-*` - so this round produced no reading at
all.

### Why it held

`sendPacket` polled twice per packet: up to 20 ms for `DSI_STATUS` bit 2 to clear and up to
50 ms for the done status. Neither can succeed on this board - the vendor only takes the TPG
FIFO path in `secure_mode` (`dsi_ctrl.c:1170-1180`), and completion is an interrupt nothing
here attaches (`alioth.dts:30921-30922`). btn5 added two more read-backs per level than btn4,
and the accumulating waits held the round past the reset.

Both polls are now optional and non-fatal: the packet is written and triggered regardless, a
refusal is recorded in `stats[6]`, and `stats[7]` counts how many were refused.

### The criterion changes

btn6 leads mode 2 with three off/on blinks (`0x28` then `0x10`, `0x11` then `0x29`, one
second dark each). That is the check that needs neither a read-back nor an observer judging a
small brightness change: if the packets reach the panel the screen goes dark and comes back,
and if it does not, the packets are not arriving and no read-back could have been trusted.

## btn6's acceptance criteria

Mode 2 leads with three off/on blinks before the level sweep, and `stats[7]` counts how many
packets were launched. The two together separate three outcomes, not two - and the third is
the one that decides what to do next.

| Screen | `stats[7]` | Meaning | Next |
| --- | --- | --- | --- |
| blinks three times | 3 | the packets reach the panel | brightness works; continue with the sweep and the iOS service |
| no blink, **observed** | 3 | the packets were launched but the panel did not act | the TPG FIFO path does not hold on this command-mode panel; implement the vendor's DMA-buffer path instead |
| no blink, **observed** | < 3 | the controller never launched the packet | the launch sequence itself is wrong; check DMA_CTRL, DMA_LENGTH and the trigger |
| **not observed** | 3 | packets launched; visibility unknown | nothing is concluded about the panel - this is not a negative result |

**"Not observed" is not "negative".** A round nobody watched the screen through says only that
the driver issued the packets. Recording it as "the panel did not blink" would send the next
round into rewriting the transport on evidence that does not exist. The two are separate rows
above for that reason.

`stats[7]` alone cannot tell "the panel received it" from "the packet never left", so a
reading of 3 with no blink is not evidence that anything reached the panel - it is evidence
that the driver issued the packet. The screen is what separates those two.

The blink itself: `0x28` (display off), 120 ms, `0x10` (sleep in), 1000 ms dark, `0x11`
(sleep out), 80 ms, `0x29` (display on), three times with 800 ms between. A one-second dark
screen is unmistakable, which is why this replaced judging a few levels of brightness on a
lock screen.

### Images that no longer exist

The boot images for btn1, btn3 and btn5 were deleted on 2026-10-31 to free space, so those
rounds cannot be reproduced byte for byte. Their readings and their sha256 are kept here:

| round | sha256 | image |
| --- | --- | --- |
| btn1 | `c9e031b879b9051278e25fe3948d3eed11167a4b8c5d33c74e08fdc3a924d8b6` | deleted |
| btn3 | `a86c32c8dc156a7a0d55232bfc0dddc5dbc2048426ad078d139b156d178f2699` | deleted (no reading; the round hung) |
| btn5 | `d7339d4b650976700c96e0c94ceb6fcdc46e13b7e06585bc3bf2eed60e863a85` | deleted (no reading; the round hung) |

btn2 (`20b2cf30585a09dc4d504f0a8d9c97729202c92eb668aca06aadddb1e4e2cb90`) and btn4
(`74461b1c76bc27ad7caca2167d908807d609457f857fa801eb1d1960336a76d4`) are still on disk
under `tmp/`, because each of them produced a reading.

## btn6's round as this branch runs it

Structure differs from btn1-btn5, which were built from the shared device repository with a
fresh `--out` root. That root is a dead end: `unixnu tree` does not rebuild the board archives,
so a new root starts missing `libVirtioMmioBlock.a`, the prelink candidate and the
display-analysis record, and each round dies one item further along than the last.

This branch therefore builds into the shared `out/alioth` and isolates itself with two locks
instead:

| lock | covers |
| --- | --- |
| `tmp/build.lock` | `defconfig` through `bootimg`, because `out/` and the kernel source tree are shared |
| `tmp/board.lock` | every `fastboot`, `adb` and serial-port use, because those devices are exclusive |

`board.toml` is this branch's own file (`alioth-bl=2` committed here), so no shared file is
patched and restored.

The image is proven to carry this round's kernel rather than assumed to: `build kernel` records
`kernel.macho`'s sha256, and after `payload` the script compares it with `payload.json`'s
`kernel_sha256`, failing with exit 7 on a mismatch.

Acceptance is the three-outcome table above. The screen is read by a person; `stats[7]` says
how many packets the driver launched, and the two together separate "reached the panel",
"launched but the panel ignored it" and "never launched".

## 2026-10-31 btn6: the panel blinks, and the brightness path is live

Image `tmp/boot-btn6.img`, sha256
`da8e4528d71cdf8a4821ed4e9a7d2e6e4c8528762a69683dfa5f856e23ea6710`, kernel
`1b1c729f60682f78ac162cd0e9c4d037b5ea1029936d05f4b2c2509b9c952363`. The image's command line
carries `alioth-bl=2`, `alioth-bt=1` and `sheng-build-tag=btn6`, and the payload's
`kernel_sha256` equals the kernel this build produced - so this round ran this branch's own
build, not the shared tree's.

Timeline, all on the Mac's clock:

```
03:37:27  fastboot boot
03:37:42  left the bootloader (5 s)
03:39:35  serial still alive, BLPHASE t=107s phase=9
03:41:07  returned to fastboot (195 s, against the 186-195 s baseline)
```

### The observation, recorded in two parts

| Observation | Status |
| --- | --- |
| the panel's brightness changed | user-confirmed, 03:38, "这个亮度确实有变化" |
| three off/on cycles | user-confirmed, 03:39, "对滴对滴" |

They are kept apart because the first alone was not enough to pass: a brightness change could
be anything, while three one-second dark periods in a row is the blink the code asks for.

### What the log says, and what it does not

`build/sheng-logs/btn6-00000003b4b2eed4`, tag `btn6`, uptime 178 s, 90 slots, `write_errors 0`.

```
B st=3 irq=0 rep=1 mask=4 apid=13/26 off=65536 pdc=20000 err=131072 spur=512
L st=2 req=0 lvl=2047 a=0 b=0 xerr=-1 irq=ffffffff ea=a eb=5 err=1280 goal=0 fade=0
L path runs=0 fades=0 irq=21/8 mode=141137 tmo=537133056 done=335544320 empty=0 last=0
```

`L path irq=21/8` is `stats[10]`/`stats[11]`: 21 packets launched, 8 of them timed out waiting
for a done this kernel cannot observe. So packets went out, and the panel acted on them - which
is exactly the first row of the three-outcome table, reached by the screen rather than by a
counter.

### A correction: `stats[7]` was not a counter

The acceptance table called for `stats[7] = 3` as the cross-check. That was wrong: the code
wrote `native_backlight_stats[7] = 3` as a literal after the blink loop, so it recorded "the
loop ran", not "three packets reached the panel". It is not evidence and must not be cited as
such. The honest cross-check is `stats[10]`, the packets-launched counter, which is 21 here
because the sweep's read-backs follow the blinks.

### What this establishes, and what it does not

Established: the DCS 0x51/0x28/0x29 packets written through DSI0's TPG FIFO reach this
command-mode panel and change it. The transport works, which is the question btn1 through btn5
could not answer.

Not established: the brightness value the panel ends at, whether a read-back works (it does
not - `xerr=-1` and `a=0` on the last row), the fade behaviour, or the iOS service in mode 3.
Those need an interrupt handler on DSI0's SPI, which nothing here attaches.
