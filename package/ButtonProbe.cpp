// SPDX-License-Identifier: BSD-2-Clause-Patent
// alioth (Redmi K40, SM8250-AC) power and volume buttons. Boot argument alioth-bt=<n>:
//   1  read-only: map the SPMI arbiter and record its version, APID count and the two APIDs
//   2  mode 1, then read the three key levels once and report the pressed mask
//   3  mode 1, then the real path: arm the peripheral interrupts and feed the HID device
//
// The three keys, from this board's own device tree (uefi-alioth/Resources/DTBs/alioth.dts):
//   power      PM8150 PON, SPMI SID 0, peripheral 0x800, interrupt 0 "kpdpwr"
//              (alioth.dts:4187-4212). The qpnp-power-on driver reads the levels from the
//              peripheral's RT_STS at base + 0x10 and tests QPNP_PON_KPDPWR_N_SET = BIT(0)
//              (qpnp-power-on.c:51,113).
//   vol down   the same PON, interrupt 1 "resin", QPNP_PON_RESIN_N_SET = BIT(1)
//              (qpnp-power-on.c:114; alioth.dts:4214-4218).
//   vol up     PM8150 GPIO 6, declared by gpio_keys as <&pm8150_gpios 6 GPIO_ACTIVE_LOW>
//              (alioth.dts:39116-39128). The GPIO block is reg = <0xc000 0xa00>
//              (alioth.dts:4258-4261) and pinctrl-spmi-gpio gives every pad 0x100 bytes, so
//              GPIO 6 is at 0xc600; its RT_STS is base + 0x10 and the level is BIT(0), which
//              pinctrl-spmi-gpio tests as PMIC_MPP_REG_RT_STS_VAL_MASK (pinctrl-spmi-gpio.c:38-39).
//              Active low, so a clear bit is a press.
//
// The PPIDs the arbiter maps: ppid = sid << 8 | (peripheral address >> 8)
// (spmi-pmic-arb.c:977,996), so PON is 0x008 and GPIO 6 is 0x18c - NOT sheng's 0x013/0x18d,
// whose board has its PON and GPIO at different peripheral pages. Copying sheng's values would
// ask the arbiter for a peripheral this phone does not have.
//
// The arbiter itself also differs from sheng's board, and not only in its bases. This board is
// qcom,spmi@c440000 with reg-names core/chnls/obsrvr/intr/cnfg =
// 0xc440000 / 0xc600000 / 0xe600000 / 0xe700000 / 0xc40a000 (alioth.dts:4172-4176), and its
// 32 MiB chnls window is the version-5 layout: channels at 0x10000 * apid with ACC_ENABLE /
// IRQ_STATUS / IRQ_CLEAR at +0x100 / +0x104 / +0x108 inside the channel (pmic_arb_acc_enable_v5
// and friends, spmi-pmic-arb.c:1069-1105), and the APID map at 0x900 + 4 * n
// (pmic_arb_apid_map_offset_v5). The observer channels are still 0x1000 * ee + 0x80 * apid
// (pmic_arb_offset_v5). The older v2/v3/v4 layouts put the channel at 0x1000 * apid with the
// interrupt registers in the "intr" window instead, so the version is read at run time and the
// matching layout is used; a version this code does not know is refused rather than guessed at.
//
// The PDC hop matches sheng: the arbiter's periph_irq is interrupts-extended = <&pdc 1 4>
// (alioth.dts:4177), PDC pin 1, and qcom,pdc-ranges = <0 0x1e0 0x5e ...> (alioth.dts:3694)
// maps PDC pin 1 to GIC SPI 0x1e0 + 1 = 481, so the GIC interrupt is 32 + 481.
//
// The interrupt is only an edge that wakes the thread; the thread reads the three levels and
// reports a change, so a missed or spurious interrupt costs a poll interval, not an event.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOService.h>
#include <kern/thread.h>
#include <pexpert/pexpert.h>

extern "C" int armcpu_spi_attach(uint32_t, bool, void (*)(void *), void *, uint32_t *, uint64_t *) __attribute__((weak_import));

// BootLog.c already composes this row, in sheng's wording:
//   [0] stage (1 mapped, 2 configured, 3 armed, negative: the step that failed)
//   [1] interrupts                    [2] reports sent to the HID device
//   [3] pressed mask (report 2 bits)  [4] PON APID     [5] GPIO APID
//   [6] interrupts of other APIDs cleared     [7] PDC version
//   [8] last error                    [9] spurious interrupts
// Report 2 bits, matching NativeHidInput's buttonsAction (mask & 0xe): 1 power, 2 volume up,
// 3 volume down.
extern "C" {volatile int32_t native_buttons_stats[10];}

