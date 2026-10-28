// SPDX-License-Identifier: BSD-2-Clause-Patent
// pipa (SM8250, Adreno 650) GPU power and clock bring-up up to a GMU that can be started: board facts plus the staged, gated
// sequence, after UniXNU-sheng's GpuPowerBoard.h (SM8550, Adreno 740). Every offset names its source: DTS = the tablet's device
// tree (uefi-pipa Resources/DTBs/pipa.dts, which agrees with Linux v6.6 sm8250.dtsi for every address used here), gcc / gpucc /
// gdsc / rcg2 / cmd-db = Linux v6.6 gcc-sm8250.c, gpucc-sm8250.c, gdsc.c, clk-rcg2.c, cmd-db.c, a6xx_gmu = a6xx_gmu.c and
// a6xx_gmu.xml.h. Nothing here touches hardware; GpuProbe.cpp owns the MMIO. Host-tested by GpuPowerBoardTests.c.
//
// Why staged: the GPU SMMU, GPUCC and the GMU and GPU register windows sit behind clocks and power domains that Darwin has no
// driver for. Reading a block whose clock or rail is off can hang the bus (a reset) instead of failing, so each stage runs only
// if the previous one showed its prerequisite on, and one round learns the whole chain from the log.
#ifndef PIPA_GPU_POWER_BOARD_H
#define PIPA_GPU_POWER_BOARD_H
#include <stdint.h>
#include <stddef.h>

// DTS: qcom,kgsl-3d0@3d00000 reg 0x3d00000 size 0x40000 (interrupt SPI 300, chipid 0x06050002); qcom,gmu@3d6a000 size 0x30000
// with PDC 0xb290000 and PDC sequence 0xb490000 (both 0x10000); the RSCC 0x3de0000 size 0x10000 (kgsl-3d0's "rscc"; Linux's
// gmu "rscc" for the a650 family); GPUCC clock-controller@3d90000 size 0x9000; kgsl-smmu@3da0000 size 0x10000; GCC 0x100000.
#define GPB_GCC_GPU_PAGE 0x00171000ULL    // GCC + 0x71000: the GPU branches
#define GPB_GCC_VOTE_PAGE 0x00152000ULL   // GCC + 0x52000: the GPLL0 branch votes
#define GPB_GPUCC_BASE 0x03d90000ULL
#define GPB_GPUCC_SIZE 0x9000ULL
#define GPB_SMMU_BASE 0x03da0000ULL
#define GPB_SMMU_GR0_SIZE 0x1000ULL       // global space 0 only
#define GPB_GPU_BASE 0x03d00000ULL
#define GPB_GPU_WINDOW 0x40000ULL         // the whole register space (DTS kgsl_3d0_reg_memory); stage 1 reads RBBM_STATUS only
#define GPB_GMU_BASE 0x03d6a000ULL
#define GPB_GMU_SIZE 0x30000ULL
#define GPB_RSCC_BASE 0x03de0000ULL
#define GPB_RSCC_SIZE 0x10000ULL

// gcc: gcc_gpu_cfg_ahb_clk 0x71004 is enabled by the gcc-sm8250 probe (regmap_update_bits 0x71004 BIT(0)); gcc_gpu_memnoc_gfx_clk
// 0x7100c (voted); gcc_gpu_snoc_dvm_gfx_clk 0x71018; gcc_ddrss_gpu_axi_clk 0x71154 (voted); the probe also sets 0x71028 bits 1:0
// ("disable the GPLL0 active input to ... GPU via MISC registers"); gcc_gpu_gpll0_clk_src / _div_clk_src are the votes 0x52000
// bits 15 / 16 (BRANCH_HALT_DELAY). Offsets below are relative to the mapped page.
#define GPB_GCC_GPU_CFG_AHB 0x004
#define GPB_GCC_GPU_MEMNOC_GFX 0x00c
#define GPB_GCC_GPU_SNOC_DVM_GFX 0x018
#define GPB_GCC_GPU_MISC 0x028
#define GPB_GCC_DDRSS_GPU_AXI 0x154
#define GPB_GCC_PLL_VOTE 0x000
#define GPB_GPLL0_VOTES ((1u << 15) | (1u << 16))
#define GPB_CBCR_CLK_OFF (1u << 31)       // clk-branch.c BRANCH_CLK_OFF; a running branch reads 0
#define GPB_CBCR_ENABLE (1u << 0)

