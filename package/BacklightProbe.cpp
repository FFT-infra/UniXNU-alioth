// SPDX-License-Identifier: BSD-2-Clause-Patent
// alioth (Redmi K40, SM8250-AC) panel brightness through DSI0's command FIFO.
// Boot argument alioth-bl=<n>:
//   1  read-only: map DSI0 and record its registers. No write of any kind.
//   2  mode 1, then the visible test: maximum, low, then alioth-bl-level=<1..0x7ff>
//      (default 0x218), two seconds apart, and a read-back of the panel's brightness after each
//   3  mode 1, then the iOS brightness service (AliothBacklight): iOS sets the level
//
// Why DCS over DSI and not an I2C backlight controller. The phone's own device tree selects
// qcom,mdss_dsi_k11a_38_08_0a_dsc_cmd as this board's panel (the stock bootargs carry
// msm_drm.dsi_display0=qcom,mdss_dsi_k11a_38_08_0a_dsc_cmd:, uefi-alioth/Resources/DTBs/alioth.dts:39468),
// and that panel node declares qcom,mdss-dsi-bl-pmic-control-type = "bl_ctrl_dcs",
// bl-min-level 1, bl-max-level 0x7ff, brightness-init-level 0x218 and mi,mdss-dsi-bl-dcs-big-endian-type
// (alioth.dts:27485-27489). This board's tree has no KTZ8866 node at all, and the one WLED node
// (qcom,pm8150l-spmi-wled, alioth.dts:5477) is status = "disabled", as are pm8150a_amoled and
// pm8150l_lcdb in alioth-sm8250.dtsi:242-244. Brightness is therefore the panel's own DCS
// register reached over DSI, not the sheng board's two KTZ8866 I2C parts.
//
// How a command reaches the panel. Through the controller's TPG command FIFO, written word by
// word, with no DMA buffer and therefore no display SMMU mapping. That is the path the vendor
// driver takes in dsi_ctrl_hw_cmn_kickoff_fifo_command (techpack/display/msm/dsi/dsi_ctrl_hw_cmn.c:703),
// and the same technique the asteroids and Pagani ports use. Offsets come from that file and
// dsi_ctrl_reg.h. The controller is qcom,dsi-ctrl-hw-v2.4 (alioth.dts:30915), which
// dsi_catalog.c:108-140 maps onto the 2.2/2.3/2.4 path, the one that carries kickoff_fifo_command.
//
// The panel is dsi_cmd_mode, not video mode, so the transfer is NOT scheduled for a
// vertical-front-porch line the way a video-mode panel needs: dsi_configure_command_scheduling
// runs only for DSI_OP_VIDEO_MODE or CUSTOM_DMA_SCHED, and the software trigger starts it directly.
//
// Packet words. The four header bytes are the MIPI packet's, in order (dsi_ctrl_copy_and_pad_cmd,
// dsi_ctrl.c:1061-1103), so the data type sits in bits 23:16 and the word count in bits 7:0. Byte
// 3 carries the flags: bit 6 payload present, bit 7 last command, bit 5 turn the lane around.
//   DCS long write 0x39, one payload word: 0xC0390003   (vendor: 0x40390003 without bit 7)
//   payload: 0x51 | high << 8 | low << 16 | 0xff << 24   (big endian, per bl-dcs-big-endian-type;
//            the 0xff is copy_and_pad_cmd's pad byte, because 7 bytes round up to 8)
//   DCS short write 0x05, no parameter: 0x80050000 | command   (vendor: 0x00050000 | command)
//   SET_MAXIMUM_RETURN_PACKET_SIZE 0x37 with one byte: 0x80150000 | high << 8 | low
//   DCS read 0x06: 0xA0060000 | command   (bit 5 turns the lane around for the read-back)
// Bit 7 is the one place this file departs from the vendor driver: the vendor sets the
// last-command flag only when a command set says so (dsi_panel.c:615), while the
// device-verified asteroids port sets it on every packet. Both are legal MIPI; the constant
// kLastCommand below makes the choice explicit and one line to flip if a panel objects.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOService.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOPlatformExpert.h>
#include <libkern/OSKextLib.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSSymbol.h>
#include <mach/kmod.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <pexpert/pexpert.h>

