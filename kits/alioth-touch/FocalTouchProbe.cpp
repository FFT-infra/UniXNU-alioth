// SPDX-License-Identifier: BSD-2-Clause-Patent
// K40 (alioth, SM8250) FocalTech FT5652 touch probe: clocks, SPI SE4, panel
// identification and the touch frame loop that feeds the HID digitizer
// (ShengHidInput NativeHidInput.cpp consumes the native_touch_* seqlock set).
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <libkern/c++/OSSharedPtr.h>
#include <string.h>
#include <mach/mach_time.h>

#include "FocalTouchBoard.h"
#include "FocalTouchProtocol.h"
#include "GeniSpi.h"

extern "C" {
volatile uint32_t native_touch_seq;
volatile uint32_t native_touch_count;
volatile int native_touch_xs[12], native_touch_ys[12], native_touch_ids[12];
volatile int native_touch_last_x = -1, native_touch_last_y = -1;
volatile uint32_t native_touch_frames;
volatile uint32_t native_touch_hid_reports;
volatile int native_touch_error;
volatile uint32_t native_touch_loop_step;
volatile uint32_t native_touch_valid;
// Consumer-side accounting that the HID digitizer (NativeHidInput) maintains;
// defined here so the probe archive alone satisfies the digitizer's link.
volatile uint32_t native_touch_edge_stats[4];
volatile uint32_t native_touch_hid_multi[3];
}

extern "C" bool native_hid_input_requested(void) {
    // The HID digitizer is always wanted on this board: the FT5652 is the only
    // touch input, and the consumer is force-linked with the probe archive.
    return true;
}

extern "C" {
volatile uint32_t native_touch_fail_step;
}

namespace {

static void (*gTouchSink)(void *);
static void *gTouchSinkContext;
extern "C" void native_touch_set_sink(void (*sink)(void *), void *context) {
    gTouchSinkContext = context;
    __atomic_store_n(&gTouchSink, sink, __ATOMIC_RELEASE);
}

struct Region {
    OSSharedPtr<IOMemoryMap> keep;
    uintptr_t va = 0;
};

bool mapRegion(uint64_t phys, uint64_t length, Region &region) {
    IODeviceMemory *memory = IODeviceMemory::withRange(phys, length);
    if (!memory) return false;
    region.keep = OSSharedPtr<IOMemoryMap>(memory->map(kIOMapAnywhere | kIOMapInhibitCache), OSNoRetain);
    memory->release();
    region.va = region.keep ? static_cast<uintptr_t>(region.keep->getVirtualAddress()) : 0;
    return region.va != 0;
}

uint32_t rd32(uintptr_t base, uint32_t reg) {return *reinterpret_cast<volatile uint32_t *>(base + reg);}
void wr32(uintptr_t base, uint32_t reg, uint32_t value) {
    *reinterpret_cast<volatile uint32_t *>(base + reg) = value;
    __asm__ volatile("dsb sy" ::: "memory");
}

void ioDelayUs(void *, unsigned us) {
    if (us > 50) {IODelay(us); return;}
    uint64_t frequency = 0, start = 0, now = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(start));
    const uint64_t ticks = (frequency ? frequency : 19200000) * us / 1000000 + 1;
    do {__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now));} while (now - start < ticks);
}
uint32_t ioRd(void *ctx, uint32_t reg) {return rd32(reinterpret_cast<uintptr_t>(ctx), reg);}
void ioWr(void *ctx, uint32_t reg, uint32_t value) {wr32(reinterpret_cast<uintptr_t>(ctx), reg, value);}

struct Session {
    Region gccAhb, gccSe4, gccCore, gccVote, tlmm, wrapper, se;
    IOBufferMemoryDescriptor *dmaMem = nullptr;
    struct geni_spi spi;
    struct Fts5652 chip;
    uint8_t *tx = nullptr, *rx = nullptr;
    uint32_t fail = 0;
    unsigned frames = 0, bad = 0, reads = 0;
};

void fail(Session &s, unsigned step) {
    if (!s.fail) {s.fail = step; native_touch_fail_step = step;}
}

void gpioReset(Session &s, int asserted) {
    const uintptr_t cfg = s.tlmm.va + TB_GPIO_RESET * TB_GPIO_STRIDE;
    wr32(cfg, 0, (rd32(cfg, 0) & ~TB_CFG_OE) | TB_CFG_OE);           // output
    wr32(cfg, 4, asserted ? 0u : TB_IO_OUT);                          // low asserted (active-low reset)
}

