// SPDX-License-Identifier: BSD-2-Clause-Patent
// Default-off staged bring-up of pipa's Adreno 650 power and clocks, after UniXNU-sheng's GpuPowerProbe.cpp (Adreno 740).
// `pipa-gpu=<n>`:
//   1  read the GCC GPU branches, GPUCC (GDSCs and branches), the GPU SMMU's global space 0 and RBBM_STATUS, each only when the
//      previous stage showed its prerequisite on (GpuPowerBoard.h gpb_run); no register is written
//   2  as 1, plus the gcc-sm8250 probe's one write: gcc_gpu_cfg_ahb_clk's enable bit, if clear
//   3  as 2, plus gdsc_enable for gpu_cx_gdsc if CX is off, and the GPU SMMU's clocks (gpu_cc_ahb_clk, the HLOS vote of its SMMU
//      clock); GX and the SMMU are never programmed
//   5  as 3, plus the AOP command DB read and searched for the ARC (gfx/mx/cx levels) and BCM (SH0, MC0, ACV, CN0) resources the
//      GMU's votes are built from, and the GMU and RSCC state words if the GMU clocks already run; nothing more is written
//   6  as 5, with the GMU clocks brought up first (GPLL0 GPU votes, gpu_cc_gmu_clk_src at 200 MHz, the five GMU branches); the GMU
//      itself stays in reset, no GMU, RSCC or GPU register is written
//   7  as 6, then, only if the clocks came up, the SMMU had no stream mapped, the command DB gave its resources and the GMU sits in
//      reset: the GMU's own SMMU context through NativeAdreno (bank 1, ASID 1; the GMU kernel stream 5 under its DT mask 0x400 as
//      two exact routes, 5 on SMR 2 and 0x405 on SMR 3), a write-combined CPU view of its five regions, and the A650 cold start
//      (package/A650Gmu.c) with the embedded a650_gmu.bin and the HFI tables built from the command DB. GX stays off.
//   8  as 7, then, only after a complete cold start: GX power through the GMU (A650GmuPowerGx: the GX_BW_PERF_VOTE for the GPU
//      level `pipa-gpu-perf=<1..7>`, default 7 = 670 MHz, the top one, where Linux starts too; then the OOB GPU_SET request), and
//      with GX on the GX GDSC, RBBM_STATUS and both GBIF halt registers, read only.
//   9  as 8, then, with GX on, the first GPU command, after UniXNU-tb128fu's GPU probe mode 7 (naContext, naHwInit, naCp): the
//      GPU's own SMMU context through NativeAdreno (bank 0 / ASID 2, the GPU's streams 0, 1, 0x400 and 0x401 of its DT iommus
//      <0 0x401> on SMR 0, 1, 4 and 5; non-coherent table walks), Linux v6.6 hw_init for the A650 (clock gating and APRIV),
//      the SQE (a650_sqe.fw without its first dword), ring 0 with CP_RB_CNTL NO_UPDATE, CP_ME_INIT, then one CP_MEM_WRITE of
//      a marker into the scratch page that the CPU reads back.
//  10  as 9, then the zap shader (a650_zap.elf of the vendor partition through PAS 13 into the DT's pil_gpu_region@8681a000:
//      init_image, mem_setup, segment copy, auth_and_reset, UniXNU-tb128fu's zapLoad) and CP_SET_SECURE_MODE(0) through the
//      ring, then the GPU interrupt (DT kgsl_3d0_irq: SPI 300, level high) through ArmCpu and a CP_NOP fence that must raise it.
//  11  as 10, then the GPU for user clients, after UniXNU-tb128fu's naPublish / naSelfTest: NativeAdreno's submission object on
//      ring 0, the CP always-on counter as the GPU clock, the device published with the A650's facts (kDeviceInfo), and two
//      rounds of the user-client path from the kernel (open, BOs, an IB of three CP_MEM_WRITEs, its fence, close); then a row
//      every 2 s with the interrupts, fence waits, submissions and user clients, and the CP's state once it stops moving.
//  12  as 11, then the staged accelerator for the original Metal discovery (UniXNU-sheng's publishMetal, UniXNU-tb128fu's
//      naMetal); the Metal plugin and Turnip come from the GPU addon (sheng-gpu-addon=1) and the owner trust module.
// The rows below go to the kernel log whenever they change (BootRamoops.c). The sequence starts 5 s after the late probe, so the
// display and the log thread already run if a read hangs the bus. Modes 1-12 ran on the tablet on 2026-10-24; until APRIV was set
// every UCHE read ended in UCHE_OOB_ACCESS and Metal's frames were all zero (see the README).
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/c++/OSSharedPtr.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include <string.h>
extern "C" {
#include "GpuPowerBoard.h"
#include "A650Gmu.h"
extern const uint8_t native_gpu_gmu_firmware[] __attribute__((weak_import));
extern const uint32_t native_gpu_gmu_firmware_size __attribute__((weak_import));
extern const uint8_t native_gpu_sqe_firmware[] __attribute__((weak_import));
extern const uint32_t native_gpu_sqe_firmware_size __attribute__((weak_import));
extern const uint8_t native_gpu_zap_firmware[] __attribute__((weak_import));
extern const uint32_t native_gpu_zap_firmware_size __attribute__((weak_import));
#include "AdrenoPM4.h"
// The physical range XNU manages (osfmk/arm/pmap/pmap.c), the interrupt state, and ArmCpu's SPI attach.
extern uint64_t vm_first_phys, vm_last_phys;
boolean_t ml_set_interrupts_enabled(boolean_t);
boolean_t ml_get_interrupts_enabled(void);
boolean_t ml_at_interrupt_context(void);
int get_preemption_level(void);
IOReturn armcpu_spi_attach(uint32_t intid, bool edge, void (*handler)(void *), void *context, uint32_t *readback, uint64_t *router);
// The staged AGXAccelerator (drivers/Adreno650/NativeAdrenoAccelerator.cpp, build-adreno740-accelerator.py --component Adreno650): its static
// initializer runs only after the original IOGPUFamily kext has loaded, because the parent metaclass does not exist before that.
// The staged AGXAccelerator is an IOAccelerator subclass and this kernel has no
// IOAccelerator class at all (IOAcceleratorFamily2 is absent from this XNU tree), so mode 12
// is compiled out. Modes 1-11 never touch it.
#ifndef PIPA_GPU_ACCELERATOR
#define PIPA_GPU_ACCELERATOR 0
#endif
#if PIPA_GPU_ACCELERATOR
extern void native_adreno_accelerator_static_init(void) __asm__("__GLOBAL__sub_I_NativeAdrenoAccelerator.cpp");
IOService *native_adreno_accelerator_allocate(void);
unsigned native_adreno_accelerator_instance_size(void);
#endif
kern_return_t OSKextLoadKextWithIdentifier(const char *);
extern kmod_info_t g_kernel_kmod_info;
// Fence waits that slept until a GPU interrupt or spun.
volatile uint64_t native_gpu_idle_sleeps, native_gpu_idle_spins;
}
#include "NativeAdrenoGmuContext.h"
#include "NativeAdrenoGpuContext.h"
#include "NativeAdrenoRing.h"
#include "NativeAdrenoCpInit.h"
#include "NativeAdrenoIrq.h"
#include "NativeAdrenoSubmit.h"
#include "NativeAdrenoTimer.h"
#include "NativeAdrenoABI.h"
#include "NativeAdrenoUserClient.h"
extern "C" {
NativeAdrenoVm *NativeAdrenoGpuContextVm();
IOReturn native_adreno_publish(NativeAdrenoVm *, NativeAdrenoSubmit *, NativeAdrenoRing *, const NativeAdrenoRingClock *,
                               const NativeAdrenoTimer *, const NativeAdrenoAbiDeviceInfo *, uint32_t, uint32_t);
uint32_t native_adreno_publish_stats(uint32_t *);
IOService *native_adreno_published_service();
extern volatile uint32_t native_adreno_pool_stats[8];
}

// Rows (at most 119 characters each, as the log copies them): 0 summary, 1 GCC, 2 GPUCC, 3 SMMU, 4-5 GMU clocks, 6-7 GMU state,
// 8 command DB ARC entries, 9 its BCM entries, 10-12 the gfx, mx and cx level lists, 13 the GMU's SMMU context, 14 the cold
// start's load, 15 its HFI messages, 16 the GMU state after it, 17 GX power, 18 the GPU's SMMU context, 19 the CP steps, 20 the
// CP's state after them, 21 the zap shader and the secure-mode exit, 22 the GPU interrupt, 23 the publication and the user-client
// rounds, 24 the Metal accelerator, 25 the live counters, 26-27 the CP's state once it stops moving, 28 the GPU SMMU's faults,
// 29 the UCHE and UBWC registers as read back.
enum {kRowSummary, kRowGcc, kRowGpucc, kRowSmmu, kRowClk, kRowClk2, kRowGmu, kRowGmu2, kRowDbArc, kRowDbBcm, kRowLevels,
      kRowContext = kRowLevels + 3, kRowLoad, kRowHfi, kRowAfter, kRowGx, kRowGpuCtx, kRowCp, kRowCpState, kRowZap, kRowIrq,
      kRowPublish, kRowMetal, kRowLive, kRowHang, kRowHang2, kRowFaults, kRowUche, kRowTiling, kRows};
extern "C" {
char native_gpu_rows[kRows][120];
volatile uint32_t native_gpu_changes;
volatile uint32_t native_gpu_step;
struct gpb_result native_gpu_result;
struct gpb_gmu_clock_on native_gpu_clocks;
struct gpb_gmu_state native_gpu_gmu_state;
}

namespace {
unsigned gMode;
constexpr uint64_t kPhys[GPB_WINDOWS] = {GPB_GCC_GPU_PAGE, GPB_GCC_VOTE_PAGE, GPB_GPUCC_BASE, GPB_SMMU_BASE, GPB_GPU_BASE,
                                         GPB_GMU_BASE, GPB_RSCC_BASE};
constexpr uint64_t kLen[GPB_WINDOWS] = {0x1000, 0x1000, GPB_GPUCC_SIZE, GPB_SMMU_GR0_SIZE, GPB_GPU_WINDOW, GPB_GMU_SIZE,
                                        GPB_RSCC_SIZE};
OSSharedPtr<IOMemoryMap> gMaps[GPB_WINDOWS];
uintptr_t gVa[GPB_WINDOWS];

bool mapWindow(unsigned w) {
    IODeviceMemory *memory = IODeviceMemory::withRange(kPhys[w], kLen[w]);
    if (!memory) return false;
    gMaps[w] = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    gVa[w] = gMaps[w] ? static_cast<uintptr_t>(gMaps[w]->getVirtualAddress()) : 0;
    return gVa[w] != 0;
}
void delayUs(void *, unsigned us) {IODelay(us);}
int rd(void *, unsigned w, uint32_t off, uint32_t *out) {
    if (w >= GPB_WINDOWS || !gVa[w] || off >= kLen[w] || (off & 3)) return -1;
    *out = *reinterpret_cast<volatile uint32_t *>(gVa[w] + off);
    return 0;
}
// The registers a mode may write; everything else (the GMU, RSCC, GPU and SMMU windows included) is refused.
bool writable(unsigned w, uint32_t off) {
    if (w == GPB_WIN_GCC_GPU && off == GPB_GCC_GPU_CFG_AHB) return gMode >= 2;
    if (w == GPB_WIN_GPUCC && (off == GPB_CC_CX_GDSCR || off == GPB_CC_AHB_CLK || off == GPB_CC_SMMU_VOTE_CLK)) return gMode >= 3;
    if (gMode < 6) return false;
    if (w == GPB_WIN_GCC_VOTE) return off == GPB_GCC_PLL_VOTE;
    if (w == GPB_WIN_GCC_GPU) return off == GPB_GCC_DDRSS_GPU_AXI || off == GPB_GCC_GPU_MEMNOC_GFX;
    if (w == GPB_WIN_GPUCC) return off == GPB_CC_GMU_RCGR || off == GPB_CC_GMU_RCGR + 4 || off == GPB_CC_CX_GMU_CLK || off == GPB_CC_CXO_CLK;
    return false;
}
int wr(void *, unsigned w, uint32_t off, uint32_t value) {
    if (w >= GPB_WINDOWS || !gVa[w] || off >= kLen[w] || (off & 3) || !writable(w, off)) return -1;
    *reinterpret_cast<volatile uint32_t *>(gVa[w] + off) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}

void changed() {native_gpu_changes = native_gpu_changes + 1;}
void rowsBasic(const gpb_result &r) {
    snprintf(native_gpu_rows[kRowSummary], sizeof native_gpu_rows[0], "GPU m%u gcc=%s cc=%s smmu=%s gpu=%s io=%d cx%u clocks%u ahb%u",
             r.mode, gpb_state_text(r.gcc), gpb_state_text(r.cc), gpb_state_text(r.smmu), gpb_state_text(r.gpu), r.io_error,
             r.powered_cx, r.powered_clocks, r.enabled_ahb);
    snprintf(native_gpu_rows[kRowGcc], sizeof native_gpu_rows[0], "GCC cfg_ahb %08x>%08x memnoc %08x dvm %08x misc %08x gpll0 votes %08x",
             r.cfg_ahb, r.cfg_ahb_after, r.memnoc, r.dvm, r.misc, r.pll_vote);
    snprintf(native_gpu_rows[kRowGpucc], sizeof native_gpu_rows[0], "GPUCC cx %08x>%08x hw %08x gx %08x ahb %08x smmu %08x>%08x gmu %08x",
             r.cx_before, r.cx_gdscr, r.cx_hwctrl, r.gx_gdscr, r.cc_ahb, r.cc_smmu_before, r.cc_smmu, r.cc_gmu);
    snprintf(native_gpu_rows[kRowSmmu], sizeof native_gpu_rows[0], "SMMU cr0 %08x id %08x %08x %08x gfsr %08x smr %u first %d %08x s2cr %08x rbbm %08x",
             r.scr0, r.id0, r.id1, r.id2, r.gfsr, r.smr_valid, r.smr_first == 0xffffffffu ? -1 : int(r.smr_first), r.smr_first_val,
             r.s2cr_first_val, r.rbbm_status);
    changed();
}

// The command DB, copied with aligned 32-bit reads (it is mapped as device memory), and the lookups mode 7 builds from:
// gfx.lvl, mx.lvl, cx.lvl, gmxc.lvl, SH0, MC0, ACV, CN0.
alignas(8) uint8_t gCmdDb[GPB_CMD_DB_BYTES];
gpb_cmd_db_entry gDbFound[8];
int gDbRc[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
void commandDb() {
    native_gpu_step = 50;
    IODeviceMemory *memory = IODeviceMemory::withRange(GPB_CMD_DB_BASE, GPB_CMD_DB_BYTES);
    OSSharedPtr<IOMemoryMap> map;
    if (memory) {
        map = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly), OSNoRetain);
        memory->release();
    }
    if (!map) {snprintf(native_gpu_rows[kRowDbArc], sizeof native_gpu_rows[0], "CMDDB nomap"); changed(); return;}
    const volatile uint32_t *src = reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());
    for (uint32_t i = 0; i < GPB_CMD_DB_BYTES / 4; ++i) {
        uint32_t v = src[i];
        memcpy(gCmdDb + 4 * i, &v, 4);
    }
    static const char *const names[8] = {"gfx.lvl", "mx.lvl", "cx.lvl", "gmxc.lvl", "SH0", "MC0", "ACV", "CN0"};
    gpb_cmd_db_entry *found = gDbFound;
    int *rc = gDbRc;
    for (unsigned i = 0; i < 8; ++i) rc[i] = gpb_cmd_db_find(gCmdDb, sizeof gCmdDb, names[i], &found[i]);
    // Row 8: version, magic and the ARC entries; row 9: the BCM entries. Each as name:address/aux bytes, or name:status.
    for (unsigned half = 0; half < 2; ++half) {
        char *row = native_gpu_rows[kRowDbArc + half];
        const size_t cap = sizeof native_gpu_rows[0];
        int n = half ? snprintf(row, cap, "CMDDB bcm") : snprintf(row, cap, "CMDDB v%08x magic %08x", gpb_le32(gCmdDb), gpb_le32(gCmdDb + 4));
        for (unsigned i = 4 * half; i < 4 * half + 4 && n > 0 && size_t(n) < cap; ++i) {
            if (rc[i]) n += snprintf(row + n, cap - n, " %s:%d", names[i], rc[i]);
            else n += snprintf(row + n, cap - n, " %s:%x/%u", names[i], found[i].address, found[i].aux_bytes);
        }
    }
    // Rows 10-12: the ARC level lists (16-bit words), as a6xx_gmu.c reads them with cmd_db_read_aux_data.
    for (unsigned i = 0; i < 3; ++i) {
        char *row = native_gpu_rows[kRowLevels + i];
        const size_t cap = sizeof native_gpu_rows[0];
        int n = snprintf(row, cap, "LVL %s", names[i]);
        for (unsigned k = 0; !rc[i] && k + 1 < found[i].aux_bytes && k < 32 && n > 0 && size_t(n) < cap; k += 2)
            n += snprintf(row + n, cap - n, " %u", gpb_le16(found[i].aux + k));
    }
    changed();
}

