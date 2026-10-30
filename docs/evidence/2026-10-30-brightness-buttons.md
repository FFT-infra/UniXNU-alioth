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
