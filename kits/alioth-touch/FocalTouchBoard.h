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

// TLMM: GPIO CFG/IN_OUT stride 0x1000.
#define TB_TLMM_BASE 0xf000000u
#define TB_GPIO_STRIDE 0x1000u
#define TB_GPIO_SPI_FIRST 8u                    // MISO
#define TB_GPIO_SPI_MOSI 9u
#define TB_GPIO_SPI_CLK 10u
#define TB_GPIO_IRQ 39u
#define TB_GPIO_RESET 101u
// TLMM GPIO_CFG fields.
#define TB_CFG_FUNC_MASK 0xfu
#define TB_CFG_DRIVE_SHIFT 6u
#define TB_CFG_DRIVE_MA6 (2u << TB_CFG_DRIVE_SHIFT)   // 6 mA: 0=2,1=4,2=6,3=8,...
#define TB_CFG_PULL_SHIFT 12u
#define TB_CFG_PULL_NONE (0u << TB_CFG_PULL_SHIFT)
#define TB_CFG_PULL_UP (2u << TB_CFG_PULL_SHIFT)
#define TB_CFG_OE (1u << 15)
#define TB_CFG_FUNC_QUP4 0u                     // gpio8/9/10 first function is qup4

// GPIO_IN_OUT.
#define TB_IO_IN 0x0u
#define TB_IO_OUT (1u << 1)

// Touch frame loop.
#define TB_FRAME_PERIOD_US 6000u                // ~160 Hz polling (panel IRQ is not wired)
#define TB_RESET_ASSERT_US 1200u
#define TB_RESET_RELEASE_US 9000u

// SPI config (from the alioth device tree: spi-max-frequency 6000000).
#define TB_SPI_HZ 6000000u
#define TB_SPI_SCLK_HZ 19200000u                // TCXO, DFS level 0
#define TB_SPI_CLK_SEL 0u
#define TB_SPI_MODE 0u
#define TB_SPI_CS 0u

static inline uint32_t tb_tlmm_ctl(unsigned gpio) {return TB_TLMM_BASE + gpio * TB_GPIO_STRIDE;}

#endif