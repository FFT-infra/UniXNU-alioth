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
//
// The PM8150 PON spare cell is the second half of the pair. The vendor kernel writes both
// in one function (drivers/power/reset/msm-poweroff.c msm_restart_prepare: qpnp_pon_set_restart_
// reason(PON_RESTART_REASON_BOOTLOADER) then __raw_writel(0x77665500, restart_reason)), and
// the one run that did return to fastboot wrote both. Every run since writes only IMEM and
// comes back to the kernel, so the missing cell is the first thing to restore. This is a
// restoration of a known-good configuration, not a proven cause: no failure has yet been
// attributed to either cell alone.
#include <kern/thread.h>
#include <machine/machine_routines.h>
#include <string.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOLocks.h>
#include <kern/sched.h>
#include <pexpert/pexpert.h>
#include "BootResetCore.h"

// The devicetree's IMEM window: qcom,msm-imem@146bf000 reg = <0x146bf000 0x1000>.
enum {ImemBase = 0x146bf000, ImemSize = 0x1000};
enum {PsciVersion = 0x84000000u, PsciFeatures = 0x8400000au, PsciSystemReset = 0x84000009u,
      PsciSystemReset2_32 = 0x84000012u, PsciSystemReset2_64 = 0xc4000012u};

// SPMI PMIC arbiter and PM8150 PON, from this board's own devicetree
// (uefi-alioth/Resources/DTBs/alioth.dts qcom,spmi@c440000):
//   reg = <0xc440000 0x1100  0xc600000 0x2000000  0xe600000 0x100000  0xe700000 0xa0000
//          0xc40a000 0x26000>   reg-names = core chnls obsrvr intr cnfg, qcom,ee = <0>
// The register offsets inside a lane and the APID lookup are the ones the sweet board's
// working SPMI writer uses (ARCHIVE/UniXNU/out/alioth/ios-sweet-0081/diag-markers-v36/src/
// bsd/dev/sweet_spmi.c), which was verified on this same phone.
enum {SpmiCore = 0x0c440000, SpmiCoreSize = 0x2000, SpmiObs = 0x0e600000, SpmiObsSize = 0x10000,
      SpmiCnfg = 0x0c40a000, SpmiCnfgSize = 0x1000, SpmiChnls = 0x0c600000};
enum {SpmiApidMap = 0x900, SpmiOwner = 0x700, SpmiMaxApids = 512};
enum {SpmiCmd = 0x00, SpmiStatus = 0x08, SpmiWdata0 = 0x10, SpmiRdata0 = 0x18};
enum {SpmiOpExtWritel = 0, SpmiStatusDone = 1, SpmiStatusErr = 0x0e, SpmiPollLimit = 1000};
// QPNP_PON_SOFT_RB_SPARE at SPMI address 0x88f on SID 0 (PPID 0x08): the PMIC's own copy of
// the restart reason. GEN2 parts keep it in bits 7:1, older parts in bits 7:2, so the subtype
// at 0x805 decides the shift. Bootloader = 2 (enum pon_restart_reason).
enum {PonSubtype = 0x805, PonSpare = 0x88f, PonGen2Subtype = 0x04, PonReasonBootloader = 0x02};
enum {SpmiObsStride = 0x80, SpmiChnlStride = 0x10000};

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

// The PMIC half of the restart reason. Reads go through the observer window, which has no
// ownership rule; writes go through the read/write channel, which only accepts a command
// when this EE owns the APID. An EXT_WRITEL issued on the observer lane completes and
// returns success but never reaches the slave, so the two paths are not interchangeable.
// Only one lane is ever written in a boot, and chnls is 32 MiB against an 8 MiB io_submap,
// so the lane is mapped on first use rather than the window.
static IOLock *pon_lock;
static vm_offset_t pon_core, pon_obs, pon_cnfg, pon_lane;
static unsigned pon_count;
static int pon_state;            // 0 untried, 1 ready, -1 unavailable
static int pon_lane_apid = -1;

static uint32_t spmi_rd(vm_offset_t base, unsigned off) {
  const uint32_t value = *(volatile uint32_t *)(base + off);
  __asm__ volatile("dmb oshld" ::: "memory");
  return value;
}

// The APID that carries this PPID and is owned by EE 0, or -1. Reads tolerate a foreign
// owner and writes do not, so the lookup for a write rejects rather than falls back.
static int pon_find_apid(unsigned ppid) {
  for (unsigned i = 0; i < pon_count; ++i) {
    const uint32_t map = spmi_rd(pon_core, SpmiApidMap + 4 * i);
    if (!map || ((map >> 8) & 0xfff) != ppid) continue;
    if ((spmi_rd(pon_cnfg, SpmiOwner + 4 * i) & 7u) == 0) return (int)i;
  }
  return -1;
}