// gpucc: gpu_cx_gdsc gdscr 0x106c, gds_hw_ctrl 0x1540, flags VOTABLE (no RETAIN_FF_ENABLE, no cxcs); gpu_gx_gdsc 0x100c (the GMU
// owns it); gpu_cc_ahb_clk 0x1078 (BRANCH_HALT_DELAY); gpu_cc_cx_gmu_clk 0x1098; gpu_cc_cxo_clk 0x109c; gpu_cc_cxo_aon_clk 0x1004;
// gpu_cc_hlos1_vote_gpu_smmu_clk 0x5000 (BRANCH_VOTED); gpu_cc_gmu_clk_src cmd_rcgr 0x1120 (hid 5, parent_map_0: 0 XO, 3 PLL1,
// 5 GPLL0, 6 GPLL0 div), 200 MHz = GPLL0 div / 1.5. The probe writes 0xf to the wake (11:8) and sleep (7:4) fields of 0x1098.
// gdsc.c: PWR_ON_MASK BIT(31), SW_COLLAPSE_MASK BIT(0); a VOTABLE gdsc with gds_hw_ctrl waits 1 us, then polls gds_hw_ctrl.
#define GPB_CC_CX_GDSCR 0x106c
#define GPB_CC_GX_GDSCR 0x100c
#define GPB_CC_CX_GDS_HW_CTRL 0x1540
#define GPB_CC_AHB_CLK 0x1078
#define GPB_CC_CX_GMU_CLK 0x1098
#define GPB_CC_CXO_CLK 0x109c
#define GPB_CC_CXO_AON_CLK 0x1004
#define GPB_CC_SMMU_VOTE_CLK 0x5000
#define GPB_CC_GMU_RCGR 0x1120
#define GPB_CC_PLL1_MODE 0x100            // gpu_cc_pll1 (Lucid); only read: the GMU at 200 MHz does not use it
#define GPB_CX_GMU_WAKE_SLEEP (0xffu << 4)
#define GPB_GDSC_POLL_US 2000             // gdsc.c STATUS_POLL_TIMEOUT_US 1500; a little longer here
#define GPB_GDSC_PWR_ON (1u << 31)
#define GPB_GDSC_SW_COLLAPSE (1u << 0)

// ARM SMMUv2 global space 0 (IHI 0062D.c): sCR0 0x0, ID0 0x20, ID1 0x24, ID2 0x28, sGFSR 0x48; SMRn 0x800, S2CRn 0xc00.
#define GPB_SMMU_SCR0 0x0
#define GPB_SMMU_ID0 0x20
#define GPB_SMMU_ID1 0x24
#define GPB_SMMU_ID2 0x28
#define GPB_SMMU_GFSR 0x48
#define GPB_SMMU_SMR(n) (0x800 + 4 * (n))
#define GPB_SMMU_S2CR(n) (0xc00 + 4 * (n))
#define GPB_SMMU_MAX_SMR 128