void gmuState(const gpb_io &io, gpb_result &r, uint32_t cx_gmu, uint32_t ahb) {
    if (!gpb_gmu_regs_readable(&r, cx_gmu, ahb)) {
        snprintf(native_gpu_rows[kRowGmu], sizeof native_gpu_rows[0], "GMU not readable (cx_gmu %08x ahb %08x)", cx_gmu, ahb);
        changed();
        return;
    }
    native_gpu_step = 60;
    gpb_gmu_state &s = native_gpu_gmu_state;
    gpb_read_gmu_state(&io, &r, &s);
    snprintf(native_gpu_rows[kRowGmu], sizeof native_gpu_rows[0], "GMU sysreset %08x fw %08x gx %08x hfi %08x gen7 %08x",
             s.sysreset, s.fw_init, s.gx_status, s.hfi_status, s.general7);
    snprintf(native_gpu_rows[kRowGmu2], sizeof native_gpu_rows[0], "GMU ao_irq %08x fence %08x rscc busy %08x status %08x",
             s.ao_irq, s.fence, s.rscc_busy, s.rscc_status);
    changed();
}

// Mode 7. The GMU's SMMU context: the whole adreno SMMU window (DTS kgsl-smmu@3da0000 size 0x10000), bank 1 / ASID 1 (Linux
// gives bank 0 to the GPU), the GMU kernel stream 5 and, for its mask 0x400, 0x405 on the next SMR.
constexpr uint64_t kSmmuWindow = 0x10000, kPdcPhys = 0x0b290000, kPdcBytes = 0x10000;
constexpr unsigned kGmuBank = 1, kGmuAsid = 1, kGmuSlots[2] = {2, 3}, kGmuSids[2] = {5, 0x405};
OSSharedPtr<IOMemoryMap> gSmmuMap, gPdcMap, gPayloadMaps[5];
A650GmuDevice gDevice;
A650GmuConfig gConfig;
AdrenoHfiStartupConfig gStartup;
A650GmuStartupResult gCold;
bool gColdOk;
extern "C" {
NativeAdrenoGmuContextReport native_gpu_gmu_context;
}
void contextProgress(uint32_t step) {native_gpu_step = 80 + step;}
volatile uint8_t *gWin[A650GmuWindows];
int gmuRead(void *, unsigned w, size_t at, uint32_t *out) {
    if (w >= A650GmuWindows || !gWin[w] || (at & 3) || at + 4 > gDevice.io.bytes[w]) return 1;
    *out = *reinterpret_cast<volatile uint32_t *>(gWin[w] + at);
    return 0;
}
int gmuWrite(void *, unsigned w, size_t at, uint32_t value) {
    if (w >= A650GmuWindows || !gWin[w] || (at & 3) || at + 4 > gDevice.io.bytes[w]) return 1;
    *reinterpret_cast<volatile uint32_t *>(gWin[w] + at) = value;
    return 0;
}
int gmuBarrier(void *) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
int gmuNow(void *, uint64_t *us) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    *us = ns / 1000;
    return 0;
}
int gmuWait(void *, unsigned us) {IODelay(us); return 0;}
void gmuColdStart(const gpb_result &r, const gpb_gmu_clock_on &o) {
    char *ctx = native_gpu_rows[kRowContext], *load = native_gpu_rows[kRowLoad];
    const size_t cap = sizeof native_gpu_rows[0];
    native_gpu_step = 79;
    if (!&native_gpu_gmu_firmware || !&native_gpu_gmu_firmware_size || !native_gpu_gmu_firmware_size) {
        snprintf(ctx, cap, "GMUCTX no firmware linked"); changed(); return;
    }
    if (o.step != 5 || o.failed || r.smmu != GPB_DONE || r.smr_valid || gDbRc[0] || gDbRc[1] || gDbRc[2] || gDbRc[4] || gDbRc[5] ||
        gDbRc[6] || gDbRc[7] || native_gpu_gmu_state.sysreset != 1) {
        snprintf(ctx, cap, "GMUCTX skipped: clocks %u/%d smmu %s smr %u cmddb %d%d%d%d%d%d%d gmu reset %08x", o.step, o.failed,
                 gpb_state_text(r.smmu), r.smr_valid, gDbRc[0], gDbRc[1], gDbRc[2], gDbRc[4], gDbRc[5], gDbRc[6], gDbRc[7],
                 native_gpu_gmu_state.sysreset);
        changed();
        return;
    }
    IODeviceMemory *memory = IODeviceMemory::withRange(GPB_SMMU_BASE, kSmmuWindow);
    if (memory) {
        gSmmuMap = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
        memory->release();
    }
    if (!gSmmuMap) {snprintf(ctx, cap, "GMUCTX nomap"); changed(); return;}
    NativeAdrenoGmuContextReport &g = native_gpu_gmu_context;
    const IOReturn status = NativeAdrenoGmuContextBringupRoutes(reinterpret_cast<volatile uint8_t *>(gSmmuMap->getVirtualAddress()),
        kSmmuWindow, kGmuBank, kGmuAsid, kGmuSlots, kGmuSids, 2, contextProgress, &g);
    snprintf(ctx, cap, "GMUCTX step %u status %x kit %u/%u routes %u id2 %x program %u/%u/%u ttbr0 %llx smr %08x s2cr %08x",
             g.step, static_cast<unsigned>(status), g.program_status, g.route_status, g.routes_verified, g.id2, g.program_phase,
             g.program_readback_valid, g.program_enabled, static_cast<unsigned long long>(g.ttbr0), g.smr, g.s2cr);
    changed();
    if (status) return;
    // The five regions (icache, dummy, debug, log, hfi): the GMU maps them Normal non-cacheable, the CPU writes them combined.
    NativeAdrenoGmuPayload payloads[5];
    const IOReturn got = NativeAdrenoGmuContextPayloads(payloads);
    if (got) {snprintf(load, cap, "LOAD payloads %x", static_cast<unsigned>(got)); changed(); return;}
    A650GmuMemory views[5];
    for (unsigned i = 0; i < 5; ++i) {
        IODeviceMemory *m = IODeviceMemory::withRange(payloads[i].physical, payloads[i].bytes);
        if (m) {
            gPayloadMaps[i] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain);
            m->release();
        }
        if (!gPayloadMaps[i]) {snprintf(load, cap, "LOAD nomap payload %u", i); changed(); return;}
        views[i] = {reinterpret_cast<volatile uint32_t *>(gPayloadMaps[i]->getVirtualAddress()), size_t(payloads[i].bytes), payloads[i].iova};
    }
    IODeviceMemory *pdc = IODeviceMemory::withRange(kPdcPhys, kPdcBytes);
    if (pdc) {
        gPdcMap = OSSharedPtr<IOMemoryMap>(pdc->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
        pdc->release();
    }
    if (!gPdcMap || !gVa[GPB_WIN_GMU] || !gVa[GPB_WIN_RSCC]) {snprintf(load, cap, "LOAD nomap window"); changed(); return;}
    gWin[A650GmuCore] = reinterpret_cast<volatile uint8_t *>(gVa[GPB_WIN_GMU]);
    gWin[A650GmuRscc] = reinterpret_cast<volatile uint8_t *>(gVa[GPB_WIN_RSCC]);
    gWin[A650GmuPdc] = reinterpret_cast<volatile uint8_t *>(gPdcMap->getVirtualAddress());
    gDevice = {};
    gDevice.io = {nullptr, {A650_GMU_CORE_BYTES, A650_GMU_RSCC_BYTES, A650_GMU_PDC_BYTES}, gmuRead, gmuWrite, gmuBarrier};
    gDevice.services = {nullptr, gmuNow, gmuWait};
    gConfig = {A650_GMU_CHIP_ID, native_gpu_gmu_firmware, native_gpu_gmu_firmware_size, views[0], views[1], views[3], views[4], 100};
    // The GMU's DDR vote for the GPU (A650Gmu.c A650GmuDdrVote): off unless pipa-gpu-bw=1 (2026-10-25: with it on, neither the GPU's
    // frame times nor the scanout copy changed, so the DDR was not what held them back).
    unsigned ddrVote = 0;
    PE_parse_boot_argn("pipa-gpu-bw", &ddrVote, sizeof ddrVote);
    A650GmuDdrVote = ddrVote ? 1 : 0;
    const AdrenoHfiStartupStatus built = A650GmuBuildStartup(gDbFound[0].aux, gDbFound[0].aux_bytes, gDbFound[1].aux,
        gDbFound[1].aux_bytes, gDbFound[2].aux, gDbFound[2].aux_bytes, gDbFound[5].address, gDbFound[4].address,
        gDbFound[6].address, gDbFound[7].address, &gStartup);
    if (built) {snprintf(load, cap, "LOAD vote table %u", built); changed(); return;}
    snprintf(load, cap, "LOAD running, gpu votes %08x..%08x gmu %08x", gStartup.gpu[1].vote, gStartup.gpu[7].vote, gStartup.gmu[1].vote);
    changed();
    native_gpu_step = 90;
    const A650GmuStartupLimits limits = {1000, 100, 50000, 2000000};
    const A650GmuStatus cold = A650GmuColdStart(&gDevice, &gConfig, &gStartup, &limits, &gCold);
    native_gpu_step = cold ? 98 : 97;
    const A650GmuResult &l = gCold.load;
    snprintf(load, cap, "LOAD status %u phase %u blocks %u version %08x init %08x hfi %08x ready %u%u ao %08x cm3 %08x reset %u rsc %u",
             cold, l.phase, l.blocks, l.firmware_version, l.firmware_init_result, l.hfi_status, l.firmware_ready_observed,
             l.hfi_ready_observed, l.ao_fault_status, l.cm3_fault_status, l.reset_verified, l.rsc_stop_status);
    const AdrenoHfiStartupResult &p = gCold.protocol;
    const AdrenoHfi &q = gDevice.hfi;
    snprintf(native_gpu_rows[kRowHfi], cap, "HFI status %u stage %u acked %x error %x transport %u sent %u %llu ms skipped %u/%x/%x health %u",
             gCold.protocol_status, p.stage, p.acknowledged_stages, p.firmware_error, p.transport_status, p.published_commands,
             static_cast<unsigned long long>(p.elapsed_us / 1000), q.skipped, q.skipped_header, q.skipped_ret, gCold.health_status);
    uint32_t after[5] = {};
    gmuRead(nullptr, A650GmuCore, 0x5000 * 4, &after[0]);   // CM3_SYSRESET
    gmuRead(nullptr, A650GmuCore, 0x501c * 4, &after[1]);   // CM3_FW_INIT_RESULT
    gmuRead(nullptr, A650GmuCore, 0x5180 * 4, &after[2]);   // HFI_CTRL_STATUS
    gmuRead(nullptr, A650GmuCore, 0x50d0 * 4, &after[3]);   // SPTPRAC_PWR_CLK_STATUS (GX)
    gmuRead(nullptr, A650GmuCore, 0x9305 * 4, &after[4]);   // AO_HOST_INTERRUPT_STATUS
    snprintf(native_gpu_rows[kRowAfter], cap, "AFTER sysreset %08x init %08x hfi %08x gx %08x ao %08x", after[0], after[1], after[2],
             after[3], after[4]);
    changed();
    gColdOk = cold == A650GmuSuccess;
}

