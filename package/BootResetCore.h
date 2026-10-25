// SPDX-License-Identifier: MIT
// Reboot-into-fastboot control for the alioth board: one word of the Qualcomm IMEM
// restart-reason cell, then a PSCI SYSTEM_RESET. The register facts are the board's
// own devicetree and the on-device evidence recorded in
// ARCHIVE/UniXNU/docs/alioth-reboot-reason-facts.md:
//
//   qcom,msm-imem@146bf000   reg = <0x146bf000 0x1000>
//   restart_reason@65c       reg = <0x65c 0x04>          -> physical 0x146bf65c
//   restart@c264000          reg = <0xc264000 0x04 ...>  -> PS_HOLD
//   psci                     compatible = "arm,psci-1.0", method = "smc"
//
// The stock bootloader reads that word as the reboot reason; 0x77665500 is what
// msm-poweroff.c writes for "bootloader" and what the phone was observed to honour
// (it returned to fastboot, not to recovery). alioth does not need the SM8550 SPMI
// arbiter path that the sheng board uses: IMEM is a direct write with a read-back,
// one order of magnitude simpler, and it is proven on this exact phone.
//
// This core is pure register-pointer code so a host test can drive it against a
// software model of the cell (package/tests/boot_reset_host.c).
#ifndef BOOT_RESET_CORE_H
#define BOOT_RESET_CORE_H
#include <stdint.h>

enum {
  BR_IMEM_REASON_BYTE = 0x65c,      // restart_reason@65c, the cell's offset inside the IMEM window
  BR_IMEM_WINDOW = 0x1000,          // the window the devicetree declares
  BR_REASON_BOOTLOADER = 0x77665500u,   // msm-poweroff.c: bootloader
  BR_REASON_RECOVERY = 0x77665501u,     // msm-poweroff.c: the fallback reason
  BR_POLLS = 100000
};

// The compiler must not move the store after the caller's reset, and the read-back
// must observe the written value rather than a cached one.
#if defined(__aarch64__)
#define BR_BARRIER() __asm__ volatile("dsb sy" ::: "memory")
#else
#define BR_BARRIER() __asm__ volatile("" ::: "memory")
#endif

// Writes the reason word and reads it back. 0 = written and verified.
// *before receives the value that was there, so a refusal can leave it alone.
static inline int br_mark(volatile uint32_t *imem, uint32_t value, uint32_t *before, uint32_t *after) {
  volatile uint32_t *cell = imem + BR_IMEM_REASON_BYTE / 4;
  *before = *cell;
  *cell = value;
  BR_BARRIER();
  *after = *cell;
  return *after == value ? 0 : 1;
}

// Reads the cell without writing. 0 = read.
static inline int br_read(volatile uint32_t *imem, uint32_t *value) {
  *value = imem[BR_IMEM_REASON_BYTE / 4];
  return 0;
}

// A value the bootloader will consume as a reason it knows. Any other value is
// left for the caller to report rather than silently overwritten.
static inline int br_known(uint32_t value) {
  return value == 0 || value == BR_REASON_BOOTLOADER || value == BR_REASON_RECOVERY;
}
#endif
