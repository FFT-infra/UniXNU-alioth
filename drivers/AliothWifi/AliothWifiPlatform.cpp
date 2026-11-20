/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* alioth QCA6390 board layer: the callbacks `nw_abi.h` asks of the board, on the registers and
 * paths this board already owns: the config window and its capability offsets, BAR0, the MSI
 * target, the SMMU stream (bypass), the DMA arena, the firmware reader and the WLAN rail votes,
 * plus the worker thread that drives the shared runtime's dispatcher. The fact sheet behind every
 * constant is docs/wifi-qca6390-board.md. */
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/vnode.h>
#include <sys/uio.h>
#include <sys/fcntl.h>
#include <sys/random.h>
#include "nw_abi.h"

/* The same entry point the package's other kernel threads use (GpuProbe, ArmCpu). */
extern "C" kern_return_t kernel_thread_start(void (*)(void *, wait_result_t), void *, thread_t *);

namespace {

/* ---- the windows this board owns ------------------------------------------------------------ */
struct Window {
    IOMemoryMap *map = nullptr;
    volatile uint8_t *v = nullptr;
    size_t n = 0;
    bool open(uint64_t pa, size_t bytes) {
        auto *memory = IODeviceMemory::withRange(pa, bytes);
        if (!memory) return false;
        map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
        memory->release();
        n = bytes;
        v = map ? reinterpret_cast<volatile uint8_t *>(map->getVirtualAddress()) : nullptr;
        return v != nullptr;
    }
    uint32_t r(size_t o) const { uint32_t x = *reinterpret_cast<volatile uint32_t *>(v + o); __asm__ volatile("dmb oshld" ::: "memory"); return x; }
    void w(size_t o, uint32_t x) { *reinterpret_cast<volatile uint32_t *>(v + o) = x; __asm__ volatile("dsb oshst" ::: "memory"); }
};

/* PCIe0 windows, from kona-pcie.dtsi:6-13 and the firmware's memory range. BAR0 is placed at the
 * start of the outbound MEM window (package/alioth_pcie.c step_config), the doorbell page at
 * 0x17a10000 raises SPI 768 when written with 768 (its record), and the config window plus the
 * ATU CFG0 view for bus 1 answered on the board (pc4). */
constexpr uint64_t kBarBase = 0x60300000, kBarBytes = 0x100000;
constexpr uint64_t kConfBase = 0x60100000, kConfBytes = 0x1000;
constexpr uint64_t kMsiDoorbell = 0x17a10000, kMsiBytes = 0x1000;
constexpr uint32_t kVectorBase = 768;      /* the first GIC SPI the doorbell raises */
constexpr uint16_t kDeviceId = 0x1101;     /* QCA6390 */
constexpr uint16_t kMsiCap = 0x50;         /* MSI capability offset, read on the board */

Window gBar, gConf, gDoorbell;
bool gOpen;

uint64_t nowMs() {
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
    return ns / 1000000;
}

/* IOLog does not reach this board's serial console (the in23c boot carried not one IOLog line),
 * so every message this layer emits also goes through the USB debug channel the console capture
 * reads. Level 7 is the shared runtime's per-packet trace and is dropped: the channel also carries
 * the console log itself. */
extern "C" size_t qcom_usb_debug_write(const void *, size_t) __attribute__((weak_import));

void wifiLog(const char *fmt, ...) {
    char body[192];
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    const int n = vsnprintf(body, sizeof body, fmt, ap);
    __builtin_va_end(ap);
    if (n <= 0) return;
    for (size_t len = strlen(body); len && body[len - 1] == '\n'; ) body[--len] = 0;
    if (&qcom_usb_debug_write) {
        char out[224];
        const int m = snprintf(out, sizeof out, "WIFI %s\n", body);
        if (m > 0) qcom_usb_debug_write(out, (size_t)m < sizeof out ? (size_t)m : sizeof out - 1);
    }
}

void say(int level, const char *line) {
    if (level < 7 && line) wifiLog("[%d] %s", level, line);
}

/* ---- config space -------------------------------------------------------------------------- */
/* Offsets with bit 31 set are inside the PCIe capability (nw_abi.h); the capability is at 0xd0 on
 * this endpoint and the shim passes capability-relative offsets, so the caller's offset is the
 * capability-relative one. The ATU CFG0 view for bus 1 is left programmed by package/alioth_pcie.c
 * step_config, so a read is a load from the config window. */
/* The window answers 32-bit loads only (msi3 hung on a 16-bit access), so a narrower request is
 * served from one aligned word and the caller's bytes are shifted out of it. ath11k asks for the
 * device id with a 16-bit read and the first version of this returned -1, which left its local
 * device_id at 0 and produced "pci device id mismatch: 0x0 0x1101" (in26). */
uint32_t cfgAt(int off) {
    if (off & 0x80000000) return 0xd0u + (uint32_t)(off & 0x7fffffff);
    return (uint32_t)off;
}

int cfgRead(int off, int size, uint32_t *v) {
    if (!gOpen || !v || (size != 1 && size != 2 && size != 4)) return -1;
    const uint32_t at = cfgAt(off) & 0xfff;
    if ((at & (uint32_t)(size - 1)) || at > 0x1000u - (uint32_t)size) return -1;
    const uint32_t word = gConf.r(at & ~3u);
    const unsigned shift = (at & 3u) * 8;
    const uint32_t mask = size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1u);
    *v = (word >> shift) & mask;
    return 0;
}

