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

// A scripted SPMI bus for bg_read_pair(). Each entry is one try's four bytes: the two bytes at the
// value address, then the two at the copy address. `tries` is how many entries are scripted; a try
// beyond that answers a pair that never agrees, which is what an unscripted bus looks like.
// `pos` counts reads performed, so complete tries are pos / 2.
typedef struct {
  uint8_t script[BG_PAIR_TRIES][4];
  int tries;
  int pos;
  int fail_at;  // read index to fail at, or -1
} FakeBus;

static int fake_read(void *ctx, uint32_t apid, uint8_t addr, uint8_t out[2]) {
  (void)apid;
  (void)addr;
  FakeBus *b = (FakeBus *)ctx;
  int idx = b->pos++;
  if (b->fail_at >= 0 && idx == b->fail_at) return -1;
  int try = idx / 2, copy = idx % 2;
  if (try < b->tries) {
    out[0] = b->script[try][copy * 2 + 0];
    out[1] = b->script[try][copy * 2 + 1];
  } else {
    out[0] = copy ? 0x55 : 0xAA;  // value and copy never agree
    out[1] = 0x00;
  }
  return 0;
}

// The vendor driver's retry loop, copied from fg-util.c:1020-1042 as it is written, so that the
// two defects the header warns about are held down by an executable check rather than by a claim.
// It answers the same scripted bus as the tests below: a match on try n is modelled by
// match_at == n, and match_at == 0 means the copies never agree. Returns 1 when the driver would
// take the value as good, 0 when it would return -EINVAL.
static int vendor_would_use_value(int match_at) {
  int tries = 0;
  while (tries++ < 5) {
    if (match_at && tries == match_at) break;
  }
  return !(tries == 5);
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

  // The paired read. A scripted bus answers each try with a value byte pair and a copy byte pair;
  // the test drives the retry count and the torn-read rejection from both ends.
  {
    FakeBus bus = {0};
    uint16_t word = 0;
    bus.fail_at = -1;

    // A clean first try, no retry needed: two reads, and the pair is used as it came.
    bus.script[0][0] = 0x30; bus.script[0][1] = 0x10;   // 0x1030 at the value address
    bus.script[0][2] = 0x30; bus.script[0][3] = 0x10;   // the copy agrees
    bus.tries = 1; bus.pos = 0;
    assert(bg_read_pair(fake_read, &bus, 0x241, BG_VBATT_LSB, BG_VBATT_LSB_CP, false, &word) == 0);
    assert(word == 0x1030);
    assert(bus.pos == 2);                                // one value read, one copy read
    assert(bg_voltage_uv(word) == 505858);               // 0x1030 = 4144 counts, 505.858 mV

    // The first try is torn and the second agrees: two tries, and the second pair wins.
    bus.script[0][0] = 0x30; bus.script[0][1] = 0x10;
    bus.script[0][2] = 0x31; bus.script[0][3] = 0x10;    // copy differs in one byte
    bus.script[1][0] = 0x34; bus.script[1][1] = 0x12;
    bus.script[1][2] = 0x34; bus.script[1][3] = 0x12;
    bus.tries = 2; bus.pos = 0;
    assert(bg_read_pair(fake_read, &bus, 0x241, BG_VBATT_LSB, BG_VBATT_LSB_CP, false, &word) == 0);
    assert(word == 0x1234);                              // not the torn 0x1030
    assert(bus.pos == 4);                                // two tries

    // A match on the last allowed try is a success here, not the false error the vendor loop
    // reports (fg-util.c:1039; tmp/tries_probe.c, match_at=5).
    for (int i = 0; i < BG_PAIR_TRIES - 1; i++) {
      bus.script[i][0] = 0x00; bus.script[i][1] = 0x00;
      bus.script[i][2] = 0x01; bus.script[i][3] = 0x00;  // disagree
    }
    bus.script[BG_PAIR_TRIES - 1][0] = 0x88; bus.script[BG_PAIR_TRIES - 1][1] = 0x13;
    bus.script[BG_PAIR_TRIES - 1][2] = 0x88; bus.script[BG_PAIR_TRIES - 1][3] = 0x13;
    bus.tries = BG_PAIR_TRIES; bus.pos = 0;
    assert(bg_read_pair(fake_read, &bus, 0x241, BG_VBATT_LSB, BG_VBATT_LSB_CP, false, &word) == 0);
    assert(word == 0x1388);
    assert(bus.pos == 2 * BG_PAIR_TRIES);

    // A pair that never agrees is an error here. The vendor loop falls out and returns the last
    // torn value as if it were good (match_at=0 in the same probe).
    bus.tries = 0; bus.pos = 0;                          // every try answers disagreeing pairs
    assert(bg_read_pair(fake_read, &bus, 0x241, BG_VBATT_LSB, BG_VBATT_LSB_CP, false, &word) == -1);
    assert(bus.pos == 2 * BG_PAIR_TRIES);                // it did use every allowed try

    // A bus error aborts at once rather than burning the remaining tries.
    bus.fail_at = 1; bus.tries = 0; bus.pos = 0;
    assert(bg_read_pair(fake_read, &bus, 0x241, BG_VBATT_LSB, BG_VBATT_LSB_CP, false, &word) == -1);
    assert(bus.pos == 2);                                // one good read, then the failure

    // The swap flag reaches the assembled word.
    bus.fail_at = -1;
    bus.script[0][0] = 0x12; bus.script[0][1] = 0x34;    // 0x1234 in v1.1 order
    bus.script[0][2] = 0x12; bus.script[0][3] = 0x34;
    bus.tries = 1; bus.pos = 0;
    assert(bg_read_pair(fake_read, &bus, 0x241, BG_VBATT_LSB, BG_VBATT_LSB_CP, true, &word) == 0);
    assert(word == 0x1234);
  }

  // The vendor loop's two defects, as behaviour rather than as comments.
  assert(vendor_would_use_value(1) == 1);   // matched on the first try: good
  assert(vendor_would_use_value(4) == 1);   // matched on the fourth: good
  assert(vendor_would_use_value(5) == 0);   // matched on the fifth: wrongly reported as an error
  assert(vendor_would_use_value(0) == 1);   // never matched: wrongly used as good

  printf("battery_host: all assertions passed\n");
  return 0;
}
