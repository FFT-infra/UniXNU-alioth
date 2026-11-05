// Host test of BatteryCore.h. It checks the SPMI addressing and the value decoding against this
// board's devicetree and the vendor fuel-gauge driver, with no device involved. Build:
//
//   cc -std=c11 -Wall -Wextra -Werror -o battery_host package/tests/battery_host.c
//   ./battery_host
//
// What is NOT covered here: whether the arbiter maps the fuel gauge to an APID, whether a read
// returns a value or a zero, and whether the paired copy agrees. Those need the phone.
#include <assert.h>
#include <stdio.h>
#include "../BatteryCore.h"

// The arbiter's own rule, restated here rather than taken from the header, so that the header's
// bg_ppid() is checked against it instead of against itself.
static uint32_t ppid_for(uint32_t sid, uint32_t peripheral) {
  return (sid << 8) | (peripheral >> 8);
}

int main(void) {
  // The device tree's own interrupt specifiers name sid 2 and peripherals 0x40 to 0x43
  // (pm8150b.dtsi:295, 318, 330, 345), so the four PPIDs are these and not sheng's.
  assert(bg_ppid(BG_BATT_SOC) == ppid_for(2, 0x4000));
  assert(bg_ppid(BG_BATT_INFO) == ppid_for(2, 0x4100));
  assert(bg_ppid(BG_RRADC) == ppid_for(2, 0x4200));
  assert(bg_ppid(BG_MEMIF) == ppid_for(2, 0x4300));
  assert(bg_ppid(BG_BATT_SOC) == 0x240);
  assert(bg_ppid(BG_BATT_INFO) == 0x241);
  // Four blocks, four peripherals: a port that reads everything through 0x241 is reading the
  // wrong page for three of them.
  assert(bg_ppid(BG_BATT_SOC) != bg_ppid(BG_BATT_INFO));

  // Absolute register addresses, fg-reg.h:244-260 added to the block base.
  assert(BG_BATT_INFO + BG_VBATT_LSB == 0x41A0);
  assert(BG_BATT_INFO + BG_IBATT_LSB == 0x41A2);
  assert(BG_BATT_INFO + BG_VBATT_LSB_CP == 0x41A6);
  assert(BG_BATT_INFO + BG_IBATT_LSB_CP == 0x41A8);
  // The copy sits six bytes past the value it copies, and the two are not adjacent.
  assert(BG_VBATT_LSB_CP - BG_VBATT_LSB == 6);
  assert(BG_IBATT_LSB_CP - BG_IBATT_LSB == 6);

  // Byte order: v2.0 and later put the low byte at the lower address; v1.1 has them swapped
  // (fg-reg.h:243). Both must assemble to the same count when read in their own order.
  assert(bg_word(0x34, 0x12, false) == 0x1234);
  assert(bg_word(0x12, 0x34, true) == 0x1234);
  assert(bg_word(0x34, 0x12, false) != bg_word(0x34, 0x12, true));

  // Voltage: 122.070 uV per count (fg-util.c:1057). Full scale of the unsigned field is about 8 V,
  // and a full battery sits near 4.2 V.
  assert(bg_voltage_uv(32767) == 3999867);          // 3.999867 V
  assert(bg_voltage_uv(0) == 0);
  assert(bg_voltage_uv(34400) == 4199208);          // 4.199208 V, a full cell

  // Current: 488.281 uA per count, two's complement in 16 bits (fg-util.c:1049-1055).
  assert(bg_current_ua(2048) == 999999);            // +0.999999 A
  assert(bg_current_ua(0x0000) == 0);
  assert(bg_current_ua(0xFFFF) == -488);            // -0.000488 A, one count below zero
  assert(bg_current_ua(0x8000) == -15999991);       // the most negative count, about -16 A
  assert(bg_current_ua(0x8000) < 0);                // the sign survives the multiply

  // The SRAM path is not the register path, and the same raw count proves how far apart they are:
  // 0x4000 through the SRAM is a 4 V battery, through the registers it is a 2 V one.
  assert(bg_sram_voltage_uv(0x4000) == 4000006);
  assert(bg_voltage_uv(0x4000) == 1999994);
  assert(BG_SRAM_VOLT_DENMTR == 2 * BG_VOLT_NUMR + 1);
  assert(BG_SRAM_CURRENT_DENMTR == BG_CURRENT_NUMR + 1);
  // The SRAM voltage field is 15 bits, so the top bit is not part of the count.
  assert(bg_sram_voltage_uv(0x8000) == 0);
  assert(bg_sram_voltage_uv(0xC000) == bg_sram_voltage_uv(0x4000));

  // State of charge is already a percentage; the PARAM entry has numerator and denominator 1.
  assert(bg_soc_percent(0) == 0);
  assert(bg_soc_percent(100) == 100);
  assert(bg_soc_percent(0xff) == 255);

  printf("battery_host: all assertions passed\n");
  return 0;
}