int cfgWrite(int off, int size, uint32_t v) {
    if (!gOpen || (size != 1 && size != 2 && size != 4)) return -1;
    const uint32_t at = cfgAt(off) & 0xfff;
    if ((at & (uint32_t)(size - 1)) || at > 0x1000u - (uint32_t)size) return -1;
    const unsigned shift = (at & 3u) * 8;
    const uint32_t mask = (size == 4 ? 0xffffffffu : ((1u << (size * 8)) - 1u)) << shift;
    /* Read-modify-write: the neighbouring fields of the same word belong to other registers. */
    const uint32_t word = gConf.r(at & ~3u);
    gConf.w(at & ~3u, (word & ~mask) | ((v << shift) & mask));
    return 0;
}

/* Vector 0 is written to the doorbell PCI address with the GIC's own IRQ number as data; vector N
 * is data kVectorBase + N. The inbound iATU translates 0x60400000.. to 0x17a10000. */
int msiTarget(uint32_t *lo, uint32_t *hi, uint32_t *data) {
    if (!lo || !hi || !data) return -1;
    *lo = 0x60400040;
    *hi = 0;
    *data = kVectorBase;
    return 0;
}

/* Physical mappings the shared stack asks for outside BAR0: the QMI device-info BAR and the
 * firmware's target-memory windows. Each call keeps its IOMemoryMap alive for the life of the
 * boot (the ABI has no unmap) and maps cache-inhibited, which is what these registers need. */
void *mapPhys(uint64_t pa, size_t n) {
    if (!pa || !n) return nullptr;
    auto *memory = IODeviceMemory::withRange(pa, n);
    if (!memory) return nullptr;
    IOMemoryMap *map = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
    memory->release();
    if (!map) { wifiLog("map_phys %#llx+%#zx did not map", (unsigned long long)pa, n); return nullptr; }
    wifiLog("map_phys %#llx+%#zx -> %p", (unsigned long long)pa, n,
            reinterpret_cast<void *>(map->getVirtualAddress()));
    return reinterpret_cast<void *>(map->getVirtualAddress());
}

/* ---- SMMU: the endpoint's stream ----------------------------------------------------------- */
/* The endpoint is SID 0x1c01 (PCIe requester 0x100, kona-pcie.dtsi:92-93). It needs a
 * pass-through stream: MHI hands the endpoint physical addresses and no page table exists here.
 *
 * Do not write S2CR TYPE=BYPASS. On this firmware family the store reads back as FAULT, which is
 * why the wifi5 boot lost all 80 free slots ("wrote 0x10000 read 0x20000"). The shape that works
 * is the one UniXNU-sheng uses for the same apps SMMU (reference/repo/UniXNU-pipa/package/
 * SmmuBypass.h, "a TYPE=BYPASS S2CR reads back as FAULT on this firmware"): keep one context bank
 * as a pass-through bank - SCTLR.M=0 (stage 1 off), CBAR=0x10000 (TYPE=S1_TRANS_S2_BYPASS),
 * CBA2R.VA64=1 - and point the stream at it with S2CR TYPE=TRANSLATE. Stage 1 disabled with
 * stage 2 bypass makes the bank identity, and several streams may share it.
 *
 * Geometry is the same as src/NativeUfs-alioth/SmmuTakeover.h: ID0[7:0] SMR groups, ID1[7:0]
 * context banks, ID1[30:28] pages per bank. S2CR is written before the SMR publishes the match.
 * A stream already on the bank is a reuse; an invalid SMR slot is a free slot. `refs` counts
 * valid streams per bank so a bank that another stream uses is never reprogrammed. */
constexpr uint64_t kSmmuBase = 0x15000000, kSmmuBytes = 0x100000;
constexpr uint32_t kEndpointSid = 0x1c01;
constexpr uint32_t kS2crTypeMask = 3u << 16, kS2crCbndx = 0xffu;
constexpr uint32_t kSmrValid = 1u << 31;
constexpr uint32_t kCbarBypass = 0x10000;   /* CBAR.TYPE=1 (S1 trans, S2 bypass), CBNDX=0 */
/* kona-pcie.dtsi:92-93 maps requester 0x0 to SID 0x1c00 and requester 0x100 to SID 0x1c01.
 * The endpoint's own DMA should arrive as 0x100, but which requester ID the root complex stamps
 * on an inbound read is a property of the controller, not of the device tree, so both are
 * claimed. An unmatched SID faults here (sCR0.USFCFG is set), and a faulted non-posted read is
 * exactly the shape that hangs the BHI_STATUS poll. */