// Mode 8. GPU register reads through the GPU window (a6xx.xml dword offsets).
extern "C" {
A650GmuGxResult native_gpu_gx;
}
constexpr uint32_t kRbbmGbifHalt = 0x0016, kRbbmGbifHaltAck = 0x0017, kRbbmStatus = 0x0210, kGbifHalt = 0x3c45,
                   kGbifHaltAck = 0x3c46;
uint32_t gpuRead(uint32_t dword) {
    uint32_t v = 0xdeadbeef;
    rd(nullptr, GPB_WIN_GPU, dword * 4, &v);
    return v;
}
// The DDR and its interconnect at their top levels, voted from this kernel through the application processor's RSC, as Linux's
// rpmh-rsc driver sends an active-only request (drivers/soc/qcom/rpmh-rsc.c) and as the UEFI's RpmhDxe did at boot. The AOP's DDR
// statistics (BootRamoops.c ddr_status) showed the DDR at 1555 MHz for the whole session, where the LPDDR5 runs up to 2736 MHz,
// and the GMU's own bandwidth vote (A650GmuDdrVote) changed nothing (2026-10-25); the GPU's copies moved about 16 GB/s, which is what
// 1555 MHz carries. The vote sends MC0 and SH0 at vote_x = vote_y = 0x3fff (each BCM takes its top level) and ACV's bit 3 (sm8250's
// bcm_acv enable_mask), commit and valid set, through active TCS 0 of DRV 2 (the downstream DTS: apps_rsc drv-2 at 0x18220000,
// qcom,tcs-offset 0xd00, tcs-config ACTIVE 2, SLEEP 3, WAKE 3, CONTROL 1), waiting for each command's completion. On the tablet
// (2026-10-25, round 223524) all three completed in 0.3 ms and the scanout copy went from 2.69 to 1.66 ms, a full-screen pass from
// 2.40 to 1.91 ms, the render thread's frame from 13.3 to 10.8 ms of CPU, an app's opening from 38 to 44 frames a second. On by
// default (the DDR stays at its top level: more power while the tablet idles); pipa-ddr-vote=0 leaves the UEFI's level. The result
// goes to the row after the CP's state (kRowHang2), which a hang would reuse.
extern "C" uint64_t ml_io_map(uint64_t phys_addr, uint64_t size);
void rpmhVoteDdr() {
    unsigned vote = 1;
    PE_parse_boot_argn("pipa-ddr-vote", &vote, sizeof vote);
    if (!vote) return;
    char *row = native_gpu_rows[kRowHang2];
    const size_t cap = sizeof native_gpu_rows[0];
    const uint32_t mc0 = gDbFound[5].address, sh0 = gDbFound[4].address, acv = gDbFound[6].address;
    if (!mc0 || !sh0 || !acv) {snprintf(row, cap, "DDRV no BCM addresses %x %x %x", mc0, sh0, acv); changed(); return;}
    volatile uint32_t *drv = (volatile uint32_t *)(uintptr_t)ml_io_map(0x18220000, 0x10000);
    if (!drv) {snprintf(row, cap, "DDRV no mapping"); changed(); return;}
    auto r = [&](uint32_t off) {return drv[off / 4];};
    auto w = [&](uint32_t off, uint32_t v) {drv[off / 4] = v; __asm__ volatile("dsb sy" ::: "memory");};
    const uint32_t config = r(0x0C);                                   // DRV_PRNT_CHLD_CONFIG
    const uint32_t tcsCount = (config >> 12) & 0x3f, perTcs = (config >> 27) & 0x1f;   // drv-id 2: bits 17:12; NCPT bits 31:27
    constexpr uint32_t kTcsBase = 0xd00, kTcsStride = 672, kCmdStride = 20;
    auto tcs = [&](unsigned id, uint32_t reg) {return kTcsBase + kTcsStride * id + reg;};
    unsigned id = 0;
    for (; id < 2; ++id) if (r(tcs(id, 0x18))) break;                 // RSC_DRV_STATUS nonzero: idle
    if (id == 2 || tcsCount < 2 || perTcs < 3) {
        snprintf(row, cap, "DDRV refused: config %08x (tcs %u, cmds %u), status %08x %08x", config, tcsCount, perTcs, r(tcs(0, 0x18)),
                 r(tcs(1, 0x18)));
        changed();
        return;
    }
    const uint32_t top = (1u << 30) | (1u << 29) | (0x3fffu << 14) | 0x3fffu, acvOn = (1u << 30) | (1u << 29) | 0x8u;
    const uint32_t addrs[3] = {mc0, sh0, acv}, datas[3] = {top, top, acvOn};
    w(tcs(id, 0x1C), 0);                                                // RSC_DRV_CMD_ENABLE
    w(kTcsBase + 0x00, r(kTcsBase + 0x00) | (1u << id));                 // RSC_DRV_IRQ_ENABLE: the completion latches in IRQ_STATUS
    for (unsigned c = 0; c < 3; ++c) {
        w(tcs(id, 0x30) + kCmdStride * c, 8u | (1u << 16) | (1u << 8));  // MSGID: length 8, write, response required
        w(tcs(id, 0x34) + kCmdStride * c, addrs[c]);
        w(tcs(id, 0x38) + kCmdStride * c, datas[c]);
    }
    w(tcs(id, 0x1C), 7);
    uint32_t control = r(tcs(id, 0x14));                                 // RSC_DRV_CONTROL: AMC trigger off, AMC off, AMC on, trigger
    control &= ~(1u << 24); w(tcs(id, 0x14), control);
    control &= ~(1u << 16); w(tcs(id, 0x14), control);
    w(tcs(id, 0x14), 1u << 16);
    w(tcs(id, 0x14), (1u << 16) | (1u << 24));
    unsigned polls = 0;
    for (; polls < 2000 && !(r(kTcsBase + 0x04) & (1u << id)); ++polls) IODelay(5);   // RSC_DRV_IRQ_STATUS
    uint32_t status[3];
    for (unsigned c = 0; c < 3; ++c) status[c] = r(tcs(id, 0x3C) + kCmdStride * c);
    w(kTcsBase + 0x08, 1u << id);                                        // RSC_DRV_IRQ_CLEAR
    w(tcs(id, 0x1C), 0);
    w(kTcsBase + 0x00, r(kTcsBase + 0x00) & ~(1u << id));
    snprintf(row, cap, "DDRV tcs %u of %u (%u cmds) polls %u cmd status %05x %05x %05x | aux mc0 %02x%02x%02x%02x w%u", id, tcsCount,
             perTcs, polls, status[0], status[1], status[2], gDbFound[5].aux_bytes > 3 ? gDbFound[5].aux[3] : 0,
             gDbFound[5].aux_bytes > 2 ? gDbFound[5].aux[2] : 0, gDbFound[5].aux_bytes > 1 ? gDbFound[5].aux[1] : 0,
             gDbFound[5].aux_bytes ? gDbFound[5].aux[0] : 0,
             gDbFound[5].aux_bytes > 5 ? gDbFound[5].aux[4] | gDbFound[5].aux[5] << 8 : 0);
    changed();
}

void gxOn() {
    unsigned perf = 7;   // 670 MHz, the top level (it was board.toml's pipa-gpu-perf=7 until 2026-10-25)
    PE_parse_boot_argn("pipa-gpu-perf", &perf, sizeof perf);
    if (perf < 1 || perf > 7) perf = 7;
    native_gpu_step = 100;
    A650GmuGxResult &g = native_gpu_gx;
    const A650GmuStatus status = A650GmuPowerGx(&gDevice, perf, &g);
    uint32_t gdscr = 0;
    rd(nullptr, GPB_WIN_GPUCC, GPB_CC_GX_GDSCR, &gdscr);
    char *row = native_gpu_rows[kRowGx];
    const size_t cap = sizeof native_gpu_rows[0];
    if (!g.gx_on) {
        snprintf(row, cap, "GX status %u perf %u vote %u/%x oob %u info %08x gx %08x>%08x gdscr %08x off", status, perf,
                 g.vote_status, g.vote_error, g.oob_polls, g.oob_info, g.gx_before, g.gx_after, gdscr);
    } else {
        native_gpu_step = 101;
        snprintf(row, cap, "GX on perf %u vote %u oob %u gx %08x>%08x gdscr %08x rbbm %08x gbif halt %x/%x rbbm halt %x/%x", perf,
                 g.vote_status, g.oob_polls, g.gx_before, g.gx_after, gdscr, gpuRead(kRbbmStatus), gpuRead(kGbifHalt),
                 gpuRead(kGbifHaltAck), gpuRead(kRbbmGbifHalt), gpuRead(kRbbmGbifHaltAck));
    }
    changed();
    rpmhVoteDdr();
}