// a6xx.xml: RBBM_STATUS dword 0x0210. a6xx_gmu.xml.h: dword offsets in the GMU block (gmu_read: mmio + 4 * offset) and in the
// RSCC block (gmu_read_rscc).
#define GPB_RBBM_STATUS_BYTES (0x0210 * 4)
#define GPB_GMU_REG(dword) ((dword) * 4u)
#define GPB_GMU_CM3_SYSRESET GPB_GMU_REG(0x5000)
#define GPB_GMU_CM3_FW_INIT_RESULT GPB_GMU_REG(0x501c)
#define GPB_GMU_SPTPRAC_PWR_CLK_STATUS GPB_GMU_REG(0x50d0)
#define GPB_GMU_HFI_CTRL_STATUS GPB_GMU_REG(0x5180)
#define GPB_GMU_GENERAL_7 GPB_GMU_REG(0x51cc)
#define GPB_GMU_AO_HOST_INTERRUPT_STATUS GPB_GMU_REG(0x9305)
#define GPB_GMU_AHB_FENCE_STATUS GPB_GMU_REG(0x9313)
#define GPB_RSCC_SEQ_BUSY_DRV0 GPB_GMU_REG(0x101)
#define GPB_RSCC_RSC_STATUS0_DRV0 GPB_GMU_REG(0x004)

// Stages. A stage records "skipped" with a reason instead of reading when its prerequisite is not met.
enum gpb_state {GPB_NOT_RUN = 0, GPB_DONE = 1, GPB_SKIPPED_PREREQ = 2, GPB_SKIPPED_MODE = 3, GPB_NOMAP = 4};

struct gpb_io {
    void *cookie;
    int (*read32)(void *cookie, unsigned window, uint32_t offset, uint32_t *out);   // 0 on success
    int (*write32)(void *cookie, unsigned window, uint32_t offset, uint32_t value);
    void (*delay_us)(void *cookie, unsigned us);                                     // may be NULL (host tests)
};
enum {GPB_WIN_GCC_GPU = 0, GPB_WIN_GCC_VOTE = 1, GPB_WIN_GPUCC = 2, GPB_WIN_SMMU = 3, GPB_WIN_GPU = 4, GPB_WIN_GMU = 5,
      GPB_WIN_RSCC = 6, GPB_WINDOWS = 7};

struct gpb_result {
    unsigned mode;                       // pipa-gpu=<mode>: 1 read only, 2 + cfg AHB, 3 + CX power, 5 + cmd-db, 6 + GMU clocks
    enum gpb_state gcc, cc, smmu, gpu;
    uint32_t cfg_ahb, cfg_ahb_after, memnoc, dvm, misc, pll_vote;
    uint32_t cx_gdscr, gx_gdscr, cc_ahb, cc_smmu, cc_gmu;
    uint32_t scr0, id0, id1, id2, gfsr;
    uint32_t smr_valid, smr_first, smr_first_val, s2cr_first_val;
    uint32_t rbbm_status;
    uint32_t cx_before, cx_hwctrl, cc_smmu_before;
    unsigned powered_cx, powered_clocks;
    unsigned enabled_ahb;
    int io_error;
};

static inline int gpb_clock_on(uint32_t cbcr) {return (cbcr & GPB_CBCR_CLK_OFF) == 0;}
// gcc_gpu_cfg_ahb_clk has hardware clock gating (hwcg bit 1 on sheng, the same branch layout here): an idle enabled branch can read
// CLK_OFF, so its enable bit is the test.
static inline int gpb_cfg_ahb_enabled(uint32_t cbcr) {return (cbcr & GPB_CBCR_ENABLE) != 0;}
static inline int gpb_gdsc_on(uint32_t gdscr) {return (gdscr & GPB_GDSC_PWR_ON) != 0;}
static inline unsigned gpb_smmu_num_smr(uint32_t id0) {return id0 & 0xff;}

static inline int gpb_rd(const struct gpb_io *io, struct gpb_result *r, unsigned window, uint32_t offset, uint32_t *out) {
    uint32_t v = 0;
    if (io->read32(io->cookie, window, offset, &v)) {r->io_error = 1; *out = 0; return -1;}
    *out = v;
    return 0;
}

