// SPDX-License-Identifier: BSD-2-Clause-Patent
// What the boot loader left in the display controller, read once at display start (boot argument native-dpu=1).
//
// This is the board's own version of UniXNU-sheng's DisplayTimingProbe.cpp, reduced to the four questions the direct scanout path
// needs answered before it may flip anything:
//   1. Which SSPP bases are live? The SM8550 offsets (0x4000/0x6000) and the SM8250 ones (0x5000/0x7000) both map inside the MDP
//      block, so writing the wrong pair lands on unrelated registers without faulting. Reading the source address the firmware
//      left tells them apart: the live pipes carry the boot framebuffer's address, the dead ones read 0.
//   2. What source format is the firmware scanning? A BGRA scanout needs SRC_FORMAT to say so; the direct path only rewrites the
//      address and stride, so every frame it flips inherits this value. Reading it is the only way to tell a correct picture from
//      a channel-swapped one on a board whose screenshot channel returns ENXIO.
//   3. What stride and size did the firmware program? kBootStride in NativeLegacyDisplay was a guess until this row existed.
//   4. Which CTL is flushing the live pipes, and with which bits?
//
// The register layout inside a pipe is the same on every DPU generation (vendor dpu_hw_sspp.c: SRC_SIZE 0x00, SRC0_ADDR 0x14,
// SRC_YSTRIDE0 0x24, SRC_FORMAT 0x30, SRC_UNPACK_PATTERN 0x34); only the pipe base differs, which is why this probe sweeps both
// candidate bases instead of trusting one.
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IODeviceMemory.h>
#include <pexpert/pexpert.h>

// Layout matches BootLog.c's readers exactly: both arrays are uint32_t there and in BootProgressStubs.c. They were uint64_t in the
// stub and uint32_t in the reader, so every index past zero read the low half of a different element -- the same mismatch the HID
// counters had. One probe fills them now, so the stub is gone and the two sides cannot drift again.
extern "C" {
volatile uint32_t native_dpu_stats[20] = {};
volatile uint32_t native_dpu_pipes[54] = {};
}

