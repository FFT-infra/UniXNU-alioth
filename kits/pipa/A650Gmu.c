/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "A650Gmu.h"
#include "AdrenoFirmware.h"
#include "AdrenoHfiVotes.h"

/* a6xx_gmu.xml.h, GMU block (dword offsets). */
enum {
    GMU_CM3_ITCM_START = 0x0c00, GMU_CM3_DTCM_START = 0x1c00,
    GMU_ICACHE_CONFIG = 0x4c00, GMU_DCACHE_CONFIG = 0x4c01, GMU_SYS_BUS_CONFIG = 0x4c0f,
    GMU_CM3_SYSRESET = 0x5000, GMU_CM3_BOOT_CONFIG = 0x5001, GMU_CM3_FW_INIT_RESULT = 0x501c, GMU_CM3_CFG = 0x502d,
    GMU_PWR_COL_INTER_FRAME_CTRL = 0x50c0, GMU_RPMH_CTRL = 0x50e8,
    GMU_CX_FAL_INTF = 0x50f0, GMU_CX_FALNEXT_INTF = 0x50f1, GMU_PWR_COL_CP_MSG = 0x5100, GMU_PWR_COL_CP_RESP = 0x5101,
    GMU_HFI_CTRL_STATUS = 0x5180, GMU_HFI_SFR_ADDR = 0x5182, GMU_HFI_QTBL_INFO = 0x5184, GMU_HFI_QTBL_ADDR = 0x5185,
    GMU_HFI_CTRL_INIT = 0x5186,
    GMU_GMU2HOST_INTR_CLR = 0x5191, GMU_GMU2HOST_INTR_INFO = 0x5192, GMU_GMU2HOST_INTR_MASK = 0x5193,
    GMU_HOST2GMU_INTR_SET = 0x5194, GMU_GENERAL_7 = 0x51cc,
    GMU_AO_HOST_INTERRUPT_CLR = 0x9304, GMU_AO_HOST_INTERRUPT_STATUS = 0x9305, GMU_AO_HOST_INTERRUPT_MASK = 0x9306,
    GMU_RSCC_CONTROL_REQ = 0x9307, GMU_RSCC_CONTROL_ACK = 0x9308, GMU_AHB_FENCE_RANGE_0 = 0x9311,
};
/* The RSCC block (gmu_write_rscc) and the PDC block (a6xx.xml.h, pdc_write). */
enum {
    RSCC_RSC_STATUS0_DRV0 = 0x004, RSCC_PDC_SEQ_START_ADDR = 0x008, RSCC_PDC_MATCH_VALUE_LO = 0x009,
    RSCC_PDC_MATCH_VALUE_HI = 0x00a, RSCC_PDC_SLAVE_ID_DRV0 = 0x00b, RSCC_HIDDEN_TCS_CMD0_ADDR = 0x00d,
    RSCC_HIDDEN_TCS_CMD0_DATA = 0x00e, RSCC_OVERRIDE_START_ADDR = 0x100, RSCC_SEQ_BUSY_DRV0 = 0x101,
    RSCC_SEQ_MEM_0_DRV0 = 0x180,
    PDC_GPU_ENABLE_PDC = 0x1140, PDC_GPU_SEQ_START_ADDR = 0x1148,
};
/* A6XX_GMU_IRQ_MASK (WDOG_BITE, HOST_AHB_BUS_ERROR, FENCE_ERR) and A6XX_HFI_IRQ_MASK (CM3_FAULT). */
#define AO_FATAL 0x29u
#define CM3_FATAL 0x00800000u
/* a6xx_gmu_power_config: GMU_RPMH_CTRL's interface, LLC, DDR, MX, CX and GFX vote enables. */
#define RPMH_CTRL_VOTES 0x00000f11u
#define FENCE_RANGE ((1u << 31) | (0xau << 18) | 0xa0u)
#define ITCM_BASE 0x00000000u
#define DTCM_BASE 0x10004000u     /* a6xx_gmu_fw_load: the a650 family's DTCM */
#define TCM_BYTES 0x4000u
#define ICACHE_IOVA 0x00004000u
#define DUMMY_IOVA 0x60000000u