// Modes 1 to 3: GCC, GPUCC (and the CX power domain with the SMMU's clocks from mode 3), the GPU SMMU's global space 0 and
// RBBM_STATUS when GX is on. `have` is a bitmask of mapped windows (1 << GPB_WIN_*); a missing window makes its stage GPB_NOMAP.
static inline void gpb_run(const struct gpb_io *io, unsigned mode, unsigned have, struct gpb_result *r) {
    __builtin_memset(r, 0, sizeof *r);
    r->mode = mode;
    if (!mode || !(have & (1u << GPB_WIN_GCC_GPU))) {r->gcc = (mode ? GPB_NOMAP : GPB_SKIPPED_MODE); return;}
    if (gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_CFG_AHB, &r->cfg_ahb)) return;
    gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_MEMNOC_GFX, &r->memnoc);
    gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_SNOC_DVM_GFX, &r->dvm);
    gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_MISC, &r->misc);
    if (have & (1u << GPB_WIN_GCC_VOTE)) gpb_rd(io, r, GPB_WIN_GCC_VOTE, GPB_GCC_PLL_VOTE, &r->pll_vote);
    r->cfg_ahb_after = r->cfg_ahb;
    if (!gpb_cfg_ahb_enabled(r->cfg_ahb) && mode >= 2) {
        // The write the gcc-sm8250 probe makes; mode 1 never writes.
        if (!io->write32(io->cookie, GPB_WIN_GCC_GPU, GPB_GCC_GPU_CFG_AHB, r->cfg_ahb | GPB_CBCR_ENABLE)) {
            r->enabled_ahb = 1;
            gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_CFG_AHB, &r->cfg_ahb_after);
        } else r->io_error = 1;
    }
    r->gcc = GPB_DONE;
    // GPUCC needs gcc_gpu_cfg_ahb_clk.
    if (!(have & (1u << GPB_WIN_GPUCC))) r->cc = GPB_NOMAP;
    else if (!gpb_cfg_ahb_enabled(r->cfg_ahb_after)) r->cc = GPB_SKIPPED_PREREQ;
    else {
        gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GDSCR, &r->cx_gdscr);
        gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_GX_GDSCR, &r->gx_gdscr);
        gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_AHB_CLK, &r->cc_ahb);
        gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_SMMU_VOTE_CLK, &r->cc_smmu);
        gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GMU_CLK, &r->cc_gmu);
        r->cc = GPB_DONE;
    }
    // Mode 3: gdsc_enable for gpu_cx_gdsc (VOTABLE with gds_hw_ctrl: clear SW_COLLAPSE, 1 us, poll gds_hw_ctrl PWR_ON), then the
    // clocks the adreno SMMU node lists (gpu_cc_ahb_clk; the HLOS vote of the SMMU clock). GX is never touched (the GMU owns it).
    r->cx_before = r->cx_gdscr; r->cc_smmu_before = r->cc_smmu;
    if (mode >= 3 && r->cc == GPB_DONE) {
        int bad = 0;
        if (!gpb_gdsc_on(r->cx_gdscr)) {
            uint32_t st = 0;
            bad = io->write32(io->cookie, GPB_WIN_GPUCC, GPB_CC_CX_GDSCR, r->cx_gdscr & ~GPB_GDSC_SW_COLLAPSE);
            if (!bad) {
                unsigned waited = 0;
                if (io->delay_us) io->delay_us(io->cookie, 1);
                for (;;) {
                    if (gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GDS_HW_CTRL, &st)) {bad = 1; break;}
                    if (gpb_gdsc_on(st) || waited >= GPB_GDSC_POLL_US) break;
                    if (io->delay_us) io->delay_us(io->cookie, 10);
                    waited += 10;
                }
                r->cx_hwctrl = st;
                gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GDSCR, &r->cx_gdscr);
                r->powered_cx = !bad && gpb_gdsc_on(st);
            }
        } else r->powered_cx = 2;        // already on
        if (bad) r->io_error = 1;
        if (r->powered_cx) {
            uint32_t a = 0, c = 0;
            int cbad = gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_AHB_CLK, &a) | gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_SMMU_VOTE_CLK, &c);
            if (!cbad && !(a & GPB_CBCR_ENABLE)) cbad |= io->write32(io->cookie, GPB_WIN_GPUCC, GPB_CC_AHB_CLK, a | GPB_CBCR_ENABLE);
            if (!cbad && !(c & GPB_CBCR_ENABLE)) cbad |= io->write32(io->cookie, GPB_WIN_GPUCC, GPB_CC_SMMU_VOTE_CLK, c | GPB_CBCR_ENABLE);
            unsigned waited = 0;
            while (!cbad) {
                if (gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_SMMU_VOTE_CLK, &r->cc_smmu)) {cbad = 1; break;}
                if (gpb_clock_on(r->cc_smmu) || waited >= 500) break;
                if (io->delay_us) io->delay_us(io->cookie, 10);
                waited += 10;
            }
            gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_AHB_CLK, &r->cc_ahb);
            r->powered_clocks = !cbad && gpb_clock_on(r->cc_smmu);
            if (cbad) r->io_error = 1;
        }
    }
    // The GPU SMMU needs CX and its vote clock. Global space 0 only.
    if (!(have & (1u << GPB_WIN_SMMU))) r->smmu = GPB_NOMAP;
    else if (r->cc != GPB_DONE || !gpb_gdsc_on(r->cx_gdscr) || !gpb_clock_on(r->cc_smmu)) r->smmu = GPB_SKIPPED_PREREQ;
    else {
        gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_SCR0, &r->scr0);
        gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_ID0, &r->id0);
        gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_ID1, &r->id1);
        gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_ID2, &r->id2);
        gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_GFSR, &r->gfsr);
        unsigned n = gpb_smmu_num_smr(r->id0);
        if (n > GPB_SMMU_MAX_SMR) n = GPB_SMMU_MAX_SMR;
        r->smr_first = 0xffffffffu;
        for (unsigned i = 0; i < n; ++i) {
            uint32_t smr = 0;
            if (gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_SMR(i), &smr)) break;
            if (smr & (1u << 31)) {
                ++r->smr_valid;
                if (r->smr_first == 0xffffffffu) {
                    r->smr_first = i; r->smr_first_val = smr;
                    gpb_rd(io, r, GPB_WIN_SMMU, GPB_SMMU_S2CR(i), &r->s2cr_first_val);
                }
            }
        }
        r->smmu = GPB_DONE;
    }
    // RBBM_STATUS is in the GX-gated space.
    if (!(have & (1u << GPB_WIN_GPU))) r->gpu = GPB_NOMAP;
    else if (r->cc != GPB_DONE || !gpb_gdsc_on(r->cx_gdscr) || !gpb_gdsc_on(r->gx_gdscr)) r->gpu = GPB_SKIPPED_PREREQ;
    else {
        gpb_rd(io, r, GPB_WIN_GPU, GPB_RBBM_STATUS_BYTES, &r->rbbm_status);
        r->gpu = GPB_DONE;
    }
}