namespace {

// The MDP block, mapped from `qcom,sde-off` (0xae00000 + 0x1000). kona-sde.dtsi:110 puts sspp_0 at 0x5000 from the
// node, which is 0x4000 from here -- and that is exactly where the live pipe answered (src0 0x9c000000).
constexpr uint64_t kMdp = 0xae01000;
// kona-sde.dtsi:6 declares 0xae00000 + 0x84208, so from 0xae01000 only 0x83208 bytes remain. 0x8f000 (sheng's block size)
// ran 44 KiB past the declared region and 0x84208 ran 4 KiB past it; 0x83208 is what the node actually covers.
constexpr uint64_t kMdpBytes = 0x84208 - 0x1000;
// Catalog pipe bases, all measured from the mdss_mdp node. dpu_hw_catalog.c:200-203 gives sspp_0 0x4000 and sspp_1 0x6000;
// the 0x5000/0x7000 pair is what kona-sde.dtsi's `qcom,sde-sspp-off` lists, and the probe keeps both so a boot says which one
// the live pipe answers at.
constexpr uint32_t kSspCandidates[4] = {0x4000, 0x6000, 0x3000, 0x5000};
constexpr uint32_t kSspSrcSize = 0x00, kSspSrc0 = 0x14, kSspYstride0 = 0x24, kSspFormat = 0x30, kSspUnpack = 0x34;
// The two registers that decide whether rewriting SRC0_ADDR is enough (dpu_hw_sspp.c:21-26,64-66 in Linux mainline):
//   SSPP_SRC1_ADDR (0x18)      the second plane address of a SOLO pipe, or RECT_1's source in a multirect pipe
//   SSPP_MULTIRECT_OPMODE (0x170)  bit1 RECT_1, bit0 RECT_0, bit2 TIME_MX; 0 means the pipe is SOLO
// dpu_hw_sspp_setup_sourceaddress writes SRC0..SRC3 for SOLO and SRC0/SRC2 (RECT_0) or SRC1/SRC3 (RECT_1) for multirect, and
// dpu_hw_setup_multirect_impl clears the opmode for SOLO. Our flip writes SRC0_ADDR only, so a pipe the firmware left in multirect
// would keep scanning its other rect from the old address -- which is exactly the shape of "flips counts up, the frame buffer
// checksum never changes". kona-sde.dtsi:157 sets `qcom,sde-has-src-split`, so the capability is there; only a read says whether
// the boot loader used it.
constexpr uint32_t kSspSrc1 = 0x18, kSspMultirect = 0x170;
// CTL 0 and CTL 1. kona-sde.dtsi:46 gives `qcom,sde-ctl-off = <0x2000 0x2200 ...>` from the node, so from 0xae01000 they are
// 0x1000 and 0x1200. The second entry was 0x15000 (sheng's SM8550 value) until 2026-11-02; no DPU block is listed there on this
// SoC, which is why every "the write hangs" experiment that used it measured a store to an address with no register.
constexpr uint32_t kCtlCandidates[2] = {0x1000, 0x1200};
constexpr uint32_t kCtlTop = 0x14;      // CTL_TOP: which layer mixers this CTL drives (techpack sde_hw_ctl.c:21)
constexpr uint32_t kCtlFlush = 0x18;
constexpr uint32_t kMdpHardwareVersion = 0x00;
// The display clock controller (kona.dtsi:2523 `qcom,dispcc@af00000`). The MDP block cannot be written while its clocks are off:
// every store hangs the bus. These two CBCRs are the gate -- disp_cc_mdss_ahb_clk (enable_reg 0x2080) and disp_cc_mdss_mdp_clk
// (enable_reg 0x200c) in drivers/clk/qcom/dispcc-kona.c:857,1235. Bit 0 is CLK_ENABLE and bit 1 is CLK_OFF, so a set bit 1 means
// the clock is gated. Read-only: this probe never enables them.
constexpr uint64_t kDispcc = 0xaf00000;
constexpr uint64_t kDispccBytes = 0x20000;
constexpr uint32_t kDispAhbCbcr = 0x2080, kDispMdpCbcr = 0x200c;
// mdss_core_gdsc sits at 0xaf03000, inside this same mapping. GDSCR is its first word.
constexpr uint32_t kGdscr = 0x3000;

// native_dpu_pipes, four words per pipe, in the order BootLog.c prints them (size / address / stride / format):
//   [0..3]   SSPP 0x5000   [4..7]   SSPP 0x7000   [8..11]  SSPP 0x4000   [12..15] SSPP 0x6000
//   [16..19] unpack pattern of each of the four, in the same order
//   [40..41] the boot framebuffer's physical address, low then high
//   [42..47] CTL 0x2000 flush / CTL 0x15000 flush / the matching CTL's layer-0 word
void dpuProbeThread(void *, wait_result_t) {
    thread_set_thread_name(current_thread(), "alioth-dpu");
    // No delay. The firmware's values are what this reads, so waiting for the render server would only let the display driver
    // overwrite them first; and a 30 s sleep made the row read st=2 (started, not finished) on any capture taken earlier than
    // that -- fix83's log was read at 28 s and showed every register as zero for that reason alone. Reading at start also
    // means the probe never races the driver it is meant to inform.
    // The clock gate, as a separate read. A set bit 1 there names the reason every MDP store hangs, which is what fix104 and
    // fix105 measured: writing the window address, the GOP address and the value already in the register all hung, while
    // skipping the write completed. dpu stats [18] ahb cbcr, [19] mdp cbcr.
    IODeviceMemory *ccMemory = IODeviceMemory::withRange(kDispcc, kDispccBytes);
    IOMemoryMap *ccMap = ccMemory ? ccMemory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (ccMemory) ccMemory->release();
    if (ccMap) {
        const volatile uint32_t *cc = reinterpret_cast<const volatile uint32_t *>(ccMap->getVirtualAddress());
        native_dpu_stats[18] = cc[kDispAhbCbcr / 4];
        native_dpu_stats[19] = cc[kDispMdpCbcr / 4];
        // mdss_core_gdsc (kona.dtsi:2765 `qcom,gdsc@af03000`), the power domain the MDP block sits in. GDSCR bit 31 is PWR_ON and
        // bit 0 is SW_COLLAPSE (drivers/clk/qcom/gdsc.c:26,32): a clear bit 31 with a set bit 0 means the domain is collapsed, and
        // every register access inside it would stall the bus. The clocks being on says nothing about this -- they are separate.
        native_dpu_stats[2] = cc[kGdscr / 4];
        ccMap->release();
    }
    IODeviceMemory *memory = IODeviceMemory::withRange(kMdp, kMdpBytes);
    IOMemoryMap *map = memory ? memory->map(kIOMapAnywhere | kIOMapInhibitCache) : nullptr;
    if (memory) memory->release();
    if (!map) { native_dpu_stats[0] = uint32_t(-1); return; }
    const volatile uint32_t *mdp = reinterpret_cast<const volatile uint32_t *>(map->getVirtualAddress());
    auto rd = [&](uint32_t offset) { return mdp[offset / 4]; };

    native_dpu_stats[1] = rd(kMdpHardwareVersion);
    for (unsigned i = 0; i < 4; ++i) {
        const uint32_t base = kSspCandidates[i];
        native_dpu_pipes[4 * i] = rd(base + kSspSrcSize);
        native_dpu_pipes[4 * i + 1] = rd(base + kSspSrc0);
        native_dpu_pipes[4 * i + 2] = rd(base + kSspYstride0);
        native_dpu_pipes[4 * i + 3] = rd(base + kSspFormat);
        native_dpu_pipes[16 + i] = rd(base + kSspUnpack);
        // A SOLO pipe's second plane address, and the multirect opmode. A non-zero opmode on the live pipe says the flip's single
        // SRC0_ADDR store leaves the other rect on the old frame.
        native_dpu_pipes[20 + i] = rd(base + kSspSrc1);
        native_dpu_pipes[24 + i] = rd(base + kSspMultirect);
    }
    PE_Video video = {};
    if (!PE_current_console(&video)) {
        native_dpu_pipes[40] = uint32_t(video.v_baseAddr);
        native_dpu_pipes[41] = uint32_t(uint64_t(video.v_baseAddr) >> 32);
    }
    // Every CTL of both candidate banks. The flush register clears itself once a flush latches, so a zero there means "nothing
    // pending", not "wrong base"; the layer-0 word is the one that stays programmed, so that is what identifies the live CTL.
    // CTL_TOP (0x14) names the layer mixers the CTL is driving, so a non-zero CTL_TOP identifies the live CTL; CTL_FLUSH (0x18)
    // clears itself once a flush latches, so a zero there means "nothing pending", not "wrong base".
    for (unsigned i = 0; i < 2; ++i) {
        const uint32_t base = kCtlCandidates[i];
        native_dpu_pipes[42 + 2 * i] = rd(base + kCtlFlush);
        native_dpu_pipes[43 + 2 * i] = rd(base + kCtlTop);
    }
    // CTL 0..5 at the SM8250 bank (ctl-off 0x2000 + 0x200 * n, kona-sde.dtsi:46) into the unused tail of the row.
    for (unsigned i = 0; i < 6; ++i) {
        const uint32_t base = kCtlCandidates[0] + 0x200 * i;
        native_dpu_stats[12 + i] = rd(base + kCtlTop);
        native_dpu_stats[6 + i] = rd(base + kCtlFlush);
    }
    native_dpu_stats[0] = 1;
    map->release();
}

}  // namespace

