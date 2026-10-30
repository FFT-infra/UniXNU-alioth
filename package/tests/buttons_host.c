// Host test of ButtonsCore.h. It checks the address arithmetic and the key-to-report mapping
// against this board's devicetree and the vendor sources, with no device involved. Build:
//
//   cc -std=c11 -Wall -Wextra -Werror -o buttons_host package/tests/buttons_host.c
//   ./buttons_host
//
// What is NOT covered here: whether the arbiter answers, whether the APIDs are found, and
// whether a press reaches the HID device. Those need the phone.
#include <assert.h>
#include <stdio.h>
#include "../ButtonsCore.h"

// The arbiter maps a peripheral to an APID by ppid = sid << 8 | (peripheral address >> 8)
// (spmi-pmic-arb.c:977,996). This board's two peripherals, from alioth.dts: the PM8150 PON at
// 0x800 and the PM8150 GPIO block at 0xc000 with 0x100 bytes per pad, so GPIO 6 is at 0xc600.
static uint32_t ppid_for(uint8_t sid, uint16_t peripheral) {
  return ((uint32_t)sid << 8) | (peripheral >> 8);
}

int main(void) {
  // The PPIDs are derived, not copied. sheng's board has 0x013 and 0x18d because its SoC puts
  // the PON and GPIO peripherals at different pages; using those here would ask this phone's
  // arbiter for a peripheral it does not have.
  assert(ppid_for(0, 0x800) == BT_PON_PPID);
  // GPIO 6's SPMI interrupt names the peripheral 0xc6 (alioth.dts:4261), and the arbiter keys
  // on that page, so the PPID is 0xc6 - not 0xc600 >> 8 from the pad's own address and not
  // sheng's 0x18d.
  assert(ppid_for(0, 0xc600) == BT_GPIO_PPID);
  assert(BT_PON_PPID != 0x013 && BT_GPIO_PPID != 0x18d);

  // The PDC hop: pin 1 to GIC SPI 0x1e0 + 1 = 481, which the driver attaches as 32 + 481.
  assert(BT_PDC_PIN == 1 && BT_SPI == 481);
  assert(BT_SPI == 0x1e0 + BT_PDC_PIN);

  // The arbiter windows are this board's, not sheng's.
  assert(BT_CORE == 0xc440000 && BT_CHNLS == 0xc600000 && BT_OBSRVR == 0xe600000);
  assert(BT_INTR == 0xe700000 && BT_CNFG == 0xc40a000 && BT_PDC == 0xb220000);

  // The version selects the layout, as the vendor ops table does. v5 keeps every channel
  // register inside the 0x10000 * apid page; a version this code does not know yields no layout,
  // which the driver treats as a refusal.
  struct bt_layout v5 = bt_layout_for(0x50000000u);
  assert(v5.apid_stride == 0x10000 && v5.acc_enable == 0x100);
  assert(v5.irq_status == 0x104 && v5.irq_clear == 0x108 && v5.apid_map == 0x900);
  assert(v5.obs_stride == 0x80);
  // The upper bound is exclusive and the lower bound inclusive, matching the vendor's range tests.
  assert(bt_layout_for(0x5fffffffu).apid_stride == 0x10000);
  assert(bt_layout_for(0x60000000u).apid_stride == 0);
  assert(bt_layout_for(0x20010000u).apid_stride == 0);   // v2 is not implemented
  assert(bt_layout_for(0x00000000u).apid_stride == 0);
  assert(bt_layout_for(0x70000000u).apid_stride == 0);

  // Channel and observer offsets for the two APIDs this board uses.
  assert(bt_channel_offset(&v5, 0) == 0);
  assert(bt_channel_offset(&v5, 3) == 0x30000);
  assert(bt_observer_offset(&v5, 0) == 0);
  assert(bt_observer_offset(&v5, 3) == 0x180);
  assert(bt_map_offset(&v5, 0) == 0x900);
  assert(bt_map_offset(&v5, 3) == 0x90c);

  // The command word: opc in bits 27+, byte address in bits 7:4, byte count in bits 2:0.
  assert(bt_command(false, 0x10) == ((1u << 27) | (0x10u << 4)));   // EXT_READL
  assert(bt_command(true, 0x10) == (0x10u << 4));                   // EXT_WRITEL
  assert((bt_command(true, 0x15) >> 4 & 0xffu) == 0x15u);

  // Channel status: bit 0 is done, bits 1..3 are failure, denied, dropped.
  assert(bt_status_result(0) == BT_NOT_DONE);
  assert(bt_status_result(1) == BT_OK);
  assert(bt_status_result(1 | 2) == BT_FAILED);
  assert(bt_status_result(1 | 4) == BT_FAILED);
  assert(bt_status_result(1 | 8) == BT_FAILED);
  assert(bt_status_result(2) == BT_NOT_DONE);    // errors without done is still running

  // The map entry: PPID in bits 19:8, IRQ owner in bit 24. The ownership entry: EE in bits 2:0.
  assert(bt_entry_ppid((BT_PON_PPID << 8) | (1u << 24)) == BT_PON_PPID);
  assert(bt_entry_irq_owner((BT_PON_PPID << 8) | (1u << 24)) == 1);
  assert(bt_entry_irq_owner(BT_PON_PPID << 8) == 0);
  assert(bt_entry_ppid(0) == 0);
  assert(bt_owner_ee(0) == 0 && bt_owner_ee(7) == 7);

  // The key-to-report mapping. The PON's RT_STS bits 0 and 1 are kpdpwr and resin, both
  // active high in that register (qpnp-power-on.c:113-114); the SPMI GPIO's bit 0 is the pad
  // level and the key is active low, so a clear bit is a press.
  assert(bt_pressed_mask(0, 1, 1) == 0);                                     // nothing pressed
  assert(bt_pressed_mask(1u << 0, 1, 1) == BT_REPORT_POWER);                 // power only
  assert(bt_pressed_mask(1u << 1, 1, 1) == BT_REPORT_VOLUME_DOWN);           // volume down only
  assert(bt_pressed_mask(0, 0, 1) == BT_REPORT_VOLUME_UP);                   // volume up only
  assert(bt_pressed_mask((1u << 0) | (1u << 1), 0, 1) ==
         (BT_REPORT_POWER | BT_REPORT_VOLUME_DOWN | BT_REPORT_VOLUME_UP));   // all three
  // A GPIO that was not read cannot report a press, and must not report one either.
  assert(bt_pressed_mask(0, 0, 0) == 0);
  assert(bt_pressed_mask(1u << 0, 0, 0) == BT_REPORT_POWER);
  // Every bit the mask can produce is inside NativeHidInput's 0xe filter.
  assert((BT_REPORT_POWER | BT_REPORT_VOLUME_UP | BT_REPORT_VOLUME_DOWN) == 0xeu);

  puts("buttons_host: all assertions passed");
  return 0;
}