namespace {
// This board's arbiter, from alioth.dts:4172-4176 (reg-names core, chnls, obsrvr, intr, cnfg).
constexpr uint64_t kCore = 0xc440000, kChnls = 0xc600000, kObsrvr = 0xe600000, kIntr = 0xe700000,
                   kCnfg = 0xc40a000, kPdc = 0xb220000;
// ppid = sid << 8 | (peripheral >> 8) with sid 0 on this board's PM8150.
constexpr uint32_t kPonPpid = 0x008, kGpioPpid = 0x18c;
// PON RT_STS bits (qpnp-power-on.c:113-114).
constexpr uint8_t kPowerBit = 1u << 0, kResinBit = 1u << 1;
// SPMI GPIO: every pad is 0x100 bytes, RT_STS at +0x10, level in BIT(0) (pinctrl-spmi-gpio.c:23,38-39).
constexpr uint8_t kGpioBit = 1u << 0;
// Peripheral registers, addressed through a channel.
constexpr uint8_t kRtSts = 0x10, kSetType = 0x11, kPolHigh = 0x12, kPolLow = 0x13, kLatchedClr = 0x14, kEnSet = 0x15;
// Channel status: DONE bit 0, and bits 1..3 are failure/denied/dropped (spmi-pmic-arb.c:78-84).
constexpr uint32_t kStatusDone = 1u << 0, kStatusErrors = 0xeu;
// PDC: pin 1, SPI 481, IRQ_i_CFG at 0x110, enable bank at 0x10 (qcom-pdc.c:31-34).
constexpr uint32_t kPdcPin = 1, kSpi = 481, kPdcCfg = 0x110, kPdcEnableBank = 0x10, kPdcVersion = 0x1000;
// Report 2 bits as NativeHidInput expects them.
constexpr uint32_t kReportPower = 1u << 1, kReportVolumeUp = 1u << 2, kReportVolumeDown = 1u << 3;

struct Window {IOMemoryMap *map; volatile uint32_t *va;};
Window gCore, gCnfg, gObs, gChn, gIntr, gPdc;
int gPon = -1, gGpio = -1;
uint32_t gApids;
uint32_t gVersion;
// The channel page size and the in-channel offsets, chosen from the arbiter's version.
uint32_t gApidStride, gAccEnable, gIrqStatus, gIrqClear, gApidMap, gObsStride;
volatile uint32_t gEvents;
void (*volatile gSink)(void *, uint32_t);
void *volatile gSinkContext;
unsigned gMode;

bool mapWindow(Window &w, uint64_t base, uint64_t size) {
    IODeviceMemory *memory = IODeviceMemory::withRange(base, size);
    w.map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    w.va = w.map ? reinterpret_cast<volatile uint32_t *>(w.map->getVirtualAddress()) : nullptr;
    return w.va != nullptr;
}
void fail(int step, int error) {native_buttons_stats[8] = error; native_buttons_stats[0] = -step;}

// The channel page for an APID, in whichever window carries the read/write channels.
volatile uint32_t *apidReg(int apid, uint32_t reg) {return gChn.va + (gApidStride * (uint32_t)apid + reg) / 4;}

// One byte of a peripheral through a channel. opc 1 = EXT_READL, 0 = EXT_WRITEL; the command
// word is (opc << 27) | (byte address << 4) | byte count (pmic_arb_fmt_cmd_v2, spmi-pmic-arb.c).
int transfer(int apid, uint8_t addr, uint8_t *value, bool write) {
    volatile uint32_t *ch = gChn.va + (gApidStride * (uint32_t)apid) / 4;
    if (write) ch[0x10 / 4] = *value;                       // WDATA0
    ch[0x00 / 4] = ((write ? 0u : 1u) << 27) | ((uint32_t)addr << 4);
    __asm__ volatile("dsb sy" ::: "memory");
    for (uint32_t n = 0; n < 100000; ++n) {
        const uint32_t s = ch[0x08 / 4];                    // STATUS
        if (s & kStatusDone) {
            if (s & kStatusErrors) return 2;
            if (!write) *value = (uint8_t)ch[0x18 / 4];     // RDATA0
            return 0;
        }
    }
    return 1;
}
int read8(int apid, uint8_t addr, uint8_t *value) {return transfer(apid, addr, value, false);}
int write8(int apid, uint8_t addr, uint8_t value) {return transfer(apid, addr, &value, true);}

// The APID that serves ppid with its interrupts owned by this EE; -1 when there is none.
int findApid(uint32_t ppid) {
    int found = -1;
    for (uint32_t i = 0; i < gApids; ++i) {
        const uint32_t map = gCore.va[(gApidMap + 4 * i) / 4];
        if (!map || ((map >> 8) & 0xfff) != ppid) continue;
        const bool irqOwner = (map >> 24) & 1;
        if (irqOwner && (gCnfg.va[(0x00 + 4 * i) / 4] & 7u) == 0) found = (int)i;
    }
    return found;
}

// Both edges of the given interrupt bits, latched state cleared, then enabled in the peripheral.
int armPeripheral(int apid, uint8_t bits) {
    uint8_t type = 0, high = 0, low = 0;
    int rc = read8(apid, kSetType, &type);
    if (!rc) rc = read8(apid, kPolHigh, &high);
    if (!rc) rc = read8(apid, kPolLow, &low);
    if (!rc) rc = write8(apid, kSetType, type | bits);
    if (!rc) rc = write8(apid, kPolHigh, high | bits);
    if (!rc) rc = write8(apid, kPolLow, low | bits);
    if (!rc) rc = write8(apid, kLatchedClr, bits);
    if (!rc) rc = write8(apid, kEnSet, bits);
    if (!rc) *apidReg(apid, gAccEnable) = 1;
    return rc;
}

// Primary interrupt context: acknowledge in the arbiter and the peripheral, then wake the thread.
// An APID that is not ours but raised the line only gets its arbiter status cleared, as the
// vendor driver's cleanup_irq does; reading another APID's registers can end in a synchronous
// external abort (sheng measured int-227 doing exactly that).
void buttonsIrq(void *) {
    bool ours = false;
    for (uint32_t word = 0; word * 32 < gApids; ++word) {
        uint32_t pending = gIntr.va[word];
        while (pending) {
            const int apid = (int)(word * 32 + __builtin_ctz(pending));
            pending &= pending - 1;
            if (apid != gPon && apid != gGpio) {
                *apidReg(apid, gIrqClear) = 0xff;
                native_buttons_stats[6] = native_buttons_stats[6] + 1;
                continue;
            }
            const uint32_t status = *apidReg(apid, gIrqStatus);
            *apidReg(apid, gIrqClear) = status;
            if (status) write8(apid, kLatchedClr, (uint8_t)status);
            ours = true;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
    // A line that keeps firing with none of our peripherals behind it would hold this core in the
    // handler: after 1000 such interrupts in a row the PDC pin is masked, which drops the line.
    static uint32_t idle;
    if (!ours) {
        native_buttons_stats[9] = native_buttons_stats[9] + 1;
        if (++idle >= 1000) {
            gPdc.va[(kPdcCfg + 4 * kPdcPin) / 4] = 4u;
            __asm__ volatile("dsb sy" ::: "memory");
            native_buttons_stats[0] = -9;
        }
        return;
    }
    idle = 0;
    native_buttons_stats[1] = native_buttons_stats[1] + 1;
    __atomic_fetch_add(&gEvents, 1, __ATOMIC_RELEASE);
    thread_wakeup((event_t)&gEvents);
}

// The pressed set, read from the three levels. All three are active low.
uint32_t pressed() {
    uint8_t pon = 0, gpio = 0xff;
    uint32_t mask = 0;
    if (!read8(gPon, kRtSts, &pon)) {
        if (pon & kPowerBit) mask |= kReportPower;
        if (pon & kResinBit) mask |= kReportVolumeDown;
    }
    if (gGpio >= 0 && !read8(gGpio, kRtSts, &gpio) && !(gpio & kGpioBit)) mask |= kReportVolumeUp;
    return mask;
}

bool setup() {
    if (!mapWindow(gCore, kCore, 0x3000) || !mapWindow(gCnfg, kCnfg, 0x4000) || !mapWindow(gIntr, kIntr, 0x1000) ||
        !mapWindow(gPdc, kPdc, 0x2000)) {fail(1, -12); return false;}
    gVersion = gCore.va[0];
    gApids = gCore.va[0x04 / 4] & 0x7ffu;
    native_buttons_stats[7] = (int32_t)gPdc.va[kPdcVersion / 4];
    // The layout follows the version, as the vendor driver's ops table does. v5 keeps every
    // channel register inside the 0x10000 * apid page; v2/v3/v4 use a 0x1000 * apid page and put
    // the interrupt registers in the "intr" window. Anything else is refused, not guessed at.
    if (gVersion >= 0x50000000u && gVersion < 0x60000000u) {
        gApidStride = 0x10000; gAccEnable = 0x100; gIrqStatus = 0x104; gIrqClear = 0x108;
        gApidMap = 0x900; gObsStride = 0x80;
    } else if (gVersion >= 0x20010000u && gVersion < 0x40000000u) {
        gApidStride = 0x1000; gAccEnable = 0; gApidMap = 0x800; gObsStride = 0x80;
    } else {
        fail(2, (int)gVersion); return false;
    }
    if (!gApids || gApids > 1024) {fail(2, (int)gVersion); return false;}
    if (!mapWindow(gObs, kObsrvr, ((uint64_t)gObsStride * gApids + 0xfff) & ~0xfffull) ||
        !mapWindow(gChn, kChnls, (uint64_t)gApidStride * gApids)) {fail(3, -12); return false;}
    native_buttons_stats[0] = 1;
    gPon = findApid(kPonPpid); gGpio = findApid(kGpioPpid);
    native_buttons_stats[4] = gPon; native_buttons_stats[5] = gGpio;
    if (gPon < 0) {fail(4, -2); return false;}
    native_buttons_stats[0] = 2;
    if (gMode == 2) return true;      // read-only: no arming, no interrupt
    int rc = armPeripheral(gPon, kPowerBit | kResinBit);
    if (!rc && gGpio >= 0) rc = armPeripheral(gGpio, kGpioBit);
    if (rc) {fail(5, rc); return false;}
    // PDC pin 1: level high, enabled. The version decides which register carries the enable.
    const uint32_t pdcVersion = gPdc.va[kPdcVersion / 4];
    if (pdcVersion >= 0x30200) {
        gPdc.va[(kPdcCfg + 4 * kPdcPin) / 4] = (1u << 3) | 4u;
    } else {
        gPdc.va[(kPdcCfg + 4 * kPdcPin) / 4] = 4u;
        gPdc.va[(kPdcEnableBank + 4 * (kPdcPin / 32)) / 4] |= 1u << (kPdcPin % 32);
    }
    __asm__ volatile("dsb sy" ::: "memory");
    if (!armcpu_spi_attach) {fail(6, -78); return false;}
    // The probe runs before the interrupt controller driver has started, so wait for it, up to a
    // minute, as sheng does (int-228: kIOReturnNotReady at 4 s).
    for (unsigned tries = 0;; ++tries) {
        rc = armcpu_spi_attach(32 + kSpi, false, buttonsIrq, nullptr, nullptr, nullptr);
        if (rc != kIOReturnNotReady || tries == 240) break;
        IOSleep(250);
    }
    if (rc) {fail(6, rc); return false;}
    native_buttons_stats[0] = 3;
    return true;
}

void buttonsThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "alioth-bt");
    if (!setup()) return;
    uint32_t seen = gEvents, last = 0;
    for (;;) {
        const uint32_t mask = pressed();
        if (mask != last) {
            last = mask;
            native_buttons_stats[3] = (int32_t)mask;
            void (*sink)(void *, uint32_t) = __atomic_load_n(&gSink, __ATOMIC_ACQUIRE);
            if (sink) {sink(gSinkContext, mask); native_buttons_stats[2] = native_buttons_stats[2] + 1;}
        }
        assert_wait((event_t)&gEvents, THREAD_UNINT);
        if (gEvents != seen) clear_wait(current_thread(), THREAD_AWAKENED);
        else thread_block(THREAD_CONTINUE_NULL);
        seen = gEvents;
    }
}
}  // namespace

extern "C" void native_buttons_set_sink(void (*sink)(void *, uint32_t), void *context) {
    gSinkContext = context;
    __atomic_store_n(&gSink, sink, __ATOMIC_RELEASE);
}

extern "C" void alioth_buttons_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("alioth-bt", &mode, sizeof mode) || !mode || mode > 3 || native_buttons_stats[0]) return;
    gMode = mode;
    native_buttons_stats[0] = 1;
    thread_t thread = nullptr;
    if (kernel_thread_start(buttonsThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else fail(7, -12);
}