static int read32(A650GmuDevice *d, unsigned w, uint32_t dword, uint32_t *v, A650GmuResult *r)
{++r->read_attempts; return d->io.read32(d->io.cookie, w, (size_t)dword * 4, v);}
static int write32(A650GmuDevice *d, unsigned w, uint32_t dword, uint32_t v, A650GmuResult *r)
{++r->write_attempts; return d->io.write32(d->io.cookie, w, (size_t)dword * 4, v);}
static int core_read(A650GmuDevice *d, uint32_t dword, uint32_t *v, A650GmuResult *r) {return read32(d, A650GmuCore, dword, v, r);}
static int core_write(A650GmuDevice *d, uint32_t dword, uint32_t v, A650GmuResult *r) {return write32(d, A650GmuCore, dword, v, r);}
static uint32_t le32(const uint8_t *p)
{return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;}
static int extent(uintptr_t at, size_t bytes) {return at && bytes && bytes - 1 <= UINTPTR_MAX - at;}
static int overlap(uintptr_t a, size_t na, uintptr_t b, size_t nb) {return a <= b ? (b - a < na) : (a - b < nb);}

static A650GmuStatus check_faults(A650GmuDevice *d, A650GmuResult *r)
{
    if (core_read(d, GMU_AO_HOST_INTERRUPT_STATUS, &r->ao_fault_status, r) ||
        core_read(d, GMU_GMU2HOST_INTR_INFO, &r->cm3_fault_status, r)) return A650GmuIoError;
    return (r->ao_fault_status & AO_FATAL) || (r->cm3_fault_status & CM3_FATAL) ? A650GmuFault : A650GmuSuccess;
}
/* gmu_poll_timeout(.., 100, 10000): a read every 100 us for at most 10 ms, also bounded by the poll budget. */
static A650GmuStatus poll(A650GmuDevice *d, unsigned w, uint32_t dword, uint32_t mask, uint32_t expected, unsigned budget,
                          int faults, uint32_t *observed, A650GmuResult *r)
{
    uint64_t start, last, now;
    if (d->services.now_us(d->services.cookie, &start)) return A650GmuClockError;
    last = start;
    for (unsigned i = 0; i < budget; ++i) {
        if (d->services.now_us(d->services.cookie, &now) || now < last) return A650GmuClockError;
        if (now - start >= 10000) return A650GmuTimeout;
        last = now; ++r->poll_reads;
        if (faults) {A650GmuStatus f = check_faults(d, r); if (f != A650GmuSuccess) return f;}
        if (read32(d, w, dword, observed, r)) return A650GmuIoError;
        if ((*observed & mask) == expected) return d->io.barrier(d->io.cookie) ? A650GmuIoError : A650GmuSuccess;
        if (i + 1 < budget && d->services.wait_us(d->services.cookie, 100)) return A650GmuClockError;
    }
    return A650GmuTimeout;
}
static void reset_after_failure(A650GmuDevice *d, A650GmuResult *r)
{
    uint32_t actual;
    r->reset_verified = 0; d->poisoned = 1; d->hfi.poisoned = 1;
    if (!core_write(d, GMU_CM3_SYSRESET, 1, r) && !d->io.barrier(d->io.cookie) &&
        !core_read(d, GMU_CM3_SYSRESET, &actual, r) && actual == 1) r->reset_verified = 1;
}
static int notify(void *cookie)
{
    A650GmuDevice *d = cookie;
    return d->io.barrier(d->io.cookie) ||
           d->io.write32(d->io.cookie, A650GmuCore, (size_t)GMU_HOST2GMU_INTR_SET * 4, 1) || d->io.barrier(d->io.cookie);
}

