/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* alioth QCA6390 board layer: the callbacks `nw_abi.h` asks of the board, on the registers and
 * paths this board already owns. State: the config window, the BAR, the MSI target and the
 * firmware reader are implemented; the endpoint's SMMU context, the DMA window and the WLAN rail
 * votes are the next steps and are marked NOT YET, so `nw_wifi_start` fails honestly until they
 * are real. The fact sheet behind every constant is docs/wifi-qca6390-board.md. */
#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <kern/clock.h>
#include <pexpert/pexpert.h>
#include "nw_abi.h"

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

void say(int level, const char *line) {
    if (level < 7) IOLog("AliothWifi[%d]: %s\n", level, line);
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

/* NOT YET: a physical mapping helper for windows other than BAR0 (the chip's WCSS window above
 * 0x80000 is reached through the BAR0 window register, not here). Returning null makes a caller
 * that needs it fail instead of dereferencing a wrong address. */
void *mapPhys(uint64_t pa, size_t n) { (void)pa; (void)n; return nullptr; }

/* ---- DMA ----------------------------------------------------------------------------------- */
/* NOT YET: the endpoint's SMMU context (SID 0x1c01) and the 0xa0000000 window. Until the context
 * is programmed, every DMA request fails with 0, which is what nw_wifi_start must see. */
void *dmaAlloc(size_t n, uint64_t *bus) { (void)n; if (bus) *bus = 0; return nullptr; }
void dmaFree(size_t n, void *p, uint64_t bus) { (void)n; (void)p; (void)bus; }
uint64_t dmaMap(void *p, size_t n, int dir) { (void)p; (void)n; (void)dir; return 0; }
void dmaSync(uint64_t bus, size_t n, int dir) { (void)bus; (void)n; (void)dir; }

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
    int fd = open(path, O_RDONLY, 0);
    if (fd < 0) { IOLog("AliothWifi: firmware %s absent (%d)\n", path, errno); return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > (16 << 20)) { close(fd); return -1; }
    uint8_t *buffer = static_cast<uint8_t *>(IOMalloc((size_t)st.st_size));
    if (!buffer) { close(fd); return -1; }
    size_t got = 0;
    while (got < (size_t)st.st_size) {
        ssize_t n = read(fd, buffer + got, (size_t)st.st_size - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != (size_t)st.st_size) { IOFree(buffer, (size_t)st.st_size); return -1; }
    Firmware &slot = gFirmware[gFirmwareCount++];
    strlcpy(slot.name, name, sizeof slot.name);
    slot.data = buffer;
    slot.size = got;
    *data = buffer;
    *size = got;
    IOLog("AliothWifi: firmware %s %zu bytes\n", name, got);
    return 0;
}

/* ---- the ABI ------------------------------------------------------------------------------- */
struct lx_platform gPlatform;
struct nw_board gBoard;

}  // namespace

extern "C" int alioth_wifi_board_open(void) {
    if (gOpen) return 0;
    if (!gBar.open(kBarBase, kBarBytes) || !gConf.open(kConfBase, kConfBytes) || !gDoorbell.open(kMsiDoorbell, kMsiBytes)) {
        IOLog("AliothWifi: a PCIe window did not map\n");
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
    gPlatform.dma_alloc = dmaAlloc;
    gPlatform.dma_free = dmaFree;
    gPlatform.dma_map = dmaMap;
    gPlatform.dma_sync = dmaSync;
    gPlatform.fw_get = fwGet;
    gPlatform.preempt_disable = nullptr;
    gPlatform.preempt_enable = nullptr;
    gBoard.bar = const_cast<void *>(static_cast<const void *>(gBar.v));
    gBoard.bar_len = kBarBytes;
    gBoard.device_id = kDeviceId;
    gBoard.msi_cap = kMsiCap;
    gBoard.irq_base = (int)kVectorBase;
    gBoard.cfg_read = cfgRead;
    gBoard.cfg_write = cfgWrite;
    gBoard.msi_target = msiTarget;
    gBoard.map_phys = mapPhys;
    gOpen = true;
    IOLog("AliothWifi: windows open (bar %p, conf %p, doorbell %p); SMMU/DMA and rails are NOT YET\n",
          gBoard.bar, gConf.v, gDoorbell.v);
    return 0;
}

extern "C" int alioth_wifi_board_start(void) {
    if (!gOpen && alioth_wifi_board_open() != 0) return -1;
    extern int nw_wifi_start(const struct lx_platform *, const struct nw_board *);
    const int r = nw_wifi_start(&gPlatform, &gBoard);
    IOLog("AliothWifi: nw_wifi_start returned %d\n", r);
    return r;
}