// Mode 6: the GMU clocks a6xx_gmu_resume enables (DT gmu clocks: ahb, gmu, cxo, axi, memnoc) at the GMU OPP's 200 MHz, after
// the GPLL0 GPU votes they hang from: 1 the votes (0x52000 bits 15/16, 10 us); 2 gpu_cc_gmu_clk_src to GPLL0 div (sel 6) / 1.5
// (hid 2), as clk_rcg2 with the root forced on around the update, since nothing has enabled the branch yet; 3 the wake/sleep
// fields of gpu_cc_cx_gmu_clk (the gpucc probe); 4 the five branches, then gpu_cc_cx_gmu_clk's CLK_OFF clear. Any timeout stops
// the sequence at its step.
#define GPB_RCG_UPDATE (1u << 0)
#define GPB_RCG_ROOT_EN (1u << 1)
#define GPB_RCG_ROOT_OFF (1u << 31)
#define GPB_RCG_CFG_CLEAR (0x1fu | (7u << 8) | (3u << 12) | (1u << 20))   // hid 5 bits, SRC_SEL, MODE, HW_CLK_CTRL
#define GPB_GMU_SRC_SEL 6u
#define GPB_GMU_SRC_HID 2u                // 2 * 1.5 - 1
struct gpb_gmu_clock_on {
    unsigned step;                       // last step started (1..4), 5 when all passed
    int failed;                          // 1 timeout or refusal at `step`, 2 I/O error
    unsigned rcg_polls, branch_polls;
    uint32_t vote, gmu_cmd, gmu_cfg, cx_gmu, ahb, cxo, axi, memnoc, pll1_mode;
};
// 0 done, 1 timeout, -1 I/O.
static inline int gpb_rcg_set(const struct gpb_io *io, struct gpb_result *r, uint32_t rcgr, uint32_t sel, uint32_t hid,
                              uint32_t *cfg_after, unsigned *polls) {
    uint32_t cmd = 0, cfg = 0;
    int timeout = 1;
    if (gpb_rd(io, r, GPB_WIN_GPUCC, rcgr, &cmd) || io->write32(io->cookie, GPB_WIN_GPUCC, rcgr, cmd | GPB_RCG_ROOT_EN)) return -1;
    for (unsigned i = 0; i < 500; ++i) {
        if (gpb_rd(io, r, GPB_WIN_GPUCC, rcgr, &cmd)) return -1;
        if (!(cmd & GPB_RCG_ROOT_OFF)) {timeout = 0; break;}
        if (io->delay_us) io->delay_us(io->cookie, 1);
    }
    if (timeout) return 1;
    if (gpb_rd(io, r, GPB_WIN_GPUCC, rcgr + 4, &cfg) ||
        io->write32(io->cookie, GPB_WIN_GPUCC, rcgr + 4, (cfg & ~GPB_RCG_CFG_CLEAR) | sel << 8 | hid)) return -1;
    if (gpb_rd(io, r, GPB_WIN_GPUCC, rcgr, &cmd) || io->write32(io->cookie, GPB_WIN_GPUCC, rcgr, cmd | GPB_RCG_UPDATE)) return -1;
    timeout = 1;
    for (unsigned i = 0; i < 500; ++i) {
        ++*polls;
        if (gpb_rd(io, r, GPB_WIN_GPUCC, rcgr, &cmd)) return -1;
        if (!(cmd & GPB_RCG_UPDATE)) {timeout = 0; break;}
        if (io->delay_us) io->delay_us(io->cookie, 1);
    }
    if (timeout) return 1;
    if (io->write32(io->cookie, GPB_WIN_GPUCC, rcgr, cmd & ~GPB_RCG_ROOT_EN)) return -1;
    return gpb_rd(io, r, GPB_WIN_GPUCC, rcgr + 4, cfg_after) ? -1 : 0;
}
static inline int gpb_set_bits(const struct gpb_io *io, struct gpb_result *r, unsigned w, uint32_t off, uint32_t bits) {
    uint32_t v = 0;
    if (gpb_rd(io, r, w, off, &v)) return -1;
    return (v & bits) == bits ? 0 : io->write32(io->cookie, w, off, v | bits);
}
static inline void gpb_gmu_clocks_on(const struct gpb_io *io, struct gpb_result *r, struct gpb_gmu_clock_on *o) {
    __builtin_memset(o, 0, sizeof *o);
    int e = 0;
    o->step = 1;
    if (r->cc != GPB_DONE || !gpb_gdsc_on(r->cx_gdscr)) {o->failed = 1; return;}
    if (gpb_set_bits(io, r, GPB_WIN_GCC_VOTE, GPB_GCC_PLL_VOTE, GPB_GPLL0_VOTES)) goto io;
    if (io->delay_us) io->delay_us(io->cookie, 10);
    o->step = 2;
    e = gpb_rcg_set(io, r, GPB_CC_GMU_RCGR, GPB_GMU_SRC_SEL, GPB_GMU_SRC_HID, &o->gmu_cfg, &o->rcg_polls);
    if (e) {o->failed = e < 0 ? 2 : 1; goto out;}
    o->step = 3;
    if (gpb_set_bits(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GMU_CLK, GPB_CX_GMU_WAKE_SLEEP)) goto io;
    o->step = 4;
    if (gpb_set_bits(io, r, GPB_WIN_GPUCC, GPB_CC_AHB_CLK, GPB_CBCR_ENABLE) ||
        gpb_set_bits(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GMU_CLK, GPB_CBCR_ENABLE) ||
        gpb_set_bits(io, r, GPB_WIN_GPUCC, GPB_CC_CXO_CLK, GPB_CBCR_ENABLE) ||
        gpb_set_bits(io, r, GPB_WIN_GCC_GPU, GPB_GCC_DDRSS_GPU_AXI, GPB_CBCR_ENABLE) ||
        gpb_set_bits(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_MEMNOC_GFX, GPB_CBCR_ENABLE)) goto io;
    e = 1;
    for (unsigned i = 0; i < 500; ++i) {
        ++o->branch_polls;
        if (gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GMU_CLK, &o->cx_gmu)) goto io;
        if (gpb_clock_on(o->cx_gmu)) {e = 0; break;}
        if (io->delay_us) io->delay_us(io->cookie, 1);
    }
    if (e) {o->failed = 1; goto out;}
    o->step = 5;
    goto out;