// Called from boot_log_tick once per second with the boot's uptime. The read happens on the first tick at or after
// kProbeAtSeconds: early enough that the firmware's values are still in the registers, late enough that the display controller
// has been powered and the MDP answers reads. Reading at display start instead (the first version) returned every register as
// zero, because at that point the block is still unclocked and reads return nothing.
// 0: the display driver calls this from start(), when the firmware's values are still the ones in the registers. The
// original 20 s waited for the MDP block to be clocked, but the block is clocked from boot (dispcc ahb=1 mdp=221 measured
// while the panel was still showing the boot frame), so waiting only risked reading a driver-written value instead.
constexpr unsigned kProbeAtSeconds = 0;
static volatile unsigned probe_done;

extern "C" void alioth_dpu_probe_tick(unsigned now) {
    // Opt-in again (native-dpu=1). It was made default-on to save a boot-argument byte, and fix114 then stopped at
    // BLPHASE t=1s while fix107 (probe off) booted to 178 s -- so the probe is back behind its flag until that is understood.
    unsigned enabled = 0;
    PE_parse_boot_argn("native-dpu", &enabled, sizeof enabled);
    if (enabled != 1) return;
    if (probe_done || now < kProbeAtSeconds) return;
    probe_done = 1;
    native_dpu_stats[0] = 2;   // claimed: the row reads st=2 until the read below finishes
    thread_t thread = nullptr;
    if (kernel_thread_start(dpuProbeThread, nullptr, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    else native_dpu_stats[0] = uint32_t(-7);
}