// BootLog.c already composes two rows from these, in sheng's wording. On this board the same
// slots carry one DSI controller, documented here so the row can be read:
//   [0] stage (1 mapped, 2 discovery done, 3 published, negative: the step that failed)
//   [1] brightness requests taken          [2] last level written
//   [3] DCS 0x52 read-back at start        [4] DCS 0x52 read-back after the last write
//   [5] transfers that failed              [6] last INT_CTRL of a failed transfer
//   [7] DSI_CTRL at start (bit 0 DSI_EN, bit 1 video engine, bit 2 command engine)
//   [8] DSI_STATUS at start (bit 2 command-mode MDP busy)
//   [9] last error                         [10] commands sent
//   [11] commands timed out                [12] last command's time in microseconds
//   [13] HW_VERSION                        [14] TPG_CTRL at start
//   [15] COMMAND_MODE_DMA_CTRL at start
extern "C" {volatile int32_t native_backlight_stats[16];}
// [0] goal level, [1] fade ms, [2] request actions taken, [3] fades begun,
// [4] reads that returned nothing, [5] last read-back count (RDBK_DATA_CTRL bits 31:16).
extern "C" {volatile int32_t native_backlight_request[6];}

namespace {
// DSI0: reg = <0xae94000 0x400 ...> (alioth.dts:30916). Every register below is inside that.
constexpr uint64_t kDsi0 = 0xae94000, kDsiBytes = 0x400;
constexpr uint32_t kHwVersion = 0x000, kCtrl = 0x004, kStatus = 0x008, kRdbkData0 = 0x06c,
                   kDmaCtrl = 0x03c, kDmaLength = 0x04c, kIntCtrl = 0x110, kTpgCtrl = 0x15c,
                   kTpgInit = 0x17c, kRdbkDataCtrl = 0x1d4, kTpgFifoReset = 0x1ec, kSwTrigger = 0x090;
constexpr uint32_t kStatusMdpBusy = 1u << 2, kIntDmaDone = 1u << 0;
constexpr uint32_t kMaxLevel = 0x7ff, kMinLevel = 1, kDefaultLevel = 0x218;
constexpr uint32_t kTimeoutUs = 50000, kIdleTimeoutUs = 20000;
constexpr uint8_t kDcsBrightness = 0x51, kDcsBrightnessGet = 0x52,
                  kDcsDisplayOn = 0x29, kDcsDisplayOff = 0x28, kDcsSleepOut = 0x11, kDcsSleepIn = 0x10;
// Flags byte bit 7. See the header comment: the vendor leaves it clear, asteroids sets it.
constexpr uint32_t kLastCommand = 1u << 31;

IOMemoryMap *gMap;
volatile uint32_t *gDsi;
IOLock *gLock;

uint32_t rd(uint32_t reg) {return gDsi[reg / 4];}
void wr(uint32_t reg, uint32_t value) {gDsi[reg / 4] = value; __asm__ volatile("dsb sy" ::: "memory");}
void fail(int step, int error) {native_backlight_stats[9] = error; native_backlight_stats[0] = -step;}
void bump(int index) {native_backlight_stats[index] = native_backlight_stats[index] + 1;}

// One packet through the TPG FIFO. words holds the header and payload words; count is how many.
// Returns 0, or -1 when the DMA-done interrupt never came or the command path stayed busy.
int sendPacket(const uint32_t *words, unsigned count, bool lp) {
    const uint64_t began = mach_absolute_time();
    // Take the FIFO only when the command path is idle, as the vendor driver does before every
    // transfer: a busy engine means an earlier packet is still in flight.
    bool idle = false;
    for (uint32_t waited = 0; waited < kIdleTimeoutUs; waited += 2) {
        if (!(rd(kStatus) & kStatusMdpBusy)) {idle = true; break;}
        IODelay(2);
    }
    if (!idle) {bump(5); native_backlight_stats[6] = (int32_t)rd(kStatus); return -1;}
    wr(kIntCtrl, (rd(kIntCtrl) & ~((1u << 24) | (1u << 28))) | kIntDmaDone);   // clear a stale done
    wr(kTpgCtrl, (1u << 1) | (1u << 2) | (3u << 16));                          // TPG enable, FIFO mode, custom pattern
    for (unsigned i = 0; i < count; ++i) wr(kTpgInit, words[i]);
    if (count & 1) wr(kTpgInit, 0);                                            // the extra DWORD the hardware wants
    uint32_t dma = rd(kDmaCtrl) & ~((1u << 31) | (1u << 30));
    dma = lp ? (dma | (1u << 26)) : (dma & ~(1u << 26));
    wr(kDmaCtrl, dma | (1u << 28));                                            // embedded mode
    wr(kDmaLength, count * 4);
    wr(kSwTrigger, 1);
    bool done = false;
    for (uint32_t waited = 0; waited < kTimeoutUs; waited += 20) {
        if (rd(kIntCtrl) & kIntDmaDone) {done = true; break;}
        IODelay(20);
    }
    if (!done) {bump(5); bump(11); native_backlight_stats[6] = (int32_t)rd(kIntCtrl);}
    wr(kIntCtrl, (rd(kIntCtrl) & ~((1u << 24) | (1u << 28))) | kIntDmaDone);
    wr(kTpgCtrl, 0);
    wr(kTpgFifoReset, 1);
    IODelay(1);
    wr(kTpgFifoReset, 0);
    bump(10);
    uint64_t ns = 0;
    absolutetime_to_nanoseconds(mach_absolute_time() - began, &ns);
    native_backlight_stats[12] = (int32_t)(ns / 1000);
    return done ? 0 : -1;
}

int shortWrite(uint8_t command) {
    const uint32_t word = kLastCommand | 0x00050000u | command;
    return sendPacket(&word, 1, false);
}

// Display off (0x28 then 0x10) or on (0x11 then 0x29) with the panel's own waits. The waits sleep
// the calling thread, so this runs on a thread or a work loop, never in an interrupt. Level 0 in
// the service means display off, as asteroids does for a panel with no backlight switch: this
// board's panel has no separate enable, so its own sleep-in is the way to go dark.
int panelPower(bool on) {
    int rc = shortWrite(on ? kDcsSleepOut : kDcsDisplayOff);
    IOSleep(on ? 80 : 120);
    rc |= shortWrite(on ? kDcsDisplayOn : kDcsSleepIn);
    IOSleep(on ? 10 : 100);
    return rc;
}

// Sets the panel's brightness. Level 0 is display off, not a DCS level of zero: this panel has
// no separate enable line, so its own sleep-in sequence is the way to go dark, and the next
// level above 0 wakes it again. Any non-zero level below the declared minimum becomes that
// minimum, so a fade cannot step through levels the panel rejects.
int writeBrightness(uint32_t level) {
    static bool lit = true;
    if (level > kMaxLevel) level = kMaxLevel;
    if (level && level < kMinLevel) level = kMinLevel;
    if (!level) {
        if (!lit) return 0;
        lit = false;
        return panelPower(false);
    }
    if (!lit) {lit = true; panelPower(true);}
    const uint32_t words[2] = {kLastCommand | 0x00390000u | 3u,
                               kDcsBrightness | ((level >> 8) & 0xffu) << 8 | (level & 0xffu) << 16 | 0xffu << 24};
    const int rc = sendPacket(words, 2, false);
    if (!rc) native_backlight_stats[2] = (int32_t)level;
    return rc;
}

// The panel's own brightness register (DCS 0x52, two bytes, big endian). Returns the value or -1.
// The controller must be told how large a return packet to expect before the read
// (SET_MAXIMUM_RETURN_PACKET_SIZE, dsi_set_max_return_size in dsi_ctrl.c), which is a short
// write with one parameter.
int32_t readBrightness() {
    wr(kRdbkDataCtrl, 0x1);
    __asm__ volatile("dsb sy" ::: "memory");
    wr(kRdbkDataCtrl, 0x0);
    __asm__ volatile("dsb sy" ::: "memory");
    const uint32_t size[2] = {kLastCommand | 0x00150000u | 2u, 2u};
    if (sendPacket(size, 2, false)) return -1;
    const uint32_t read[1] = {kLastCommand | 0x00060000u | 0x20u | kDcsBrightnessGet};
    if (sendPacket(read, 1, false)) return -1;
    IOSleep(2);
    const uint32_t count = rd(kRdbkDataCtrl) >> 16;
    native_backlight_request[5] = (int32_t)count;
    if (!count) {bump(4); return -1;}
    // RDBK_DATA0..3 carry the returned packet, read highest word first and byte-swapped
    // (get_cmd_read_data, dsi_ctrl_hw_cmn.c:828-860). The payload is the two brightness bytes.
    const uint32_t word = __builtin_bswap32(rd(kRdbkData0));
    return (int32_t)((word >> 8) & 0xffu) << 8 | (int32_t)(word & 0xffu);
}

// Reads only: what the boot loader left the controller in. Called from the read-only mode.
void snapshot() {
    native_backlight_stats[13] = (int32_t)rd(kHwVersion);
    native_backlight_stats[7] = (int32_t)rd(kCtrl);
    native_backlight_stats[8] = (int32_t)rd(kStatus);
    native_backlight_stats[14] = (int32_t)rd(kTpgCtrl);
    native_backlight_stats[15] = (int32_t)rd(kDmaCtrl);
}
}  // namespace

