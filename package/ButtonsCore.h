// SPDX-License-Identifier: BSD-2-Clause-Patent
// The address arithmetic for this board's power and volume buttons, kept out of the driver so a
// host test can check it against the vendor kernel's own rules without a device.
//
// Every constant here is derived in ButtonProbe.cpp's header comment from this board's device
// tree (uefi-alioth/Resources/DTBs/alioth.dts) and from the vendor SPMI arbiter, PDC and
// qpnp-power-on sources under reference/k40-kernel. This file holds only the arithmetic, with no
// MMIO and no IOKit, so package/tests/buttons_host.c can compile it with a host compiler.
#ifndef BUTTONS_CORE_H
#define BUTTONS_CORE_H
#include <stdint.h>
#include <stdbool.h>

// This board's arbiter windows, from reg-names core/chnls/obsrvr/intr/cnfg (alioth.dts:4172-4176).
// They are NOT the sheng board's (0xc400000 / 0xc500000 / 0xc440000 / 0xc4c0000 / 0xc42d000),
// whose SoC puts the same blocks elsewhere.
enum {
  BT_CORE = 0xc440000, BT_CHNLS = 0xc600000, BT_OBSRVR = 0xe600000, BT_INTR = 0xe700000,
  BT_CNFG = 0xc40a000, BT_PDC = 0xb220000,
  BT_PON_PPID = 0x008,     // sid 0, peripheral 0x800: ppid = sid << 8 | (addr >> 8)
  // GPIO 6's own SPMI interrupt names peripheral 0xc6, not 0xc600: the pad's interrupt
  // sub-peripheral is 0xc6 and the arbiter keys on the peripheral page, so ppid = 0xc6
  // (alioth.dts:4261, interrupts = <0x00 0xc6 0x00 0x00> named pm8150_gpio6).
  BT_GPIO_PPID = 0x0c6,
  BT_PDC_PIN = 1, BT_SPI = 481,   // PDC pin 1 -> GIC SPI 0x1e0 + 1
};

// The arbiter's version decides the channel layout (spmi-pmic-arb.c:1069-1105, 1113).
enum {BT_LAYOUT_NONE, BT_LAYOUT_V2, BT_LAYOUT_V5};
struct bt_layout {uint32_t apid_stride, acc_enable, irq_status, irq_clear, apid_map, obs_stride;};

// The layout for a version, or {0} for one this code does not know. v5 keeps every channel
// register inside the 0x10000 * apid page; v2/v3/v4 use 0x1000 * apid and put the interrupt
// registers in the "intr" window instead, which this driver does not implement, so those are
// reported as a version it refuses rather than half-supported.
static inline struct bt_layout bt_layout_for(uint32_t version) {
  struct bt_layout none = {0, 0, 0, 0, 0, 0};
  struct bt_layout v5 = {0x10000, 0x100, 0x104, 0x108, 0x900, 0x80};
  if (version >= 0x50000000u && version < 0x60000000u) return v5;
  return none;
}

// The channel page for an APID inside the chnls window.
static inline uint32_t bt_channel_offset(const struct bt_layout *l, uint32_t apid) {
  return l->apid_stride * apid;
}
// The observer channel for an APID inside the obsrvr window (0x1000 * ee + 0x80 * apid, ee 0).
static inline uint32_t bt_observer_offset(const struct bt_layout *l, uint32_t apid) {
  return l->obs_stride * apid;
}
// The APID map entry for an APID inside the core window.
static inline uint32_t bt_map_offset(const struct bt_layout *l, uint32_t apid) {
  return l->apid_map + 4 * apid;
}

// The command word for one SPMI byte transfer: (opc << 27) | (byte address << 4) | byte count,
// with opc 1 read and 0 write (pmic_arb_fmt_cmd_v2, spmi-pmic-arb.c).
static inline uint32_t bt_command(bool write, uint8_t address) {
  return ((write ? 0u : 1u) << 27) | ((uint32_t)address << 4);
}
// Channel status: DONE is bit 0 and bits 1..3 mean the transfer failed, was denied or dropped.
// The three outcomes are separate so a caller cannot mistake "still running" for a failure.
enum {BT_OK = 0, BT_NOT_DONE = 1, BT_FAILED = 2};
static inline int bt_status_result(uint32_t status) {
  if (!(status & 1u)) return BT_NOT_DONE;
  return (status & 0xeu) ? BT_FAILED : BT_OK;
}

// A map entry names the PPID in bits 19:8 and the IRQ-owner flag in bit 24.
static inline uint32_t bt_entry_ppid(uint32_t entry) {return (entry >> 8) & 0xfffu;}
static inline int bt_entry_irq_owner(uint32_t entry) {return (int)((entry >> 24) & 1u);}
// The ownership entry names the EE allowed to write the APID in bits 2:0.
static inline uint32_t bt_owner_ee(uint32_t entry) {return entry & 7u;}

// The pressed set as the HID device's report 2 bits. All three keys are active low: the PON's
// RT_STS bits 0 and 1 are kpdpwr and resin (qpnp-power-on.c:113-114), and the SPMI GPIO's
// RT_STS bit 0 is the pad level (pinctrl-spmi-gpio.c:38-39). Report 2 bits, as
// NativeHidInput::buttonsAction keeps with mask & 0xe: 1 power, 2 volume up, 3 volume down.
enum {BT_REPORT_POWER = 1u << 1, BT_REPORT_VOLUME_UP = 1u << 2, BT_REPORT_VOLUME_DOWN = 1u << 3};
static inline uint32_t bt_pressed_mask(uint8_t pon_rt_sts, uint8_t gpio_rt_sts, int have_gpio) {
  uint32_t mask = 0;
  if (pon_rt_sts & (1u << 0)) mask |= BT_REPORT_POWER;
  if (pon_rt_sts & (1u << 1)) mask |= BT_REPORT_VOLUME_DOWN;
  if (have_gpio && !(gpio_rt_sts & 1u)) mask |= BT_REPORT_VOLUME_UP;
  return mask;
}
#endif