// Mode 9. GPU register access for NativeAdreno (dword indices) through the GPU window.
extern "C" {
extern volatile uint32_t native_adreno_readonly_imports_owned;
extern volatile uint32_t native_adreno_smmu_coherent_walk, native_adreno_wc_enabled, native_adreno_written_imports_owned,
    native_adreno_pool_mib;
NativeAdrenoGmuContextReport native_gpu_context;
uint32_t native_gpu_cp[24];
}
constexpr unsigned kGpuBank = 0, kGpuAsid = 2, kGpuSlots[4] = {0, 1, 4, 5}, kGpuSids[4] = {0, 1, 0x400, 0x401};
NativeAdrenoGpuBuffer gNb[5];
OSSharedPtr<IOMemoryMap> gNbMaps[5];
volatile uint8_t *gNbCpu[5];
NativeAdrenoRing *gRing;
NativeAdrenoCpInit *gCpInit;
int naWrite(void *, uint32_t reg, uint32_t value) {
    if (reg >= GPB_GPU_WINDOW / 4 || !gVa[GPB_WIN_GPU]) return 1;
    *reinterpret_cast<volatile uint32_t *>(gVa[GPB_WIN_GPU] + reg * 4) = value;
    __asm__ volatile("dsb sy" ::: "memory");
    return 0;
}
int naRead(void *, uint32_t reg, uint32_t *value) {
    if (reg >= GPB_GPU_WINDOW / 4 || !gVa[GPB_WIN_GPU]) return 1;
    *value = *reinterpret_cast<volatile uint32_t *>(gVa[GPB_WIN_GPU] + reg * 4);
    return 0;
}
int naBarrier(void *) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
// The kernel buffers are Normal non-cacheable for the GPU and write-combined for the CPU: ordering only, no cache maintenance.
int naPublish(void *, const volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
int naAcquire(void *, volatile uint32_t *, size_t) {__asm__ volatile("dsb sy" ::: "memory"); return 0;}
uint64_t naNow(void *) {uint64_t us = 0; gmuNow(nullptr, &us); return us;}
// The fence waits' pause: with the GPU interrupt attached (mode 10) a waiter that may block sleeps until the next GPU interrupt or
// 100 us, whichever is first; before that, or where it may not block, it spins 20 us (UniXNU-tb128fu, sheng int-143).
volatile bool gIrqAttached;
volatile uint32_t gIrqCount, gIrqSeen, gIrqLast, gIrqRepeat, gIrqMasked;
int naIdle(void *) {
    if (gIrqAttached && ml_get_interrupts_enabled() && !ml_at_interrupt_context() && get_preemption_level() == 0) {
        assert_wait_timeout(event_t(&gIrqCount), THREAD_UNINT, 100, NSEC_PER_USEC);
        thread_block(THREAD_CONTINUE_NULL);
        native_gpu_idle_sleeps = native_gpu_idle_sleeps + 1;
    } else {
        IODelay(20);
        native_gpu_idle_spins = native_gpu_idle_spins + 1;
    }
    return 0;
}
void naProgress(uint32_t step) {native_gpu_step = 110 + step;}
// No AHB fence status: the A650's GMU runs without IFPC here, so the GPU domain stays up and a WPTR write is not dropped.
NativeAdrenoRingIo gRio{nullptr, naWrite, naRead, naBarrier, nullptr, nullptr};
NativeAdrenoRingCache gCache{nullptr, naPublish, naAcquire};
NativeAdrenoRingClock gClock{nullptr, naNow, naIdle};
void gw(uint32_t dword, uint32_t value) {naWrite(nullptr, dword, value);}
void gw64(uint32_t dword, uint64_t value) {gw(dword, uint32_t(value)); gw(dword + 1, uint32_t(value >> 32));}
uint32_t smmuRead(uint32_t at) {return *reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress() + at);}
void smmuWrite(uint32_t at, uint32_t v) {
    *reinterpret_cast<volatile uint32_t *>(gSmmuMap->getVirtualAddress() + at) = v;
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t smmuBank0() {
    // IDR1: PAGESIZE (bit 31) and NUMPAGENDXB (bits 30:28): the context banks start after 2^(NUMPAGENDXB + 1) pages (0x8000 here).
    const uint32_t id1 = smmuRead(0x24);
    return ((id1 >> 31) ? 0x10000u : 0x1000u) << (((id1 >> 28) & 7) + 1);
}

// The GPU's context: bank 0 (arm-smmu-qcom gives the GPU bank 0), the four streams of iommus <0 0x401> (stream 0 under the mask
// 0x401) on the SMR slots the GMU does not use, a 2 GiB aperture of 4 KiB pages and NativeAdreno's five kernel buffers. The table
// walks are non-coherent (cleaned tables) as on UniXNU-tb128fu, which works whether or not the walker snoops.
bool gpuContext() {
    char *row = native_gpu_rows[kRowGpuCtx];
    const size_t cap = sizeof native_gpu_rows[0];
    native_gpu_step = 110;
    native_adreno_smmu_coherent_walk = 0;
    native_adreno_wc_enabled = 1;
    native_adreno_written_imports_owned = 1;
    // The Metal frontend cleans the read-only imports (the apps' CPU-drawn IOSurfaces) whose seed changed before each commit, so the
    // kernel does not clean all of them at every submission (2026-10-25: about 7 MB and 1.2 ms a submission while apps opened);
    // pipa-gpu-ro-own=0 brings the kernel's cleaning back.
    unsigned readOnlyOwned = 1;
    PE_parse_boot_argn("pipa-gpu-ro-own", &readOnlyOwned, sizeof readOnlyOwned);
    native_adreno_readonly_imports_owned = readOnlyOwned ? 1 : 0;
    // MiB, the BO pool taken before memory fragments; pipa-gpu-pool=0 for scattered BOs only. 768 since Turnip keeps freed BOs for reuse
    // (tu_knl_darwin.cc BO cache, up to 48 MiB): at 384 the pool ran at 340 of 384 MiB without it (2026-10-25), and a BO past the pool is
    // scattered pages whose write-combine change and its undoing cost milliseconds each.
    unsigned pool = 768;
    PE_parse_boot_argn("pipa-gpu-pool", &pool, sizeof pool);
    native_adreno_pool_mib = pool;
    NativeAdrenoGmuContextReport &g = native_gpu_context;
    const IOReturn ctx = NativeAdrenoGpuContextBringupRoutes(reinterpret_cast<volatile uint8_t *>(gSmmuMap->getVirtualAddress()),
        kSmmuWindow, kGpuBank, kGpuAsid, kGpuSlots, kGpuSids, 4, naProgress, &g);
    // CB0 ACTLR without CPRE (arm_mmu500_reset, MMU-500 errata 841119/826419), then TLBIALL, TLBSYNC and TLBSTATUS of the bank.
    uint32_t actlr = 0;
    if (!ctx) {
        const uint32_t bank0 = smmuBank0();
        actlr = smmuRead(bank0 + 0x4);
        smmuWrite(bank0 + 0x4, actlr & ~2u);
        smmuWrite(bank0 + 0x618, 0);
        smmuWrite(bank0 + 0x7f0, 0);
        for (unsigned i = 0; i < 1000 && (smmuRead(bank0 + 0x7f4) & 1); ++i) IODelay(1);
    }
    snprintf(row, cap, "GPUCTX step %u status %x kit %u/%u routes %u program %u/%u/%u ttbr0 %llx smr %08x s2cr %08x actlr %x pool %u",
             g.step, static_cast<unsigned>(ctx), g.program_status, g.route_status, g.routes_verified, g.program_phase,
             g.program_readback_valid, g.program_enabled, static_cast<unsigned long long>(g.ttbr0), g.smr, g.s2cr, actlr, pool);
    changed();
    return ctx == kIOReturnSuccess;
}

// CP register protection of the A650: Linux v6.6 a650_protect, encoded as A6XX_PROTECT_RDONLY (start | (count - 1) << 18) and
// A6XX_PROTECT_NORDWR (that | 1 << 31); the last entry goes into CP_PROTECT(47) with an unbounded range.
constexpr uint32_t rdonly(uint32_t reg, uint32_t len) {return ((len & 0x3FFF) << 18) | (reg & 0x3FFFF);}
constexpr uint32_t nordwr(uint32_t reg, uint32_t len) {return (1u << 31) | rdonly(reg, len);}
constexpr uint32_t kProtect[] = {
    rdonly(0x00000, 0x04ff), rdonly(0x00501, 0x0005), rdonly(0x0050b, 0x02f4), nordwr(0x0050e, 0x0000), nordwr(0x00510, 0x0000),
    nordwr(0x00534, 0x0000), nordwr(0x00800, 0x0082), nordwr(0x008a0, 0x0008), nordwr(0x008ab, 0x0024), rdonly(0x008de, 0x00ae),
    nordwr(0x00900, 0x004d), nordwr(0x0098d, 0x0272), nordwr(0x00e00, 0x0001), nordwr(0x00e03, 0x000c), nordwr(0x03c00, 0x00c3),
    rdonly(0x03cc4, 0x1fff), nordwr(0x08630, 0x01cf), nordwr(0x08e00, 0x0000), nordwr(0x08e08, 0x0000), nordwr(0x08e50, 0x001f),
    nordwr(0x08e80, 0x027f), nordwr(0x09624, 0x01db), nordwr(0x09e60, 0x0011), nordwr(0x09e78, 0x0187), nordwr(0x0a630, 0x01cf),
    nordwr(0x0ae02, 0x0000), nordwr(0x0ae50, 0x032f), nordwr(0x0b604, 0x0000), nordwr(0x0b608, 0x0007), nordwr(0x0be02, 0x0001),
    nordwr(0x0be20, 0x17df), nordwr(0x0f000, 0x0bff), rdonly(0x0fc00, 0x1fff), nordwr(0x18400, 0x1fff), nordwr(0x1a800, 0x1fff),
    nordwr(0x1f400, 0x0443), rdonly(0x1f844, 0x007b), nordwr(0x1f887, 0x001b), nordwr(0x1f8c0, 0x0000),
};
constexpr unsigned kProtectCount = sizeof kProtect / sizeof kProtect[0];
static_assert(kProtectCount == 39, "a650_protect has 39 entries");

// pipa-gpu-gmem16m=1: the UCHE GMEM window at 16 MiB and reported there (the rounds before this), instead of Linux's base 0.
bool gUcheGmem16M;

// Linux v6.6 a650_hwcg (a6xx_gpu.c), as register offsets of a6xx.xml.
constexpr uint32_t kHwcg[][2] = {
    {0x00b0, 0x02222222}, {0x00b4, 0x02222220}, {0x00b8, 0x00000080}, {0x00bc, 0x0000f3cf},   // SP0 CNTL, CNTL2, DELAY, HYST
    {0x00c0, 0x02222222}, {0x00c4, 0x22222222}, {0x00c8, 0x22222222}, {0x00cc, 0x00022222},   // TP0 CNTL..CNTL4
    {0x00d0, 0x11111111}, {0x00d4, 0x11111111}, {0x00d8, 0x11111111}, {0x00dc, 0x00011111},   // TP0 DELAY..DELAY4
    {0x00e0, 0x77777777}, {0x00e4, 0x77777777}, {0x00e8, 0x77777777}, {0x00ec, 0x00077777},   // TP0 HYST..HYST4
    {0x00f0, 0x22222222}, {0x00f4, 0x01002222}, {0x00f8, 0x00002220}, {0x0100, 0x00040f00},   // RB0 CNTL, CNTL2, CCU0, RB_CCU0 HYST
    {0x0104, 0x25222022}, {0x0105, 0x00005555}, {0x0106, 0x00000011}, {0x0107, 0x00445044},   // RAC CNTL, CNTL2, DELAY, HYST
    {0x0108, 0x04222222}, {0x0111, 0x00002222}, {0x0114, 0x00222222}, {0x0117, 0x00000002},   // TSE_RAS_RBBM, MODE VFD, GPC, DELAY HLSQ_2
    {0x011b, 0x00002222}, {0x0109, 0x00004000}, {0x0112, 0x00002222}, {0x0115, 0x00000200},   // MODE HLSQ, DELAY TSE_RAS_RBBM, VFD, GPC
    {0x011c, 0x00000000}, {0x010a, 0x00000000}, {0x0113, 0x00000000}, {0x0116, 0x04104004},   // DELAY HLSQ, HYST TSE_RAS_RBBM, VFD, GPC
    {0x011d, 0x00000000}, {0x0120, 0x00000222}, {0x0121, 0x00000111}, {0x0122, 0x00000777},   // HYST HLSQ, TEX_FCHE CNTL, DELAY, HYST
    {0x010b, 0x22222222}, {0x0110, 0x00000004}, {0x010f, 0x00000002}, {0x0533, 0x00000182},   // UCHE CNTL, HYST, DELAY; ISDB_CNT
    {0x0044, 0x00000000}, {0x0042, 0x00000000}, {0x0118, 0x00000222}, {0x0119, 0x00000111},   // RAC_THRESHOLD, SP_HYST; GMU_GX CNTL, DELAY
    {0x011a, 0x00000555},                                                                     // GMU_GX HYST
};
static_assert(sizeof kHwcg / sizeof kHwcg[0] == 49, "a650_hwcg has 49 entries");

// a6xx_set_hwcg of Linux v6.6: with the SP clock off (GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL bit 0), the table or zeros, then the SP clock
// on and RBBM_CLOCK_CNTL. Without it (2026-10-24, before this) nothing that runs a shader wrote anything: the plugin's Turnip check
// filled a buffer correctly (no shader) but its compute shader wrote no word and its triangle no pixel ("T s0 r0 v512 c0 g0/4096"),
// and the UI's frames were all zero. pipa-gpu-hwcg=0 writes zeros (no gating, the clocks run) instead of the table.
void setHwcg() {
    uint32_t *n = native_gpu_cp;
    unsigned on = 1;
    PE_parse_boot_argn("pipa-gpu-hwcg", &on, sizeof on);
    n[9] = gpuRead(0x00ae);                                             // RBBM_CLOCK_CNTL as found
    gmuRead(nullptr, A650GmuCore, 0x80 * 4, &n[10]);                     // GPU_GMU_GX_SPTPRAC_CLOCK_CONTROL as found
    gmuWrite(nullptr, A650GmuCore, 0x80 * 4, n[10] & ~1u);
    for (const auto &r : kHwcg) gw(r[0], on ? r[1] : 0);
    uint32_t sptp = 0;
    gmuRead(nullptr, A650GmuCore, 0x80 * 4, &sptp);
    gmuWrite(nullptr, A650GmuCore, 0x80 * 4, sptp | 1u);
    gw(0x00ae, on ? 0x8aa8aa82 : 0);                                    // RBBM_CLOCK_CNTL
    gmuRead(nullptr, A650GmuCore, 0x80 * 4, &n[11]);
    n[12] = gpuRead(0x00ae);
}

// hw_init of Linux v6.6 a6xx_gpu.c for the A650 (a650 family, with a GMU); register offsets from a6xx.xml / a6xx_gmu.xml.
// Hardware clock gating as Linux (setHwcg), and APRIV (pipa-gpu-apriv=0 leaves it off; the ring uses NO_UPDATE either way).
void hwInit() {
    gw(0x3C45, 0); (void)gpuRead(0x3C45);                               // GBIF_HALT
    gw(0x0016, 0); (void)gpuRead(0x0016);                               // RBBM_GBIF_HALT
    gw(0xF803, 0);                                                      // RBBM_SECVID_TSB_CNTL
    gw64(0xF800, 0); gw(0xF802, 0);                                     // TSB_TRUSTED_BASE, _SIZE: no trusted range
    static const uint32_t addrMode[] = {0x842, 0xC01, 0x8601, 0x8E05, 0x9E01, 0xBE05, 0xA601, 0x9601, 0xE00, 0xAE01, 0xB601, 0xF810};
    for (uint32_t reg : addrMode) gw(reg, 1);                           // *_ADDR_MODE_CNTL: 64-bit
    setHwcg();
    unsigned gmem16m = 0;
    PE_parse_boot_argn("pipa-gpu-gmem16m", &gmem16m, sizeof gmem16m);
    gUcheGmem16M = gmem16m != 0;
    for (uint32_t reg = 0x3C03; reg <= 0x3C06; ++reg) gw(reg, 0x00071620);   // GBIF_QSB_SIDE0..3 (a650 family)
    gw(0x0011, 0x3);                                                    // RBBM_GBIF_CLIENT_QOS_CNTL
    gw(0x050B, 0xffffffff);                                             // RBBM_PERFCTR_GPU_BUSY_MASKED
    gw64(0xE05, 0x0001ffffffffffc0ull);                                 // UCHE_WRITE_RANGE_MAX
    gw64(0xE09, 0x0001fffffffff000ull);                                 // UCHE_TRAP_BASE
    gw64(0xE07, 0x0001fffffffff000ull);                                 // UCHE_WRITE_THRU_BASE
    // UCHE_GMEM_RANGE_MIN/MAX are left alone, as Linux does on the a650 family (GMEM base 0, kDeviceInfo); NativeAdreno's kernel
    // buffers now lie at 32 MiB and up, outside that window (drivers/Adreno650 NativeAdrenoGpuContext.h). With the window
    // moved to 16 MiB instead (as on UniXNU-sheng and UniXNU-tb128fu) every shader access still ended in UCHE_OOB_ACCESS
    // (2026-10-24: Turnip's check filled a buffer but its compute shader wrote nothing and its triangle drew nothing).
    if (gUcheGmem16M) {gw64(0xE0B, 0x1000000); gw64(0xE0D, 0x1000000 + 0x120000 - 1);}
    gw(0xE18, 0x804); gw(0xE17, 0x4);                                   // UCHE_FILTER_CNTL, UCHE_CACHE_WAYS
    gw(0x8C2, 0x02000140); gw(0x8C1, 0x8040362c);                       // CP_ROQ_THRESHOLDS_2/_1 (a640/a650 family)
    gw(0x8C3, 128);                                                     // CP_MEM_POOL_SIZE
    gw(0x9E00, 0x00300200);                                             // PC_DBG_ECO_CNTL (A650)
    gw(0x98D, 1);                                                       // CP_AHB_CNTL: error response
    gw(0x500, 1);                                                       // RBBM_PERFCTR_CNTL
    gw(0x8D0, 0);                                                       // CP_PERFCTR_CP_SEL(0): PERF_CP_ALWAYS_COUNT
    // a6xx_set_ubwc_config for the A650: hbb_lo 3 (highest bank bit 16, DTS qcom,highest-bank-bit), amsbc 1, rgb565 predicator 1,
    // uavflagprd_inv 2, min_acc_len 0, ubwc_mode 0.
    gw(0x8E08, 0x816); gw(0xB604, 0x6); gw(0xAE02, 0x26);               // RB, TPL1, SP _NC_MODE_CNTL
    gw(0xE01, 3u << 21);                                                // UCHE_MODE_CNTL
    gw(0x001F, (1u << 30) | 0x1fffff);                                  // RBBM_INTERFACE_HANG_INT_CNTL
    gw(0xE19, 1);                                                       // UCHE_CLIENT_PF
    gw(0xB608, 0); gw(0xB609, 0x3fe05ff4); gw(0xB60A, 0x3fa0ebee);       // TPL1_BICUBIC_WEIGHTS_TABLE_0..4 (a650 family)
    gw(0xB60B, 0x3f5193ed); gw(0xB60C, 0x3f0243f0);
    // The CX GMU counter 0 counts busy ticks; the power counter on (GMU registers).
    gmuWrite(nullptr, A650GmuCore, 0x930e * 4, 0xff000000);              // GPU_GMU_AO_GPU_CX_BUSY_MASK
    uint32_t sel = 0;
    gmuRead(nullptr, A650GmuCore, 0x5041 * 4, &sel);
    gmuWrite(nullptr, A650GmuCore, 0x5041 * 4, (sel & ~0xffu) | (1u << 5));   // GMU_CX_GMU_POWER_COUNTER_SELECT_0
    gmuWrite(nullptr, A650GmuCore, 0x5040 * 4, 1);                       // GMU_CX_GMU_POWER_COUNTER_ENABLE
    // a6xx_set_cp_protect: enable, fault on violation, last span unbounded.
    gw(0x84F, (1u << 0) | (1u << 1) | (1u << 3));                       // CP_PROTECT_CNTL
    for (unsigned i = 0; i + 1 < kProtectCount; ++i) gw(0x850 + i, kProtect[i]);
    gw(0x850 + 47, kProtect[kProtectCount - 1]);                        // CP_PROTECT(47)
    // CP_APRIV_CNTL as Linux on the A650 (hw_apriv): the CP's own instruction, ring and rptr accesses as privileged. Without it
    // (2026-10-24) every read of the shaders and the texture pipe ended in UCHE_OOB_ACCESS: Turnip's check filled a buffer (no
    // read) but its compute shader wrote nothing and its image copy nothing ("T s0 r0 v512 c0 g0/4096"), and Metal's frames were
    // all zero; with it (and linux-firmware's SQE) the same check passed ("T s0 r0 v1024 c1024 g512/0").
    unsigned apriv = 1;
    PE_parse_boot_argn("pipa-gpu-apriv", &apriv, sizeof apriv);
    if (apriv) gw(0x844, (1u << 6) | (1u << 5) | (1u << 3) | (1u << 2) | (1u << 1));
    gw(0x038, NativeAdrenoIrq::maskA6xx());                             // RBBM_INT_0_MASK: A6XX_INT_MASK
}

// The UCHE's registers as read back (UCHE_MODE_CNTL, WRITE_RANGE_MAX, WRITE_THRU_BASE, TRAP_BASE, GMEM_RANGE_MIN/MAX, 0E12,
// CLIENT_PF) and the NC_MODE_CNTL of RB, TPL1, SP and RBBM: right after hw_init, and again from the live loop when they change
// (Turnip's command streams write some of them). Shader accesses ended in UCHE_OOB_ACCESS (2026-10-24).
void ucheRow() {
    auto r64 = [](uint32_t dword) {return uint64_t(gpuRead(dword)) | uint64_t(gpuRead(dword + 1)) << 32;};
    char line[sizeof native_gpu_rows[0]];
    snprintf(line, sizeof line, "UCHE m%x w%llx t%llx/%llx g%llx-%llx 0e12 %x pf %x | nc %x %x %x %x",
             gpuRead(0xE01), (unsigned long long)r64(0xE05), (unsigned long long)r64(0xE07), (unsigned long long)r64(0xE09),
             (unsigned long long)r64(0xE0B), (unsigned long long)r64(0xE0D), gpuRead(0xE12), gpuRead(0xE19), gpuRead(0x8E08),
             gpuRead(0xB604), gpuRead(0xAE02), gpuRead(0x534));
    if (strncmp(line, native_gpu_rows[kRowUche], sizeof line)) {
        strlcpy(native_gpu_rows[kRowUche], line, sizeof native_gpu_rows[0]);
        changed();
    }
}

// The tiling the last render pass left in the GPU (read every 2 s while the UI runs): GRAS_SC_BIN_CNTL and RB_CNTL carry the bin size in
// pixels (BINW = width / 32, BINH = height / 16) and where the bin buffers live, VSC_BIN_SIZE the tiler's, RB_CCU_CACHE_CNTL the CCU
// cache offsets and sizes, and RB_MRT_0_INFO/BASE the first colour target. Decoded, because GMEM passes put 4-row slices of the frame in
// the wrong place (2026-10-24) and the bin size says which tiling the hardware was actually asked to render.
__attribute__((unused)) void tilingRow() {
    const uint32_t rb = gpuRead(0x8800), gras = gpuRead(0x80A1), vsc = gpuRead(0x0C02);
    char line[sizeof native_gpu_rows[0]];
    snprintf(line, sizeof line, "TILE bin %ux%u rb %08x gras %08x vsc %ux%u | mrt0 %08x %08x%08x vrs %08x",
             (gras & 0x3f) * 32, ((gras >> 8) & 0x7f) * 16, rb, gras, (vsc & 0xff) * 32, ((vsc >> 8) & 0x1ff) * 16,
             gpuRead(0x8822), gpuRead(0x8824), gpuRead(0x8823), gpuRead(0x88F4));
    if (strncmp(line, native_gpu_rows[kRowTiling], sizeof line)) {
        strlcpy(native_gpu_rows[kRowTiling], line, sizeof native_gpu_rows[0]);
        changed();
    }
}

// The kernel side of a submission (NativeAdrenoSubmit.cpp native_adreno_submit_phase), in the tiling row's place while the frame rate is
// the question (2026-10-25: Turnip's Submit call took 1 to 1.4 ms of the render thread): microseconds a submission for the buffers'
// validation, the job sweep and references, the write-combine re-assertion and audit, the BO publication, the TLB invalidation and the
// ring write, and the submissions counted.
extern "C" volatile uint64_t native_adreno_submit_phase[7] __attribute__((weak_import));
// With the cache maintenance behind the publication: passes, MB and ms so far (NativeAdrenoCpu.cpp), the submission at which the
// write-combine audit stopped trusting the client views (0 = never) and the pages the re-assertion found back at the default attribute.
extern "C" volatile uint64_t native_adreno_cache_stats[5] __attribute__((weak_import));
extern "C" volatile uint32_t native_adreno_wc_distrusted __attribute__((weak_import));
extern "C" volatile uint32_t native_adreno_wc_audit[6] __attribute__((weak_import));
void submitRow() {
    if (!&native_adreno_submit_phase) return;
    const uint64_t n = native_adreno_submit_phase[6];
    if (!n) return;
    uint64_t us[6];
    for (unsigned i = 0; i < 6; ++i) {
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(native_adreno_submit_phase[i], &ns);
        us[i] = ns / 1000 / n;
    }
    char line[sizeof native_gpu_rows[0]];
    uint64_t maintained_ns = 0;
    if (&native_adreno_cache_stats) absolutetime_to_nanoseconds(native_adreno_cache_stats[0], &maintained_ns);
    snprintf(line, sizeof line, "SUBT n%llu us check %llu sweep %llu wc %llu pub %llu tlb %llu ring %llu | mt %llu %lluMB %llums d%u r%u",
             (unsigned long long)n, (unsigned long long)us[0], (unsigned long long)us[1], (unsigned long long)us[2],
             (unsigned long long)us[3], (unsigned long long)us[4], (unsigned long long)us[5],
             &native_adreno_cache_stats ? (unsigned long long)native_adreno_cache_stats[3] : 0ull,
             &native_adreno_cache_stats ? (unsigned long long)(native_adreno_cache_stats[1] >> 20) : 0ull,
             (unsigned long long)(maintained_ns / 1000000), &native_adreno_wc_distrusted ? native_adreno_wc_distrusted : 0u,
             &native_adreno_wc_audit ? native_adreno_wc_audit[5] : 0u);
    if (strncmp(line, native_gpu_rows[kRowTiling], sizeof line)) {
        strlcpy(native_gpu_rows[kRowTiling], line, sizeof native_gpu_rows[0]);
        changed();
    }
}

// Register blocks as kernel log lines ("gpu regs <when> <first register>: eight values"), to compare the state after the secure exit
// with the state at the first UCHE_OOB_ACCESS: the UCHE (0xE00..0xE3F; the A5XX's UCHE logs the address of an out-of-bounds access
// in its TRAP_LOG registers, the A6XX's are not documented), RBBM status and interrupts, CP_ADDR_MODE and CP_APRIV, the GBIF (system
// cache IDs, QSB sides, halt), the first registers of SP, TPL1 and HLSQ, the TSB registers hw_init writes, and last
// RBBM_SECVID_TRUST_CNTL (0xF400), which Linux writes only when there is no zap shader and which may be closed to this side.
void regDump(const char *when) {
    static const uint32_t kBlocks[][2] = {{0xE00, 64}, {0x200, 24}, {0x840, 8}, {0x3C00, 8}, {0x3C40, 8}, {0xAE00, 16},
                                          {0xB600, 16}, {0xBE00, 16}, {0xF800, 24}, {0xF400, 8}};
    for (const auto &b : kBlocks)
        for (uint32_t r = b[0]; r < b[0] + b[1]; r += 8) {
            uint32_t v[8];
            for (unsigned i = 0; i < 8; ++i) v[i] = gpuRead(r + i);
            IOLog("gpu regs %s %04x: %08x %08x %08x %08x %08x %08x %08x %08x\n", when, r, v[0], v[1], v[2], v[3], v[4], v[5], v[6],
                  v[7]);
        }
}

// SQE, hw_init, ring 0, CP_ME_INIT, then a CP_MEM_WRITE into the scratch page that the CPU reads back.
bool cpStart() {
    uint32_t *n = native_gpu_cp;
    char *row = native_gpu_rows[kRowCp], *state = native_gpu_rows[kRowCpState];
    const size_t cap = sizeof native_gpu_rows[0];
    auto report = [&](const char *what) {
        snprintf(row, cap, "CP %s step %u sqe %u/%u ring %x init %x fence %x/%u write %x marker %08x | hwcg %x>%x sptp %x>%x", what, n[0],
                 n[1], n[2], n[3], n[4], n[5], n[6], n[7], n[8], n[9], n[12], n[10], n[11]);
        // RBBM_STATUS, CP_RB_RPTR, CP_RB_WPTR, CP_HW_FAULT, CP_INTERRUPT_STATUS, RBBM_INT_0_STATUS, SMMU CB0 FSR and FAR
        const uint32_t bank0 = smmuBank0();
        snprintf(state, cap, "CPSTATE rbbm %08x rptr %x wptr %x hwfault %x cpint %x int0 %08x cb0 fsr %08x far %08x%08x",
                 gpuRead(0x210), gpuRead(0x806), gpuRead(0x807), gpuRead(0x821), gpuRead(0x823), gpuRead(0x201),
                 smmuRead(bank0 + 0x58), smmuRead(bank0 + 0x64), smmuRead(bank0 + 0x60));
        changed();
    };
    native_gpu_step = 120;
    n[0] = 1;
    if (NativeAdrenoGpuContextBuffers(gNb)) {report("no buffers"); return false;}
    for (unsigned i = 0; i < 5; ++i) {
        IODeviceMemory *m = IODeviceMemory::withRange(gNb[i].physical, gNb[i].bytes);
        if (m) {
            gNbMaps[i] = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain);
            m->release();
        }
        if (!gNbMaps[i]) {report("nomap"); return false;}
        gNbCpu[i] = reinterpret_cast<volatile uint8_t *>(gNbMaps[i]->getVirtualAddress());
    }
    // The SQE without its first dword (adreno_fw_create_bo), read back.
    n[0] = 2;
    if (!&native_gpu_sqe_firmware || !&native_gpu_sqe_firmware_size || native_gpu_sqe_firmware_size < 8 ||
        native_gpu_sqe_firmware_size - 4 > gNb[0].bytes) {report("no sqe"); return false;}
    const uint32_t sqeBytes = native_gpu_sqe_firmware_size - 4;
    n[1] = sqeBytes;
    for (uint32_t i = 0; i < sqeBytes; ++i) gNbCpu[0][i] = native_gpu_sqe_firmware[4 + i];
    __asm__ volatile("dsb sy" ::: "memory");
    for (uint32_t i = 0; i < sqeBytes && !n[2]; ++i) if (gNbCpu[0][i] != native_gpu_sqe_firmware[4 + i]) n[2] = i + 1;
    if (n[2]) {report("sqe readback"); return false;}
    n[0] = 3; native_gpu_step = 121;
    hwInit();
    ucheRow();
    gw64(0x830, gNb[0].iova);                                           // CP_SQE_INSTR_BASE, before CP_SQE_CNTL
    // Ring 0: 32 KiB at its IOVA, the memptrs page for the fence; A6XX forms (NO_UPDATE, CACHE_FLUSH_TS fences).
    n[0] = 4; native_gpu_step = 122;
    gRing = new NativeAdrenoRing;
    gCpInit = new NativeAdrenoCpInit;
    if (!gRing || !gCpInit || gRing->setGeneration(6)) {report("no ring"); return false;}
    n[3] = uint32_t(gRing->attach(&gRio, &gCache, const_cast<uint8_t *>(gNbCpu[1]), gNb[1].bytes, gNb[1].iova,
                                  const_cast<uint8_t *>(gNbCpu[2]), gNb[2].bytes, gNb[2].iova));
    if (n[3]) {report("ring attach"); return false;}
    gw(0x808, 1);                                                       // CP_SQE_CNTL: start the microcode
    // a6xx_cp_init: CP_ME_INIT with its eight words.
    n[0] = 5; native_gpu_step = 123;
    NativeAdrenoCpInitConfig config{gNb[0].iova, 0, 1};
    config.generation = 6;
    uint32_t fence = 0;
    n[4] = uint32_t(gCpInit->run(&gRio, gRing, &config, &fence));
    if (n[4]) {report("cp init"); return false;}
    n[5] = uint32_t(gRing->wait(fence, &gClock, 200000, &n[6]));
    if (n[5]) {report("cp init fence"); return false;}
    // The first command through NativeAdreno's mapping: CP_MEM_WRITE (address lo/hi, data) into the scratch page.
    n[0] = 6; native_gpu_step = 124;
    volatile uint32_t *scratch = reinterpret_cast<volatile uint32_t *>(gNbCpu[4]);
    scratch[0] = 0;
    __asm__ volatile("dsb sy" ::: "memory");
    uint32_t words[4] = {};
    if (AdrenoPM4Type7(0x3d, 3, &words[0]) != AdrenoPM4Success) {report("pm4"); return false;}
    words[1] = uint32_t(gNb[4].iova); words[2] = uint32_t(gNb[4].iova >> 32); words[3] = 0xA6500C0D;
    if (gRing->submit(words, 4, &fence)) {report("submit"); return false;}
    uint32_t seen = 0;
    n[7] = uint32_t(gRing->wait(fence, &gClock, 200000, &seen));
    n[8] = scratch[0];
    if (n[7] || n[8] != 0xA6500C0D) {report("mem write"); return false;}
    n[0] = 7;
    report("ok");
    return true;
}

