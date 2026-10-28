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
#include <pexpert/pexpert.h>
#include <libkern/c++/OSSharedPtr.h>
#include <string.h>
#include <mach/mach_time.h>

#include "FocalTouchBoard.h"
#include "AliothRpmh.h"
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
/* Contacts that arrived with a zero area. Cumulative over the boot; printed on the TOUCHREG line
 * so a run says whether a light tap lands in this case at all. */
volatile uint32_t native_touch_area0;
/* The raw range the part actually reports, before scaling. The device tree's 0..8639 x 0..19199
 * is what the scaling assumes, and a HID dump showed x reaching its maximum, which is only
 * possible if the real raw maximum is at or above 8640. */
volatile int native_touch_raw_minx = 0x7fffffff, native_touch_raw_maxx = -1;
volatile int native_touch_raw_miny = 0x7fffffff, native_touch_raw_maxy = -1;
/* How often the part tags a contact down (0), up (1), move (2) or either of the two spare codes,
 * and the last contact of the frame that preceded a release. A swipe that ends near the bottom of
 * the panel and still arrives tagged 0 or 2 would be published as a press at the release point,
 * which is one candidate for the system going home on its own; a release tagged 1 is dropped
 * before publishing and cannot do that. */
volatile uint32_t native_touch_ev[4];
volatile int native_touch_rel_x, native_touch_rel_y, native_touch_rel_ev = -1;
static int lastSeenX, lastSeenY, lastSeenEv = -1;
// Consumer-side accounting that the HID digitizer (NativeHidInput) maintains;
// defined here so the probe archive alone satisfies the digitizer's link.
// native_touch_edge_stats and native_touch_hid_multi belong to ShengHidInput: NativeHidInput.cpp
// defines them and is now linked. Defining them here as well made the kernel link fail with two
// duplicate symbols the first time the HID object was actually pulled in.
}

extern "C" bool native_hid_input_requested(void) {
    // The HID digitizer is always wanted on this board: the FT5652 is the only
    // touch input, and the consumer is force-linked with the probe archive.
    return true;
}

// GPIO_CFG as written and as read back, for the three SPI pins.
// What gpio8's CFG register reads at each candidate TLMM base.
extern "C" {
volatile unsigned native_touch_tlmm_probe[3];
}
// Touch health, published for the boot log: how many register polls ran, the last power mode
// read from the part (0xffff when the read failed), and how many recovery sequences fired.
// The first bytes of the last frame's payload and the last coordinates published, so a frame
// that reports no contacts and a frame that reports contacts at a wrong place look different
// on the console.
extern "C" {
volatile unsigned native_touch_frame_raw[8];
}
// Interrupt-line state: 0 asserted (a report is pending), 1 idle, plus how often it was seen
// asserted. Without these a loop that never sees the line is indistinguishable from a panel
// that never has anything to report.
extern "C" {
volatile unsigned native_touch_irq_level;
volatile unsigned native_touch_irq_low;
}
extern "C" {
volatile unsigned native_touch_reg_polls;
volatile unsigned native_touch_power_mode;
volatile unsigned native_touch_recoveries;
}
// The FTS identity read: app id and the three per-read results (app, rom, version).
// How long the part took to answer its identity registers, and whether it ever did.
// The last frame read's error, as a positive number (0 = read fine).
extern "C" {
volatile unsigned native_touch_frame_rc;
}
extern "C" {
volatile unsigned native_touch_ready;
volatile unsigned native_touch_ready_ms;
}
extern "C" {
volatile unsigned native_touch_app_id;
volatile unsigned native_touch_app_rc[3];
}
extern "C" {
volatile unsigned native_touch_pin_cfg[3];
volatile unsigned native_touch_pin_want[3];
}
extern "C" {
volatile uint32_t native_touch_fail_step;
// 0 = the touch rail voted; non-zero is the RPMh refusal, for the console line.
volatile int native_touch_power_result = -1;
}