io:
    o->failed = 2;
out:
    gpb_rd(io, r, GPB_WIN_GCC_VOTE, GPB_GCC_PLL_VOTE, &o->vote);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_GMU_RCGR, &o->gmu_cmd);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_GMU_RCGR + 4, &o->gmu_cfg);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CX_GMU_CLK, &o->cx_gmu);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_AHB_CLK, &o->ahb);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_CXO_CLK, &o->cxo);
    gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_DDRSS_GPU_AXI, &o->axi);
    gpb_rd(io, r, GPB_WIN_GCC_GPU, GPB_GCC_GPU_MEMNOC_GFX, &o->memnoc);
    gpb_rd(io, r, GPB_WIN_GPUCC, GPB_CC_PLL1_MODE, &o->pll1_mode);
}
// The GMU and RSCC blocks are read only when CX is on and gpu_cc_ahb_clk and gpu_cc_cx_gmu_clk run.
static inline int gpb_gmu_regs_readable(const struct gpb_result *r, uint32_t cx_gmu, uint32_t cc_ahb) {
    return r->cc == GPB_DONE && gpb_gdsc_on(r->cx_gdscr) && gpb_clock_on(cc_ahb) && gpb_clock_on(cx_gmu);
}
struct gpb_gmu_state {uint32_t sysreset, fw_init, gx_status, hfi_status, general7, ao_irq, fence, rscc_busy, rscc_status;};
static inline void gpb_read_gmu_state(const struct gpb_io *io, struct gpb_result *r, struct gpb_gmu_state *s) {
    gpb_rd(io, r, GPB_WIN_GMU, GPB_GMU_CM3_SYSRESET, &s->sysreset);
    gpb_rd(io, r, GPB_WIN_GMU, GPB_GMU_CM3_FW_INIT_RESULT, &s->fw_init);
    gpb_rd(io, r, GPB_WIN_GMU, GPB_GMU_SPTPRAC_PWR_CLK_STATUS, &s->gx_status);
    gpb_rd(io, r, GPB_WIN_GMU, GPB_GMU_HFI_CTRL_STATUS, &s->hfi_status);
    gpb_rd(io, r, GPB_WIN_GMU, GPB_GMU_GENERAL_7, &s->general7);
    gpb_rd(io, r, GPB_WIN_GMU, GPB_GMU_AO_HOST_INTERRUPT_STATUS, &s->ao_irq);
    gpb_rd(io, r, GPB_WIN_GMU, GPB_GMU_AHB_FENCE_STATUS, &s->fence);
    gpb_rd(io, r, GPB_WIN_RSCC, GPB_RSCC_SEQ_BUSY_DRV0, &s->rscc_busy);
    gpb_rd(io, r, GPB_WIN_RSCC, GPB_RSCC_RSC_STATUS0_DRV0, &s->rscc_status);
}