// Mode 10. SIP SMC as UniXNU-tb128fu's scmCall (qcom_scm-smc.c): x0 function, x1 arginfo, x2.. arguments; INTERRUPTED (1)
// resumes with x0 = 1 and the returned x6, V2_EBUSY (-12) retries after 30 ms up to 20 times. This core's IRQs stay masked
// across the call and its resumes (tb128fu-022: an unmasked init_image was interrupted 10000 times in a row).
struct ScmRes {int64_t a0; uint64_t a1;};
uint32_t gScmResumes;
ScmRes scmCall(uint64_t fn, uint64_t arginfo, uint64_t p0, uint64_t p1, uint64_t p2) {
    const boolean_t was = ml_set_interrupts_enabled(FALSE);
    ScmRes out{-1000, 0};
    uint64_t first = fn, a6 = 0;
    gScmResumes = 0;
    for (unsigned resume = 0, busy = 0; resume < 10000; gScmResumes = resume) {
        register uint64_t x0 asm("x0") = first, x1 asm("x1") = arginfo, x2 asm("x2") = p0, x3 asm("x3") = p1;
        register uint64_t x4 asm("x4") = p2, x5 asm("x5") = 0, x6 asm("x6") = a6, x7 asm("x7") = 0;
        asm volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3), "+r"(x4), "+r"(x5), "+r"(x6), "+r"(x7)
                     :: "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
        if (x0 == 1) {first = 1; a6 = x6; ++resume; continue;}
        if (int64_t(x0) == -12 && busy++ < 20) {first = fn; a6 = 0; IODelay(30000); continue;}
        out = ScmRes{int64_t(x0), x1};
        break;
    }
    ml_set_interrupts_enabled(was);
    return out;
}
// ARM_SMCCC_CALL_VAL(STD, SMC_64, OWNER_SIP, svc << 8 | cmd): PIL is service 2 (qcom_scm.h).
constexpr uint64_t kScmPil(uint32_t cmd) {return 0x42000000u | (0x02u << 8) | cmd;}
constexpr uint32_t kGpuPasId = 13;                                  // DT qcom,kgsl-hyp qcom,pas-id
constexpr uint64_t kZapPhys = 0x8681a000, kZapBytes = 0x2000;       // DT pil_gpu_region@8681a000, no-map
uint32_t le32(const uint8_t *p) {return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;}
uint16_t le16(const uint8_t *p) {return uint16_t(p[0] | p[1] << 8);}
void cleanToPoc(const void *p, size_t len) {
    uint64_t ctr; __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    const size_t line = 4u << ((ctr >> 16) & 15);
    for (uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~(line - 1); a < reinterpret_cast<uintptr_t>(p) + len; a += line)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
uint32_t msSince(uint64_t start) {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
    return uint32_t(ns / 1000000);
}
extern "C" {
int32_t native_gpu_zap[16];   // step, init/mem/auth results, resumes, timings, segment facts (see zapRow)
}
IOBufferMemoryDescriptor *gZapMeta;
void zapRow(const char *what) {
    const int32_t *z = native_gpu_zap;
    snprintf(native_gpu_rows[kRowZap], sizeof native_gpu_rows[0],
             "ZAP %s step %d init %x/%x mem %x/%x auth %x/%x resumes %d ms %d/%d/%d phdrs %d reloc %d span %x secure %d meta %x000",
             what, z[0], z[1], z[2], z[3], z[4], z[5], z[6], z[7], z[8], z[9], z[10], z[11], z[12], z[13], z[14], z[15]);
    changed();
}
// qcom_mdt_load for the zap shader (UniXNU-tb128fu zapLoad): the ELF header and program headers plus the hash segment as
// metadata for init_image, mem_setup for a relocatable image, the loadable segments copied into the reserved region, then
// auth_and_reset. z[0] is the step reached (negative: why it stopped).
bool zapLoad() {
    int32_t *z = native_gpu_zap;
    auto stop = [&](int why) {z[0] = why; zapRow("stopped"); return false;};
    native_gpu_step = 130;
    z[0] = 1;
    // The region must not be memory XNU hands out: TrustZone locks it after authentication.
    if (kZapPhys + kZapBytes > vm_first_phys && kZapPhys < vm_last_phys) return stop(-20);
    if (!&native_gpu_zap_firmware || !&native_gpu_zap_firmware_size) return stop(-21);
    const uint8_t *fw = native_gpu_zap_firmware;
    const uint64_t size = native_gpu_zap_firmware_size;
    if (size < 52 || fw[0] != 0x7f || fw[1] != 'E' || fw[2] != 'L' || fw[3] != 'F' || fw[4] != 1) return stop(-1);
    const uint32_t phoff = le32(fw + 28);
    const unsigned phent = le16(fw + 42), phnum = le16(fw + 44);
    z[11] = int32_t(phnum);
    if (phent != 32 || phnum < 2 || phnum > 16 || phoff + uint64_t(phnum) * 32 > size) return stop(-2);
    auto ph = [&](unsigned i, unsigned field) {return le32(fw + phoff + i * 32 + field * 4);};   // type off va pa filesz memsz flags
    if (ph(0, 0) == 1) return stop(-3);
    unsigned hash = 0;
    for (unsigned i = 1; i < phnum && !hash; ++i) if (((ph(i, 6) >> 24) & 7) == 2) hash = i;
    if (!hash) return stop(-4);
    uint64_t lo = ~0ull, hi = 0;
    bool relocate = false;
    for (unsigned i = 0; i < phnum; ++i) {
        if (ph(i, 0) != 1 || ((ph(i, 6) >> 24) & 7) == 2 || !ph(i, 5)) continue;   // mdt_phdr_loadable
        if (ph(i, 6) & (1u << 27)) relocate = true;
        lo = lo < ph(i, 3) ? lo : ph(i, 3);
        const uint64_t end = (uint64_t(ph(i, 3)) + ph(i, 5) + 0xfff) & ~0xfffull;
        hi = hi > end ? hi : end;
    }
    z[12] = relocate; z[13] = int32_t(hi - lo);
    if (lo >= hi || hi - lo > kZapBytes) return stop(-5);
    const uint64_t ehdr = ph(0, 4), hashBytes = ph(hash, 4), hashOff = ph(hash, 1);
    if (ehdr > size || hashOff + hashBytes > size || ehdr + hashBytes > 0x4000) return stop(-6);
    // Metadata: physically contiguous, written back to the point of coherency (Linux: dma_alloc_coherent, below 4 GiB). XNU's
    // memory on pipa starts at 0xffc20000 and almost none of it lies below 4 GiB (2026-10-24: a below-4-GiB allocation failed,
    // after a search that may have held the boot back), so any address below 2^40: the SMC64 call carries it in full, and
    // TrustZone answers init_image with an error if it refuses it.
    z[0] = 2;
    const size_t metaBytes = size_t((ehdr + hashBytes + 0xfff) & ~0xfffull);
    gZapMeta = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task, static_cast<IOOptionBits>(kIODirectionInOut) | static_cast<IOOptionBits>(kIOMemoryPhysicallyContiguous),
        metaBytes, 0xFFFFFFF000ULL);
    if (!gZapMeta || gZapMeta->prepare() != kIOReturnSuccess) return stop(-7);
    uint8_t *meta = static_cast<uint8_t *>(gZapMeta->getBytesNoCopy());
    const uint64_t metaPhys = gZapMeta->getPhysicalSegment(0, nullptr);
    z[15] = int32_t(metaPhys >> 12);   // the metadata's page number, for the row
    if (!meta || !metaPhys) return stop(-8);
    for (uint64_t i = 0; i < ehdr; ++i) meta[i] = fw[i];
    for (uint64_t i = 0; i < hashBytes; ++i) meta[ehdr + i] = fw[hashOff + i];
    cleanToPoc(meta, metaBytes);
    // qcom_scm_pas_init_image: ARGS(2, VAL, RW).
    z[0] = 3; native_gpu_step = 131;
    uint64_t start = mach_absolute_time();
    ScmRes r = scmCall(kScmPil(0x01), 2 | (2u << 6), kGpuPasId, metaPhys, 0);
    z[1] = int32_t(r.a0); z[2] = int32_t(r.a1); z[8] = int32_t(msSince(start)); z[7] = int32_t(gScmResumes);
    if (r.a0 || r.a1) return stop(3);
    if (relocate) {
        // qcom_scm_pas_mem_setup: ARGS(3).
        z[0] = 4; native_gpu_step = 132;
        start = mach_absolute_time();
        r = scmCall(kScmPil(0x02), 3, kGpuPasId, kZapPhys, hi - lo);
        z[3] = int32_t(r.a0); z[4] = int32_t(r.a1); z[9] = int32_t(msSince(start));
        if (r.a0 || r.a1) return stop(4);
    }
    // qcom_mdt_load_no_init into the reserved region (relocatable: offsets from the lowest segment address).
    z[0] = 5; native_gpu_step = 133;
    {
        IODeviceMemory *m = IODeviceMemory::withRange(kZapPhys, kZapBytes);
        OSSharedPtr<IOMemoryMap> map;
        if (m) {map = OSSharedPtr<IOMemoryMap>(m->map(kIOMapAnywhere | kIOMapWriteCombineCache), OSNoRetain); m->release();}
        if (!map) return stop(-9);
        volatile uint8_t *region = reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress());
        const uint64_t base = relocate ? lo : kZapPhys;
        for (unsigned i = 0; i < phnum; ++i) {
            if (ph(i, 0) != 1 || ((ph(i, 6) >> 24) & 7) == 2 || !ph(i, 5)) continue;
            const uint64_t at = uint64_t(ph(i, 3)) - base, file = ph(i, 4), mem = ph(i, 5), off = ph(i, 1);
            if (ph(i, 3) < base || at + mem > kZapBytes || file > mem || off + file > size) return stop(-10);
            for (uint64_t b = 0; b < file; ++b) region[at + b] = fw[off + b];
            for (uint64_t b = file; b < mem; ++b) region[at + b] = 0;
        }
        __asm__ volatile("dsb sy" ::: "memory");
    }   // the CPU view is gone before TrustZone takes the region
    // qcom_scm_pas_auth_and_reset: ARGS(1).
    z[0] = 6; native_gpu_step = 134;
    start = mach_absolute_time();
    r = scmCall(kScmPil(0x05), 1, kGpuPasId, 0, 0);
    z[5] = int32_t(r.a0); z[6] = int32_t(r.a1); z[10] = int32_t(msSince(start));
    if (r.a0 || r.a1) return stop(6);
    z[0] = 7;   // TrustZone authenticated the zap shader
    return true;
}
// The zap shader, then CP_SET_SECURE_MODE(0) through the ring (hw_init after a6xx_zap_shader_init).
bool leaveSecure() {
    if (!zapLoad()) return false;
    native_gpu_step = 135;
    uint32_t words[2] = {}, fence = 0, seen = 0;
    if (AdrenoPM4Type7(0x66, 1, &words[0]) != AdrenoPM4Success || gRing->submit(words, 2, &fence)) {zapRow("submit"); return false;}
    native_gpu_zap[14] = int32_t(gRing->wait(fence, &gClock, 200000, &seen));
    zapRow(native_gpu_zap[14] ? "secure exit timeout" : "ok");
    return native_gpu_zap[14] == 0;
}

