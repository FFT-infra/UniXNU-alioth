// SPDX-License-Identifier: BSD-2-Clause-Patent
// K40 (alioth, SM8250 / kona) FocalTech touch board constants. Addresses follow the
// sm8250 GCC / TLMM maps and the alioth device tree (touch on QUPv3 wrapper 0 SE4,
// SPI clock 6 MHz, IRQ GPIO 39, reset GPIO 101, SPI pins GPIO 8/9/10 function qup4).
#ifndef FOCAL_TOUCH_BOARD_H
#define FOCAL_TOUCH_BOARD_H

#include <stdint.h>

// QUPv3 wrapper 0: serial engine 4 is the touch SPI controller (0x980000 + 4*0x4000).
#define TB_QUP_WRAPPER 0x9c0000u
#define TB_SE4_BASE 0x990000u
#define TB_SE4_SIZE 0x4000u

// GCC.
#define TB_GCC_BASE 0x100000u
// Shared clock-enable vote register for QUPv3 wrapper 0.
#define TB_GCC_VOTE_ENABLE 0x52008u
// QUPv3 wrapper 0 branch bits (enable, low-active halt registers live in GCC).
#define TB_VOTE_M_AHB (1u << 6)
#define TB_VOTE_S_AHB (1u << 7)
#define TB_VOTE_CORE (1u << 8)
#define TB_VOTE_CORE_2X (1u << 9)
#define TB_VOTE_SE4 (1u << 14)
#define TB_VOTE_ALL (TB_VOTE_M_AHB | TB_VOTE_S_AHB | TB_VOTE_CORE | TB_VOTE_CORE_2X | TB_VOTE_SE4)
// Branch halt registers (CBCR, CLK_OFF = bit 0; hardware clock gating = bit 1).
#define TB_HALT_M_AHB 0x17004u
#define TB_HALT_S_AHB 0x17008u
#define TB_HALT_CORE 0x23000u
#define TB_HALT_CORE_2X 0x23008u
#define TB_HALT_SE4 0x174ccu
// SE4 clock source RCG command register; DFS configuration at +0x14 (perf cfg/m/n
// stride 0x4; the driver programs level 0, TCXO 19.2 MHz = 6 MHz SPI /2).
#define TB_SE4_RCG 0x174d0u
#define TB_RCG_DFSR_CMD 0x14u
#define TB_RCG_CFG 0x4u
#define TB_RCG_PERF_CFG(l) (TB_RCG_DFSR_CMD + 0x8 + (l)*0x4u)
#define TB_RCG_PERF_M(l) (TB_RCG_DFSR_CMD + 0x48 + (l)*0x4u)
#define TB_RCG_PERF_N(l) (TB_RCG_DFSR_CMD + 0x88 + (l)*0x4u)
#define TB_GCC_GPLL0_MODE (0x52000u)
#define TB_GCC_CBCR_CLK_OFF (1u << 0)

// TLMM. The controller's device-tree reg is <0xf000000 0x1000000>, but that 16 MiB range is
// divided into per-tile register windows and a pin answers only through its own tile. The K40
// kernel's own driver states the offsets (reference/k40-kernel/drivers/pinctrl/qcom/
// pinctrl-kona.c:20-21):
//     #define NORTH 0x00900000
//     #define SOUTH 0x00500000
// and assigns each pin to one of them (PINGROUP entries at :1445-1495): gpio8, 9 and 10 are
// NORTH, gpio33 and gpio39 are SOUTH. A pin's registers are at tile + 0x1000 * pin.
//
// Measured 2026-10-27: addressing 0xf000000 + 0x1000 * pin directly, or 0x0f900000 alone,
// returns 0 for every read and swallows every write -- which is exactly what a pin whose
// registers are never reached looks like. Both windows are mapped, each 256 KiB, which covers
// the highest offset used here (gpio39 at 0x27000) and stays inside the 8 MiB io_submap.
#define TB_TLMM_BASE 0x0f000000u
#define TB_TLMM_NORTH_OFFSET 0x00900000u
#define TB_TLMM_SOUTH_OFFSET 0x00500000u
#define TB_TLMM_TILE_SIZE 0x40000u
// Pins at or below this number live in the north tile, the rest in the south. That boundary
// matches every pin this driver touches; the K40 table is not strictly monotonic (gpio0-3 are
// SOUTH), so a pin outside this set would need its own entry rather than this rule.
#define TB_TLMM_PIN_SPLIT 11u
#define TB_GPIO_STRIDE 0x1000u
#define TB_GPIO_SPI_FIRST 8u                    // MISO
#define TB_GPIO_SPI_MOSI 9u
#define TB_GPIO_SPI_CLK 10u
/* Chip select. The GENI engine drives it, so this is not a software GPIO: the pin only has to
 * be muxed to qup4 so the engine's CS reaches it. The device tree's SPI node takes two pin
 * groups, pinctrl-0 = <0x29a 0x6e6>: 0x29a is qupv3_se4_spi_active (gpio8/9/10) and 0x6e6 is
 * ts_cs_active, which is gpio11 with function "qup4", drive-strength 16 mA, bias-pull-up.
 * Leaving it unmuxed was why every read returned an all-zero frame: the slave was never
 * selected. */