static A650GmuStatus validate(A650GmuDevice *d, const A650GmuConfig *c)
{
    static const size_t windows[A650GmuWindows] = {A650_GMU_CORE_BYTES, A650_GMU_RSCC_BYTES, A650_GMU_PDC_BYTES};
    if (!d || !c || !d->io.read32 || !d->io.write32 || !d->io.barrier || !d->services.now_us || !d->services.wait_us ||
        c->chip_id != A650_GMU_CHIP_ID || !c->firmware || c->firmware_bytes < 16 || c->firmware_bytes > 0x100000 ||
        !extent((uintptr_t)c->firmware, c->firmware_bytes) || !c->poll_budget || c->poll_budget > 1024)
        return A650GmuArgument;
    if (d->poisoned) return A650GmuPoisoned;
    if (d->boot_attempted) return A650GmuBusy;
    for (unsigned i = 0; i < A650GmuWindows; ++i) if (d->io.bytes[i] != windows[i]) return A650GmuArgument;
    const A650GmuMemory *mem[4] = {&c->icache, &c->dummy, &c->log, &c->hfi};
    for (unsigned i = 0; i < 4; ++i) {
        const A650GmuMemory *m = mem[i];
        if (!m->bytes || (m->bytes & 4095) || !extent((uintptr_t)m->cpu, m->bytes) || ((uintptr_t)m->cpu & 4095) ||
            !m->iova || (m->iova & 4095) || m->iova > UINT32_MAX - (m->bytes - 1) ||
            overlap((uintptr_t)m->cpu, m->bytes, (uintptr_t)c->firmware, c->firmware_bytes)) return A650GmuArgument;
        for (unsigned j = 0; j < i; ++j)
            if (overlap((uintptr_t)m->cpu, m->bytes, (uintptr_t)mem[j]->cpu, mem[j]->bytes) ||
                overlap((uintptr_t)m->iova, m->bytes, (uintptr_t)mem[j]->iova, mem[j]->bytes)) return A650GmuArgument;
    }
    /* a6xx_gmu_init for the a650: the icache region at 0x4000 (16 MiB - 16 KiB), the dummy page at 0x60000000; the log
     * (16 KiB, its size goes into PWR_COL_CP_MSG) and the HFI queues (16 KiB) anywhere. */
    if (c->icache.iova != ICACHE_IOVA || c->icache.bytes < 0x10000 || c->dummy.iova != DUMMY_IOVA || c->log.bytes != 0x4000 ||
        c->hfi.bytes != 0x4000) return A650GmuArgument;
    return A650GmuSuccess;
}
/* Where a planned block goes: 1 ITCM, 2 the icache region, 3 DTCM, 4 the dummy region. */
static volatile uint32_t *block_memory(const A650GmuConfig *c, const AdrenoFirmwareBlock *b)
{
    if (b->region_identifier == 2) return c->icache.cpu + (b->address - ICACHE_IOVA) / 4;
    if (b->region_identifier == 4) return c->dummy.cpu + (b->address - DUMMY_IOVA) / 4;
    return 0;
}
static uint32_t block_register(const AdrenoFirmwareBlock *b)
{
    return b->region_identifier == 1 ? GMU_CM3_ITCM_START + (b->address - ITCM_BASE) / 4
                                     : GMU_CM3_DTCM_START + (b->address - DTCM_BASE) / 4;
}