// The GPU interrupt (DT kgsl_3d0_irq: SPI 300, level high = INTID 332). As a6xx_irq: read RBBM_INT_0_STATUS and clear it through
// RBBM_INT_CLEAR_CMD before anything else, so the level line drops before the EOI; the same status 100000 times in a row masks
// the GPU interrupts. Each interrupt wakes the fence waiters (naIdle).
void gpuIrq(void *) {
    const uint32_t status = gpuRead(0x201);
    if (status) gw(0x37, status);
    gIrqRepeat = (status == gIrqLast && !(status & (1u << 20))) ? gIrqRepeat + 1 : 0;
    gIrqLast = status;
    gIrqSeen = gIrqSeen | status;
    gIrqCount = gIrqCount + 1;
    if (gIrqRepeat > 100000) {gw(0x38, 0); gIrqMasked = 1;}
    thread_wakeup(event_t(&gIrqCount));
}
// The secure-mode fence left CP_CACHE_FLUSH_TS set, so a working line fires as soon as it is enabled; a CP_NOP fence must fire it
// again.
bool gpuInterrupt() {
    native_gpu_step = 140;
    uint32_t readback = 0;
    uint64_t router = ~0ull;
    const IOReturn attached = armcpu_spi_attach(32 + 300, false, gpuIrq, nullptr, &readback, &router);
    gIrqAttached = attached == kIOReturnSuccess;
    IOSleep(20);
    const uint32_t first = gIrqCount;
    uint32_t nop = 0, fence = 0, seen = 0;
    const bool fenced = AdrenoPM4Type7(0x10, 0, &nop) == AdrenoPM4Success && !gRing->submit(&nop, 1, &fence) &&
                        !gRing->wait(fence, &gClock, 200000, &seen);
    IOSleep(20);
    snprintf(native_gpu_rows[kRowIrq], sizeof native_gpu_rows[0],
             "IRQ attach %x readback %x router %llx count %u>%u nop fence %u seen %08x status now %08x masked %u",
             static_cast<unsigned>(attached), readback, static_cast<unsigned long long>(router), first, gIrqCount, fenced, gIrqSeen,
             gpuRead(0x201), gIrqMasked);
    changed();
    return gIrqAttached && fenced;
}

// Mode 11. Facts for the Vulkan driver: chip id 0x06050002 (Linux adreno_device.c; Turnip's Darwin backend makes GPU id 650 of
// it), GMEM 1 MiB + 128 KiB at base 0 as Linux reports it for the a650 family (Turnip reads input attachments through it in GMEM
// passes; 16 MiB with pipa-gpu-gmem16m=1), the UCHE trap base hw_init writes, the client VA window above the kernel buffers and
// below the 2 GiB aperture, and the UBWC facts hw_init programs (highest bank bit 16; swizzle levels 2 and 3, as Linux's default
// for every A6XX but the A610; eight-channel macrotiles: RBBM_NC_MODE_CNTL reads 1 at reset here (2026-10-24), and Linux's
// ubwc_config for the SM8250 has macrotile_mode set; UBWC 4.0 of the SM8250).
NativeAdrenoAbiDeviceInfo kDeviceInfo = {0x06050002u, 0x120000u, 0, 0x1fffffffff000ull, 0x20000000ull, 0x60000000ull,
                                         16, 6, 1, 4, NativeAdrenoDeviceClientIova, 0};