// The iOS-facing service, shaped after the two ports that already answer CoreBrightness
// (asteroids' AsteroidsBacklight.cpp and sheng's AppleARMBacklight.cpp): publish
// IODisplayParameters with min/max, accept brightness and FadePeriod in setProperties, step the
// fade on a 16 ms timer, and present as AppleARMBacklight so backboardd's sandbox profile lets
// it set those keys.
class AliothBacklight : public IOService {
    OSDeclareDefaultStructors(AliothBacklight)
    IOWorkLoop *loop = nullptr;
    IOTimerEventSource *fade = nullptr;
    uint32_t level = 0, from = 0, to = 0;
    uint64_t start = 0, period = 0;

    static uint64_t now() {return mach_absolute_time();}

    void publish(uint32_t value) {
        OSDictionary *brightness = OSDictionary::withCapacity(3), *parameters = OSDictionary::withCapacity(1);
        OSNumber *min = OSNumber::withNumber((uint64_t)kMinLevel, 32), *max = OSNumber::withNumber((uint64_t)kMaxLevel, 32),
                 *current = OSNumber::withNumber((uint64_t)value, 32);
        if (brightness && parameters && min && max && current) {
            brightness->setObject("min", min); brightness->setObject("max", max); brightness->setObject("value", current);
            parameters->setObject("brightness", brightness);
            setProperty("IODisplayParameters", parameters);
        }
        OSSafeReleaseNULL(min); OSSafeReleaseNULL(max); OSSafeReleaseNULL(current);
        OSSafeReleaseNULL(brightness); OSSafeReleaseNULL(parameters);
    }
    // Presents as AppleARMBacklight: the vtable is copied and getMetaClass (slot 7) answers that
    // metaclass, so IOObjectConformsTo and the sandbox walk both see the name CoreBrightness looks
    // for. The same technique and the same reason as sheng (int-275/276) and asteroids.
    static inline const OSMetaClass *gAppleClass = nullptr;
    static const OSMetaClass *appleMetaClass(const OSMetaClassBase *) {return gAppleClass;}
    bool impersonate() {
        enum {kSlots = 320, kGetMetaClass = 7};
        const OSSymbol *name = OSSymbol::withCString("AppleARMBacklight");
        gAppleClass = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
        OSSafeReleaseNULL(name);
        if (!gAppleClass) return false;
        uintptr_t *copy = static_cast<uintptr_t *>(IOMalloc(kSlots * sizeof(uintptr_t)));
        if (!copy) return false;
        memcpy(copy, *reinterpret_cast<uintptr_t **>(this), kSlots * sizeof(uintptr_t));
        copy[kGetMetaClass] = reinterpret_cast<uintptr_t>(&appleMetaClass);
        __asm__ volatile("dmb ish" ::: "memory");
        *reinterpret_cast<uintptr_t **>(this) = copy;
        return true;
    }
    // Steps a fade from the level now shown to the goal, on the service's own timer.
    static void fadeStep(OSObject *owner, IOTimerEventSource *sender) {
        AliothBacklight *self = static_cast<AliothBacklight *>(owner);
        if (!self) return;
        const uint64_t elapsed = now() - self->start;
        uint32_t next = self->to;
        if (elapsed < self->period)
            next = (uint32_t)((int64_t)self->from + ((int64_t)self->to - (int64_t)self->from) * (int64_t)elapsed / (int64_t)self->period);
        if (next != self->level) {
            IOLockLock(gLock);
            writeBrightness(next);
            IOLockUnlock(gLock);
            self->level = next;
        }
        if (next != self->to) sender->setTimeoutMS(16);
    }
    // A request taken on the work loop: fade from the level now shown to the newest goal.
    static IOReturn takeRequest(OSObject *owner, void *, void *, void *, void *) {
        AliothBacklight *self = static_cast<AliothBacklight *>(owner);
        if (!self) return kIOReturnBadArgument;
        const uint32_t goal = (uint32_t)native_backlight_request[0];
        const uint64_t milliseconds = (uint64_t)native_backlight_request[1];
        self->fade->cancelTimeout();
        self->from = self->level;
        self->to = goal;
        self->start = now();
        nanoseconds_to_absolutetime(milliseconds * NSEC_PER_MSEC, &self->period);
        native_backlight_request[3] = native_backlight_request[3] + 1;
        if (!self->period) {
            IOLockLock(gLock);
            writeBrightness(goal);
            IOLockUnlock(gLock);
            self->level = goal;
            return kIOReturnSuccess;
        }
        fadeStep(self, self->fade);
        return kIOReturnSuccess;
    }
    static IOReturn begin(OSObject *owner, void *, void *, void *, void *) {
        AliothBacklight *self = static_cast<AliothBacklight *>(owner);
        if (!self) return kIOReturnBadArgument;
        IOLockLock(gLock);
        const int32_t read = readBrightness();
        IOLockUnlock(gLock);
        if (read > 0 && (uint32_t)read <= kMaxLevel) {self->level = (uint32_t)read; native_backlight_stats[3] = read;}
        else {self->level = kDefaultLevel; native_backlight_stats[3] = -1;}
        self->publish(self->level);
        // Only the built-in display gets backlight-control, and only with the conformance above:
        // sheng measured (int-236) that publishing it without the AppleARMBacklight conformance
        // stops the libEDR server from starting and freezes the screen.
        if (self->impersonate()) {
            self->setProperty("NativeSandboxGrantProcess", "backboardd");
            self->setProperty("backlight-control", true);
        }
        self->registerService();
        native_backlight_stats[0] = 3;
        return kIOReturnSuccess;
    }
public:
    bool startService() {
        loop = IOWorkLoop::workLoop();
        fade = IOTimerEventSource::timerEventSource(this, &AliothBacklight::fadeStep);
        if (!loop || !fade || loop->addEventSource(fade)) return false;
        loop->runAction(&AliothBacklight::begin, this);
        return true;
    }
    IOReturn setProperties(OSObject *properties) override {
        OSDictionary *dictionary = OSDynamicCast(OSDictionary, properties);
        if (!dictionary) return kIOReturnBadArgument;
        OSNumber *brightness = OSDynamicCast(OSNumber, dictionary->getObject("brightness"));
        if (!brightness) return kIOReturnSuccess;
        OSNumber *fadePeriod = OSDynamicCast(OSNumber, dictionary->getObject("FadePeriod"));
        const int32_t requested = (int32_t)brightness->unsigned32BitValue();
        const uint32_t goal = requested < 0 ? 0 : (uint32_t)requested > kMaxLevel ? kMaxLevel : (uint32_t)requested;
        uint64_t milliseconds = fadePeriod ? ((uint64_t)fadePeriod->unsigned32BitValue() * 1000) >> 16 : 0;
        if (milliseconds > 5000) milliseconds = 5000;
        bump(1);
        native_backlight_request[0] = (int32_t)goal;
        native_backlight_request[1] = (int32_t)milliseconds;
        native_backlight_request[2] = native_backlight_request[2] + 1;
        // Handed to the work loop without waiting for it: a runAction here would block
        // backboardd's brightness thread on the work loop's gate (sheng int-277 measured 53
        // requests in the first 41 s and none after).
        loop->runAction(&AliothBacklight::takeRequest, this, nullptr, nullptr, nullptr);
        publish(goal);
        return kIOReturnSuccess;
    }
};
OSDefineMetaClassAndStructors(AliothBacklight, IOService)