A650GmuStatus A650GmuLoadAndStartHfi(A650GmuDevice *d, const A650GmuConfig *c, A650GmuResult *r)
{
    if (!r) return A650GmuArgument;
    *r = (A650GmuResult){0}; r->resources_must_retain = 1;
    A650GmuStatus status = validate(d, c);
    if (status != A650GmuSuccess) return status;
    const AdrenoFirmwareRegion regions[] = {{ITCM_BASE, TCM_BYTES, 1}, {ICACHE_IOVA, c->icache.bytes, 2},
                                            {DTCM_BASE, TCM_BYTES, 3}, {DUMMY_IOVA, c->dummy.bytes, 4}};
    AdrenoFirmwareBlock blocks[48]; size_t count = 0;
    if (AdrenoFirmwarePlanGmu(c->firmware, c->firmware_bytes, regions, 4, blocks, 48, &count) != AdrenoFirmwareSuccess)
        return A650GmuFirmware;
    r->blocks = (uint32_t)count;
    /* Phase 1, read only: the GMU held in reset, the RSC sequencer idle, no stale HFI readiness. */
    r->phase = 1;
    uint32_t value;
    if (core_read(d, GMU_CM3_SYSRESET, &value, r)) return A650GmuIoError;
    if (value != 1) return A650GmuBusy;
    r->reset_verified = 1;
    if (read32(d, A650GmuRscc, RSCC_SEQ_BUSY_DRV0, &value, r)) return A650GmuIoError;
    if (value) return A650GmuBusy;
    if (core_read(d, GMU_HFI_CTRL_STATUS, &value, r)) return A650GmuIoError;
    if (value & 1) return A650GmuStateMismatch;
    /* Phase 2: a6xx_gmu_resume's interrupt clear and mask (GMU2HOST cleared too, so a stale bit cannot read as a CM3
     * fault), then a6xx_gmu_rpmh_init for the a650 and a6xx_rpmh_stop. */
    r->phase = 2;
    d->boot_attempted = 1;
    if (core_write(d, GMU_AO_HOST_INTERRUPT_CLR, ~0u, r) || core_write(d, GMU_AO_HOST_INTERRUPT_MASK, ~AO_FATAL, r) ||
        core_write(d, GMU_GMU2HOST_INTR_CLR, ~0u, r) || d->io.barrier(d->io.cookie)) {status = A650GmuIoError; goto failed;}
    status = check_faults(d, r);
    if (status != A650GmuSuccess) goto failed;
    static const struct {unsigned window; uint32_t dword, value;} rsc[] = {
        {A650GmuRscc, RSCC_RSC_STATUS0_DRV0, 1u << 24},                 /* disable SDE clock gating */
        {A650GmuRscc, RSCC_PDC_SLAVE_ID_DRV0, 1},
        {A650GmuRscc, RSCC_HIDDEN_TCS_CMD0_DATA, 0}, {A650GmuRscc, RSCC_HIDDEN_TCS_CMD0_ADDR, 0},
        {A650GmuRscc, RSCC_HIDDEN_TCS_CMD0_DATA + 2, 0}, {A650GmuRscc, RSCC_HIDDEN_TCS_CMD0_ADDR + 2, 0},
        {A650GmuRscc, RSCC_HIDDEN_TCS_CMD0_DATA + 4, 0x80000000u}, {A650GmuRscc, RSCC_HIDDEN_TCS_CMD0_ADDR + 4, 0},
        {A650GmuRscc, RSCC_OVERRIDE_START_ADDR, 0},
        {A650GmuRscc, RSCC_PDC_SEQ_START_ADDR, 0x4520},
        {A650GmuRscc, RSCC_PDC_MATCH_VALUE_LO, 0x4510}, {A650GmuRscc, RSCC_PDC_MATCH_VALUE_HI, 0x4514},
        /* the a650 family's RSC sequencer microcode for sleep and wake-up */
        {A650GmuRscc, RSCC_SEQ_MEM_0_DRV0, 0xeaaae5a0u}, {A650GmuRscc, RSCC_SEQ_MEM_0_DRV0 + 1, 0xe1a1ebabu},
        {A650GmuRscc, RSCC_SEQ_MEM_0_DRV0 + 2, 0xa2e0a581u}, {A650GmuRscc, RSCC_SEQ_MEM_0_DRV0 + 3, 0xecac82e2u},
        {A650GmuRscc, RSCC_SEQ_MEM_0_DRV0 + 4, 0x0020edadu},
        /* pdc_in_aop: the PDC sequence and its TCS commands belong to the AOP; only start and enable it */
        {A650GmuPdc, PDC_GPU_SEQ_START_ADDR, 0}, {A650GmuPdc, PDC_GPU_ENABLE_PDC, 0x80000001u},
    };
    for (unsigned i = 0; i < sizeof rsc / sizeof rsc[0]; ++i)
        if (write32(d, rsc[i].window, rsc[i].dword, rsc[i].value, r)) {status = A650GmuIoError; goto failed;}
    if (d->io.barrier(d->io.cookie)) {status = A650GmuIoError; goto failed;}
    /* a6xx_rpmh_stop: Linux reports a timeout here and goes on; so does this. */
    if (core_write(d, GMU_RSCC_CONTROL_REQ, 1, r)) {status = A650GmuIoError; goto failed;}
    {
        A650GmuStatus stop = poll(d, A650GmuRscc, RSCC_RSC_STATUS0_DRV0, 1u << 16, 1u << 16, c->poll_budget, 0, &value, r);
        if (stop == A650GmuIoError || stop == A650GmuClockError) {status = stop; goto failed;}
        r->rsc_stop_status = stop;
    }
    if (core_write(d, GMU_RSCC_CONTROL_REQ, 0, r)) {status = A650GmuIoError; goto failed;}
    /* Phase 3, a6xx_gmu_fw_start (cold): the FAL interfaces, register retention, the RSC wake-up. */
    r->phase = 3;
    if (core_write(d, GMU_CX_FALNEXT_INTF, 1, r) || core_write(d, GMU_CX_FAL_INTF, 1, r) || core_write(d, GMU_GENERAL_7, 1, r) ||
        core_write(d, GMU_RSCC_CONTROL_REQ, 1u << 1, r) || d->io.barrier(d->io.cookie)) {status = A650GmuIoError; goto failed;}
    status = poll(d, A650GmuCore, GMU_RSCC_CONTROL_ACK, 1u << 1, 1u << 1, c->poll_budget, 1, &r->rscc_ack, r);
    if (status != A650GmuSuccess) goto failed;
    status = poll(d, A650GmuRscc, RSCC_SEQ_BUSY_DRV0, ~0u, 0, c->poll_budget, 1, &value, r);
    if (status != A650GmuSuccess) goto failed;
    if (core_write(d, GMU_RSCC_CONTROL_REQ, 0, r)) {status = A650GmuIoError; goto failed;}
    /* Phase 4, a6xx_gmu_fw_load: zero the GMU's memory (the GMU is still in reset), set up the HFI queues, copy every block,
     * then read every word back before the GMU is released. */
    r->phase = 4;
    {
        const A650GmuMemory *zero[3] = {&c->icache, &c->dummy, &c->log};
        r->memory_changed = 1;
        for (unsigned m = 0; m < 3; ++m) for (size_t i = 0; i < zero[m]->bytes / 4; ++i) zero[m]->cpu[i] = 0;
    }
    if (AdrenoHfiInitializeStopped(&d->hfi, c->hfi.cpu, c->hfi.bytes, c->hfi.iova, d, notify) != AdrenoHfiSuccess) {
        status = A650GmuArgument; goto failed;
    }
    for (size_t i = 0; i < count; ++i) {
        const AdrenoFirmwareBlock *b = blocks + i;
        volatile uint32_t *memory = block_memory(c, b);
        for (size_t at = 0; at < b->bytes; at += 4) {
            const uint32_t word = le32(c->firmware + b->payload_offset + at);
            if (memory) memory[at / 4] = word;
            else if (core_write(d, block_register(b) + (uint32_t)(at / 4), word, r)) {status = A650GmuIoError; goto failed;}
        }
    }
    if (d->io.barrier(d->io.cookie)) {status = A650GmuIoError; goto failed;}
    for (size_t i = 0; i < count; ++i) {
        const AdrenoFirmwareBlock *b = blocks + i;
        volatile uint32_t *memory = block_memory(c, b);
        for (size_t at = 0; at < b->bytes; at += 4) {
            if (memory) value = memory[at / 4];
            else if (core_read(d, block_register(b) + (uint32_t)(at / 4), &value, r)) {status = A650GmuIoError; goto failed;}
            if (value != le32(c->firmware + b->payload_offset + at)) {status = A650GmuStateMismatch; goto failed;}
        }
    }
    /* Phase 5: the boot registers and a6xx_gmu_power_config (idle level ACTIVE: no IFPC, no SPTP collapse), each read back. */
    r->phase = 5;
    {
        /* The GMU's chip id: the upper 16 bits, then the minor and patch ids in four bits each. */
        const uint32_t chipid = (c->chip_id & 0xffff0000u) | ((c->chip_id << 4) & 0xf000u) | ((c->chip_id << 8) & 0x0f00u);
        const uint32_t regs[] = {GMU_CM3_FW_INIT_RESULT, GMU_CM3_BOOT_CONFIG, GMU_HFI_QTBL_ADDR, GMU_HFI_QTBL_INFO,
                                 GMU_AHB_FENCE_RANGE_0, GMU_CM3_CFG, GMU_HFI_SFR_ADDR, GMU_PWR_COL_CP_MSG, GMU_SYS_BUS_CONFIG,
                                 GMU_ICACHE_CONFIG, GMU_DCACHE_CONFIG, GMU_PWR_COL_INTER_FRAME_CTRL};
        const uint32_t values[] = {0, 2, (uint32_t)c->hfi.iova, 1, FENCE_RANGE, 0x4052, chipid,
                                   (uint32_t)c->log.iova | (uint32_t)(c->log.bytes / 4096 - 1), 1, 1, 1, 0x9c40400u};
        for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; ++i)
            if (core_write(d, regs[i], values[i], r)) {status = A650GmuIoError; goto failed;}
        if (core_read(d, GMU_RPMH_CTRL, &value, r) || core_write(d, GMU_RPMH_CTRL, value | RPMH_CTRL_VOTES, r) ||
            d->io.barrier(d->io.cookie)) {status = A650GmuIoError; goto failed;}
        for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; ++i) {
            if (core_read(d, regs[i], &value, r)) {status = A650GmuIoError; goto failed;}
            if (value != values[i]) {status = A650GmuStateMismatch; goto failed;}
        }
        if (core_read(d, GMU_RPMH_CTRL, &value, r)) {status = A650GmuIoError; goto failed;}
        if ((value & RPMH_CTRL_VOTES) != RPMH_CTRL_VOTES) {status = A650GmuStateMismatch; goto failed;}
    }
    /* Phase 6, a6xx_gmu_start: the firmware version in DTCM picks the ready value, then reset, the log index, release. */
    r->phase = 6;
    if (core_read(d, GMU_CM3_DTCM_START + 0xff8, &r->firmware_version, r)) {status = A650GmuIoError; goto failed;}
    {
        const uint32_t mask = r->firmware_version <= 0x20010004u ? 0xffffffffu : 0x1ffu;
        const uint32_t ready = mask == 0xffffffffu ? 0xbabefaceu : 0x100u;
        if (core_write(d, GMU_CM3_SYSRESET, 1, r) || core_write(d, GMU_PWR_COL_CP_RESP, 0, r) || d->io.barrier(d->io.cookie)) {
            status = A650GmuIoError; goto failed;
        }
        r->reset_verified = 0;
        if (core_write(d, GMU_CM3_SYSRESET, 0, r) || d->io.barrier(d->io.cookie)) {status = A650GmuIoError; goto failed;}
        status = poll(d, A650GmuCore, GMU_CM3_FW_INIT_RESULT, mask, ready, c->poll_budget, 1, &r->firmware_init_result, r);
        if (status != A650GmuSuccess) goto failed;
        r->firmware_ready_observed = 1;
    }
    /* Phase 7, a6xx_gmu_hfi_start. */
    r->phase = 7;
    if (core_write(d, GMU_HFI_CTRL_INIT, 1, r) || d->io.barrier(d->io.cookie)) {status = A650GmuIoError; goto failed;}
    status = poll(d, A650GmuCore, GMU_HFI_CTRL_STATUS, 1, 1, c->poll_budget, 1, &r->hfi_status, r);
    if (status != A650GmuSuccess) goto failed;
    status = check_faults(d, r);
    if (status != A650GmuSuccess) goto failed;
    r->hfi_ready_observed = 1; r->phase = 8;
    return A650GmuSuccess;