NativeAdrenoSubmit *gSubmit;
NativeAdrenoTimer *gTimer;
NativeAdrenoTimeIo gTimerIo{nullptr, naRead};
extern "C" {
uint32_t native_gpu_publish[16];   // submit attach, timer attach, publish, user-client rounds (see publishRow)
}
void publishRow(const char *what) {
    const uint32_t *p = native_gpu_publish;
    snprintf(native_gpu_rows[kRowPublish], sizeof native_gpu_rows[0],
             "PUBLISH %s submit %x timer %x publish %x | uc %u/%u first %x at %u cpumap %x | rounds %d %d",
             what, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], int32_t(p[8]), int32_t(p[9]));
    changed();
}

// The submission object on ring 0, the CP always-on counter as the GPU clock, and the published device for user clients.
bool publish() {
    uint32_t *p = native_gpu_publish;
    native_gpu_step = 150;
    NativeAdrenoVm *vm = NativeAdrenoGpuContextVm();
    gSubmit = new NativeAdrenoSubmit;
    gTimer = new NativeAdrenoTimer;
    if (!vm || !gSubmit || !gTimer) {publishRow("no objects"); return false;}
    kDeviceInfo.gmem_base = gUcheGmem16M ? 0x1000000 : 0;
    p[0] = uint32_t(gSubmit->attach(vm, gRing));
    p[1] = uint32_t(gTimer->attach(&gTimerIo, NativeAdrenoTimeCp));
    p[2] = uint32_t(native_adreno_publish(vm, gSubmit, gRing, &gClock, p[1] ? nullptr : gTimer, &kDeviceInfo, 31,
                                          native_gpu_context.output_bits));
    publishRow(p[0] || p[2] ? "failed" : "ok");
    return !p[0] && !p[2];
}

// The user-client round of UniXNU-tb128fu (sheng wrapperRound): exactly the path a user-space client takes, minus the Mach trap.
// newUserClient with the plugin's open type (a second open is refused while one is live), the external-method table, two BOs
// mapped for the GPU and, through BoCpuMap and clientMemoryForType, write-combined for the CPU, an IB of three CP_MEM_WRITEs
// submitted and waited for, the target read back, then clientClose. p[3..6]: steps passed, steps, first error and its step;
// p[7]: the first clientMemoryForType status. Returns the matching dwords (of 4) or -1.
IOReturn ucCall(IOUserClient *uc, uint32_t method, const void *in, void *out) {
    IOExternalMethodArguments a{};
    a.version = kIOExternalMethodArgumentsCurrentVersion;
    a.selector = method;
    const size_t is = NativeAdrenoUserClient::inputSize(method), os = NativeAdrenoUserClient::outputSize(method);
    a.structureInput = is ? in : nullptr;
    a.structureInputSize = uint32_t(is);
    a.structureOutput = os ? out : nullptr;
    a.structureOutputSize = uint32_t(os);
    return uc->externalMethod(method, &a, nullptr, nullptr, nullptr);
}
int ucRound(IOService *svc) {
    uint32_t *p = native_gpu_publish;
    auto step = [&](IOReturn status) {
        const uint32_t at = p[4]++;
        if (!status) ++p[3];
        else if (!p[5]) {p[5] = uint32_t(status); p[6] = at;}
        return !status;
    };
    constexpr UInt32 kPluginOpenType = 0x4147;   // NativeAdrenoPublish.cpp NativeAdrenoPluginOpenType
    IOUserClient *uc = nullptr;
    if (!step(svc->newUserClient(kernel_task, nullptr, kPluginOpenType, nullptr, &uc)) || !uc) return -1;
    IOUserClient *second = nullptr;
    step(svc->newUserClient(kernel_task, nullptr, kPluginOpenType, nullptr, &second) == kIOReturnBusy ? kIOReturnSuccess
                                                                                                      : kIOReturnError);
    if (second) second->release();
    NativeAdrenoAbiParams params{};
    step(ucCall(uc, NativeAdrenoMethodQuery, nullptr, &params));
    NativeAdrenoAbiDeviceInfo info{};
    step(ucCall(uc, NativeAdrenoMethodGetDeviceInfo, nullptr, &info));
    step(info.chip_id == kDeviceInfo.chip_id && info.gmem_bytes == kDeviceInfo.gmem_bytes ? kIOReturnSuccess : kIOReturnError);
    NativeAdrenoAbiBoAlloc alloc{4096, 12, 0};
    NativeAdrenoAbiBoAllocReply ib{}, tgt{};
    int matched = -1;
    IOMemoryDescriptor *desc[2] = {};
    IOMemoryMap *maps[2] = {};
    if (step(ucCall(uc, NativeAdrenoMethodBoAlloc, &alloc, &ib)) && step(ucCall(uc, NativeAdrenoMethodBoAlloc, &alloc, &tgt))) {
        const uint64_t ibIova = 0x20004000, tgtIova = 0x20005000;   // inside the client window of kDeviceInfo
        NativeAdrenoAbiBoMap mapIb{ib.handle, ibIova, 1, 0, 2, 0}, mapTgt{tgt.handle, tgtIova, 3, 0, 2, 0};
        NativeAdrenoAbiBoMapReply r{};
        NativeAdrenoAbiBoCpuMap cmIb{ib.handle}, cmTgt{tgt.handle};
        NativeAdrenoAbiBoCpuMapReply crIb{}, crTgt{};
        if (step(ucCall(uc, NativeAdrenoMethodBoMap, &mapIb, &r)) && step(ucCall(uc, NativeAdrenoMethodBoMap, &mapTgt, &r)) &&
            step(ucCall(uc, NativeAdrenoMethodBoCpuMap, &cmIb, &crIb)) && step(ucCall(uc, NativeAdrenoMethodBoCpuMap, &cmTgt, &crTgt))) {
            IOOptionBits opt[2] = {};
            const uint32_t tokens[2] = {crIb.token, crTgt.token};
            bool mapped = true;
            for (int i = 0; i < 2; ++i) {
                const IOReturn status = uc->clientMemoryForType(tokens[i], &opt[i], &desc[i]);
                if (!p[7]) p[7] = uint32_t(status);
                if (status || !desc[i]) {mapped = false; continue;}
                maps[i] = desc[i]->createMappingInTask(kernel_task, 0, opt[i]);
                if (!maps[i]) {mapped = false; if (!p[7]) p[7] = 0xbad;}
            }
            // Type 0 and a stale token must be refused.
            IOMemoryDescriptor *none = nullptr;
            IOOptionBits o = 0;
            step(uc->clientMemoryForType(0, &o, &none) != kIOReturnSuccess && !none ? kIOReturnSuccess : kIOReturnError);
            step(mapped ? kIOReturnSuccess : kIOReturnNoMemory);
            if (mapped) {
                volatile uint32_t *iw = reinterpret_cast<volatile uint32_t *>(maps[0]->getVirtualAddress());
                volatile uint32_t *tw = reinterpret_cast<volatile uint32_t *>(maps[1]->getVirtualAddress());
                for (unsigned i = 0; i < 1024; ++i) tw[i] = 0xdeadbeef;
                const uint32_t values[3] = {0x0badc0deu, 0xfeed0001u, 0x600d0003u};
                uint32_t words[12];
                bool built = true;
                for (unsigned i = 0; i < 3; ++i) {
                    built = built && AdrenoPM4Type7(0x3d, 3, &words[i * 4]) == AdrenoPM4Success;
                    words[i * 4 + 1] = uint32_t(tgtIova + i * 4); words[i * 4 + 2] = uint32_t(tgtIova >> 32);
                    words[i * 4 + 3] = values[i];
                }
                for (unsigned i = 0; i < 12; ++i) iw[i] = words[i];
                __asm__ volatile("dsb sy" ::: "memory");
                NativeAdrenoAbiSubmit submit{};
                submit.buffer_count = 1;
                submit.buffers[0] = {ib.handle, ibIova, sizeof words};
                NativeAdrenoAbiSubmitReply sr{};
                if (step(built ? kIOReturnSuccess : kIOReturnInternalError) && step(ucCall(uc, NativeAdrenoMethodSubmit, &submit, &sr))) {
                    NativeAdrenoAbiWaitFence wait{sr.fence, 200000000ull};
                    if (step(ucCall(uc, NativeAdrenoMethodWaitFence, &wait, nullptr))) {
                        __asm__ volatile("dsb sy" ::: "memory");
                        matched = (tw[0] == values[0]) + (tw[1] == values[1]) + (tw[2] == values[2]) + (tw[3] == 0xdeadbeef);
                    }
                }
                NativeAdrenoAbiTimestamp ts{};
                step(ucCall(uc, NativeAdrenoMethodGetTimestamp, nullptr, &ts));
            }
        }
    }
    for (auto *m : maps) if (m) m->release();
    for (auto *d : desc) if (d) d->release();
    step(uc->clientClose());
    uc->release();
    return matched;
}
// Two rounds: the second proves the client slot and the IOVAs recycle after clientClose. pipa-gpu-selftest=0 skips them.
bool selfTest() {
    unsigned on = 1;
    PE_parse_boot_argn("pipa-gpu-selftest", &on, sizeof on);
    if (!on) return true;
    native_gpu_step = 155;
    IOService *svc = native_adreno_published_service();
    if (!svc) {publishRow("no service"); return false;}
    native_gpu_publish[8] = uint32_t(ucRound(svc));
    native_gpu_step = 156;
    native_gpu_publish[9] = uint32_t(ucRound(svc));
    const bool ok = native_gpu_publish[8] == 4 && native_gpu_publish[9] == 4;
    publishRow(ok ? "ok" : "self test failed");
    return ok;
}

// Mode 12: the staged AGXAccelerator for the original Metal discovery (sheng publishMetal, UniXNU-tb128fu naMetal). The plugin
// bundle arrives through the boot-time addon mount (sheng-gpu-addon=1) and the owner trust module; the render server is the one
// process granted its client, with the two that crashed in CoreImage's OpenGL ES path without a Metal device on UniXNU-tb128fu.
#if PIPA_GPU_ACCELERATOR
bool metal() {
    char *row = native_gpu_rows[kRowMetal];
    const size_t cap = sizeof native_gpu_rows[0];
    unsigned stage = 0;
    auto stop = [&](IOReturn why) {
        snprintf(row, cap, "METAL stopped at %u: %x", stage, static_cast<unsigned>(why));
        changed();
        return false;
    };
    native_gpu_step = 160;
    stage = 1;
    const kern_return_t kext = OSKextLoadKextWithIdentifier("com.apple.iokit.IOGPUFamily");
    if (kext) return stop(kext);
    const OSSymbol *name = OSSymbol::withCString("IOAcceleratorES");
    const OSMetaClass *parent = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    OSSafeReleaseNULL(name);
    if (!parent || parent->getClassSize() != 136) return stop(kIOReturnNotFound);
    stage = 2;
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) return stop(kIOReturnNoResources);
    native_adreno_accelerator_static_init();
    const IOReturn loaded = OSMetaClass::postModLoad(handle);
    if (loaded) return stop(loaded);
    if (native_adreno_accelerator_instance_size() != 136) return stop(kIOReturnInvalid);
    stage = 3;
    IOService *accelerator = native_adreno_accelerator_allocate();
    if (!accelerator) return stop(kIOReturnNoMemory);
    if (!accelerator->init()) {accelerator->release(); return stop(kIOReturnError);}
    accelerator->setProperty("MetalPluginName", "NativeAdrenoMetal");
    accelerator->setProperty("NativeSandboxGrantProcess", "backboardd,iconservicesagent,Preferences");
    accelerator->setProperty("MetalPluginClassName", "NativeAdrenoMetalDevice");
    // Turnip's options for this board, which replace the plugin's default "sysmem,nocb" (NativeAdrenoMetalPlugin.mm
    // applyBoardTurnipDebug). That default keeps every pass in system memory against a hang of the A740's concurrent binning (BV
    // and BR); the A650 has no concurrent binning, so by default Turnip picks tiled GMEM rendering with binning or system memory
    // per pass, as on Linux ("nocb" is a no-op here and keeps the variable non-empty). pipa-turnip-debug=<options> replaces it
    // for a round, e.g. pipa-turnip-debug=sysmem,nocb.
    char turnip[64] = "nocb";
    PE_parse_boot_arg_str("pipa-turnip-debug", turnip, sizeof turnip);
    // Canned Turnip option sets for a round, so an A/B on the GMEM input attachment bug (HANDOFF-pipa.md 10.14) fits in the
    // 1000-byte command line: pipa-tu=1..5 (8 bytes) instead of pipa-turnip-debug=<46 bytes>.
    unsigned canned = 0;
    if (PE_parse_boot_argn("pipa-tu", &canned, sizeof canned) && canned) {
        static const char *const sets[] = {"",
            "pipa_gmemin",                       // 1 GMEM for input attachments, the corner bug on display
            "pipa_gmemin,pipa_iaflush",          // 2 invalidate the UCHE before every draw that reads GMEM
            "pipa_gmemin,pipa_iaplain",          // 3 read them through their sysmem descriptor instead
            "pipa_gmemin,pipa_uchegmem",         // 4 program UCHE_GMEM_RANGE from Turnip itself
            "pipa_gmemin,pipa_iaflush,pipa_uchegmem",   // 5 both
            "sysmem",                            // 6 every pass in system memory (GPU time against GMEM, 2026-10-25)
            "gmem",                              // 7 every pass in GMEM, the autotuner's sysmem picks too
            "pipa_small-262144",                 // 8 passes up to 512x512 in system memory
            "nolrz",                             // 9 no low-resolution Z
            "pipa_gmemplain",                    // 10 passes whose draws read no input attachment may take GMEM
            "pipa_gmemplain,gmem",               // 11 and take it whenever it fits
            "pipa_fbloop"};                      // 12 the single primitive mode only for the pipelines that read one
        if (canned < sizeof sets / sizeof sets[0]) snprintf(turnip, sizeof turnip, "%s", sets[canned]);
    }
    accelerator->setProperty("NativeTurnipDebug", turnip);
    // Device capabilities the Metal frontend withholds in every process (NativeMetalWithhold, as NMB_WITHHOLD): pipa-metal-withhold=
    // apple-families answers no to every Apple GPU family, as UniXNU-tb128fu does (Core Animation blends by framebuffer fetch on an
    // Apple1 device, NativeBridgeMetalFrontend.mm). With the families reported, the home screen's widget platters had their rounded
    // corners drawn in 4 x 4 pixel blocks with holes (2026-10-24).
    // apple-families is the default since 2026-10-25 (it was board.toml's argument, 35 bytes of the 1000-byte command line);
    // pipa-metal-withhold=none reports the families again.
    char withhold[64] = "apple-families";
    PE_parse_boot_arg_str("pipa-metal-withhold", withhold, sizeof withhold);
    if (withhold[0] && strcmp(withhold, "none"))
        accelerator->setProperty("NativeMetalWithhold", withhold);
    stage = 4;
    if (!accelerator->attach(IOService::getPlatform())) {accelerator->release(); return stop(kIOReturnNotAttached);}
    accelerator->registerService();
    snprintf(row, cap, "METAL ok: accelerator registered (IOGPUFamily loaded, class size 136), Turnip options %s", turnip);
    changed();
    return true;
}
#else
bool metal() {
    publishRow("unavailable: this kernel has no IOAccelerator");
    return false;
}
#endif