constexpr uint32_t kEndpointSids[] = {0x1c01, 0x1c00};
constexpr uint32_t kPrimarySid = 0x1c01;
Window gSmmu;
/* What the stream claim found, for the console: [0] ID0, [1] sCR0, [2] GFSR before, [3] the
 * slot, [4] the S2CR readback, [5] the SMR readback, [6] GFSR after. */
volatile uint32_t gStreamClaim[7];

/* ---- PCIe TBU ------------------------------------------------------------------------------- */
/* Inbound DMA from the endpoint crosses anoc_1_pcie_tbu (0x151a1000, SIDs 0x1c00..0x1c1f;
 * msm-arm-smmu-kona.dtsi:348-353) before it reaches the apps SMMU. The vendor driver halts a TBU
 * while it reprograms a domain and resumes it afterwards (arm-smmu.c:6031/6109); a TBU that the
 * bootloader left halted stalls every inbound read. That is the shape of the BHI doorbell hang:
 * the host's next read of the endpoint never completes while the endpoint waits for an image
 * that cannot arrive. The halt bit is register 0x0 bit 16 and the acknowledgement is 0x20. */
constexpr uint64_t kPcieTbuBase = 0x151a1000, kPcieTbuStatus = 0x15182238;
constexpr uint32_t kTbuHaltBit = 1u << 16;

void resumePcieTbu() {
    Window tbu, status;
    if (!tbu.open(kPcieTbuBase, 0x1000)) { wifiLog("pcie_tbu did not map"); return; }
    const uint32_t haltBefore = tbu.r(0x0), ackBefore = tbu.r(0x20);
    uint32_t statusA = 0, statusB = 0;
    if (status.open(kPcieTbuStatus, 8)) { statusA = status.r(0); statusB = status.r(4); }
    if (haltBefore & kTbuHaltBit) {
        tbu.w(0x0, haltBefore & ~kTbuHaltBit);
        __asm__ volatile("dsb sy" ::: "memory");
    }
    const uint32_t haltAfter = tbu.r(0x0), ackAfter = tbu.r(0x20);
    wifiLog("pcie_tbu ctrl=%#x->%#x ack=%#x->%#x status=%#x/%#x", haltBefore, haltAfter,
            ackBefore, ackAfter, statusA, statusB);
}

unsigned freeSmrSlot(unsigned groups) {
    for (unsigned i = 0; i < groups; ++i)
        if (!(gSmmu.r(0x800 + 4 * i) & kSmrValid)) return i;
    return 0xffffffffu;
}

/* S2CR first, then the SMR that publishes the match. Readbacks are the only proof that the
 * writes landed: this firmware stores a TYPE=BYPASS S2CR as FAULT (see the header comment), and a
 * stream whose SMR looks right but whose S2CR did not land would still fault. */
bool claimSid(uint32_t sid, unsigned bank, unsigned groups, bool exids, unsigned *outSlot,
              uint32_t *outS2cr, uint32_t *outSmr) {
    const unsigned slot = freeSmrSlot(groups);
    if (slot == 0xffffffffu) { wifiLog("no free SMR slot for SID %#x", sid); return false; }
    const uint32_t smrValue = exids ? sid : (kSmrValid | sid);
    gSmmu.w(0xc00 + 4 * slot, bank);            /* TYPE=TRANSLATE into the pass-through bank */
    const uint32_t s2cr = gSmmu.r(0xc00 + 4 * slot);
    if (s2cr != bank) {
        wifiLog("S2CR at slot %u did not take bank %u: read %#x", slot, bank, s2cr);
        return false;
    }
    gSmmu.w(0x800 + 4 * slot, smrValue);
    const uint32_t smr = gSmmu.r(0x800 + 4 * slot);
    if (smr != smrValue) {
        wifiLog("SMR at slot %u did not take %#x: read %#x", slot, smrValue, smr);
        return false;
    }
    *outSlot = slot; *outS2cr = s2cr; *outSmr = smr;
    return true;
}