failed:
    reset_after_failure(d, r);
    return status;
}

typedef struct {A650GmuDevice *device; A650GmuStartupResult *result;} StartupContext;
static int startup_now(void *cookie, uint64_t *value)
{A650GmuDevice *d = ((StartupContext *)cookie)->device; return d->services.now_us(d->services.cookie, value);}
static int startup_wait(void *cookie, unsigned us)
{A650GmuDevice *d = ((StartupContext *)cookie)->device; return d->services.wait_us(d->services.cookie, us);}
static int startup_health(void *cookie)
{
    StartupContext *c = cookie; A650GmuDevice *d = c->device; A650GmuStartupResult *r = c->result;
    uint32_t reset, hfi;
    r->health_status = check_faults(d, &r->load);
    if (r->health_status != A650GmuSuccess) return 1;
    if (core_read(d, GMU_CM3_SYSRESET, &reset, &r->load) || core_read(d, GMU_HFI_CTRL_STATUS, &hfi, &r->load))
        r->health_status = A650GmuIoError;
    else if (reset != 0 || !(hfi & 1)) r->health_status = A650GmuStateMismatch;
    return r->health_status != A650GmuSuccess;
}
A650GmuStatus A650GmuColdStart(A650GmuDevice *d, const A650GmuConfig *c, const AdrenoHfiStartupConfig *config,
                               const A650GmuStartupLimits *limits, A650GmuStartupResult *r)
{
    if (!r) return A650GmuArgument;
    *r = (A650GmuStartupResult){0}; r->load.resources_must_retain = r->protocol.resources_must_retain = 1;
    r->protocol_status = AdrenoHfiStartupInvalid;
    if (!d || !c || !limits || AdrenoHfiStartupValidate(config) != AdrenoHfiStartupSuccess) return A650GmuArgument;
    StartupContext context = {d, r};
    AdrenoHfiStartupServices services = {&context, startup_now, startup_wait, startup_health, limits->polls_per_command,
                                         limits->wait_interval_us, limits->command_timeout_us, limits->total_timeout_us};
    if (AdrenoHfiStartupValidateServices(&services) != AdrenoHfiStartupSuccess) return A650GmuArgument;
    if (d->poisoned || d->startup.poisoned) return A650GmuPoisoned;
    if (d->startup.attempted || d->startup.completed) return A650GmuBusy;
    A650GmuStatus status = A650GmuLoadAndStartHfi(d, c, &r->load);
    if (status != A650GmuSuccess) return status;
    r->protocol_status = AdrenoHfiStartupRun(&d->startup, &d->hfi, config, &services, &r->protocol);
    if (r->protocol_status == AdrenoHfiStartupSuccess) {
        /* a6xx_gmu_resume after a6xx_hfi_start: clear GMU2HOST and unmask only the CM3 fault. */
        if (core_write(d, GMU_GMU2HOST_INTR_CLR, ~0u, &r->load) || core_write(d, GMU_GMU2HOST_INTR_MASK, ~CM3_FATAL, &r->load) ||
            d->io.barrier(d->io.cookie)) {status = A650GmuIoError; goto failed;}
        return A650GmuSuccess;
    }
    switch (r->protocol_status) {
    case AdrenoHfiStartupClockError: case AdrenoHfiStartupWaitError: status = A650GmuClockError; break;
    case AdrenoHfiStartupTimeout: status = A650GmuTimeout; break;
    case AdrenoHfiStartupPlatformFault: status = r->health_status; break;
    default: status = A650GmuProtocolError; break;
    }
failed:
    reset_after_failure(d, &r->load); d->startup.poisoned = 1;
    return status;
}

