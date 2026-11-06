// SPDX-License-Identifier: BSD-2-Clause-Patent
// The fuel gauge's addressing and its value decoding, kept out of the driver so a host test can
// check them against the vendor kernel's own constants without a device.
//
// The gauge on this board is not a firmware protocol and not an I2C part: it is qcom,fg-gen4 on
// pm8150b, reached over SPMI. Everything below comes from the vendor kernel under
// reference/k40-kernel: the peripheral addresses from the device tree
// (arch/arm64/boot/dts/vendor/qcom/pm8150b.dtsi:285-350), the register offsets from
// drivers/power/supply/qcom/fg-reg.h, and the scaling from drivers/power/supply/qcom/fg-util.c.
// No MMIO and no IOKit here, so package/tests/battery_host.c compiles it with a host compiler.
#ifndef BATTERY_CORE_H
#define BATTERY_CORE_H
#include <stdint.h>
#include <stdbool.h>

// The FG is one SPMI slave with four 0x100-byte blocks, each its own peripheral. The device tree's
// interrupt specifiers spell the sid and the peripheral out: <0x2 0x40 ..> through <0x2 0x43 ..>
// (pm8150b.dtsi:295, 318, 330, 345), so sid 2 and peripherals 0x40 to 0x43.
enum {
  BG_SID = 2,
  BG_BATT_SOC = 0x4000,
  BG_BATT_INFO = 0x4100,
  BG_RRADC = 0x4200,
  BG_MEMIF = 0x4300,
};

// The arbiter maps a peripheral to an APID by ppid = sid << 8 | (peripheral address >> 8)
// (spmi-pmic-arb.c:977,996), the same rule ButtonProbe.cpp uses for the PON and the pads. Derived,
// never copied: a board with the same chip at another page would get a different PPID.
static inline uint32_t bg_ppid(uint32_t peripheral) {
  return ((uint32_t)BG_SID << 8) | (peripheral >> 8);
}

// Register offsets inside the batt-info block, fg-reg.h:244-260. The value and its copy (CP) are
// six bytes apart, and the LSB/MSB order is silicon-dependent: fg-reg.h:243 says the offsets below
// are for v2.0 and above, and that v1.1 has LSB and MSB the other way round. Read both bytes and
// assemble with bg_word() rather than assuming.
enum {
  BG_VBATT_LSB = 0xA0, BG_VBATT_MSB = 0xA1,
  BG_IBATT_LSB = 0xA2, BG_IBATT_MSB = 0xA3,
  BG_ESR_LSB   = 0xA4, BG_ESR_MSB   = 0xA5,
  BG_VBATT_LSB_CP = 0xA6, BG_VBATT_MSB_CP = 0xA7,
  BG_IBATT_LSB_CP = 0xA8, BG_IBATT_MSB_CP = 0xA9,
  BG_ESR_LSB_CP   = 0xAA, BG_ESR_MSB_CP   = 0xAB,
};

// The scaling of the direct-register path, fg-util.c:1012-1013 and 1057-1058. The driver divides
// by the numerator and multiplies by the denominator, so the unit is one LSB's worth:
// voltage 122.070 uV per count, current 488.281 uA per count.
enum { BG_VOLT_NUMR = 122070, BG_CURRENT_NUMR = 488281, BG_DENR = 1000 };

// The SRAM path's denominators, qpnp-fg-gen4.c:423-427 (PARAM(id, word, offset, len, numrtr,
// denmtr, ...) with numrtr 1000). They are not the same numbers as the direct-register path, and
// that is not a typo: the current differs by one count and the voltage by a factor of two plus
// one, because a SRAM word carries one more bit of the same measurement. A port must pick one path
// and say which it uses; a test below pins both.
enum { BG_SRAM_VOLT_DENMTR = 244141, BG_SRAM_CURRENT_DENMTR = 488282 };

// Assemble a two-byte value. swapped is true on v1.1 silicon, where the lower address holds the
// high byte.
static inline uint16_t bg_word(uint8_t at_lsb, uint8_t at_msb, bool swapped) {
  return swapped ? (uint16_t)(((uint16_t)at_lsb << 8) | at_msb)
                 : (uint16_t)(((uint16_t)at_msb << 8) | at_lsb);
}