static int pon_ready(char *text, unsigned cap) {
  if (pon_state) return pon_state > 0 ? 0 : 1;
  pon_state = -1;
  pon_lock = IOLockAlloc();
  if (!pon_lock) {
    snprintf(text, cap, "pon: no lock");
    return 1;
  }
  pon_core = ml_io_map_unmappable((vm_offset_t)SpmiCore, (vm_size_t)SpmiCoreSize, 7);
  pon_cnfg = ml_io_map_unmappable((vm_offset_t)SpmiCnfg, (vm_size_t)SpmiCnfgSize, 7);
  pon_obs = ml_io_map_unmappable((vm_offset_t)SpmiObs, (vm_size_t)SpmiObsSize, 7);
  if (!pon_core || !pon_cnfg || !pon_obs) {
    snprintf(text, cap, "pon: map refused core=%d cnfg=%d obs=%d", pon_core ? 1 : 0, pon_cnfg ? 1 : 0, pon_obs ? 1 : 0);
    return 1;
  }
  const uint32_t version = spmi_rd(pon_core, 0);
  pon_count = spmi_rd(pon_core, 4) & 0x7ffu;
  if (version < 0x50000000u || version >= 0x70000000u || !pon_count || pon_count > SpmiMaxApids) {
    snprintf(text, cap, "pon: arbiter %08x apids=%u unsupported", version, pon_count);
    return 1;
  }
  pon_state = 1;
  return 0;
}

// One byte through the observer lane. 0 = read.
static int pon_read(unsigned addr, uint8_t *out) {
  const int apid = pon_find_apid((addr >> 8) & 0xfff);
  if (apid < 0) return 1;
  const unsigned off = SpmiObsStride * (unsigned)apid;
  *((volatile uint32_t *)(pon_obs + off + SpmiCmd)) = ((uint32_t)1u << 27) | ((addr & 0xffu) << 4);
  __asm__ volatile("dsb osh" ::: "memory");
  for (unsigned i = 0; i < SpmiPollLimit; ++i) {
    const uint32_t status = spmi_rd(pon_obs, off + SpmiStatus);
    if (status & SpmiStatusDone) {
      if (status & SpmiStatusErr) return 2;
      *out = (uint8_t)spmi_rd(pon_obs, off + SpmiRdata0);
      return 0;
    }
    IOSleep(1);
  }
  return 3;
}

// One byte through the read/write lane. 0 = written. The lane stays mapped for the boot:
// nothing unmaps it again, and the mapping is one 64 KiB page of an 8 MiB submap.
static int pon_write(unsigned addr, uint8_t value) {
  const int apid = pon_find_apid((addr >> 8) & 0xfff);
  if (apid < 0) return 1;
  if (pon_lane_apid != apid) {
    if (pon_lane_apid >= 0) return 4;   // one lane per boot is all this path needs
    pon_lane = ml_io_map_unmappable((vm_offset_t)(SpmiChnls + SpmiChnlStride * (unsigned)apid), (vm_size_t)SpmiChnlStride, 7);
    if (!pon_lane) return 5;
    pon_lane_apid = apid;
  }
  *((volatile uint32_t *)(pon_lane + SpmiWdata0)) = value;
  __asm__ volatile("dsb osh" ::: "memory");
  *((volatile uint32_t *)(pon_lane + SpmiCmd)) = (uint32_t)SpmiOpExtWritel << 27 | ((addr & 0xffu) << 4);
  __asm__ volatile("dsb osh" ::: "memory");
  for (unsigned i = 0; i < SpmiPollLimit; ++i) {
    const uint32_t status = spmi_rd(pon_lane, SpmiStatus);
    if (status & SpmiStatusDone) return (status & SpmiStatusErr) ? 2 : 0;
    IOSleep(1);
  }
  return 3;
}

// Records bootloader in the PM8150 spare cell and reads it back. 0 = taken. The subtype is
// read rather than assumed, because the wrong shift records a reason the bootloader ignores.
static int pon_mark_bootloader(char *text, unsigned cap) {
  if (pon_ready(text, cap)) return 1;
  IOLockLock(pon_lock);
  uint8_t subtype = 0, before = 0, after = 0;
  int rc = pon_read(PonSubtype, &subtype);
  if (rc) {
    IOLockUnlock(pon_lock);
    snprintf(text, cap, "pon: subtype read rc=%d", rc);
    return 2;
  }
  const unsigned shift = subtype >= PonGen2Subtype ? 1u : 2u;
  const uint8_t mask = (uint8_t)(0xffu << shift), want = (uint8_t)(PonReasonBootloader << shift);
  (void)pon_read(PonSpare, &before);
  rc = pon_write(PonSpare, (uint8_t)((before & (uint8_t)~mask) | want));
  if (!rc) rc = pon_read(PonSpare, &after);
  IOLockUnlock(pon_lock);
  const int taken = !rc && (after & mask) == want;
  snprintf(text, cap, "pon: subtype %02x shift %u spare %02x -> %02x want %02x rc=%d %s", subtype, shift, before, after, want, rc,
           taken ? "ok" : "NOT TAKEN");
  return taken ? 0 : 3;
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
  ml_io_unmap((vm_offset_t)imem, ImemSize);
  // The PMIC cell is recorded alongside IMEM, the pair the vendor kernel writes. Its own
  // refusal is reported but does not fail the mark: IMEM alone is the cell the bootloader
  // reads, and a missing PMIC is worth knowing rather than worth aborting a boot over.
  char pon[128] = "";
  const int pon_rc = pon_mark_bootloader(pon, sizeof(pon));
  snprintf(text, cap, "mark: %08x -> %08x rc=%d | %s pon_rc=%d", before, after, rc, pon, pon_rc);
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
  ml_io_unmap((vm_offset_t)imem, ImemSize);
  // The PMIC cell must be written before the reset, and its refusal must be visible in the
  // text that only survives when the reset does not happen. It does not stop the reset.
  char pon[128] = "";
  const int pon_rc = pon_mark_bootloader(pon, sizeof(pon));
  snprintf(text, cap, "reset: %08x -> %08x rc=%d | %s pon_rc=%d", before, after, rc, pon, pon_rc);
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