enum {GMU_SPTPRAC_PWR_CLK_STATUS = 0x50d0};
#define GX_OFF_BITS 0xc0u         /* GX_HM_GDSC_POWER_OFF | GX_HM_CLK_OFF */
#define OOB_GPU_SET_REQUEST 30
#define OOB_GPU_SET_ACK 31
static int hfi_wait(void *cookie) {A650GmuDevice *d = cookie; return d->services.wait_us(d->services.cookie, 100);}
/* Whether the GMU votes DDR bandwidth for the GPU (A650GmuBuildStartup's second bandwidth level, chosen by A650GmuPowerGx). Linux votes
 * the GPU's DDR bandwidth from the kernel through the interconnect framework and gives the GMU an "off" level only; this kernel has no
 * RPMh voter of its own, so without the GMU's vote the GPU ran on whatever DDR level the rest of the system left (2026-10-25: the scanout
 * copy moved 41 MB in 2.6 ms, about 16 GB/s, where the LPDDR5 reaches twice that). kgsl lets the GMU vote the same BCMs. 0 by default
 * (the host tests' table), the board sets it. */
unsigned A650GmuDdrVote;
A650GmuStatus A650GmuPowerGx(A650GmuDevice *d, unsigned perf_index, A650GmuGxResult *r)
{
    if (!r) return A650GmuArgument;
    *r = (A650GmuGxResult){0};
    if (!d || perf_index < 1 || perf_index > 7) return A650GmuArgument;
    if (d->poisoned || d->hfi.poisoned) return A650GmuPoisoned;
    if (!d->startup.completed) return A650GmuStateMismatch;
    A650GmuResult io = {0};
    if (core_read(d, GMU_SPTPRAC_PWR_CLK_STATUS, &r->gx_before, &io)) return A650GmuIoError;
    const uint32_t vote[3] = {1, perf_index, A650GmuDdrVote ? 1u : 0u};
    uint32_t out[16]; size_t count = 0;
    r->vote_status = AdrenoHfiExchange(&d->hfi, 30, vote, 3, 1000, hfi_wait, out, 16, &count, &r->vote_error);
    if (r->vote_status != AdrenoHfiSuccess) return r->vote_status == AdrenoHfiFirmwareError ? A650GmuProtocolError : A650GmuTimeout;
    if (core_write(d, GMU_HOST2GMU_INTR_SET, 1u << OOB_GPU_SET_REQUEST, &io) || d->io.barrier(d->io.cookie)) return A650GmuIoError;
    A650GmuStatus status = A650GmuTimeout;
    for (r->oob_polls = 1; r->oob_polls <= 100; ++r->oob_polls) {
        if (core_read(d, GMU_GMU2HOST_INTR_INFO, &r->oob_info, &io)) return A650GmuIoError;
        if (r->oob_info & (1u << OOB_GPU_SET_ACK)) {status = A650GmuSuccess; break;}
        if (d->services.wait_us(d->services.cookie, 100)) return A650GmuClockError;
    }
    if (core_write(d, GMU_GMU2HOST_INTR_CLR, 1u << OOB_GPU_SET_ACK, &io) || d->io.barrier(d->io.cookie)) return A650GmuIoError;
    if (core_read(d, GMU_SPTPRAC_PWR_CLK_STATUS, &r->gx_after, &io)) return A650GmuIoError;
    r->gx_on = status == A650GmuSuccess && !(r->gx_after & GX_OFF_BITS);
    if (status == A650GmuSuccess && !r->gx_on) status = A650GmuStateMismatch;
    return status;
}