// Voltage is an unsigned 16-bit count: 65535 counts is about 8 V, above any battery.
static inline int32_t bg_voltage_uv(uint16_t raw) {
  return (int32_t)(((uint64_t)raw * BG_VOLT_NUMR) / BG_DENR);
}

// Current is a 16-bit two's complement count whose sign bit is bit 15 (fg-util.c:1052,
// sign_extend32(temp, 15)). The sign's direction is the vendor driver's, and this file does not
// reinterpret it: whatever the gauge calls positive, this returns positive.
static inline int32_t bg_current_ua(uint16_t raw) {
  return (int32_t)(((int64_t)(int16_t)raw * BG_CURRENT_NUMR) / BG_DENR);
}

// The same two measurements through the SRAM path, for the day the memory interface is used
// instead. Voltage masks to 15 bits (fg-util.c:38, VOLTAGE_15BIT_MASK) and is unsigned.
static inline int32_t bg_sram_voltage_uv(uint16_t raw) {
  return (int32_t)(((uint64_t)(raw & 0x7fffu) * BG_SRAM_VOLT_DENMTR) / BG_DENR);
}
static inline int32_t bg_sram_current_ua(uint16_t raw) {
  return (int32_t)(((int64_t)(int16_t)raw * BG_SRAM_CURRENT_DENMTR) / BG_DENR);
}

// Every measurement is read as a pair: the value at one address and its shadow copy six bytes
// later, retried until the two agree (fg-util.c:1014-1050). The retry is not optional -- a single
// read can return one byte of an old value and one of a new one while the gauge updates.
//
// The vendor loop that does this is wrong in two directions, and this port implements what it
// meant rather than what it does. `while (tries++ < MAX_READ_TRIES) { ...; if (match) break; }`
// followed by `if (tries == MAX_READ_TRIES) return -EINVAL;` leaves tries at 6 when the loop runs
// to the end, so a pair that NEVER matches falls through and its torn value is returned as good;
// and a match on the fifth and last try leaves tries at 5, so a good read is reported as an
// error. package/tests/battery_host.c reproduces the vendor loop and asserts both outcomes.
// BG_PAIR_TRIES is
// the driver's own MAX_READ_TRIES, and the loop below is a plain counted loop: after this many
// tries with no agreement the read fails, which is the only reading of the driver's intent that
// is safe.
typedef int (*bg_read_fn)(void *ctx, uint32_t apid, uint8_t addr, uint8_t out[2]);
enum { BG_PAIR_TRIES = 5 };

// Read one two-byte value and its copy, and hand back the assembled word. Returns 0 on a matching
// pair and -1 if no try agreed or the bus read failed. The two bytes of one read are taken back
// to back, as the vendor driver's fg_read(fg, addr, buf, 2) does, so the caller's read function
// must perform a burst read rather than two one-byte accesses.
static inline int bg_read_pair(bg_read_fn read, void *ctx, uint32_t apid,
                               uint8_t value_addr, uint8_t copy_addr, bool swapped,
                               uint16_t *out) {
  for (int t = 0; t < BG_PAIR_TRIES; t++) {
    uint8_t v[2], c[2];
    if (read(ctx, apid, value_addr, v) != 0) return -1;
    if (read(ctx, apid, copy_addr, c) != 0) return -1;
    if (v[0] == c[0] && v[1] == c[1]) {
      *out = bg_word(v[0], v[1], swapped);
      return 0;
    }
  }
  return -1;
}

// State of charge is a raw percentage in both paths: PARAM(BATT_SOC, ..., 4, 1, 1, 0, ...) in
// qpnp-fg-gen4.c:417 has numerator and denominator 1, so there is nothing to convert. It is kept
// as a function anyway, so that a caller cannot mistake it for a value needing scaling.
static inline int32_t bg_soc_percent(uint8_t raw) { return (int32_t)raw; }

#endif
