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
int cfgRead(int off, int size, uint32_t *v) {
    if (!gOpen || !v) return -1;
    uint32_t at;
    if (off & 0x80000000) at = 0xd0u + (uint32_t)(off & 0x7fffffff);
    else at = (uint32_t)off;
    if (size != 4) return -1;
    *v = gConf.r(at & 0xfff);
    return 0;
}

int cfgWrite(int off, int size, uint32_t v) {
    if (!gOpen) return -1;
    uint32_t at;
    if (off & 0x80000000) at = 0xd0u + (uint32_t)(off & 0x7fffffff);
    else at = (uint32_t)off;
    if (size != 4) return -1;
    gConf.w(at & 0xfff, v);
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
/* The first MHI boot uses stream bypass: the SMMUv2 model this board already runs in
 * src/NativeUfs-alioth/SmmuTakeover.h puts the stream count at GR0 0x20, SMR[n] at 0x800 + 4n,
 * S2CR[n] at 0xc00 + 4n, with an exact match written as 0x80000000 | sid. The endpoint is SID
 * 0x1c01 (requester 0x100). A free slot is claimed and given S2CR = 0 (type 0, bypass), after
 * which the endpoint's DMA is untranslated and the arena below hands out physical addresses.
 * The UFS takeover picked its own slot before this runs, and the scan only takes a slot whose
 * SMR is invalid, so the two never collide. The translated 0xa0000000 window with the ArmSmmu4K
 * kit comes after MHI is proven; the record carries that plan. */
constexpr uint64_t kSmmuBase = 0x15000000, kSmmuBytes = 0x100000;
constexpr uint32_t kEndpointSid = 0x1c01;
Window gSmmu;

bool claimStream() {
    if (!gSmmu.open(kSmmuBase, kSmmuBytes)) { wifiLog("apps_smmu did not map\n"); return false; }
    const unsigned count = gSmmu.r(0x20) & 0xff;
    if (!count || count > 128) { wifiLog("apps_smmu stream count %u\n", count); return false; }
    for (unsigned slot = 0; slot < count; ++slot) {
        if (gSmmu.r(0x800 + 4 * slot) & 0x80000000u) continue;
        gSmmu.w(0x800 + 4 * slot, 0x80000000u | kEndpointSid);
        gSmmu.w(0xc00 + 4 * slot, 0);          /* S2CR type 0: bypass */
        __asm__ volatile("dsb sy" ::: "memory");
        const uint32_t back = gSmmu.r(0x800 + 4 * slot);
        wifiLog("SID %#x claimed at SMR slot %u (readback %#x), S2CR bypass\n", kEndpointSid, slot, back);
        return (back & 0x80000000u) && (back & 0xffffu) == kEndpointSid;
    }
    wifiLog("no free SMR slot for SID %#x\n", kEndpointSid);
    return false;
}

/* ---- DMA: one physically contiguous arena --------------------------------------------------- */
/* BHI offers the image and the rings as physical addresses under bypass, so the arena must be
 * physically contiguous; inTaskWithPhysicalMask gives that the same way the GPU driver gets its
 * firmware pages. Slices are handed out first-fit and never returned to a pool. */
constexpr size_t kArenaBytes = 32 << 20;
IOBufferMemoryDescriptor *gArena;
uint8_t *gArenaVa;
size_t gArenaUsed;

bool arenaOpen() {
    if (gArena) return true;
    gArena = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                              kArenaBytes, 0xfffffffffffff000ULL);
    if (!gArena) { wifiLog("no contiguous %zu-byte arena\n", kArenaBytes); return false; }
    if (gArena->prepare() != kIOReturnSuccess) { gArena->release(); gArena = nullptr; return false; }
    gArenaVa = static_cast<uint8_t *>(gArena->getBytesNoCopy());
    gArenaUsed = 0;
    wifiLog("DMA arena %zu bytes at %p\n", kArenaBytes, gArenaVa);
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
    const size_t aligned = (n + 63) & ~(size_t)63;
    if (gArenaUsed + aligned > kArenaBytes) { wifiLog("arena exhausted (%zu + %zu)\n", gArenaUsed, aligned); return nullptr; }
    void *p = gArenaVa + gArenaUsed;
    const uint64_t pa = arenaPhys(p);
    if (!pa) return nullptr;
    gArenaUsed += aligned;
    if (bus) *bus = pa;
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
    for (unsigned i = 0; i < gFirmwareCount; ++i)
        if (!strcmp(gFirmware[i].name, name)) { *data = gFirmware[i].data; *size = gFirmware[i].size; return 0; }
    if (gFirmwareCount >= sizeof gFirmware / sizeof gFirmware[0]) return -1;
    char path[160];
    snprintf(path, sizeof path, "/private/var/ngpu/firmware/qca6390/%s", name);
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
    strlcpy(slot.name, name, sizeof slot.name);
    slot.data = buffer;
    slot.size = got;
    *data = buffer;
    *size = got;
    wifiLog("firmware %s %zu bytes\n", name, got);
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
    if (!claimStream()) return -1;
    wifiLog("stream claimed");
    if (!arenaOpen()) return -1;
    wifiLog("arena up");
    /* The three readings that say whether the endpoint is there at all: BAR0's TCSR_SOC_HW_VERSION
     * carries the WCNSS major in GENMASK(11,8) and must read 2 for this ath11k, and the config
     * window must answer the endpoint's own ids. They come last, so a boot that stops here has already
     * reported the rails, the SMMU stream and the arena, and the hang belongs to the endpoint. */
    {
        const uint32_t hwVersion = gBar.r(0x0224);
        wifiLog("bar0[0x224]=%#x major=%u config_id=%#x class=%#x", hwVersion, (hwVersion >> 8) & 0xf,
                gConf.r(0x00), gConf.r(0x08));
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
    thread_terminate(current_thread());
    thread_block(THREAD_CONTINUE_NULL);
}

extern "C" int alioth_wifi_board_start(void) {
    if (!gOpen && alioth_wifi_board_open() != 0) return -1;
    extern int nw_wifi_start(const struct lx_platform *, const struct nw_board *);
    const int r = nw_wifi_start(&gPlatform, &gBoard);
    wifiLog("nw_wifi_start returned %d\n", r);
    if (r == 0) {
        thread_t thread = nullptr;
        if (kernel_thread_start(wifiWorker, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
        else wifiLog("the wifi worker thread did not start\n");
    }
    return r;
}

/* The kernel's entry: the late display probe calls this (weakly) after the PCIe bring-up when the
 * boot argument alioth-wifi=1 is set. Failure is logged and leaves the rest of the boot alone. */
extern "C" void alioth_wifi_start(void) {
    const int r = alioth_wifi_board_start();
    wifiLog("start returned %d\n", r);
}
