// Host test of BootResetCore.h against a software model of the IMEM restart-reason cell.
// The model is deliberately trivial: IMEM is plain memory, so the test can check the exact
// word, the exact offset, and that a refusal leaves the neighbours alone. Build and run:
//
//   cc -std=c11 -Wall -Wextra -Werror -o boot_reset_host package/tests/boot_reset_host.c
//   ./boot_reset_host
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../BootResetCore.h"

// The devicetree window: qcom,msm-imem@146bf000 reg = <0x146bf000 0x1000>.
static uint32_t imem[BR_IMEM_WINDOW / 4];

static void setup(uint32_t cell) {
  memset(imem, 0, sizeof imem);
  // Neighbours the write must not touch: restart_reason@65c sits between mem_dump_table@10
  // and boot_stats@6b0, so mark the words on either side of the cell.
  imem[BR_IMEM_REASON_BYTE / 4 - 1] = 0xaaaaaaaa;
  imem[BR_IMEM_REASON_BYTE / 4] = cell;
  imem[BR_IMEM_REASON_BYTE / 4 + 1] = 0xbbbbbbbb;
}

int main(void) {
  uint32_t before = 0, after = 0, value = 0;

  // The cell's word is the devicetree offset, not the physical address: 0x65c / 4 = 0x197.
  assert(BR_IMEM_REASON_BYTE == 0x65c && BR_IMEM_REASON_BYTE % 4 == 0);
  assert(BR_IMEM_REASON_BYTE / 4 == 0x197);

  // A clean cell is a value the bootloader knows.
  setup(0);
  assert(br_known(0) && br_known(BR_REASON_BOOTLOADER) && br_known(BR_REASON_RECOVERY));
  assert(!br_known(0x12345678) && !br_known(0x77665502));

  // Read without writing.
  setup(0x77665501);
  assert(br_read(imem, &value) == 0 && value == 0x77665501);
  assert(imem[BR_IMEM_REASON_BYTE / 4 - 1] == 0xaaaaaaaa && imem[BR_IMEM_REASON_BYTE / 4 + 1] == 0xbbbbbbbb);

  // Write and verify: the whole word is replaced, the neighbours stay.
  setup(0);
  assert(br_mark(imem, BR_REASON_BOOTLOADER, &before, &after) == 0);
  assert(before == 0 && after == BR_REASON_BOOTLOADER);
  assert(imem[BR_IMEM_REASON_BYTE / 4] == 0x77665500);
  assert(imem[BR_IMEM_REASON_BYTE / 4 - 1] == 0xaaaaaaaa && imem[BR_IMEM_REASON_BYTE / 4 + 1] == 0xbbbbbbbb);

  // Writing over a reason the bootloader already knows replaces it and reports the old one.
  setup(0x77665501);
  assert(br_mark(imem, BR_REASON_BOOTLOADER, &before, &after) == 0);
  assert(before == BR_REASON_RECOVERY && after == BR_REASON_BOOTLOADER);

  // Idempotent: marking twice leaves the same word.
  assert(br_mark(imem, BR_REASON_BOOTLOADER, &before, &after) == 0);
  assert(before == BR_REASON_BOOTLOADER && after == BR_REASON_BOOTLOADER);

  printf("PASS: IMEM restart-reason offset, clean/known values, read without write, whole-word "
         "write with neighbours intact, reason replacement, idempotent mark\n");
  return 0;
}