bool claimStream() {
    if (!gSmmu.open(kSmmuBase, kSmmuBytes)) { wifiLog("apps_smmu did not map\n"); return false; }
    const uint32_t id0 = gSmmu.r(0x20), id1 = gSmmu.r(0x24), id2 = gSmmu.r(0x28);
    const uint32_t scr0 = gSmmu.r(0x0), sacr = gSmmu.r(0x10), gfsrBefore = gSmmu.r(0x48);
    const unsigned groups = id0 & 0xff, banks = id1 & 0xff;
    const unsigned pages = 1u << (((id1 >> 28) & 7) + 1);
    const bool exids = (id0 & (1u << 8)) != 0;
    gStreamClaim[0] = id0; gStreamClaim[1] = scr0; gStreamClaim[2] = gfsrBefore;
    wifiLog("apps_smmu id0=%#x id1=%#x id2=%#x scr0=%#x sacr=%#x gfsr=%#x groups=%u banks=%u pages=%u exids=%u",
            id0, id1, id2, scr0, sacr, gfsrBefore, groups, banks, pages, exids ? 1u : 0u);
    if (!groups || groups > 128 || banks < 2 || banks > 128 || pages > 128 || (pages + banks) * 4096 > 0xd0000) {
        wifiLog("unsupported apps_smmu geometry (ID0 %#x ID1 %#x)", id0, id1);
        return false;
    }
    unsigned refs[128] = {};
    for (unsigned i = 0; i < groups; ++i) {
        const uint32_t smr = gSmmu.r(0x800 + 4 * i);
        if (!(smr & kSmrValid)) continue;
        const uint32_t s2cr = gSmmu.r(0xc00 + 4 * i);
        if ((s2cr & kS2crTypeMask) == 0 && (s2cr & kS2crCbndx) < banks) ++refs[s2cr & kS2crCbndx];
    }
    /* Top bank first, as the sheng kit does. A bank is usable when stage 1 is off. Prefer one no
     * valid stream references; a bank already in the pass-through shape may be shared. */
    int chosen = -1; bool ready = false;
    for (int bank = (int)banks - 1; bank >= 1; --bank) {
        const uint32_t sctlr = gSmmu.r((pages + (unsigned)bank) * 4096);
        if (sctlr & 1) continue;    /* stage 1 enabled: live */
        const uint32_t cbar = gSmmu.r(0x1000 + 4 * (unsigned)bank);
        const uint32_t cba2r = gSmmu.r(0x1800 + 4 * (unsigned)bank);
        const bool isReady = sctlr == 0 && cbar == kCbarBypass && (cba2r & ~1u) == 0 && (cba2r & 1u);
        if (!refs[bank]) { chosen = bank; ready = isReady; break; }
        if (isReady && chosen < 0) { chosen = bank; ready = true; }
    }
    if (chosen < 0) {
        wifiLog("no pass-through context bank for SID %#x: %u banks, no unreferenced bank with stage 1 off", kPrimarySid, banks);
        return false;
    }
    const unsigned bank = (unsigned)chosen;
    const uint32_t cb = (pages + bank) * 4096;
    if (!ready) {
        gSmmu.w(cb, 0);
        gSmmu.w(0x1800 + 4 * bank, 1);          /* CBA2R.VA64: 64-bit pass-through addresses */
        gSmmu.w(0x1000 + 4 * bank, kCbarBypass);
        const uint32_t sctlr = gSmmu.r(cb), cbar = gSmmu.r(0x1000 + 4 * bank), cba2r = gSmmu.r(0x1800 + 4 * bank);
        if (sctlr || cbar != kCbarBypass || cba2r != 1) {
            wifiLog("bank %u did not take the pass-through shape: sctlr=%#x cbar=%#x cba2r=%#x", bank, sctlr, cbar, cba2r);
            return false;
        }
    }
    bool primary = false;
    for (uint32_t sid : kEndpointSids) {
        unsigned slot = 0; uint32_t s2cr = 0, smr = 0;
        const bool ok = claimSid(sid, bank, groups, exids, &slot, &s2cr, &smr);
        wifiLog("SID %#x %s: slot=%u bank=%u smr=%#x s2cr=%#x", sid, ok ? "claimed" : "FAILED", slot, bank, smr, s2cr);
        if (!ok) continue;
        if (sid == kPrimarySid) {
            primary = true;
            gStreamClaim[3] = slot; gStreamClaim[4] = s2cr; gStreamClaim[5] = smr;
        }
    }
    if (!primary) return false;
    /* The S2CR/SMR table is cached by the SMMU just like a translation. Linux invalidates the
     * non-secure global TLB and waits on sTLBGSTATUS after installing a stream mapping
     * (arm-smmu.c: arm_smmu_master_alloc_smes), and a stale unmatched-stream fault is exactly one
     * of the entries this one has to displace. Bounded: the status bit normally clears at once. */
    const uint32_t gsyncBefore = gSmmu.r(0x74);
    gSmmu.w(0x68, 0);                           /* sTLBIALLNSNH */
    gSmmu.w(0x70, 0);                           /* sTLBGSYNC */
    unsigned spins = 0;
    while ((gSmmu.r(0x74) & 1u) && spins < 10000) ++spins;
    /* sCR0.USFCFG (bit 10) makes an unmatched stream fault instead of bypassing. Any requester
     * this table still does not name would otherwise hang on its first read; clear it and report
     * whether the write landed. */
    const uint32_t scr0Before = gSmmu.r(0x0);
    gSmmu.w(0x0, scr0Before & ~(1u << 10));
    const uint32_t scr0After = gSmmu.r(0x0);
    const uint32_t gfsrAfter = gSmmu.r(0x48);
    gStreamClaim[6] = gfsrAfter;
    wifiLog("SMMU sync: gsync=%#x spins=%u scr0=%#x->%#x usfcfg=%u gfsr=%#x->%#x bank=%u refs=%u",
            gsyncBefore, spins, scr0Before, scr0After, (scr0After >> 10) & 1u, gfsrBefore, gfsrAfter,
            bank, refs[bank]);
    return true;
}