// Where the CP stands when it stops moving. Once a submission is outstanding and RPTR, IB1 base and IB1 remaining dwords are the
// same as one beat (2 s) before, the rows are refreshed (the ring itself may be drained: RPTR passes an IB's packet when the CP
// fetches it, before the IB runs; 2026-10-24's hang left RPTR = WPTR). The first row: RBBM_STATUS..STATUS3 (0x210..0x213), CP_RB_RPTR/WPTR,
// IB1 and IB2 base and remaining dwords (0x928/0x92a, 0x92b/0x92d), CP_HW_FAULT 0x821, CP_INTERRUPT_STATUS 0x823,
// CP_PROTECT_STATUS 0x824. The second: the SQE PC and the header of the packet it is on (CP_SQE_STAT_ADDR 0x908 = 0, then
// CP_SQE_STAT_DATA 0x909 twice, as crashdec reads them), context bank 0 FSR, FAR and FSYNR0, and the IB positions of the first
// beat that saw the stall, so two log periods tell a stuck CP from a slow one.
void hangRows(unsigned t) {
    static unsigned firstT;
    static uint64_t firstIb1, firstIb2;
    static uint32_t firstRem1, firstRem2;
    static bool seen;
    const uint64_t ib1 = gpuRead(0x928) | uint64_t(gpuRead(0x929)) << 32, ib2 = gpuRead(0x92b) | uint64_t(gpuRead(0x92c)) << 32;
    const uint32_t rem1 = gpuRead(0x92a), rem2 = gpuRead(0x92d);
    if (!seen) {seen = true; firstT = t; firstIb1 = ib1; firstIb2 = ib2; firstRem1 = rem1; firstRem2 = rem2;}
    snprintf(native_gpu_rows[kRowHang], sizeof native_gpu_rows[0],
        "HANG st%08x %x/%x/%x r%x/%x | ib1 %llx+%x ib2 %llx+%x | hw%x cpi%x prot%x", gpuRead(0x210), gpuRead(0x211),
        gpuRead(0x212), gpuRead(0x213), gpuRead(0x806), gpuRead(0x807), (unsigned long long)ib1, rem1, (unsigned long long)ib2, rem2,
        gpuRead(0x821), gpuRead(0x823), gpuRead(0x824));
    gw(0x908, 0);
    const uint32_t pc = gpuRead(0x909), op = gpuRead(0x909);
    const uint32_t bank0 = smmuBank0();
    snprintf(native_gpu_rows[kRowHang2], sizeof native_gpu_rows[0],
        "HANG sqe pc%x op%08x | fsr%x far%08x%08x fsynr%x | first t%u ib1 %llx+%x ib2 %llx+%x", pc, op, smmuRead(bank0 + 0x58),
        smmuRead(bank0 + 0x64), smmuRead(bank0 + 0x60), smmuRead(bank0 + 0x68), firstT, (unsigned long long)firstIb1, firstRem1,
        (unsigned long long)firstIb2, firstRem2);
    changed();
}

// The live row, every 2 s once the device is published: interrupts (count, last status, all seen, masked), submissions (submitted,
// completed, timeouts, poisoned), user clients opened/closed, the BO pool (MiB in use, peak, size; BOs that did not fit and fell
// back to scattered pages, scattered BOs in all), and the GPU's busy share of the last beat in per mille. Only changed values reach
// the log. (The fence waits that slept or spun, native_gpu_idle_sleeps/spins, made way for the pool's figures.)
[[noreturn]] void heartbeat() {
    uint32_t lastRptr = ~0u, lastIb1 = ~0u, lastRem1 = ~0u;
    uint64_t lastBusy = 0, lastNs = 0;
    for (unsigned t = 0;; t += 2) {
        // The GMU's busy counter as Linux's a6xx_gpu_busy reads it: GMU_CX_GMU_POWER_COUNTER_XOCLK_0 (0x5044/0x5045), 19.2 MHz
        // ticks while the GPU is busy (hw_init writes 0x20, Linux's BIT(5), into POWER_COUNTER_SELECT_0's low byte), against the time
        // since the last beat.
        uint32_t lo = 0, hi = 0, hi2 = 0;
        gmuRead(nullptr, A650GmuCore, 0x5045 * 4, &hi);
        gmuRead(nullptr, A650GmuCore, 0x5044 * 4, &lo);
        gmuRead(nullptr, A650GmuCore, 0x5045 * 4, &hi2);
        if (hi2 != hi) gmuRead(nullptr, A650GmuCore, 0x5044 * 4, &lo);
        const uint64_t busy = uint64_t(hi2) << 32 | lo;
        uint64_t ns = 0;
        absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
        const uint32_t permille = lastNs && ns > lastNs && busy >= lastBusy
            ? uint32_t((busy - lastBusy) * 1000000000ull / 19200000ull * 1000 / (ns - lastNs)) : 0;
        lastBusy = busy; lastNs = ns;
        const NativeAdrenoSubmitStats st = gSubmit->stats();
        const uint32_t rptr = gpuRead(0x806), ib1 = gpuRead(0x928), rem1 = gpuRead(0x92a);
        if (st.submissions != st.completed && rptr == lastRptr && ib1 == lastIb1 && rem1 == lastRem1) hangRows(t);
        lastRptr = rptr; lastIb1 = ib1; lastRem1 = rem1;
        uint32_t closed = 0;
        const uint32_t opened = native_adreno_publish_stats(&closed);
        char line[sizeof native_gpu_rows[0]];
        snprintf(line, sizeof line,
            "LIVE irq%u last%x seen%x m%u | sub %u/%u t%u p%u | uc %u/%u | pool %u/%u/%u fb%u sc%u | busy %u",
            gIrqCount, gIrqLast, gIrqSeen, gIrqMasked, st.submissions, st.completed, st.timeouts, st.poisoned_jobs, opened, closed,
            native_adreno_pool_stats[3], native_adreno_pool_stats[4], native_adreno_pool_stats[0], native_adreno_pool_stats[2],
            native_adreno_pool_stats[5], permille);
        // The idle counters move on every fence wait: refresh the row only when the work itself changed.
        static uint32_t lastSub = ~0u, lastOpened = ~0u, lastIrqMasked = ~0u;
        if (st.submissions != lastSub || opened != lastOpened || gIrqMasked != lastIrqMasked || t % 30 == 0) {
            strlcpy(native_gpu_rows[kRowLive], line, sizeof native_gpu_rows[0]);
            lastSub = st.submissions; lastOpened = opened; lastIrqMasked = gIrqMasked;
            changed();
        }
        // The GPU SMMU's global and context faults: sCR0 (0x0; USFCFG bit 10: 0 lets a stream no SMR matches through untranslated),
        // sGFSR (0x48; bit 1 USF, an unidentified stream), sGFSYNR0..2 (0x50..0x58; GFSYNR1's low half the stream id), and context
        // bank 0's FSR (0x58), FSYNR0 (0x68) and FAR. A stream of the GPU outside its four routes would read and write without a
        // fault of its context bank.
        const uint32_t bank0 = smmuBank0();
        snprintf(line, sizeof line, "FAULTS cr0 %x gfsr %x syn %x/%x/%x | cb0 fsr %x syn %x far %x%08x", smmuRead(0x0), smmuRead(0x48),
                 smmuRead(0x50), smmuRead(0x54), smmuRead(0x58), smmuRead(bank0 + 0x58), smmuRead(bank0 + 0x68),
                 smmuRead(bank0 + 0x64), smmuRead(bank0 + 0x60));
        if (strncmp(line, native_gpu_rows[kRowFaults], sizeof line)) {
            strlcpy(native_gpu_rows[kRowFaults], line, sizeof native_gpu_rows[0]);
            changed();
        }
        ucheRow();
        submitRow();
        static bool oobDumped;
        if (!oobDumped && (gIrqSeen & (1u << 24))) {oobDumped = true; regDump("oob");}   // RBBM_INT_0 UCHE_OOB_ACCESS
        IOSleep(2000);
    }
}

// The CPU's memory bandwidth, as a hint of the DDR level: Linux votes the GPU's DDR bandwidth through the interconnect framework
// (opp-peak-kBps of each GPU level) and the GMU's own table has one "off" level (a650_build_bw_table); this kernel votes nothing,
// so DDR runs at whatever the firmware left. 32 MiB set with memset and copied with memcpy, the best of three, in MB/s (2026-10-24,
// two rounds: memset 18.2 and 19.3 GB/s, memcpy 8.4 and 8.1 GB/s).
void memBench() {
    constexpr size_t kBytes = 32u << 20;
    OSSharedPtr<IOBufferMemoryDescriptor> bufA(IOBufferMemoryDescriptor::withCapacity(kBytes, kIODirectionInOut), OSNoRetain);
    OSSharedPtr<IOBufferMemoryDescriptor> bufB(IOBufferMemoryDescriptor::withCapacity(kBytes, kIODirectionInOut), OSNoRetain);
    uint8_t *a = bufA ? static_cast<uint8_t *>(bufA->getBytesNoCopy()) : nullptr;
    uint8_t *b = bufB ? static_cast<uint8_t *>(bufB->getBytesNoCopy()) : nullptr;
    if (!a || !b) IOLog("gpu mem: no 32 MiB buffers (%d %d)\n", a != nullptr, b != nullptr);
    if (a && b) {
        uint64_t best[2] = {~0ull, ~0ull};
        for (int i = 0; i < 3; ++i) {
            const uint64_t t0 = mach_absolute_time();
            memset(a, i + 1, kBytes);
            const uint64_t t1 = mach_absolute_time();
            memcpy(b, a, kBytes);
            const uint64_t t2 = mach_absolute_time();
            if (t1 - t0 < best[0]) best[0] = t1 - t0;
            if (t2 - t1 < best[1]) best[1] = t2 - t1;
        }
        uint64_t ns[2] = {};
        absolutetime_to_nanoseconds(best[0], &ns[0]);
        absolutetime_to_nanoseconds(best[1], &ns[1]);
        IOLog("gpu mem: memset %llu MB/s, memcpy %llu MB/s (32 MiB, best of 3)\n",
              ns[0] ? (unsigned long long)(uint64_t(kBytes) * 1000 / ns[0]) : 0ull, ns[1] ? (unsigned long long)(uint64_t(kBytes) * 1000 / ns[1]) : 0ull);
    }
}

void gpuThread(void *, wait_result_t) {
    IOSleep(5000);
    memBench();
    native_gpu_step = 1;
    unsigned have = 0;
    for (unsigned w = 0; w < GPB_WINDOWS; ++w) if (mapWindow(w)) have |= 1u << w;
    // Which of the probe's register windows actually mapped. Without this, a step that skips
    // because its window is missing is indistinguishable from one that ran and failed.
    snprintf(native_gpu_rows[kRowLoad], sizeof native_gpu_rows[0], "MAP have=%02x want=%02x", have, (1u << GPB_WINDOWS) - 1);
    const gpb_io io = {nullptr, rd, wr, delayUs};
    gpb_result &r = native_gpu_result;
    native_gpu_step = 10;
    gpb_run(&io, gMode, have, &r);
    rowsBasic(r);
    if (gMode < 5) {native_gpu_step = 99; return;}
    commandDb();
    uint32_t cx_gmu = r.cc_gmu, ahb = r.cc_ahb;
    if (gMode >= 6) {
        native_gpu_step = 70;
        gpb_gmu_clock_on &o = native_gpu_clocks;
        gpb_gmu_clocks_on(&io, &r, &o);
        snprintf(native_gpu_rows[kRowClk], sizeof native_gpu_rows[0],
                 "CLK step %u failed %d polls %u/%u gpll0 %08x rcg %08x cfg %08x cx_gmu %08x",
                 o.step, o.failed, o.rcg_polls, o.branch_polls, o.vote, o.gmu_cmd, o.gmu_cfg, o.cx_gmu);
        snprintf(native_gpu_rows[kRowClk2], sizeof native_gpu_rows[0], "CLK ahb %08x cxo %08x axi %08x memnoc %08x pll1 %08x",
                 o.ahb, o.cxo, o.axi, o.memnoc, o.pll1_mode);
        changed();
        cx_gmu = o.cx_gmu; ahb = o.ahb;
    }
    gmuState(io, r, cx_gmu, ahb);
    if (gMode >= 7) gmuColdStart(r, native_gpu_clocks);
    if (gMode >= 8 && gColdOk) gxOn();
    const bool cp = gMode >= 9 && native_gpu_gx.gx_on && gpuContext() && cpStart();
    const bool irq = gMode >= 10 && cp && leaveSecure() && gpuInterrupt();
    if (irq) regDump("init");
    if (gMode >= 11 && irq && publish()) {
        if (selfTest() && gMode >= 12) metal();
        native_gpu_step = 99;
        heartbeat();
    }
    native_gpu_step = 99;
}
}  // namespace

extern "C" void native_gpu_probe(void) {
    unsigned mode = 0;
    if (gMode || !PE_parse_boot_argn("pipa-gpu", &mode, sizeof mode) || !mode) return;
    gMode = mode > 12 ? 12 : mode;
    thread_t thread = nullptr;
    if (kernel_thread_start(gpuThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