AdrenoHfiStartupStatus A650GmuBuildStartup(const uint8_t *gfx, size_t gfx_bytes, const uint8_t *mx, size_t mx_bytes,
    const uint8_t *cx, size_t cx_bytes, uint32_t mc0, uint32_t sh0, uint32_t acv, uint32_t cn0, AdrenoHfiStartupConfig *out)
{
    /* DTS gpu-opp-table_v2 (opp-hz, opp-microvolt = RPMh level), with the off entry Linux's a6xx_gmu_pwrlevels_probe adds;
     * the GMU's single OPP of sm8250.dtsi gmu_opp_table (200 MHz, RPMH_REGULATOR_LEVEL_MIN_SVS = 48). */
    static const uint64_t gpu_hz[8] = {0, 305000000, 400000000, 441600000, 490000000, 525000000, 587000000, 670000000};
    static const uint32_t gpu_level[8] = {0, 64, 128, 144, 192, 224, 256, 320};
    static const uint64_t gmu_hz[2] = {0, 200000000};
    static const uint32_t gmu_level[2] = {0, 48};
    if (!out || !gfx || !mx || !cx || !mc0 || !sh0 || !acv || !cn0) return AdrenoHfiStartupInvalid;
    AdrenoHfiStartupConfig c = {0};
    uint32_t votes[16];
    if (AdrenoHfiArcVotes(gpu_level, 8, gfx, gfx_bytes, mx, mx_bytes, votes, 16)) return AdrenoHfiStartupInvalid;
    c.gpu_count = 8;
    for (unsigned i = 0; i < 8; ++i) c.gpu[i] = (AdrenoHfiPerformanceLevel){gpu_hz[i], votes[i]};
    if (AdrenoHfiArcVotes(gmu_level, 2, cx, cx_bytes, mx, mx_bytes, votes, 16)) return AdrenoHfiStartupInvalid;
    c.gmu_count = 2;
    for (unsigned i = 0; i < 2; ++i) c.gmu[i] = (AdrenoHfiPerformanceLevel){gmu_hz[i], votes[i]};
    /* a650_build_bw_table: one "off" level; the DDR commands MC0, SH0, ACV and the CNOC command CN0, in that order. With A650GmuDdrVote a
     * second level asks the most of MC0 and SH0 (BCM_TCS_CMD commit and valid, vote_x = vote_y = 0x3fff: the BCMs take their top level)
     * and sets ACV's bit 3, sm8250's bcm_acv enable_mask (Linux icc-rpmh sm8250.c). */
    c.bandwidth_count = A650GmuDdrVote ? 2 : 1;
    c.ddr_count = 3; c.ddr_wait_mask = 1;
    c.ddr_addresses[0] = mc0; c.ddr_addresses[1] = sh0; c.ddr_addresses[2] = acv;
    c.ddr_data[0][0] = c.ddr_data[0][1] = c.ddr_data[0][2] = 0x40000000u;
    if (A650GmuDdrVote) {
        c.ddr_data[1][0] = c.ddr_data[1][1] = 0x60000000u | (0x3fffu << 14) | 0x3fffu;
        c.ddr_data[1][2] = 0x60000008u;
    }
    c.cnoc_count = 1; c.cnoc_wait_mask = 1;
    c.cnoc_addresses[0] = cn0;
    c.cnoc_data[0][0] = 0x40000000u; c.cnoc_data[1][0] = 0x60000001u;
    if (AdrenoHfiStartupValidate(&c) != AdrenoHfiStartupSuccess) return AdrenoHfiStartupInvalid;
    *out = c;
    return AdrenoHfiStartupSuccess;
}