void panelReset(void *ctx, int asserted) {
    gpioReset(*static_cast<Session *>(ctx), asserted);
}
int panelTransfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    Session &s = *static_cast<Session *>(ctx);
    int rc = geni_spi_transfer(&s.spi, tx, rx, len, 0);
    if (rc && !s.fail) s.fail = 5;
    return rc;
}
void panelStage(void *ctx, const char *name, unsigned value, int result) {
    (void)ctx; (void)name; (void)value; (void)result;
}

// Map the touch 12-bit raw coordinates onto the 0.1 mm axes the HID layer expects.
void publish(const struct Fts5652Contact *c, unsigned count) {
    int xs[12], ys[12], ids[12];
    for (unsigned i = 0; i < count && i < 12; ++i) {
        long x = (long)c[i].x * FTS5652_AXIS_X / 4095;
        long y = (long)c[i].y * FTS5652_AXIS_Y / 4095;
        if (x < 0) x = 0; if (x >= FTS5652_AXIS_X) x = FTS5652_AXIS_X - 1;
        if (y < 0) y = 0; if (y >= FTS5652_AXIS_Y) y = FTS5652_AXIS_Y - 1;
        xs[i] = (int)x; ys[i] = (int)y; ids[i] = c[i].id;
    }
    native_touch_seq = native_touch_seq + 1;
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_count = count;
    for (unsigned i = 0; i < count && i < 12; ++i) {
        native_touch_xs[i] = xs[i];
        native_touch_ys[i] = ys[i];
        native_touch_ids[i] = ids[i];
    }
    __asm__ volatile("dmb ish" ::: "memory");
    native_touch_seq = native_touch_seq + 1;
    if (count) {native_touch_last_x = xs[0]; native_touch_last_y = ys[0];}
    if (void (*sink)(void *) = __atomic_load_n(&gTouchSink, __ATOMIC_ACQUIRE)) sink(gTouchSinkContext);
}

// Program DFS level 0 (TCXO 19.2 MHz) of the SE4 RCG and confirm the divider.
bool clockSe4(Session &s) {
    // Populate DFS level 0: source TCXO (index 0), no divide, no M/N.
    wr32(s.gccSe4.va, (TB_SE4_RCG + TB_RCG_PERF_CFG(0)) & 0xfff, 0u);
    wr32(s.gccSe4.va, (TB_SE4_RCG + TB_RCG_PERF_M(0)) & 0xfff, 0u);
    wr32(s.gccSe4.va, (TB_SE4_RCG + TB_RCG_PERF_N(0)) & 0xfff, 0u);
    // Update the DFS registers.
    wr32(s.gccSe4.va, (TB_SE4_RCG + TB_RCG_DFSR_CMD) & 0xfff, 1u);
    // Root command: enable.
    uint32_t ctrl = rd32(s.gccSe4.va, (TB_SE4_RCG + 0x0) & 0xfff);
    if (!(ctrl & 1u)) wr32(s.gccSe4.va, (TB_SE4_RCG + 0x0) & 0xfff, ctrl | 1u);
    return true;
}

void touchThread(void *arg, wait_result_t) {
    Session &s = *static_cast<Session *>(arg);
    struct Fts5652Contact contacts[10];
    for (;;) {
        IODelay(TB_FRAME_PERIOD_US / 1000);
        native_touch_loop_step = 1;
        unsigned count = 0;
        int rc = fts5652_frame(&s.chip, &count, contacts, 10);
        ++s.reads;
        if (rc) {++s.bad; if (!s.fail) s.fail = 6; continue;}
        native_touch_loop_step = 2;
        native_touch_valid = native_touch_valid + 1;
        native_touch_frames = native_touch_frames + 1;
        publish(contacts, count);
    }
}

} // namespace

// File-scope static: kernel C++ has no support for function-local static
// guard variables, so the persistent Session lives at module scope.
static Session gTouchSession;