// The AOP command DB (cmd-db.c): DTS reserved-memory@80860000 size 0x20000 (no-map, below DarwinBoot's window). A header of the
// version, the magic 0x0c0330db and eight 16-byte slave records {slv_id, header_offset, data_offset, cnt, version}, then the
// checksum and a reserved word; the data area starts at 144. A slave's entries (24 bytes: id[8], priority[2], addr, len, offset)
// sit at data + header_offset, an entry's aux data at data + data_offset + offset. The ARC resources (slave 3) carry their levels
// as 16-bit words: a6xx_gmu.c builds the GPU (gfx.lvl with mx.lvl) and GMU (cx.lvl with mx.lvl) votes from them.
#define GPB_CMD_DB_BASE 0x80860000ULL
#define GPB_CMD_DB_BYTES 0x20000u
#define GPB_CMD_DB_MAGIC 0x0c0330dbu
#define GPB_CMD_DB_DATA 144u
struct gpb_cmd_db_entry {uint32_t address, slave; const uint8_t *aux; unsigned aux_bytes;};
static inline uint32_t gpb_le16(const uint8_t *p) {return (uint32_t)p[0] | (uint32_t)p[1] << 8;}
static inline uint32_t gpb_le32(const uint8_t *p) {return gpb_le16(p) | gpb_le16(p + 2) << 16;}
// An entry id is 8 bytes, zero padded: equal to `name` (at most 8 characters) when every byte matches up to its end.
static inline int gpb_cmd_db_id_is(const uint8_t *id, const char *name) {
    for (unsigned k = 0; k < 8; ++k) {
        if (id[k] != (uint8_t)name[k]) return 0;
        if (!name[k]) return 1;
    }
    return 1;
}
// 0 found, 1 not in the database, -1 a bad header or an entry/aux outside the database.
static inline int gpb_cmd_db_find(const uint8_t *db, size_t bytes, const char *name, struct gpb_cmd_db_entry *out) {
    if (bytes < GPB_CMD_DB_DATA || gpb_le32(db + 4) != GPB_CMD_DB_MAGIC) return -1;
    for (unsigned s = 0; s < 8; ++s) {
        const uint8_t *h = db + 8 + 16 * s;
        uint32_t slave = gpb_le16(h), heads = gpb_le16(h + 2), data = gpb_le16(h + 4), count = gpb_le16(h + 6);
        if (!slave) break;
        size_t at = GPB_CMD_DB_DATA + heads;
        if (at + (size_t)count * 24 > bytes) return -1;
        for (uint32_t i = 0; i < count; ++i) {
            const uint8_t *e = db + at + 24 * i;
            if (!gpb_cmd_db_id_is(e, name)) continue;
            size_t aux = GPB_CMD_DB_DATA + data + gpb_le16(e + 22), len = gpb_le16(e + 20);
            if (aux + len > bytes) return -1;
            *out = (struct gpb_cmd_db_entry){gpb_le32(e + 16), slave, db + aux, (unsigned)len};
            return 0;
        }
    }
    return 1;
}
static inline const char *gpb_state_text(enum gpb_state s) {
    return s == GPB_DONE ? "ok" : s == GPB_SKIPPED_PREREQ ? "skip" : s == GPB_SKIPPED_MODE ? "off" : s == GPB_NOMAP ? "nomap" : "-";
}
#endif