#define TB_GPIO_CS 11u
#define TB_GPIO_IRQ 39u
#define TB_GPIO_RESET 33u
// TLMM GPIO_CFG fields. The layout is [1:0] pull, [5:2] function, [8:6] drive, [9] output;
// this is the vendor and mainline layout, and it is the one the ios-sweet port writes on
// this phone (sweet_touch.c:238-247). The previous constants here used function 0..3, pull at
// bit 12 and output at bit 15, which no TLMM register has: every pin write landed in the wrong
// bits, so the SPI pins were never muxed and CS was never driven.
#define TB_CFG_PULL_MASK 0x3u
#define TB_CFG_PULL_NONE 0u
#define TB_CFG_PULL_UP 3u
#define TB_CFG_FUNC_SHIFT 2u
#define TB_CFG_FUNC_SPI 1u                      // "qup4" is function 1, not 0
#define TB_CFG_DRIVE_SHIFT 6u
#define TB_CFG_DRIVE_MA2 0u                     // (ma / 2 - 1): 0=2 mA, 1=4, 2=6, 3=8
#define TB_CFG_DRIVE_MA8 3u
#define TB_CFG_DRIVE_MA16 7u                    // (16 / 2) - 1: the CS pin per the DT
#define TB_CFG_OE (1u << 9)                     // output enable, bit 9
// GPIO_IN_OUT is the CFG register + 4: bit 0 reads the pin, bit 1 drives it.
// Writing 2 drives high, writing 0 drives low; the previous duplicate block here declared
// TB_IO_IN twice, so the C++ file saw whichever definition came last.
#define TB_IO_OUT (1u << 1)
#define TB_IO_HIGH (1u << 1)

// Touch frame loop.
#define TB_FRAME_PERIOD_US 6000u                // ~160 Hz polling (panel IRQ is not wired)
// Health poll cadence: the power-mode and identity registers are read every this many frames
// (about once a minute at the frame period), which is what decides whether the part needs the
// recovery sequence.
#define TB_REG_POLL_FRAMES 600u
#define TB_RESET_ASSERT_US 1200u
#define TB_RESET_RELEASE_US 9000u
// Rail settle time while reset is held low: the vendor driver and the ios-sweet port both wait
// 3 ms then 15 ms after the supplies come up and before the chip is released.
#define TB_POWER_SETTLE_US 18000u

// SPI config (from the alioth device tree: spi-max-frequency 6000000).
#define TB_SPI_HZ 6000000u
#define TB_SPI_SCLK_HZ 19200000u                // TCXO, DFS level 0
#define TB_SPI_CLK_SEL 0u
#define TB_SPI_MODE 0u
#define TB_SPI_CS 0u

// A pin's CFG register: its tile, then 0x1000 per pin number.
static inline uint32_t tb_tlmm_ctl(unsigned gpio) {
    const uint32_t tile = TB_TLMM_BASE +
        (gpio <= TB_TLMM_PIN_SPLIT ? TB_TLMM_NORTH_OFFSET : TB_TLMM_SOUTH_OFFSET);
    return tile + gpio * TB_GPIO_STRIDE;
}

#endif