/* ---- DMA: one physically contiguous arena --------------------------------------------------- */
/* BHI offers the image and the rings as physical addresses under bypass, so the arena must be
 * physically contiguous; inTaskWithPhysicalMask gives that the same way the GPU driver gets its
 * firmware pages. Slices are handed out first-fit and never returned to a pool. */
constexpr size_t kArenaBytes = 32 << 20;
IOBufferMemoryDescriptor *gArena;
uint8_t *gArenaVa;
size_t gArenaUsed;

uint64_t arenaPhys(const void *p);

bool arenaOpen() {
    if (gArena) return true;
    /* dma_alloc_coherent promises the driver memory the device may read while the CPU still holds
     * its own writes. IOBufferMemoryDescriptor's own mapping is cacheable (cache mode 0 is
     * kIOMemoryBufferCacheDefault), so the arena is mapped again with kIOMapInhibitCache and every
     * slice is handed out of that mapping. The first transfer that depends on it is MHI's SBL load:
     * it memcpy's the image into the buffer and rings the doorbell with no cache operation of its
     * own. in32 rang that doorbell and then stopped while reading BHI_STATUS.
     *
     * The coherent mask ath11k asks for is 32 bits (pci.c: ATH11K_PCI_COHERENT_DMA_MASK 32), so the
     * arena is requested below 4 GiB first; a boot where that fails says so and falls back. */
    gArena = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                              kArenaBytes, 0x00000000fffff000ULL);
    if (!gArena) {
        wifiLog("no contiguous %zu-byte arena below 4 GiB; asking for any address\n", kArenaBytes);
        gArena = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                                  kArenaBytes, 0xfffffffffffff000ULL);
    }
    if (!gArena) { wifiLog("no contiguous %zu-byte arena\n", kArenaBytes); return false; }
    if (gArena->prepare() != kIOReturnSuccess) { gArena->release(); gArena = nullptr; return false; }
    IOMemoryMap *map = gArena->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache);
    if (!map) { wifiLog("the arena did not map uncached\n"); gArena->release(); gArena = nullptr; return false; }
    gArenaVa = reinterpret_cast<uint8_t *>(map->getVirtualAddress());
    gArenaUsed = 0;
    wifiLog("DMA arena %zu bytes at %p phys %#llx (uncached)\n", kArenaBytes, gArenaVa,
            (unsigned long long)arenaPhys(gArenaVa));
    return gArenaVa != nullptr;
}

uint64_t arenaPhys(const void *p) {
    if (!gArena || !p) return 0;
    const uint8_t *at = static_cast<const uint8_t *>(p);
    if (at < gArenaVa || at >= gArenaVa + kArenaBytes) return 0;
    IOByteCount length = 0;
    const addr64_t pa = gArena->getPhysicalSegment((vm_offset_t)(at - gArenaVa), &length, kIOMemoryMapperNone);
    return (uint64_t)pa;
}

void *dmaAlloc(size_t n, uint64_t *bus) {
    if (!arenaOpen() || !n) return nullptr;
    /* dma_alloc_coherent is page aligned. MHI's BHI buffer and its transfer rings are handed to
     * the endpoint as addresses; the BHI image the endpoint fetches during boot is the first of
     * them, so the slices carry that same alignment instead of a 64-byte one. */
    const size_t aligned = (n + 4095) & ~(size_t)4095;
    if (gArenaUsed + aligned > kArenaBytes) { wifiLog("arena exhausted (%zu + %zu)\n", gArenaUsed, aligned); return nullptr; }
    void *p = gArenaVa + gArenaUsed;
    const uint64_t pa = arenaPhys(p);
    if (!pa) return nullptr;
    gArenaUsed += aligned;
    if (bus) *bus = pa;
    /* MHI hands the endpoint these addresses. The SBL load is the first transfer the endpoint
     * makes for itself, so the address and size it is given are part of that reading; the count
     * stays bounded because a quiet boot makes a few dozen of these. */
    static unsigned shown;
    if (shown < 24) wifiLog("dma[%u] %zu bytes phys %#llx", shown++, aligned, (unsigned long long)pa);
    return p;
}
void dmaFree(size_t n, void *p, uint64_t bus) { (void)n; (void)p; (void)bus; }
uint64_t dmaMap(void *p, size_t n, int dir) { (void)n; (void)dir; return arenaPhys(p); }
/* arm64 declares flush_dcache(vm_offset_t, unsigned, int phys) and no separate invalidate; a
 * clean-and-invalidate covers both directions for this arena. */
void dmaSync(uint64_t bus, size_t n, int dir) {
    (void)dir;
    if (!bus || !n) return;
    flush_dcache((vm_offset_t)bus, (unsigned)n, 1);
}

/* ---- firmware ------------------------------------------------------------------------------ */
/* The addon writes the QCA6390 hw2.0 set under /private/var/ngpu/firmware/qca6390; each file is
 * read once and kept. A missing file is reported by name, never substituted. */
struct Firmware {
    char name[48];
    uint8_t *data;
    size_t size;
};
Firmware gFirmware[8];
unsigned gFirmwareCount;