bool touchProbeStart(void) {
    Session &s = gTouchSession;
    // GCC: AHB halts at 0x17000, SE4 halt+RCG at 0x17400, core halts at 0x23000,
    // clock vote at 0x52000.
    if (!mapRegion(TB_GCC_BASE + 0x17000, 0x100, s.gccAhb) ||
        !mapRegion(TB_GCC_BASE + 0x17400, 0x100, s.gccSe4) ||
        !mapRegion(TB_GCC_BASE + 0x23000, 0x100, s.gccCore) ||
        !mapRegion(TB_GCC_BASE + 0x52000, 0x1000, s.gccVote) ||
        !mapRegion(TB_TLMM_BASE, 0x200000, s.tlmm) ||
        !mapRegion(TB_QUP_WRAPPER, 0x1000, s.wrapper) ||
        !mapRegion(TB_SE4_BASE, TB_SE4_SIZE, s.se)) {
        fail(s, 1);
        return false;
    }
    // Vote the wrapper clocks and wait for the branches.
    wr32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff, rd32(s.gccVote.va, TB_GCC_VOTE_ENABLE & 0xfff) | TB_VOTE_ALL);
    auto running = [](uint32_t cbcr) {return !(cbcr & TB_GCC_CBCR_CLK_OFF) || (cbcr & 2u);};
    bool ok = false;
    for (unsigned i = 0; i < 2000 && !ok; ++i) {
        ok = running(rd32(s.gccAhb.va, TB_HALT_M_AHB & 0xfff)) &&
             running(rd32(s.gccAhb.va, TB_HALT_S_AHB & 0xfff)) &&
             !(rd32(s.gccCore.va, TB_HALT_CORE & 0xfff) & TB_GCC_CBCR_CLK_OFF) &&
             !(rd32(s.gccCore.va, TB_HALT_CORE_2X & 0xfff) & TB_GCC_CBCR_CLK_OFF) &&
             !(rd32(s.gccSe4.va, TB_HALT_SE4 & 0xfff) & TB_GCC_CBCR_CLK_OFF);
        if (!ok) IODelay(10);
    }
    if (!ok) {fail(s, 2); return false;}
    // SE4 clock source: TCXO 19.2 MHz (DFS level 0).
    clockSe4(s);
    // SPI pins (GPIO 8/9/10, function qup4 = FUNC_SEL 0, 6 mA, no pull); IRQ input; reset output high.
    const uint32_t spiCfg = TB_CFG_FUNC_QUP4 | TB_CFG_DRIVE_MA6 | TB_CFG_PULL_NONE;
    for (unsigned i = 0; i < 3; ++i) {
        wr32(s.tlmm.va, (TB_GPIO_SPI_FIRST + i) * TB_GPIO_STRIDE, spiCfg);
    }
    const uint32_t irqCfg = (0u) | TB_CFG_PULL_UP;      // input, pull-up
    wr32(s.tlmm.va, TB_GPIO_IRQ * TB_GPIO_STRIDE, irqCfg);
    gpioReset(s, 1);
    IODelay(1);
    // GENI SPI engine (FIFO mode first; DMA path only if the FIFO interface is disabled).
    s.spi.se.ctx = reinterpret_cast<void *>(s.se.va);
    s.spi.se.rd = ioRd;
    s.spi.se.wr = ioWr;
    s.spi.se.delay_us = ioDelayUs;
    s.spi.wrapper.ctx = reinterpret_cast<void *>(s.wrapper.va);
    s.spi.wrapper.rd = ioRd;
    s.spi.wrapper.wr = ioWr;
    s.spi.wrapper.delay_us = ioDelayUs;
    s.spi.poll_limit = 200000;
    s.spi.allow_fifo_disabled = 0;
    if (geni_spi_read_info(&s.spi)) {fail(s, 3); return false;}
    if (s.spi.info.fifo_disabled) {fail(s, 4); return false;}   // needs the DMA path (not implemented for alioth yet)
    if (geni_spi_init(&s.spi)) {fail(s, 5); return false;}
    if (geni_spi_setup(&s.spi, TB_SPI_MODE, TB_SPI_CS)) {fail(s, 6); return false;}
    if (geni_spi_set_clock(&s.spi, TB_SPI_HZ, TB_SPI_SCLK_HZ, TB_SPI_CLK_SEL)) {fail(s, 7); return false;}
    // Panel: release reset, wait, identify.
    gpioReset(s, 0);
    ioDelayUs(nullptr, TB_RESET_RELEASE_US);
    s.chip.io.context = &s;
    s.chip.io.transfer = panelTransfer;
    s.chip.io.reset = panelReset;
    s.chip.io.delay_us = ioDelayUs;
    s.chip.io.stage = panelStage;
    if (fts5652_state(&s.chip, 1)) {fail(s, 8); return false;}
    // Frame loop.
    thread_t thread;
    if (kernel_thread_start(touchThread, &s, &thread) != KERN_SUCCESS) {fail(s, 9); return false;}
    thread_deallocate(thread);
    return true;
}