// SPDX-License-Identifier: MIT
// Board glue for BootResetCore.h on alioth: map the IMEM restart-reason window, write
// "bootloader" into it, read it back, then PSCI SYSTEM_RESET. Nothing here runs without
// BootLog's boot arguments (sheng-autoreset=<seconds>, see BootLog.c).
//
// Why IMEM and not the SPMI PMIC arbiter that the sheng board uses: alioth's devicetree
// puts the reason cell at qcom,msm-imem@146bf000 + 0x65c = 0x146bf65c, and the phone was
// observed to accept 0x77665500 there and return to fastboot
// (ARCHIVE/UniXNU/docs/alioth-reboot-reason-facts.md). The SPMI path needs the arbiter at
// 0xc440000/0xc600000 with a different layout, and its write lane is 32 MiB wide, which
// does not fit an 8 MiB io_submap. IMEM is one word: map, write, read back, reset.
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <string.h>
#include <IOKit/IOLib.h>
#include <kern/sched.h>
#include <pexpert/pexpert.h>
#include "BootResetCore.h"

// The devicetree's IMEM window: qcom,msm-imem@146bf000 reg = <0x146bf000 0x1000>.
enum {ImemBase = 0x146bf000, ImemSize = 0x1000};
enum {PsciVersion = 0x84000000u, PsciFeatures = 0x8400000au, PsciSystemReset = 0x84000009u,
      PsciSystemReset2_32 = 0x84000012u, PsciSystemReset2_64 = 0xc4000012u};

// ml_io_map hands out permanent mappings (io_submap, KMA_PERMANENT) that ml_io_unmap cannot
// free: its kmem_free on kernel_map panics with the map lock held. Mappings that are
// unmapped again must be unmappable ones (flags 7 = VM_WIMG_IO).
extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, uint32_t flags);

static uint64_t psci(uint64_t fid, uint64_t a1) {
  register uint64_t x0 __asm__("x0") = fid, x1 __asm__("x1") = a1, x2 __asm__("x2") = 0, x3 __asm__("x3") = 0;
  __asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) :: "memory", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13",
                   "x14", "x15", "x16", "x17");
  return x0;
}

static volatile uint32_t *map_imem(void) {
  return (volatile uint32_t *)ml_io_map_unmappable((vm_offset_t)ImemBase, (vm_size_t)ImemSize, 7);
}

// Reads the cell without writing it. Fills text; 0 when the cell reads and holds a value
// the bootloader knows.
int boot_reset_probe(char *text, unsigned cap) {
  const uint64_t ver = psci(PsciVersion, 0);
  const int64_t f_reset = (int64_t)psci(PsciFeatures, PsciSystemReset), f2_32 = (int64_t)psci(PsciFeatures, PsciSystemReset2_32),
                f2_64 = (int64_t)psci(PsciFeatures, PsciSystemReset2_64);
  volatile uint32_t *imem = map_imem();
  if (!imem) {
    snprintf(text, cap, "psci v%x reset=%lld reset2_32=%lld reset2_64=%lld | imem map refused", (unsigned)ver, (long long)f_reset,
             (long long)f2_32, (long long)f2_64);
    return 1;
  }
  uint32_t value = 0;
  br_read(imem, &value);
  const int known = br_known(value);
  snprintf(text, cap, "psci v%x reset=%lld reset2_32=%lld reset2_64=%lld | imem %08x = %08x known=%d", (unsigned)ver, (long long)f_reset,
           (long long)f2_32, (long long)f2_64, (unsigned)ImemBase + BR_IMEM_REASON_BYTE, value, known);
  ml_io_unmap((vm_offset_t)imem, ImemSize);
  return known ? 0 : 1;
}

// Writes "bootloader" into the cell and verifies it, without restarting. Done once early in
// an unattended run, it sends every later restart to fastboot: the timed one below, or a
// long press of the power key. 0 = written and verified.
int boot_reset_mark_bootloader(char *text, unsigned cap) {
  volatile uint32_t *imem = map_imem();
  if (!imem) {
    snprintf(text, cap, "mark refused: imem map refused");
    return 1;
  }
  uint32_t before = 0, after = 0;
  const int rc = br_mark(imem, BR_REASON_BOOTLOADER, &before, &after);
  snprintf(text, cap, "mark: %08x -> %08x rc=%d", before, after, rc);
  ml_io_unmap((vm_offset_t)imem, ImemSize);
  return rc;
}

// Writes "bootloader" into the cell, verifies it, then PSCI SYSTEM_RESET. Returns only when
// something refused (text says what); the cell is left as it was on a refusal.
int boot_reset_bootloader(char *text, unsigned cap) {
  volatile uint32_t *imem = map_imem();
  if (!imem) {
    snprintf(text, cap, "reset refused: imem map refused");
    return 1;
  }
  uint32_t before = 0, after = 0;
  const int rc = br_mark(imem, BR_REASON_BOOTLOADER, &before, &after);
  snprintf(text, cap, "reset: %08x -> %08x rc=%d", before, after, rc);
  ml_io_unmap((vm_offset_t)imem, ImemSize);
  if (rc) return 10 + rc;
  psci(PsciSystemReset, 0);
  return 99;   // SYSTEM_RESET returned
}

// The APSS watchdog (qcom,wdt@17c10000, bark-time 0x4e20 = 20 s, pet-time 0x3a98 = 15 s,
// qcom,ipi-ping). kona has no Gunyah, so the SMCCC vendor calls the sheng board uses are
// not available here. This board's watchdog is the SoC's own and it is armed by the
// bootloader, not by us: pinging it from the kernel would need the register protocol the
// mainline driver implements, which is not written yet. Report that plainly rather than
// pretend a watchdog is armed.
volatile int32_t boot_reset_wdt_stats[4];   // [0] reserved, [1] reserved, [2] reserved, [3] pings sent
int boot_reset_watchdog(uint32_t deadline, char *text, unsigned cap) {
  snprintf(text, cap, "watchdog: not armed; kona has no Gunyah and the APSS watchdog protocol is not implemented (deadline %us)", deadline);
  return 1;
}