int fwGet(const char *name, const uint8_t **data, size_t *size) {
    if (!name || !data || !size) return -1;
    /* The ABI passes the name the driver asked for, which ath11k builds as
     * "ath11k/QCA6390/hw2.0/<file>"; this board keeps the files flat, so the lookup is by the last
     * component. That is also what makes the API1 fallback work at all: ath11k tries
     * firmware-2.bin first, and when it is absent (this board ships amss.bin, m3.bin and
     * board-2.bin) it asks for exactly those three names. */
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (!*base) return -1;
    for (unsigned i = 0; i < gFirmwareCount; ++i)
        if (!strcmp(gFirmware[i].name, base)) { *data = gFirmware[i].data; *size = gFirmware[i].size; return 0; }
    if (gFirmwareCount >= sizeof gFirmware / sizeof gFirmware[0]) return -1;
    char path[160];
    snprintf(path, sizeof path, "/private/var/ngpu/firmware/qca6390/%s", base);
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t vp = NULLVP;
    int error = vnode_open(path, FREAD, 0, 0, &vp, ctx);
    if (error) { wifiLog("firmware %s open %d\n", path, error); return -1; }
    struct vnode_attr va;
    VATTR_INIT(&va);
    VATTR_WANTED(&va, va_data_size);
    error = vnode_getattr(vp, &va, ctx);
    if (error || va.va_data_size <= 0 || va.va_data_size > (16 << 20)) {
        wifiLog("firmware %s size %d (%d)\n", path, error, (int)va.va_data_size);
        vnode_close(vp, FREAD, ctx);
        return -1;
    }
    const size_t bytes = (size_t)va.va_data_size;
    uint8_t *buffer = static_cast<uint8_t *>(IOMalloc(bytes));
    if (!buffer) { vnode_close(vp, FREAD, ctx); return -1; }
    size_t got = 0;
    while (got < bytes) {
        uio_t uio = uio_create(1, (off_t)got, UIO_SYSSPACE, UIO_READ);
        if (!uio) break;
        uio_addiov(uio, CAST_USER_ADDR_T(buffer + got), bytes - got);
        const int read_error = VNOP_READ(vp, uio, 0, ctx);
        const size_t step = (bytes - got) - (size_t)uio_resid(uio);
        uio_free(uio);
        if (read_error || !step) break;
        got += step;
    }
    vnode_close(vp, FREAD, ctx);
    if (got != bytes) { IOFree(buffer, bytes); return -1; }
    Firmware &slot = gFirmware[gFirmwareCount++];
    strlcpy(slot.name, base, sizeof slot.name);
    slot.data = buffer;
    slot.size = got;
    *data = buffer;
    *size = got;
    wifiLog("firmware %s %zu bytes\n", base, got);
    return 0;
}

/* ---- the ABI ------------------------------------------------------------------------------- */
struct lx_platform gPlatform;
struct nw_board gBoard;

}  // namespace

/* The five cnss2 supplies, with alioth's override that puts aon and dig both on pm8150_s6
 * (smpa6), so four distinct rails carry five supplies (docs/wifi-qca6390-board.md). */
extern "C" bool alioth_rpmh_rail_vote(const char *name, uint32_t millivolts);

bool voteWlanRails() {
    static const struct { const char *name; uint32_t mv; } rails[] = {
        {"smpa6", 950},   /* vdd-wlan-aon and vdd-wlan-dig */
        {"smpa4", 1800},  /* vdd-wlan-io */
        {"smpa5", 1900},  /* vdd-wlan-rfa1 */
        {"smpc8", 1350},  /* vdd-wlan-rfa2 */
    };
    for (const auto &rail : rails)
        if (!alioth_rpmh_rail_vote(rail.name, rail.mv)) {
            wifiLog("rail %s did not take the vote\n", rail.name);
            return false;
        }
    return true;
}

/* The endpoint's BAR answers only after package/alioth_pcie.c has trained the link, opened the root
 * complex's command register and programmed the outbound window; that is step 17, and it runs only
 * when the command line carries pcie=N. A read of an unbacked BAR address hangs the CPU, which is
 * what in24/in25 did (both boots stopped at "opening the PCIe windows" and the board then needed a
 * power cycle). So the readiness check comes before any window is touched.
 *
 * Step 19 is the one this layer needs, not 17: 17 programs the outbound window the BAR is reached
 * through, and 19 programs the inbound window that turns the endpoint's MSI write at PCI
 * 0x60400040 into the doorbell at 0x17a10000. Without 19 the vectors this layer hands out would
 * never arrive. */
extern "C" uint32_t alioth_pcie_completed_step(void) __attribute__((weak_import));
/* The endpoint's DMA path: an inbound iATU region over the arena, identity-translated. */
extern "C" uint32_t alioth_pcie_dma_window(uint32_t pci_base, uint32_t system_base, uint32_t size)
    __attribute__((weak_import));
constexpr uint32_t kPcieStepInboundWindow = 19;

