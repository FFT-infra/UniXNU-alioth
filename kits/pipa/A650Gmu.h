/* SPDX-License-Identifier: BSD-2-Clause-Patent */
/* pipa's Adreno 650 GMU cold start, after UniXNU-sheng's ShengGmu.c (Adreno 740), in the order of Linux v6.6 a6xx_gmu.c
 * for the a650 family: a6xx_gmu_rpmh_init (the GPU RSC and, with the PDC sequence held by the AOP, only the PDC start),
 * a6xx_rpmh_stop, then a6xx_gmu_resume's cold path (a6xx_gmu_fw_start: the FAL interfaces, register retention, the RSC
 * wake-up, the firmware blocks into ITCM, DTCM and the instruction cache region, the boot registers, a6xx_gmu_power_config,
 * a6xx_gmu_start, a6xx_gmu_hfi_start) and a6xx_hfi_start (performance table, bandwidth table, core firmware start, start)
 * through UniXNU's kits/AdrenoFirmware HFI transport. Register dword offsets are a6xx_gmu.xml.h's and a6xx.xml.h's (the PDC
 * block), relative to the GMU block 0x3d6a000, the RSCC block 0x3de0000 and the PDC block 0xb290000 of the tablet's device
 * tree. No clock, power, SMMU or GX action happens here: the caller has CX on, the GMU clocks running and the GMU's own SMMU
 * context mapping the four memory regions. Host-tested by A650GmuTests.c. */
#ifndef PIPA_A650_GMU_H
#define PIPA_A650_GMU_H
#include "AdrenoHfiStartup.h"
#ifdef __cplusplus
extern "C" {
#endif
enum {A650GmuCore, A650GmuRscc, A650GmuPdc, A650GmuWindows};
#define A650_GMU_CORE_BYTES 0x30000u
#define A650_GMU_RSCC_BYTES 0x10000u
#define A650_GMU_PDC_BYTES 0x10000u
#define A650_GMU_CHIP_ID 0x06050002u   /* DTS qcom,chipid */
typedef enum {
    A650GmuSuccess, A650GmuArgument, A650GmuFirmware, A650GmuBusy, A650GmuIoError, A650GmuStateMismatch, A650GmuTimeout,
    A650GmuClockError, A650GmuPoisoned, A650GmuFault, A650GmuProtocolError
} A650GmuStatus;
typedef struct {
    void *cookie;
    size_t bytes[A650GmuWindows];
    int (*read32)(void *, unsigned window, size_t byte_offset, uint32_t *);
    int (*write32)(void *, unsigned window, size_t byte_offset, uint32_t);
    int (*barrier)(void *);
} A650GmuIo;
typedef struct {
    void *cookie;
    int (*now_us)(void *, uint64_t *);
    int (*wait_us)(void *, unsigned);
} A650GmuServices;
/* A region the GMU reaches through its SMMU context, and the CPU's view of it (Normal non-cacheable or write-combined). */
typedef struct {volatile uint32_t *cpu; size_t bytes; uint64_t iova;} A650GmuMemory;
typedef struct {
    uint32_t chip_id;
    const uint8_t *firmware; size_t firmware_bytes;   /* a650_gmu.bin */
    A650GmuMemory icache, dummy, log, hfi;            /* a6xx_gmu_init for the a650: 0x4000, 0x60000000, then any */
    unsigned poll_budget;                              /* polls of 100 us, each poll also stopped at 10 ms */
} A650GmuConfig;
typedef struct {
    A650GmuIo io; A650GmuServices services; AdrenoHfi hfi;
    uint32_t poisoned, boot_attempted;
    AdrenoHfiStartup startup;
} A650GmuDevice;
typedef struct {
    uint32_t phase, write_attempts, read_attempts, poll_reads;
    uint32_t blocks, memory_changed, reset_verified;
    uint32_t rsc_stop_status, rscc_ack, firmware_version, firmware_init_result, hfi_status;
    uint32_t firmware_ready_observed, hfi_ready_observed;
    uint32_t ao_fault_status, cm3_fault_status;
    uint32_t resources_must_retain;
} A650GmuResult;
/* The RSC setup, firmware load and GMU release up to the HFI queues' readiness. One attempt per device; any failure after
 * the first write puts the GMU back in reset and poisons the device. */
A650GmuStatus A650GmuLoadAndStartHfi(A650GmuDevice *, const A650GmuConfig *, A650GmuResult *);
typedef struct {
    unsigned polls_per_command, wait_interval_us;
    uint64_t command_timeout_us, total_timeout_us;
} A650GmuStartupLimits;
typedef struct {
    A650GmuResult load;
    AdrenoHfiStartupResult protocol;
    AdrenoHfiStartupStatus protocol_status;
    A650GmuStatus health_status;
} A650GmuStartupResult;
/* The load, then the HFI startup messages with their acknowledgements; then the GMU2HOST interrupt clear and mask of
 * a6xx_gmu_resume. */
A650GmuStatus A650GmuColdStart(A650GmuDevice *, const A650GmuConfig *, const AdrenoHfiStartupConfig *,
    const A650GmuStartupLimits *, A650GmuStartupResult *);
/* After a complete cold start: GX power through the GMU, as Linux brings it up. a6xx_gmu_set_initial_freq ->
 * a6xx_hfi_set_freq sends GX_BW_PERF_VOTE {ack_type 1 (blocking), perf_index, bandwidth 0}; hw_init's
 * a6xx_gmu_set_oob(GMU_OOB_GPU_SET) sets HOST2GMU bit 30 and waits for GMU2HOST bit 31 (100 us apart, 10 ms), then clears it.
 * GX is on when SPTPRAC_PWR_CLK_STATUS shows neither GX_HM_GDSC_POWER_OFF (bit 6) nor GX_HM_CLK_OFF (bit 7)
 * (a6xx_gmu_gx_is_on). perf_index 1..7 picks the GPU level of A650GmuBuildStartup's table (305 to 670 MHz). */
typedef struct {
    uint32_t vote_status, vote_error, oob_polls, oob_info, gx_before, gx_after;
    uint32_t gx_on;
} A650GmuGxResult;
/* Nonzero: the bandwidth table gets a top DDR level and GX power votes it (A650Gmu.c). Set before A650GmuBuildStartup. */
extern unsigned A650GmuDdrVote;
A650GmuStatus A650GmuPowerGx(A650GmuDevice *, unsigned perf_index, A650GmuGxResult *);
/* The HFI tables for pipa: the GPU levels of the DTS gpu-opp-table_v2 (305 to 670 MHz, RPMh levels 64 to 320) and the GMU's
 * 200 MHz at MIN_SVS (48), each with an off entry, voted from the command DB's gfx/mx and cx/mx level lists (16-bit words);
 * the bandwidth table is a650_build_bw_table's single "off" level, with the BCM addresses from the command DB instead of
 * Linux's constants (they agree for MC0, SH0 and ACV; CN0 is 0x50074 on this tablet, where Linux writes 0x500a4). */
AdrenoHfiStartupStatus A650GmuBuildStartup(const uint8_t *gfx, size_t gfx_bytes, const uint8_t *mx, size_t mx_bytes,
    const uint8_t *cx, size_t cx_bytes, uint32_t mc0, uint32_t sh0, uint32_t acv, uint32_t cn0, AdrenoHfiStartupConfig *out);
#ifdef __cplusplus
}
#endif
#endif