// The last register read the protocol core reported. Declared here, outside the anonymous
// namespace below, because a definition inside it has internal linkage and exports no symbol;
// the console reader in the display probe would then fail to link. Measured 2026-10-27: this
// exact placement is what made the four names undefined at the final link.
// What the last refused CRC compared, and the bytes around it, for the console line.
extern "C" {
volatile unsigned native_touch_crc_expected, native_touch_crc_received;
volatile unsigned native_touch_hdr0, native_touch_hdr1, native_touch_hdr2, native_touch_hdr3;
volatile unsigned native_touch_data0, native_touch_data1, native_touch_data2;
}
extern "C" {
volatile int native_touch_stage_result;
volatile unsigned native_touch_stage_value;
volatile unsigned native_touch_stage_total;
char native_touch_stage_name[32];
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
    Region gccAhb, gccSe4, gccCore, gccVote, tlmmNorth, tlmmSouth, wrapper, se;
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

// One pin: mux function, pull, drive strength and direction, in the TLMM bit layout
// [1:0] pull, [5:2] function, [8:6] drive, [9] output.
/* Map the two TLMM tiles and report what each one reads. A pin answers only through its own
 * tile; the offsets come from the K40 kernel's pinctrl-kona.c:20-21. Reading the register a
 * known pin would occupy tells us whether a tile is really there, which is the difference
 * between "the address is wrong" and "the write did not land". */
void probeTlmm(Session &s) {
    static const struct { uint64_t phys; const char *name; } tiles[2] = {
        { TB_TLMM_BASE + TB_TLMM_NORTH_OFFSET, "north" },
        { TB_TLMM_BASE + TB_TLMM_SOUTH_OFFSET, "south" },
    };
    Region *slots[2] = { &s.tlmmNorth, &s.tlmmSouth };
    for (unsigned i = 0; i < 2; ++i) {
        if (!mapRegion(tiles[i].phys, TB_TLMM_TILE_SIZE, *slots[i])) {
            native_touch_tlmm_probe[i] = 0xdead0000u | i;
            continue;
        }
        /* gpio8 is in the north tile and gpio33 in the south, so each tile is read at a pin it
         * actually owns rather than at a fixed offset that may mean nothing there. */
        const unsigned pin = i == 0 ? TB_GPIO_SPI_FIRST : 33u;
        native_touch_tlmm_probe[i] = rd32(slots[i]->va, pin * TB_GPIO_STRIDE);
    }
}
void gpioConfigure(Session &s, unsigned pin, unsigned function, unsigned pull, unsigned drive, bool output) {
    const uintptr_t base = pin <= TB_TLMM_PIN_SPLIT ? s.tlmmNorth.va : s.tlmmSouth.va;
    const uintptr_t cfg = base + pin * TB_GPIO_STRIDE;
    const uint32_t want = pull | (function << TB_CFG_FUNC_SHIFT) | (drive << TB_CFG_DRIVE_SHIFT) |
                          (output ? TB_CFG_OE : 0u);
    wr32(cfg, 0, want);
    /* Read the register back. A pin write that lands outside the controller reads as zero or
     * as an unrelated register, which is indistinguishable from a correct write unless it is
     * compared. boot-usb46 and usb47 configured nothing because the TLMM window was wrong and
     * no readback existed to say so. */
    const uint32_t got = rd32(cfg, 0);
    /* The three SPI pins are 8, 9 and 10, not 0..2: indexing by pin number is what made the
     * first version of this readback report zeros for both sides, which said nothing about
     * the hardware because the array was never written. Index by position instead. */
    if (pin >= TB_GPIO_SPI_FIRST && pin < TB_GPIO_SPI_FIRST + 3) {
        const unsigned slot = pin - TB_GPIO_SPI_FIRST;
        native_touch_pin_cfg[slot] = got;
        native_touch_pin_want[slot] = want;
    }
}
void gpioDrive(Session &s, unsigned pin, bool high) {
    const uintptr_t base = pin <= TB_TLMM_PIN_SPLIT ? s.tlmmNorth.va : s.tlmmSouth.va;
    const uintptr_t io = base + pin * TB_GPIO_STRIDE + 4;
    wr32(io, 0, high ? TB_IO_HIGH : 0u);
}
// The pad's input level: GPIO_IN_OUT is CFG + 4 and bit 0 carries it.
int gpioRead(Session &s, unsigned pin) {
    const uintptr_t base = pin <= TB_TLMM_PIN_SPLIT ? s.tlmmNorth.va : s.tlmmSouth.va;
    return (int)(rd32(base + pin * TB_GPIO_STRIDE, 4) & 1u);
}

void gpioReset(Session &s, int asserted) {
    gpioConfigure(s, TB_GPIO_RESET, 0, TB_CFG_PULL_UP, TB_CFG_DRIVE_MA8, true);
    gpioDrive(s, TB_GPIO_RESET, !asserted);   // active low
    /* The interrupt line was only ever read, never configured: function 0 (GPIO), pulled up,
     * as an input. Left in whatever state the bootloader chose, gpioRead on it says nothing
     * about the part -- which is what "the part reports int=01 while we never see the line go
     * low" looks like. */
    gpioConfigure(s, TB_GPIO_IRQ, 0, TB_CFG_PULL_UP, TB_CFG_DRIVE_MA8, false);
}

void panelReset(void *ctx, int asserted) {
    gpioReset(*static_cast<Session *>(ctx), asserted);
}
int panelTransfer(void *ctx, const uint8_t *tx, uint8_t *rx, size_t len) {
    Session &s = *static_cast<Session *>(ctx);
    /* No software chip select. This board's device tree gives the focaltech node only a
     * reset-gpio and an irq-gpio, and the SE4 pin group (qupv3_se4_spi_active) lists just
     * gpio8/9/10: there is no CS pin to drive. The GENI engine owns chip select and asserts
     * it per transfer. An earlier version drove GPIO 11 as CS here, which the hardware does
     * not use and which would fight the engine's own timing. */
    int rc = geni_spi_transfer(&s.spi, tx, rx, len, 0);
    if (rc && !s.fail) s.fail = 5;
    return rc;
}
// The protocol core reports every register read here: the name of the step, the value it got
// and the transfer result. This used to drop all three, so a refusal at step 8 said only
// "the FTS5652 handshake failed" and named neither the register nor the bytes. Keep the last
// few so the console line after the failure can say what the chip actually answered.
struct StageRecord {
    const char *name;
    unsigned value;
    int result;
};
static StageRecord gStageLog[8];
static unsigned gStageCount;
// The console cannot reach gStageLog directly: it is static to this translation unit. The
// last record is republished here for whoever reads it, as a fixed-size, allocation-free form
// the boot code can print.
void panelStage(void *ctx, const char *name, unsigned value, int result) {
    (void)ctx;
    StageRecord &slot = gStageLog[gStageCount % 8];
    slot.name = name;
    slot.value = value;
    slot.result = result;
    ++gStageCount;
    native_touch_stage_value = value;
    native_touch_stage_result = result;
    native_touch_stage_total = gStageCount;
    unsigned i = 0;
    if (name) {
        for (; i + 1 < sizeof(native_touch_stage_name) && name[i]; ++i) {
            native_touch_stage_name[i] = name[i];
        }
    }
    native_touch_stage_name[i] = 0;
}

// Map the touch 12-bit raw coordinates onto the 0.1 mm axes the HID layer expects.
void publish(const struct Fts5652Contact *c, unsigned count) {
    int xs[12], ys[12], ids[12];
    if (count == 0) {
        /* Nothing in this frame: the one before it was the release. Keep its last contact so the
         * diagnostic can say whether the part tags a release as up (1, dropped before it can be
         * published) or as down/move (0 or 2, published as a press where the finger left). */
        native_touch_rel_x = lastSeenX;
        native_touch_rel_y = lastSeenY;
        native_touch_rel_ev = lastSeenEv;
    } else {
        for (unsigned i = 0; i < count && i < 12; ++i) {
            if (c[i].event < 4) native_touch_ev[c[i].event] = native_touch_ev[c[i].event] + 1;
        }
        lastSeenX = c[0].x;
        lastSeenY = c[0].y;
        lastSeenEv = c[0].event;
    }
    for (unsigned i = 0; i < count && i < 12; ++i) {
        /* The decoded coordinate is a 15-bit field (0..32767): the low three bits of each axis
         * ride in the pressure byte. The part's own range is this board's display-coords from
         * the device tree, 0..8639 x 0..19199. The HID layer's contract is a different space:
         * NativeHidInput::touchSend transforms what it receives from 0..30480 / 0..20320 into
         * the report's 0..32767, so publish in that space rather than in raw panel units, or
         * every touch lands at a fraction of where the finger was. */
        /* What the part actually reports, before any scaling. The device tree says 0..8639 x
         * 0..19199 and the scaling below assumes exactly that; a dump of the HID side showed x
         * saturating at its maximum, which can only happen if raw x reaches 8640 or more, so the
         * assumed range has to be checked against what arrives rather than trusted. */
        if (c[i].x < native_touch_raw_minx) native_touch_raw_minx = c[i].x;
        if (c[i].x > native_touch_raw_maxx) native_touch_raw_maxx = c[i].x;
        if (c[i].y < native_touch_raw_miny) native_touch_raw_miny = c[i].y;
        if (c[i].y > native_touch_raw_maxy) native_touch_raw_maxy = c[i].y;
        long x = (long)c[i].x * FTS5652_HID_AXIS_X / FTS5652_AXIS_X;
        long y = (long)c[i].y * FTS5652_HID_AXIS_Y / FTS5652_AXIS_Y;
        if (x < 0) x = 0; if (x >= FTS5652_HID_AXIS_X) x = FTS5652_HID_AXIS_X - 1;
        if (y < 0) y = 0; if (y >= FTS5652_HID_AXIS_Y) y = FTS5652_HID_AXIS_Y - 1;
        xs[i] = (int)x; ys[i] = (int)y; ids[i] = c[i].id;
    }
    /* The part reports up to five points on a panel nobody is touching, each carrying no area.
     * Published as they are, iOS sees a palm and ignores every contact -- which is what a round
     * with reports=781, max=5 and no reaction looks like. Keep only contacts the part gives a
     * non-zero area. */
    {
        unsigned kept = 0;
        for (unsigned i = 0; i < count && i < 12; ++i) {
            /* The frame's per-contact event says down (0), up (1) or move (2), and nothing
             * here ever looked at it: every published contact carried tip and valid set, so a
             * lift went out as a contact that is still down. The HID path signals a lift by the
             * contact disappearing from the count, so a contact whose event is "up" must not be
             * published. */
            if (c[i].event == 1) continue;
            /* A zero area is not a reason to drop the contact. The vendor driver for this part
             * substitutes 9 and reports it anyway (focaltech_core.c:508-510), and a HID dump of
             * this panel showed the shape that dropping it produces: a finger with mask=0
             * touch=1 (no Range, no Position -- UIKit does not accept it as a begin) immediately
             * followed by mask=3 touch=0 at the same x/y with no move in between, which is a
             * press released in the same breath. A tap cannot survive that; a drag survives
             * because its area stays non-zero. Counted and kept. */
            if (c[i].area == 0) native_touch_area0 = native_touch_area0 + 1;
            xs[kept] = xs[i]; ys[kept] = ys[i]; ids[kept] = ids[i];
            ++kept;
        }
        count = kept;
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

/* The panel can stop reporting fingers while still answering on the bus. Measured
 * 2026-10-28 on boot-usb61: contacts read 1, 2 and 3 while the user touched during early boot,
 * then 0 for the whole lock-screen session, so a swipe never reached the system even though
 * every frame passed its CRC and the probe had started.
 *
 * Two registers tell the states apart: the power mode (0xA5, 3 = sleep) and the identity pair
 * (0xA3/0x9F). Read them every TB_REG_POLL_FRAMES frames, report them, and when they show
 * sleep or a lost identity run the recovery the board's own driver runs -- hold reset, release
 * it, and wait for the identity registers to answer again. That is the same sequence that
 * brought the part up at boot, so it needs no new protocol. */
// The USB console, when the board links it. Weak, so a board without it still compiles.
extern "C" size_t qcom_usb_debug_write(const void *, size_t) __attribute__((weak_import));

static void touchHealthCheck(Session &s) {
    uint8_t power = 0, idh = 0, idl = 0;
    const int powerRc = fts5652_read_reg(&s.chip, 0xa5, &power);
    const int idhRc = fts5652_read_reg(&s.chip, 0xa3, &idh);
    int idlRc = -1;
    if (!idhRc) idlRc = fts5652_read_reg(&s.chip, 0x9f, &idl);
    /* Read-only state the board's own FocalTech driver dumps (fts_dumpreg_show, focaltech_ex_fun.c).
     * 0x8F counts the interrupts the part has raised; it separates "the part never saw a touch"
     * from "the part saw one and the line to GPIO 39 is wrong", which the level alone cannot.
     * 0x91 is its flow/work counter, 0x8B the charger mode and 0xD0 the gesture mode. Nothing
     * here is written: 0xA5 has no vendor documentation for this board and a wrong write can
     * leave the part half-configured. */
    uint8_t intCount = 0, workCount = 0, charger = 0, gesture = 0;
    const int intRc = fts5652_read_reg(&s.chip, 0x8f, &intCount);
    if (intRc) intCount = 0xff;
    (void) fts5652_read_reg(&s.chip, 0x91, &workCount);
    (void) fts5652_read_reg(&s.chip, 0x8b, &charger);
    (void) fts5652_read_reg(&s.chip, 0xd0, &gesture);
    native_touch_reg_polls = native_touch_reg_polls + 1;
    native_touch_power_mode = powerRc ? 0xffffu : power;
    const bool asleep = powerRc == 0 && power == 0x03;
    const bool lost = idhRc != 0 || idlRc != 0 || idh == 0x00 || idh == 0xff || idl == 0xff;
    char text[224];
    if (&qcom_usb_debug_write) {
        int n = snprintf(text, sizeof(text), "TOUCHREG polls=%u power=%02x(rc=%d) id=%02x%02x(rc=%d/%d) int=%02x(rc=%d) work=%02x chg=%02x ges=%02x a0=%u rx=%d..%d ry=%d..%d\n",
            native_touch_reg_polls, power, powerRc, idh, idl, idhRc, idlRc, intCount, intRc, workCount, charger, gesture,
            __atomic_load_n(&native_touch_area0, __ATOMIC_RELAXED),
            __atomic_load_n(&native_touch_raw_minx, __ATOMIC_RELAXED), __atomic_load_n(&native_touch_raw_maxx, __ATOMIC_RELAXED),
            __atomic_load_n(&native_touch_raw_miny, __ATOMIC_RELAXED), __atomic_load_n(&native_touch_raw_maxy, __ATOMIC_RELAXED));
        if (n > 0) qcom_usb_debug_write(text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
    }
    /* The USB console has stopped a second or two into some boots and taken every measurement
     * with it, while the kernel itself kept running to its deadline: the persistent log showed
     * 120 s, 61 rounds and a clean reset for exactly such a run. Write the same numbers into the
     * kernel log once a second as well, because that one reaches the log on userdata and survives
     * whatever happens to the console. */
    {
        const int minx = __atomic_load_n(&native_touch_raw_minx, __ATOMIC_RELAXED);
        const int maxx = __atomic_load_n(&native_touch_raw_maxx, __ATOMIC_RELAXED);
        const int miny = __atomic_load_n(&native_touch_raw_miny, __ATOMIC_RELAXED);
        const int maxy = __atomic_load_n(&native_touch_raw_maxy, __ATOMIC_RELAXED);
        const unsigned a0 = __atomic_load_n(&native_touch_area0, __ATOMIC_RELAXED);
        static int lastMinx = -2, lastMaxx = -2, lastMiny = -2, lastMaxy = -2;
        static unsigned lastA0 = 0xffffffffu;
        static int reported = 0;
        const int changed = a0 != lastA0 || minx != lastMinx || maxx != lastMaxx ||
                            miny != lastMiny || maxy != lastMaxy;
        /* The kernel message buffer holds the last second or two of output at most -- a usb180
         * sample spanned one second -- so a line printed every second is gone long before the log
         * is read. Print when a value changes, and once more late in the boot so at least one
         * complete line survives. */
        if (changed || (!reported && native_touch_reg_polls >= 55)) {
            reported = 1;
            lastA0 = a0; lastMinx = minx; lastMaxx = maxx; lastMiny = miny; lastMaxy = maxy;
            printf("TREG a0=%u rx=%d..%d ry=%d..%d ev=%u/%u/%u/%u rel=%d,%d,e%d int=%02x work=%02x\n",
                   a0, minx, maxx, miny, maxy,
                   __atomic_load_n(&native_touch_ev[0], __ATOMIC_RELAXED),
                   __atomic_load_n(&native_touch_ev[1], __ATOMIC_RELAXED),
                   __atomic_load_n(&native_touch_ev[2], __ATOMIC_RELAXED),
                   __atomic_load_n(&native_touch_ev[3], __ATOMIC_RELAXED),
                   __atomic_load_n(&native_touch_rel_x, __ATOMIC_RELAXED),
                   __atomic_load_n(&native_touch_rel_y, __ATOMIC_RELAXED),
                   __atomic_load_n(&native_touch_rel_ev, __ATOMIC_RELAXED),
                   intCount, workCount);
        }
    }
    if (!asleep && !lost) return;
    gpioReset(s, 1);
    ioDelayUs(nullptr, TB_RESET_ASSERT_US);
    gpioReset(s, 0);
    const int ready = fts5652_wait_ready(&s.chip, 1000, nullptr);
    native_touch_recoveries = native_touch_recoveries + 1;
    if (&qcom_usb_debug_write) {
        int n = snprintf(text, sizeof(text), "TOUCHRECOVER n=%u asleep=%u lost=%u ready=%d\n",
            native_touch_recoveries, asleep ? 1u : 0u, lost ? 1u : 0u, ready);
        if (n > 0) qcom_usb_debug_write(text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
    }
}

void touchThread(void *arg, wait_result_t) {
    Session &s = *static_cast<Session *>(arg);
    struct Fts5652Contact contacts[10];
    unsigned frames = 0;
    /* native-touch-diag=1: one round that answers "is the part scanning at all" before any
     * frame is read. The frame loop issues an SPI read every few hundred milliseconds, and a
     * part with no data still answers those, so it cannot tell a part that sees nothing from
     * one whose report never reaches the host. This phase issues no frame read at all: reset,
     * wait as long as the board's own driver waits, then watch 0x8F, the part's interrupt
     * counter, while the user holds a finger down. A counter that stays put means the sensor
     * is not detecting; one that climbs while GPIO 39 stays high means the report exists and
     * the line to the host is what is wrong. */
    unsigned diag = 0;
    PE_parse_boot_argn("native-touch-diag", &diag, sizeof(diag));
    if (diag) {
        gpioReset(s, 1);
        ioDelayUs(nullptr, TB_RESET_ASSERT_US);
        gpioReset(s, 0);
        IOSleep(300);       /* fts_reset_proc(200) then fts_wait_tp_to_valid on the board's driver */
        for (unsigned second = 0; second < 30; ++second) {
            uint8_t power = 0, intCount = 0, work = 0, charger = 0, gesture = 0;
            (void) fts5652_read_reg(&s.chip, 0xa5, &power);
            (void) fts5652_read_reg(&s.chip, 0x8f, &intCount);
            (void) fts5652_read_reg(&s.chip, 0x91, &work);
            (void) fts5652_read_reg(&s.chip, 0x8b, &charger);
            (void) fts5652_read_reg(&s.chip, 0xd0, &gesture);
            const unsigned level = (unsigned) gpioRead(s, TB_GPIO_IRQ);
            if (&qcom_usb_debug_write) {
                char text[176];
                int n = snprintf(text, sizeof(text),
                    "TOUCHWATCH t=%us irq=%u power=%02x int=%02x work=%02x chg=%02x ges=%02x\n",
                    second, level, power, intCount, work, charger, gesture);
                if (n > 0) qcom_usb_debug_write(text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
            }
            IOSleep(1000);
        }
    }
    for (;;) {
        /* Sleep, do not IODelay. IODelay takes microseconds, so the old
         * IODelay(TB_FRAME_PERIOD_US / 1000) asked for 6 us, not the 6 ms the constant means,
         * and on this board IODelay also blocks a whole timer tick whatever it is asked for
         * (no powergate-latency in the device tree, so delay_for_interval never spins). The
         * frame loop therefore ran at roughly 1200 Hz instead of 166 Hz: measured
         * 2026-10-28, the first boot where the touch thread actually ran (boot-usb57) issued
         * seven times the designed SPI traffic and never reached SpringBoard in 534 s, while
         * the same board reached the lock screen in about 104 s with the probe inactive.
         * IOSleep yields the CPU between frames, which a polled input thread must do. */
        IOSleep(TB_FRAME_PERIOD_US / 1000);
        frames = frames + 1;
        if (frames % TB_REG_POLL_FRAMES == 0) touchHealthCheck(s);
        /* The part raises its interrupt line while it holds a report: the reference driver for
         * this SoC family states it as "low = the chip has a report" and looks at the line
         * rather than reading the frame register on a timer. Reading on a blind timer is what
         * this loop used to do, and boot-usb64 saw one report in nine hundred seconds of
         * polling while the user touched repeatedly. Read the line first and only fetch a frame
         * when the part says it has one. */
        native_touch_irq_level = (unsigned)gpioRead(s, TB_GPIO_IRQ);
        if (native_touch_irq_level != 0 && (frames % 50) != 0) { continue; }
        if (native_touch_irq_level == 0) native_touch_irq_low = native_touch_irq_low + 1;
        native_touch_loop_step = 1;
        unsigned count = 0;
        int rc = fts5652_frame(&s.chip, &count, contacts, 10);
        ++s.reads;
        if (rc) {
            ++s.bad;
            /* Publish the frame failure the same way a probe failure is published. Without
             * this a loop that never reads a frame is silent: s.fail is a member of the
             * session and nothing copies it to the counter the console reads. */
            if (native_touch_fail_step == 0) native_touch_fail_step = 6;
            native_touch_frame_rc = (unsigned)(-rc);
            continue;
        }
        native_touch_frame_rc = 0;
        native_touch_loop_step = 2;
        native_touch_valid = native_touch_valid + 1;
        native_touch_frames = native_touch_frames + 1;
        /* Keep the head of the payload that produced this frame. A frame that reports zero
         * contacts and a frame whose contacts land off-screen look identical in the counters,
         * and these bytes separate them. */
        for (unsigned i = 0; i < 8; ++i) native_touch_frame_raw[i] = s.chip.rx[7 + i];
        /* The CRC trailer, next to the head of the payload it covers. An all-0xFF payload
         * carries 0xF11C, so "expected" is what a real empty frame looks like; a received
         * value of 0xFFFF instead means the bus returned nothing and the frame path was
         * never actually validated. */
        if (diag && &qcom_usb_debug_write) {
            char text[144];
            int n = snprintf(text, sizeof(text), "TOUCHFRAME crc=%04x exp=%04x head=%02x%02x%02x%02x%02x%02x\n",
                s.chip.last_crc_received, s.chip.last_crc_expected,
                s.chip.rx[7], s.chip.rx[8], s.chip.rx[9], s.chip.rx[10], s.chip.rx[11], s.chip.rx[12]);
            if (n > 0) qcom_usb_debug_write(text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
        }
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
        !(probeTlmm(s), s.tlmmNorth.va && s.tlmmSouth.va) ||
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
    // MISO, MOSI and CLK: qup4 function, 6 mA, no pull. Function 1, not 0.
    for (unsigned i = 0; i < 3; ++i) {
        gpioConfigure(s, TB_GPIO_SPI_FIRST + i, TB_CFG_FUNC_SPI, TB_CFG_PULL_NONE, TB_CFG_DRIVE_MA2 + 2, false);
    }
    // Chip select: muxed to qup4 so the GENI engine's own CS reaches the pin. Not driven here.
    gpioConfigure(s, TB_GPIO_CS, TB_CFG_FUNC_SPI, TB_CFG_PULL_UP, TB_CFG_DRIVE_MA16, false);
    // The panel interrupt is only left as an input; the frame loop polls instead of waiting.
    gpioConfigure(s, TB_GPIO_IRQ, 0, TB_CFG_PULL_UP, TB_CFG_DRIVE_MA2 + 2, false);
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
    // Supply the rail before releasing reset. Nothing powers the touch controller at boot:
    // on this board the FTS5652 is on an RPMh-controlled LDO, and without the vote it does
    // not answer on SPI at all. Measured on the same phone with the ios-sweet port: every
    // register read timed out until an L11C vote was added, and touch worked immediately
    // afterwards (ARCHIVE/ios-sweet/NOTES.md:24 and :29). A refusal here is not fatal to the
    // rest of the boot, so it is reported and the probe continues: the identify below then
    // fails on its own and names the read that timed out.
    {
        /* Two rails, in this order, and both are required. The chip's own supply is ldoc10 at
         * 3.0 V and its I/O rail is ldoc1 at 1.8 V; ldoc11 at 3.2 V is the *panel's* I/O and
         * does not power the touch controller. Measured 2026-10-27 on boot-usb44: voting
         * ldoc11 alone made the SPI bus answer, but every frame read failed its Kermit CRC
         * (-74), which is what a powered-but-wrongly-supplied part returns. The rails and
         * voltages are the ones the ios-sweet port used on this same phone
         * (diag-markers-v30/src/bsd/dev/sweet_touch.c:69-71). */
        static const struct { const char *rail; unsigned mv; } rails[] = {
            { "ldoc10", 3000 },   /* the chip's vdd */
            { "ldoc1",  1800 },   /* its I/O */
        };
        int power = 0;
        for (unsigned i = 0; i < sizeof(rails) / sizeof(rails[0]) && !power; ++i) {
            power = sweet_rpmh_vreg_enable(rails[i].rail, rails[i].mv);
        }
        native_touch_power_result = power;
        if (power) {
            if (!s.fail) {s.fail = 10; native_touch_fail_step = 10;}
            return false;
        }
        /* The rails need time to settle while reset is held low, before the chip is released.
         * The vendor order (and the ios-sweet port's power_on) is reset low, rails up, then
         * 3 ms + 15 ms of settle. Without it the chip can answer on the bus with data that
         * fails its CRC, which is what boot-usb44 showed. */
        ioDelayUs(nullptr, TB_POWER_SETTLE_US);
    }
    // Panel: release reset, wait, identify.
    gpioReset(s, 0);
    ioDelayUs(nullptr, TB_RESET_RELEASE_US);
    s.chip.io.context = &s;
    s.chip.io.transfer = panelTransfer;
    s.chip.io.reset = panelReset;
    s.chip.io.delay_us = ioDelayUs;
    s.chip.io.stage = panelStage;
    /* Give the part time to finish coming up before the identity read. The board's own driver
     * allows a full second for this; reading once after 9 ms returned 0xef for every register. */
    unsigned waited = 0;
    const int ready = fts5652_wait_ready(&s.chip, 1000, &waited);
    native_touch_ready_ms = waited;
    native_touch_ready = ready == 0 ? 1u : 0u;
    if (fts5652_state(&s.chip, 1)) {
        native_touch_crc_expected = s.chip.last_crc_expected;
        native_touch_crc_received = s.chip.last_crc_received;
        native_touch_hdr0 = s.chip.last_header[0];
        native_touch_hdr1 = s.chip.last_header[1];
        native_touch_hdr2 = s.chip.last_header[2];
        native_touch_hdr3 = s.chip.last_header[3];
        native_touch_data0 = s.chip.last_data0;
        native_touch_data1 = s.chip.last_data1;
        native_touch_data2 = s.chip.last_data2;
        /* The app id is the two bytes the identity read returns; it is what the support check
         * compares. Report it and the per-read results. */
        native_touch_app_id = ((unsigned)s.chip.app[0] << 8) | s.chip.app[1];
        native_touch_app_rc[0] = (unsigned)s.chip.app_result;
        native_touch_app_rc[1] = (unsigned)s.chip.rom_result;
        native_touch_app_rc[2] = (unsigned)s.chip.version_result;
        fail(s, 8);
        return false;
    }
    // Frame loop.
    thread_t thread;
    if (kernel_thread_start(touchThread, &s, &thread) != KERN_SUCCESS) {fail(s, 9); return false;}
    thread_deallocate(thread);
    return true;
}