extern "C" int alioth_wifi_board_open(void) {
    if (gOpen) return 0;
    if (!&alioth_pcie_completed_step || alioth_pcie_completed_step() < kPcieStepInboundWindow) {
        wifiLog("the PCIe controller is not ready (step %u of %u); add pcie=%u to the command line",
                &alioth_pcie_completed_step ? alioth_pcie_completed_step() : 0u, kPcieStepInboundWindow,
                kPcieStepInboundWindow);
        return -1;
    }
    wifiLog("opening the PCIe windows");
    if (!gBar.open(kBarBase, kBarBytes) || !gConf.open(kConfBase, kConfBytes) || !gDoorbell.open(kMsiDoorbell, kMsiBytes)) {
        wifiLog("a PCIe window did not map");
        return -1;
    }
    gPlatform.now_ms = nowMs;
    gPlatform.now_ns = nullptr;
    gPlatform.boottime_ns = nullptr;
    gPlatform.sleep_ms = [](unsigned ms) { IOSleep(ms); };
    gPlatform.busy_delay_us = [](unsigned us) { IODelay(us); };
    gPlatform.poll = [] {};
    gPlatform.log = say;
    gPlatform.halt = [] {};
    // The ABI frees without a size, so every block carries its length in a 16-byte header.
    gPlatform.alloc = [](size_t n) -> void * {
        char *block = static_cast<char *>(IOMallocZero(n + 16));
        if (!block) return nullptr;
        *reinterpret_cast<size_t *>(block) = n;
        return block + 16;
    };
    gPlatform.free = [](void *p) {
        if (!p) return;
        char *block = static_cast<char *>(p) - 16;
        IOFree(block, *reinterpret_cast<size_t *>(block) + 16);
    };
    if (!voteWlanRails()) return -1;
    wifiLog("rails voted");
    resumePcieTbu();
    if (!claimStream()) return -1;
    wifiLog("stream claimed: ID0 %#x sCR0 %#x slot %u S2CR %#x SMR %#x GFSR %#x->%#x",
            gStreamClaim[0], gStreamClaim[1], gStreamClaim[3], gStreamClaim[4], gStreamClaim[5],
            gStreamClaim[2], gStreamClaim[6]);
    if (!arenaOpen()) return -1;
    wifiLog("arena up");
    /* The endpoint's first self-started transfer is MHI's SBL load, and it reads this arena. Give
     * it an inbound window over exactly that range before anything asks it to read: the MSI window
     * covers one 4 KiB page and nothing else. */
    if (alioth_pcie_dma_window) {
        const uint32_t base = (uint32_t)arenaPhys(gArenaVa);
        const uint32_t window = alioth_pcie_dma_window(base, base, (uint32_t)kArenaBytes);
        wifiLog("dma inbound window %#x..%#x -> %#x: %s", base, base + (uint32_t)kArenaBytes - 1, base,
                window ? "enabled and reads back" : "DID NOT read back");
    } else {
        wifiLog("no inbound-window hook; the endpoint has no DMA path");
    }
    /* The three readings that say whether the endpoint is there at all: BAR0's TCSR_SOC_HW_VERSION
     * carries the WCNSS major in GENMASK(11,8) and must read 2 for this ath11k, and the config
     * window must answer the endpoint's own ids. They come last, so a boot that stops here has already
     * reported the rails, the SMMU stream and the arena, and the hang belongs to the endpoint. */
    {
        const uint32_t hwVersion = gBar.r(0x0224);
        /* MHICFG carries the BHI and BHIe offsets at 0x28 and 0x2c; both are below the window
         * start, so they are safe to read here. A value at or above 0x80000 is an offset the
         * window at 0x80000 must be selected for, which is what the recipe's MHI register hook
         * now does. */
        wifiLog("bar0[0x224]=%#x major=%u config_id=%#x class=%#x bhi=%#x bhie=%#x window=%#x", hwVersion,
                (hwVersion >> 8) & 0xf, gConf.r(0x00), gConf.r(0x08), gBar.r(0x028), gBar.r(0x02c),
                gBar.r(0x310c));
    }
    gPlatform.dma_alloc = dmaAlloc;
    gPlatform.dma_free = dmaFree;
    gPlatform.dma_map = dmaMap;
    gPlatform.dma_sync = dmaSync;
    gPlatform.fw_get = fwGet;
    gPlatform.preempt_disable = nullptr;
    gPlatform.preempt_enable = nullptr;
    gBoard.bar = const_cast<void *>(static_cast<const volatile void *>(gBar.v));
    gBoard.bar_len = kBarBytes;
    gBoard.device_id = kDeviceId;
    gBoard.msi_cap = kMsiCap;
    gBoard.irq_base = (int)kVectorBase;
    gBoard.cfg_read = cfgRead;
    gBoard.cfg_write = cfgWrite;
    gBoard.msi_target = msiTarget;
    gBoard.map_phys = mapPhys;
    gOpen = true;
    wifiLog("windows open (bar %p, conf %p, doorbell %p), rails voted, SID claimed, arena up\n",
          gBoard.bar, gConf.v, gDoorbell.v);
    return 0;
}

/* The station runtime asks the board for nonces (the shared patch in build-alioth-ath11k.py
 * routes nw_sta's nonce to this symbol); XNU's CSPRNG is the source, never a timer. */