extern kmod_info_t g_kernel_kmod_info;
// The service's static initializer leaves __mod_init_func so the board probe can run it between
// preModLoad and postModLoad, the same staging build-gop-legacy-display.py applies to
// NativeLegacyDisplay.cpp. The symbol is named after this file.
extern "C" void alioth_backlight_static_init(void) __asm__("__GLOBAL__sub_I_BacklightProbe.cpp");

namespace {
unsigned gMode;
uint32_t gLevel = kDefaultLevel;

void panelThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "alioth-bl");
    // Mode 1 is read-only: snapshot is the whole job, and the thread returns without writing.
    if (gMode == 1) return;
    // The visible test, then a read-back of what the panel now holds.
    IOLockLock(gLock);
    writeBrightness(kMaxLevel);
    IOLockUnlock(gLock);
    IOSleep(2000);
    IOLockLock(gLock);
    writeBrightness(0x40);
    IOLockUnlock(gLock);
    IOSleep(2000);
    IOLockLock(gLock);
    writeBrightness(gLevel);
    const int32_t back = readBrightness();
    IOLockUnlock(gLock);
    native_backlight_stats[4] = back;
    native_backlight_stats[0] = 2;
    if (gMode == 2) return;
    // The service.
    void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
    if (!handle) {fail(5, -12); return;}
    alioth_backlight_static_init();
    const OSReturn status = OSMetaClass::postModLoad(handle);
    if (status) {fail(5, status); return;}
    AliothBacklight *backlight = OSTypeAlloc(AliothBacklight);
    if (!backlight || !backlight->init() || !backlight->attach(IOService::getPlatform()) || !backlight->startService()) {
        if (backlight) backlight->release();
        fail(6, -19);
        return;
    }
    backlight->release();
}
}  // namespace

extern "C" void alioth_backlight_probe(void) {
    unsigned mode = 0;
    if (!PE_parse_boot_argn("alioth-bl", &mode, sizeof mode) || !mode || mode > 3 || native_backlight_stats[0]) return;
    PE_parse_boot_argn("alioth-bl-level", &gLevel, sizeof gLevel);
    if (gLevel > kMaxLevel) gLevel = kMaxLevel;
    gLock = IOLockAlloc();
    IODeviceMemory *memory = IODeviceMemory::withRange(kDsi0, kDsiBytes);
    gMap = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    gDsi = gMap ? reinterpret_cast<volatile uint32_t *>(gMap->getVirtualAddress()) : nullptr;
    if (!gLock || !gDsi) {gDsi = nullptr; fail(1, -12); return;}
    snapshot();
    native_backlight_stats[0] = 1;
    gMode = mode;
    thread_t thread = nullptr;
    if (kernel_thread_start(panelThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else fail(4, -12);
}