extern "C" void AliothWifiRandom(void *out, size_t n) {
    if (out && n) read_random(out, (u_int)n);
}

/* The shared runtime owns no interrupts: its handlers run from its dispatcher, and the station
 * glue's nw_wifi_step() raises every registered vector and pumps it. This thread is that clock
 * until the endpoint's MSI vectors are wired to a GIC handler (then delivery becomes
 * nw_wifi_irq_pending(bits) from the handler, which the shared ABI already carries). */
extern "C" int nw_wifi_step(void);
extern "C" struct nw_wifi_stats nw_wifi_stats;

static volatile int gWorkerStop;
static void wifiWorker(void *, wait_result_t) {
    uint64_t next = 0, started = nowMs();
    for (;;) {
        if (gWorkerStop) break;
        const int busy = nw_wifi_step();
        const uint64_t now = nowMs();
        if (!next) next = now + 2000;
        if (now >= next) {
            next = now + 2000;
            wifiLog("t=%llus stage=%u scans=%u frames=%u beacons=%u bss=%u err=%d",
                    (unsigned long long)((now - started) / 1000), nw_wifi_stats.stage, nw_wifi_stats.scans,
                    nw_wifi_stats.frames, nw_wifi_stats.beacons, nw_wifi_stats.bss_seen, nw_wifi_stats.last_err);
        }
        IOSleep(busy ? 1 : 5);
    }
}

/* The firmware arrives late.  The GPU addon carries it, and the kernel installs the addon from
 * native_owner_data_prepare, which runs on the first exec -- after this call site, which sits in
 * the late display probe.  in29 asked for amss.bin at that point and read ENOENT while the addon
 * was still 60 lines of console away from being written.  So the start waits for the file, on this
 * thread, with a bounded deadline: the PCIe steps and the windows are already up and nothing else
 * needs them. */
static bool firmwareArrived(uint32_t *waitedMs) {
    constexpr uint32_t kDeadlineMs = 60000, kStepMs = 250;
    const char *const files[] = {"/private/var/ngpu/firmware/qca6390/amss.bin",
                                 "/private/var/ngpu/firmware/qca6390/m3.bin",
                                 "/private/var/ngpu/firmware/qca6390/board-2.bin"};
    for (uint32_t waited = 0;; waited += kStepMs) {
        bool all = true;
        for (const char *path : files) {
            vnode_t vp = NULLVP;
            if (vnode_lookup(path, 0, &vp, vfs_context_kernel())) { all = false; break; }
            vnode_put(vp);
        }
        if (all) { *waitedMs = waited; return true; }
        if (waited >= kDeadlineMs) { *waitedMs = waited; return false; }
        IOSleep(kStepMs);
    }
}

extern "C" int alioth_wifi_board_start(void) {
    if (!gOpen && alioth_wifi_board_open() != 0) return -1;
    return 0;
}

/* Everything after the windows belongs on its own thread, for one reason: the firmware arrives
 * from the GPU addon, and the kernel installs that addon from native_owner_data_prepare on the
 * first exec -- which cannot happen while this call site, inside the late display probe, has not
 * returned.  Waiting here would deadlock the boot, so the wait happens here instead. */
static void wifiStart(void *, wait_result_t) {
    uint32_t waited = 0;
    if (!firmwareArrived(&waited)) {
        wifiLog("the firmware did not appear within %u ms; the addon that carries it installs on the first exec",
                waited);
        goto done;
    }
    wifiLog("firmware present after %u ms", waited);
    {
        extern int nw_wifi_start(const struct lx_platform *, const struct nw_board *);
        const int r = nw_wifi_start(&gPlatform, &gBoard);
        wifiLog("nw_wifi_start returned %d", r);
        if (r != 0) goto done;
    }
    wifiWorker(nullptr, THREAD_UNINT);
done:
    thread_terminate(current_thread());
    thread_block(THREAD_CONTINUE_NULL);
}

/* The kernel's entry: the late display probe calls this (weakly) after the PCIe bring-up when the
 * boot argument alioth-wifi=1 is set. Failure is logged and leaves the rest of the boot alone. */
extern "C" void alioth_wifi_start(void) {
    const int r = alioth_wifi_board_start();
    unsigned mode = 0;
    PE_parse_boot_argn("alioth-wifi", &mode, sizeof mode);
    /* alioth-wifi=2 stops after the board setup: rails, PCIe windows, TBU, SMMU stream, arena and
     * the endpoint's identity reads. It measures the whole path that can wedge the NoC without
     * ringing the MHI doorbell, whose first non-posted read is what stops the boot. */
    if (r == 0 && mode >= 2) {
        wifiLog("setup only (alioth-wifi=%u); the runtime is not started", mode);
        return;
    }
    wifiLog("start returned %d; the runtime starts on its own thread once the firmware is there", r);
    if (r != 0) return;
    thread_t thread = nullptr;
    if (kernel_thread_start(wifiStart, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else wifiLog("the wifi start thread did not start");
}
