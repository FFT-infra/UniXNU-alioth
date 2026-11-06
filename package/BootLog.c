// SPDX-License-Identifier: MIT
// Diagnostic log on UFS (boot argument sheng-log=1): every 2 s the text the board page would show (the diagnostic page, the live
// banner, the GPU rows, a caught panic message) and the kernel message buffer are written to the last 32 MiB of userdata through
// NativeUfs's bounded log window, so a run can be read back afterwards (scripts/read-sheng-log.py over the Linux log reader).
//
// Window layout (4096-byte blocks): 0 header, 1..32 the kernel message buffer (oldest first), 33.. text slots of 8 blocks in a ring,
// then the export (version 3, int-150): the newest complete blob that user space handed over (boot_log_export_put: the Metal plugin's
// pipeline caches, so the next image can start with them) lies in NativeUfs's export region, the 256 MiB below the window, which the
// reader fetches on its own. Its two halves alternate, so the header always names a blob that is wholly on the medium. (Version 2 kept a
// 16 MiB area at the window's tail; the UI's pipeline caches outgrew it in int-148 and int-149.)
// The header is written last in each round, so a reader never trusts a slot or an export the header does not count yet.
#include <kern/thread.h>
#include <kern/task.h>
#include <kern/processor.h>
#include <arm/cpu_data_internal.h>
#include <kern/clock.h>
#include <string.h>
#include <pexpert/pexpert.h>
#include <kern/misc_protos.h>
#include <IOKit/IOLib.h>

// proc_name(int, char *, int), from bsd/sys/proc.h. Declared by hand: that header pulls a second set of
// the exported bsd headers and -Werror rejects the resulting macro redefinitions. thread_get_thread_name
// (osfmk/kern/thread.h) is declared for the same reason: keeping thread.h out avoids its macro set.
extern void proc_name(int, char *, int) __attribute__((weak_import));
extern void thread_get_thread_name(thread_t, char *) __attribute__((weak_import));
extern int native_ufs_log_open(uint64_t *, uint64_t *, const char **) __attribute__((weak_import));
extern int native_ufs_log_write(uint64_t, uint32_t, const void *) __attribute__((weak_import));
extern int native_ufs_log_read(uint64_t, uint32_t, void *) __attribute__((weak_import));
// Same window, byte granular. native_ufs_log_read writes blocks*4096 bytes, so it needs a
// buffer that large; this one reads through a bounce block and fills only what is asked.
extern int native_ufs_log_read_bytes(uint64_t, uint32_t, void *) __attribute__((weak_import));
extern size_t qcom_usb_debug_write(const void *, size_t) __attribute__((weak_import));
extern uint64_t native_ufs_export_blocks(void) __attribute__((weak_import));
extern int native_ufs_export_write(uint64_t, uint32_t, const void *) __attribute__((weak_import));
extern int native_ufs_log_sync(void) __attribute__((weak_import));
// Rounds the storage path made the writer stand down for, because its host lock was held.
extern volatile uint32_t native_ufs_log_skips __attribute__((weak_import));
extern unsigned native_ufs_store_gpt_check(void) __attribute__((weak_import));
extern uint32_t cs_text_fixup_parsed __attribute__((weak_import));
extern uint32_t cs_text_fixup_applied __attribute__((weak_import));
extern char boot_progress_page_text[48][120] __attribute__((weak_import));
extern char native_gpu_rows[][256] __attribute__((weak_import));   // must match GpuProbe's definition
extern char boot_progress_banner_text[49][120] __attribute__((weak_import));
extern volatile unsigned boot_progress_banner_rows __attribute__((weak_import));
extern char boot_progress_ext[8][48] __attribute__((weak_import));
extern char boot_progress_ext_wide[2][112] __attribute__((weak_import));
extern char boot_progress_panic_text[2048] __attribute__((weak_import));
extern volatile uint32_t native_data_persist_state __attribute__((weak_import));
extern volatile uint32_t native_data_persist_header __attribute__((weak_import));
extern volatile uint32_t native_data_persist_gpt __attribute__((weak_import));
extern volatile uint32_t native_data_persist_gpt_after __attribute__((weak_import));
extern volatile uint64_t native_data_persist_pages __attribute__((weak_import));
extern volatile uint64_t native_data_persist_loaded __attribute__((weak_import));
extern volatile uint64_t native_data_persist_writes __attribute__((weak_import));
extern volatile uint64_t native_data_persist_syncs __attribute__((weak_import));
extern volatile uint64_t native_data_persist_errors __attribute__((weak_import));
extern volatile uint64_t native_data_persist_first_lba __attribute__((weak_import));
extern volatile int native_gpu_addon_files __attribute__((weak_import));
extern volatile int native_gpu_addon_written __attribute__((weak_import));
extern volatile int native_gpu_addon_same __attribute__((weak_import));
extern volatile int native_gpu_addon_failed __attribute__((weak_import));
extern volatile int native_gpu_addon_error __attribute__((weak_import));
extern volatile int native_gpu_addon_stage __attribute__((weak_import));
extern volatile int native_gpu_addon_status __attribute__((weak_import));
extern volatile int native_gpu_addon2_stage __attribute__((weak_import));
extern volatile int32_t sheng_wallpaper_reset[8] __attribute__((weak_import));
extern volatile int32_t sheng_wallpaper_scan[4] __attribute__((weak_import));
extern unsigned sheng_wallpaper_walked __attribute__((weak_import));
extern volatile uint64_t boot_progress_monitor_ticks __attribute__((weak_import));
extern volatile unsigned boot_progress_monitor_step __attribute__((weak_import));
extern volatile unsigned boot_progress_display_owned __attribute__((weak_import));
// The display's own swap counters, read by the supervisor's console line so a boot whose monitor and writer have both
// stopped still says whether any client ever submitted a frame.
extern volatile uint32_t native_legacy_swap_validation_calls __attribute__((weak_import));
extern volatile uint32_t native_legacy_swap_accepted_count __attribute__((weak_import));
extern volatile uint32_t native_legacy_swap_rejected_count __attribute__((weak_import));
extern volatile uint32_t native_legacy_swap_validation_reason __attribute__((weak_import));
extern volatile uint32_t native_legacy_swap_req[12] __attribute__((weak_import));
extern volatile uint32_t native_legacy_prepare_calls __attribute__((weak_import));
/* [0] status, [1] bytes, [2..] the bytes of the PosterBoard wallpaper choice. */
extern volatile int32_t sheng_wallpaper_cfg[130] __attribute__((weak_import));
extern volatile int32_t sheng_wallpaper_probe_ran __attribute__((weak_import));
extern volatile int32_t sheng_wallpaper_scan_ran __attribute__((weak_import));
extern volatile int32_t sheng_wallpaper_scan_app __attribute__((weak_import));
extern volatile int32_t sheng_wallpaper_scan_group __attribute__((weak_import));
extern volatile int32_t sheng_wallpaper_list_ran __attribute__((weak_import));
extern char sheng_wallpaper_names[256] __attribute__((weak_import));
extern volatile int32_t sheng_wallpaper_db_ran __attribute__((weak_import));
extern char sheng_wallpaper_db[400] __attribute__((weak_import));
extern volatile int native_gpu_addon2_status __attribute__((weak_import));
extern volatile int native_gpu_addon3_stage __attribute__((weak_import));
extern volatile int native_gpu_addon3_status __attribute__((weak_import));
extern volatile int native_cryptex_os_stage __attribute__((weak_import));
extern volatile int native_cryptex_os_status __attribute__((weak_import));
extern volatile int native_gestalt_cache_status __attribute__((weak_import));
extern volatile int native_ls_store_status __attribute__((weak_import));
extern volatile uint64_t native_data_persist_present __attribute__((weak_import));
extern volatile int native_staged_apps_stage __attribute__((weak_import));
extern volatile int32_t native_buttons_stats[16] __attribute__((weak_import));
extern volatile int32_t native_backlight_stats[16] __attribute__((weak_import));
extern volatile int native_staged_apps_dirs __attribute__((weak_import));
extern volatile int native_staged_apps_files __attribute__((weak_import));
extern volatile int native_staged_apps_mib __attribute__((weak_import));
extern volatile int native_staged_apps_failed __attribute__((weak_import));
extern volatile int native_staged_apps_error __attribute__((weak_import));
extern char native_staged_apps_path[96] __attribute__((weak_import));
extern const char *boot_progress_tag(void) __attribute__((weak_import));
extern unsigned boot_ui_trace_role_rows(unsigned, char [][120], unsigned, unsigned) __attribute__((weak_import));
extern unsigned boot_ui_trace_hot_rows(char [][120], unsigned) __attribute__((weak_import));
extern unsigned long vm_kernel_slide;
extern volatile uint64_t native_gop_wait_stats[7] __attribute__((weak_import));
/* Direct scanout (NativeLegacyDisplay.cpp): the fourteen counters the display keeps while the DPU pipelines read the compositor's
 * surface instead of a copy of it. They were written from the start and never printed, so a boot that took the direct path and one
 * that fell back to the GPU copy read the same from the log. Index meaning, from the increments in NativeLegacyDisplay.cpp:
 *  [0] flips issued  [1] surfaces mapped into a slot  [2] slot map refused  [3] latches over 50 ms
 *  [4] longest latch us  [5] total latch us  [6] slots recycled  [7] no free slot at flip time
 *  [8] flips back to the boot loader's memory  [9] flips that had to wait for the GPU
 *  [10] total wait-for-GPU us  [11] longest wait us  [12] wait-for-GPU given up at 250 ms  [13] waits of 8 ms or more */
extern volatile uint32_t native_legacy_direct[14] __attribute__((weak_import));
/* Where the direct path stopped when it did not run: 1 no request, 2 not primary, 3 waiting for the NativeUfs window, 4 window
 * present, 5 mapped, 6 gave up. Printed whenever the request was made, so "never asked" and "asked and refused" are separable. */
extern volatile uint32_t native_legacy_direct_stage __attribute__((weak_import));
extern volatile uint32_t native_legacy_direct_flip_branch __attribute__((weak_import));
/* Where the last flip() got to, and what CTL 0's CTL_TOP (0x14) and CTL_FLUSH (0x18) read. A hang inside a store leaves the
 * stage at the step before it, which names the store that stopped. [0] CTL_TOP before the flush, [1] CTL_FLUSH read back after. */
extern volatile uint32_t native_legacy_direct_flip_stage __attribute__((weak_import));
extern volatile uint32_t native_legacy_direct_ctl[2] __attribute__((weak_import));
/* After a flip: [0]/[1] VIG 0/1's SRC0_ADDR read back, [2]/[3] 1 when the store took. flips counts stores, not frames -- fix128
 * flipped 550 times while the frame buffer's checksum stayed the same, so this says whether the register took the address. */
extern volatile uint32_t native_legacy_direct_vig[4] __attribute__((weak_import));
/* The swap queue's own gates (NativeLegacyDisplay.cpp stat(index)): [0] swaps validated, [1] kickOnce entered, [2] skipped because
 * transferred, [3] a precondition failed, [4] no pending swap or it was not validated, [5] a latch was still outstanding, [9] a
 * direct flip completed, [11] a retire completed, [15] the deepest validated queue. When flips stops growing, one of these is the
 * gate that stopped it: fix118b read flips=99 for the last 104 s, and only [4] can explain a queue that simply went empty. */
extern volatile uint32_t native_legacy_swapstat[16] __attribute__((weak_import));
extern volatile uint32_t native_adreno_fence_wait[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_stall[24] __attribute__((weak_import));
/* CP_RB_WPTR writes and their GMU AHB fence: [0] writes, [1] writes that needed a repeat, [2] repeats, [3] writes still
 * not fenced after 200 tries. A write whose fence never clears is the board's own evidence for "the CP's AHB path is
 * not answering", which is the one hypothesis every round since fix54 has assumed and never measured. */
extern volatile uint32_t native_adreno_wptr_fence[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_close_step[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_cp_aperture[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_table_switches[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_ttbr0_tlb[3] __attribute__((weak_import));
/* [0] hangs, [1] restarts, [2] failed restarts, [3] the last hung fence, [4] the last restart s IOReturn, [5] replayed, [6] hangs with a guilty job, [7] hangs of the kernel s own VM. */
extern volatile uint32_t native_adreno_recovery[8] __attribute__((weak_import));
// Per-hang counters and the address-space residency: hangs/restarts/failed/replayed/guilty/kernel-VM, then
// live/max VMs with the last table error and step, then the SMMU faults ended before a restart and their FSR.
extern volatile uint32_t native_adreno_recovery_fault[5] __attribute__((weak_import));
extern volatile uint32_t native_adreno_hang_first[16] __attribute__((weak_import));
extern volatile uint32_t native_adreno_ring_pos[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_ring_stall[26] __attribute__((weak_import));
extern volatile uint32_t native_adreno_table_switches[8] __attribute__((weak_import));
/* Where the last recover() stopped: [0] the step (1 fence read, 2 restart hook, 3 table write, 4 nop encode, 5 replay vm, 6 replay submit), [1] the IOReturn or value there, [2] fence, [3] last submitted. */
extern volatile uint32_t native_adreno_recover_trace[4] __attribute__((weak_import));
extern uint32_t native_gpu_restart_step;
extern volatile uint32_t native_adreno_wfi_dropped __attribute__((weak_import));
extern volatile uint32_t native_adreno_wfi_try[5] __attribute__((weak_import));
extern volatile uint32_t native_adreno_trace_stage __attribute__((weak_import));
extern volatile uint32_t native_adreno_stall_mem[59] __attribute__((weak_import));
extern volatile uint32_t native_adreno_unstick[15] __attribute__((weak_import));
extern volatile uint64_t native_adreno_wait_outer[4] __attribute__((weak_import));
/* Timer deadlines from ArmCpu: how far ahead of the counter each core's virtual timer sits after
 * rtclock_intr, and how often that lead was under 1000 ticks. A deadline in the past makes XNU
 * program its 10-tick minimum, so the timer fires far more often than any thread asked, which is
 * what the wakeups monitor reports on this board. */
extern volatile int32_t armcpu_timer_ahead[8] __attribute__((weak_import));
extern volatile uint32_t armcpu_timer_short[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_export_stats[4] __attribute__((weak_import));
extern volatile int32_t native_touch_strokes[16][7] __attribute__((weak_import));
extern volatile uint32_t native_touch_stroke_count __attribute__((weak_import));
extern volatile uint32_t native_wifi_stats[29] __attribute__((weak_import));
extern volatile uint32_t native_wifi_dump[108] __attribute__((weak_import));
extern volatile uint32_t native_wifi_pmic[472] __attribute__((weak_import));
extern volatile uint32_t native_wifi_ep[16] __attribute__((weak_import));
extern volatile uint32_t native_wifi_pon[12] __attribute__((weak_import));
extern volatile uint32_t native_wifi_cfg[32] __attribute__((weak_import));
extern volatile uint32_t native_wifi_chip[16] __attribute__((weak_import));
extern volatile uint32_t native_wifi_ev[48] __attribute__((weak_import));
extern volatile uint32_t native_wifi_ldo[24] __attribute__((weak_import));
extern volatile uint32_t native_wifi_mhi[16] __attribute__((weak_import));
extern volatile uint32_t native_wifi_mhi_ms[8] __attribute__((weak_import));
extern char native_wifi_text[512][112] __attribute__((weak_import));
extern volatile uint32_t native_wifi_text_n __attribute__((weak_import));
extern char native_wifi_note[128][112] __attribute__((weak_import));
extern volatile uint32_t native_wifi_note_n __attribute__((weak_import));
extern int native_ufs_extra_fault(uint32_t *) __attribute__((weak_import));
extern struct {uint32_t stage, scans, frames, beacons, bss_seen; int last_err;} nw_wifi_stats __attribute__((weak_import));
extern volatile uint64_t nw_wifi_where[4] __attribute__((weak_import));
extern volatile uint32_t native_dpu_stats[20] __attribute__((weak_import));
/* DpuProbe.cpp's entry, called from the tick below. This is C, so the C++ side's extern "C" is spelled by the symbol itself:
 * alioth_dpu_probe_tick is defined at C linkage there. Weak so the archive still links when the probe is not built. */
extern void alioth_dpu_probe_tick(unsigned now) __attribute__((weak_import));
/* The GPU frequency governor, once a second: [0] level in force, [1] changes, [2] submissions last second, [3] ticks. */
// native_gpu_freq_tick is deliberately not declared here any more: see the step-4 comment in boot_log_tick.
extern volatile uint32_t native_gpu_freq[9] __attribute__((weak_import));
/* The display bank's stage-1 window (NativeUfs's DpuWindow.h). native_ufs_dpu_state's bit 31 means the table was installed and
 * survived the 50 ms fault watch; any other value is the step that refused, and 0 means the window was never attempted.
 * Without this row a boot that took the direct path and one that refused it read the same, which is what made fix85's stop
 * unattributable. native_ufs_dpu_smmu: [0] = bit31 read | valid SMRs << 8 | banks << 16, [4 + 9k] = SMR index | bank << 16
 * for display stream k. */
extern volatile uint32_t native_ufs_dpu_state __attribute__((weak_import));
extern volatile uint32_t native_ufs_dpu_smmu[24] __attribute__((weak_import));
extern volatile uint32_t native_touch_jitter[3] __attribute__((weak_import));
extern volatile uint32_t native_touch_hold[14] __attribute__((weak_import));
extern char native_lookup_log[160][112] __attribute__((weak_import));
extern volatile unsigned native_lookup_count __attribute__((weak_import));
extern volatile uint32_t native_dpu_pipes[54] __attribute__((weak_import));
extern volatile int32_t native_adsp_stats[16] __attribute__((weak_import));
extern volatile int32_t native_audio_stats[16] __attribute__((weak_import));
extern char native_adsp_text[96] __attribute__((weak_import));
extern volatile int32_t native_battery_stats[16] __attribute__((weak_import));
extern volatile uint32_t native_swipe_stats[16] __attribute__((weak_import));
extern volatile int32_t native_touch_extent[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_method_stats[20][4] __attribute__((weak_import));
/* Where a submission's time goes, in mach_absolute_time ticks summed over the successful ones (NativeAdrenoSubmit.cpp:19-23):
 * [0] validation, [1] the sweep of completed jobs, [2] the write-combine re-assertion and audit, [3] the BO publication (cache
 * maintenance), [4] the coherence sample, [5] the IB record and the ring write, [6] the count. Divided by the count it names the
 * part of a submission that costs milliseconds, which the [gpu methods] row cannot: that row has one number for the whole call. */
extern volatile uint64_t native_adreno_submit_phase[7] __attribute__((weak_import));
extern volatile uint64_t native_adreno_vm_phase_us[2][8] __attribute__((weak_import));
/* Per map kind: count, total us, total spans (NativeAdrenoVm.cpp:350-353, :400-404). kind 0 is driver memory, 1 an import
 * mapped exclusive, 2 an import that needed the full alias check. The porting guide asks for this row next to [vm map]. */
extern volatile uint64_t native_adreno_vm_kind_us[5][3] __attribute__((weak_import));
extern volatile uint32_t native_adreno_map_fail[7][2] __attribute__((weak_import));
extern volatile uint32_t native_adreno_pool_stats[8] __attribute__((weak_import));
extern volatile uint32_t native_adreno_wc_audit[6] __attribute__((weak_import));
extern volatile uint32_t native_adreno_submit_ibs[4] __attribute__((weak_import));
extern volatile uint32_t native_touch_edge_stats[4] __attribute__((weak_import));
extern volatile uint32_t native_touch_hid_multi[3] __attribute__((weak_import));
extern volatile uint32_t native_touch_frames __attribute__((weak_import));
extern volatile uint32_t native_touch_valid __attribute__((weak_import));
extern volatile uint32_t native_touch_decoded __attribute__((weak_import));
extern volatile uint32_t native_touch_seq __attribute__((weak_import));
extern volatile uint32_t native_touch_count __attribute__((weak_import));
extern volatile unsigned native_touch_frame_raw[8] __attribute__((weak_import));
extern volatile uint32_t native_touch_loop_step __attribute__((weak_import));
extern volatile unsigned native_touch_frame_rc __attribute__((weak_import));
extern volatile unsigned native_touch_irq_low __attribute__((weak_import));
extern volatile uint32_t native_touch_core __attribute__((weak_import));
// One declarator per line: the attribute belongs to the declarator it follows, so a list leaves
// every name but the last strongly referenced and `&sym ? ...` then reads as always true (-Werror).
extern volatile uint32_t armcpu_core_timer[8] __attribute__((weak_import));
// Timer interrupts whose deadline was already past when the core got to them (fewer than 1000 ticks
// ahead), and the last one's margin. A blocked IOSleep that never returns needs a missed deadline on
// some core; these two say whether that is happening and on which.
extern volatile uint32_t armcpu_timer_short[8] __attribute__((weak_import));
extern volatile int32_t armcpu_timer_ahead[8] __attribute__((weak_import));
extern volatile uint32_t armcpu_core_sgi[8] __attribute__((weak_import));
extern volatile uint32_t armcpu_smp_running __attribute__((weak_import));
// Per-secondary bring-up step (1 nub started .. 7 running) and the check that failed, so the CPUT
// row can say whether a core that stops taking interrupts ever claimed to be running at all.
extern volatile uint32_t armcpu_smp_step[8] __attribute__((weak_import));
extern volatile uint32_t armcpu_smp_fail[8] __attribute__((weak_import));
// The liveness watchdog lives in ArmCpu: this file declares a core, ArmCpu records it, and with
// armcpu-deaf-skip=1 its signalCPU stops poking that core. Both counters are here so a console row
// can show the watchdog ran instead of leaving it to be inferred from an absent effect.
extern volatile uint32_t armcpu_core_deaf_mask __attribute__((weak_import));
extern volatile uint32_t armcpu_core_deaf_events __attribute__((weak_import));
extern volatile uint64_t armcpu_ipi_skipped_deaf __attribute__((weak_import));
extern void armcpu_core_note_deaf(uint32_t core) __attribute__((weak_import));
// Reads the suspect core's own redistributor frame from a healthy core: pending, enabled, active and WAKER.
// Pending-but-not-taken says the core is masked or halted; cleared enables say its GIC was turned off;
// nothing pending says no sender targets it.
extern int armcpu_core_gicr_probe(uint32_t core, uint32_t out[5]) __attribute__((weak_import));
// The phase that core last wrote, and the count (units of 1024) at which it entered the handler. A deaf
// core with phase 2 was inside the timer handler, 4 or 5 inside the IPI handler, 7 inside a device
// handler, and 0 or 6 was not in a handler at all.
extern volatile uint32_t armcpu_core_phase[8] __attribute__((weak_import));
extern volatile uint32_t armcpu_core_irq_enter[8] __attribute__((weak_import));
extern volatile uint64_t armcpu_timer_interrupts __attribute__((weak_import));
extern volatile uint64_t armcpu_ipi_interrupts __attribute__((weak_import));
extern volatile uint64_t armcpu_ipi_sent __attribute__((weak_import));
// quiesceCPU() turns the core's virtual timer off (CNTV_CTL=0) and its Group 1 interrupts off
// (ICC_IGRPEN1_EL1=0); haltCPU() calls it. Only initCPU turns them back on, so a core that halts and
// is woken by a path that does not re-init it stops taking timer interrupts, and any thread blocked
// on a deadline there never wakes. These two counters say whether that path runs at all.
extern volatile uint32_t armcpu_quiesce_calls __attribute__((weak_import));
extern volatile uint32_t armcpu_halt_calls __attribute__((weak_import));
extern volatile uint32_t armcpu_irq_phase __attribute__((weak_import));
// boot-ui-trace's worker: how many times it was entered, how many captures it started, and whether one
// is still in flight. Six "STK none" rows on 2026-11-06 could mean the worker never reached its 44th
// call, or that a capture started and never finished; these four numbers separate them.
extern volatile unsigned boot_process_attempts __attribute__((weak_import));
extern volatile unsigned boot_process_passes __attribute__((weak_import));
extern volatile unsigned boot_process_seen __attribute__((weak_import));
extern volatile unsigned boot_process_busy __attribute__((weak_import));
extern int boot_kthread_report(const char *, char *, unsigned) __attribute__((weak_import));
extern volatile uint32_t native_touch_fail_step __attribute__((weak_import));
extern volatile int32_t native_backlight_request[6] __attribute__((weak_import));
extern volatile uint64_t native_adreno_reassert_ticks __attribute__((weak_import));
extern volatile uint64_t native_adreno_cache_stats[5] __attribute__((weak_import));
// NativeAdrenoPublish.cpp: per process name that opened the GPU, and the processes that asked the Metal discovery for its plugin.
struct native_adreno_client_record {char name[20]; uint32_t opens, refused, closes, slot, peak_mib, last_refusal;};
extern struct native_adreno_client_record native_adreno_client_log[16] __attribute__((weak_import));
/* The driver's own layout: NativeAdrenoPublish.cpp declares `struct NativeAdrenoClientText
 * {char name[20]; char text[64];}`. The reader used to add a second 64-byte line and a counter,
 * so every entry after the first was read at the wrong stride. */
struct native_adreno_client_text {char name[20]; char text[64];};
extern struct native_adreno_client_text native_adreno_client_text[8] __attribute__((weak_import));
/* [0] the marker count, [1..16] the plugin's newest progress markers, from NativeAdrenoPublish.cpp. */
extern volatile uint32_t native_adreno_metal_marks[17] __attribute__((weak_import));
extern volatile uint32_t native_adreno_mount_fail[12] __attribute__((weak_import));
// {site,count} pairs: which poison() call in NativeAdrenoSubmit.cpp refused, and how often.
extern volatile uint32_t native_adreno_submit_poison[6] __attribute__((weak_import));
// The hang-recovery step that refused (step, status) and the counters around it: hangs, restarts, failed.
extern volatile uint32_t native_adreno_recovery[8] __attribute__((weak_import));
/* The display service's own user-client record: whether any process opened it at all, which pid,
 * and the status of the last open. A boot that never takes the display splits in two here — nobody
 * opened it, or somebody opened it and never swapped. */
/* Plain externs: these live in LegacyDisplayProbe.cpp, which is linked into the same kernel
 * archive, and weak_import on a data symbol makes clang fold `&sym` to true (-Werror). */
extern volatile uint32_t native_legacy_client_calls, native_legacy_client_type;
extern volatile int native_legacy_client_pid, native_legacy_client_status;

extern volatile uint32_t native_adreno_window_overflow[4][3] __attribute__((weak_import));
extern volatile uint32_t native_adreno_client_stats[5][8] __attribute__((weak_import));
struct native_adreno_asker_record {char name[20]; uint32_t reads;};
extern struct native_adreno_asker_record native_adreno_metal_askers[12] __attribute__((weak_import));
extern char native_gpu_coherence_row[120] __attribute__((weak_import));
extern size_t boot_progress_profile_rows(char *, size_t) __attribute__((weak_import));
extern size_t boot_progress_spawn_rows(char *, size_t) __attribute__((weak_import));
extern volatile uint64_t native_gpu_idle_sleeps __attribute__((weak_import));
extern volatile uint64_t native_gpu_idle_spins __attribute__((weak_import));
extern volatile uint32_t native_adreno_coherent_skips[4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_io_coherent __attribute__((weak_import));
extern char native_cpufreq_row[120] __attribute__((weak_import));
extern uint32_t native_gpu_perf_vote[2] __attribute__((weak_import));
extern volatile uint32_t native_adreno_pool_status __attribute__((weak_import));
extern volatile uint32_t native_adreno_pool_wc __attribute__((weak_import));
extern volatile uint64_t native_adreno_fence_phys __attribute__((weak_import));
extern int (*native_adreno_dram_read32)(uint64_t, uint32_t *) __attribute__((weak_import));
extern int (*native_adreno_cached_read32)(uint64_t, uint32_t *) __attribute__((weak_import));
extern volatile uint32_t native_adreno_ring_log[4][4 + 112] __attribute__((weak_import));
extern volatile uint32_t native_adreno_ring_log_count __attribute__((weak_import));
extern volatile uint32_t native_adreno_ib_log[4][6 + 96 + 96 + 2 + 16 + 80] __attribute__((weak_import));
extern volatile uint32_t native_adreno_ib_log_count __attribute__((weak_import));
extern volatile uint64_t *native_adreno_boundary_log __attribute__((weak_import));
extern volatile uint32_t native_adreno_boundary_count __attribute__((weak_import));
extern volatile uint32_t native_adreno_boundary_lost __attribute__((weak_import));
/* The submission's coherence sample and the write-combined distrust count (NativeAdrenoSubmit.cpp:289-301): [0] samples taken,
 * [1..3] the cached and uncached reads of the first IB word, [4] submissions that found a dirty line and dropped write-combined,
 * [5..7] the first such sample. A third submission whose IB head reads as zeros is only "Metal wrote nothing" if [4] stays 0. */
extern volatile uint32_t native_adreno_coherence[8] __attribute__((weak_import));
extern unsigned native_legacy_properties_now(char *, unsigned) __attribute__((weak_import));
extern unsigned native_legacy_clients_dump(char *, unsigned) __attribute__((weak_import));
extern unsigned native_legacy_swap_timeline(char *, unsigned) __attribute__((weak_import));
extern unsigned native_legacy_prepare_report(char *, unsigned) __attribute__((weak_import));
extern unsigned native_legacy_vsync_counts(uint64_t *, unsigned *) __attribute__((weak_import));
extern volatile uint32_t native_legacy_direct_stage __attribute__((weak_import));
extern void boot_progress_panel_sample(unsigned *) __attribute__((weak_import));
extern volatile uint64_t native_gop_gpu_offers __attribute__((weak_import));
extern volatile uint64_t native_gop_gpu_completions __attribute__((weak_import));
extern volatile uint64_t native_gop_gpu_failures __attribute__((weak_import));
extern volatile uint64_t native_gop_gpu_abandoned __attribute__((weak_import));
extern char native_adreno_metal_rows[48][96] __attribute__((weak_import));
extern unsigned native_legacy_state_words(char *, unsigned) __attribute__((weak_import));
extern volatile uint32_t native_adreno_view_diag[6] __attribute__((weak_import));
extern int boot_reset_probe(char *, unsigned) __attribute__((weak_import));
extern int boot_reset_bootloader(char *, unsigned) __attribute__((weak_import));
extern int boot_reset_mark_bootloader(char *, unsigned) __attribute__((weak_import));
extern int boot_reset_watchdog(uint32_t, char *, unsigned) __attribute__((weak_import));
extern void boot_reset_pet(void) __attribute__((weak_import));
// A plain extern, not weak_import: it lives in this package (BootReset.c) and &symbol on a weak
// data symbol is a -Wpointer-bool-conversion error under -Werror. [0] armed, [1] pets so far.
extern volatile int32_t boot_reset_wdt_stats[4];
void boot_log_tick(unsigned now);
extern volatile uint64_t native_adreno_import_diag[10] __attribute__((weak_import));
extern volatile uint32_t native_adreno_import_stage __attribute__((weak_import));
extern volatile uint64_t native_adreno_import_live[3 + 8 * 4] __attribute__((weak_import));
extern volatile uint32_t native_adreno_import_persistent[2] __attribute__((weak_import));
extern volatile uint64_t native_adreno_import_fail_pa __attribute__((weak_import)), native_adreno_import_fail_offset __attribute__((weak_import));
// What the reset probe and the autoreset attempt saw, for the [reset] section of the log.
// The mark line is "mark: %08x -> %08x rc=%d | <pon line> pon_rc=%d", about 100 bytes with the
// PON text at 50, so 64 truncated it before pon_rc and left the PON verdict unreadable even when
// the write happened. 160 leaves room for a wider status word and a longer PON line.
static char boot_log_reset_probe_text[240], boot_log_reset_result[200], boot_log_reset_mark[160], boot_log_reset_watchdog[192];
// The kernel message buffer (bsd/sys/msgbuf.h); only these leading fields are read.
struct boot_log_msgbuf {int msg_magic, msg_size, msg_bufx, msg_bufr; char *msg_bufc;};
extern struct boot_log_msgbuf *msgbufp;

/* Two message-buffer snapshots, not one, and the second is frozen.
 *
 * copy_msgbuf (below) copies the newest LogMsgBlocks*LogBlock bytes of the kernel msgbuf. The
 * kernel msgbuf is CONFIG_MSG_BSIZE_DEV = 131072 (config/MASTER:286) and the region is the same
 * 131072 bytes, so keep == size and the copy is the WHOLE ring: every round stores the same
 * ~132 s window with ~2 s of new content. Rotating that copy across several blocks would only
 * stack overlapping windows a few seconds apart. It cannot lengthen the window, because the
 * bytes that have already fallen out of the kernel ring no longer exist to copy.
 *
 * What a failed kext probe needs is the opposite of a longer window: one snapshot taken while
 * the boot-time IOLog is still inside the ring, kept where later rounds cannot overwrite it.
 * IOLog does reach msgbuf (IOLib.cpp:1581 -> os/log.c:789 -> subr_prf.c:391); the loss was that
 * a fixed destination block is rewritten every round, so after ~132 s the boot-time probe text
 * was gone from the only copy on disk.
 *
 * So: the working snapshot stays at LogMsgFirst and keeps rotating content as before, and a
 * second snapshot is written ONCE at uptime LogFrozenAtS into LogFrozenFirst and never touched
 * again. The trigger is seconds, not rounds: a round is 2 s of sleep plus a UFS write and the
 * occasional GPT check or diagnostic export, so rounds and seconds drift apart. 100 s leaves
 * 32 s of margin under the ~132 s ring, so the frozen copy holds t=0..100 s: the whole
 * boot-time probe window. Its block number goes into the header (frozen_first, 0 = not taken)
 * so a reader finds it without guessing; frozen_round records which round actually took it.
 *
 * Cost: LogFrozenBlocks = 32 blocks = 128 KiB, the same as the working copy. LogSlotFirst moves
 * from 33 to 65, and slot_count from 509 to 507 in the 8192-block window. Blocks 33..64 were
 * unused tail before, so no other consumer moves.
 */
enum {LogBlock = 4096, LogMsgFirst = 1, LogMsgBlocks = 32,
      LogFrozenFirst = 33, LogFrozenBlocks = 32, LogFrozenAtS = 100,
      LogSlotFirst = LogFrozenFirst + LogFrozenBlocks,          /* 33 + 32 = 65 */
      LogSlotBlocks = 16, LogPeriodMs = 2000, LogMaxErrors = 3, LogRetryMs = 10000,
      LogExportStage = 64};   // export bytes staged per write: 256 KiB
struct boot_log_header {
  char magic[8];                 // "SHLOG001"
  uint32_t version, header_bytes;
  uint64_t boot_id;              // CNTPCT when the writer started: tells one run from the next
  uint64_t first_lba;            // absolute 4096-byte LBA of block 0 on LUN 0
  uint32_t window_blocks, msg_first, msg_blocks, msg_bytes, slot_first, slot_blocks, slot_count, slots_written;
  uint32_t uptime_s, rounds, write_errors, gpt_open, gpt_last, state;
  char tag[32];
  // version 3: the newest complete export at block export_first of the export region (export_blocks long, ending export_below blocks
  // below block 0 of this window); export_bytes 0 = nothing exported yet
  uint32_t export_first, export_blocks, export_bytes, export_generation;
  uint32_t export_below;
  // version 4: the diagnostic file export (BootDiagExport.c) at block diag_first of the export region, diag_bytes long (0 = none)
  uint32_t diag_first, diag_bytes;
  // version 5: the frozen early-boot msgbuf snapshot at block frozen_first (LogFrozenBlocks long), taken once at
  // uptime LogFrozenAtS and never rewritten; 0 = not taken yet. msg_first still names the live working snapshot.
  uint32_t frozen_first, frozen_round;
};
// Writer state for the board page: 0 off, 1 waiting for the window, 2 writing, 3 stopped after errors, 4 stopped (GPT changed).
volatile uint32_t boot_log_state, boot_log_rounds, boot_log_errors;
// Rounds the writer stood down for because the storage path held the host lock. A skip is not an
// error: the round is not counted, the slot index does not advance, and the next round is 2 s away.
static volatile uint32_t boot_log_skips;
volatile uint32_t boot_log_slots_hint;   // the writer's slots_written, for the console heartbeat
volatile uint64_t boot_log_first_lba;
static int boot_log_started;
// The timed return is the board's, not the writer's. The writer can block in a UFS write --
// measured on boot 43ba9162: rounds 3, uptime 4 s, write_errors 0, state 2 -- and while the
// deadline lived in its loop nothing restarted that board, so an unattended round never came
// back. This runs from the board monitor's once-a-second tick instead, which does not share a
// thread with the writer.
static uint64_t tick_start_cntpct;
static unsigned tick_seen_seq, tick_seen_at;
static int tick_reset_tried;
/* Once the deadline's reset has been attempted, stop petting the hardware watchdog: on this
 * board the software path returns 99 every time, so the bark is the only reset that leaves the
 * SoC, and it must be allowed to fire. */
static int reset_pet_stopped;
static uint32_t tick_autoreset;
static volatile unsigned log_sync_seq;
// Instrumentation for the [tick] log row: the autoreset clock's own counters, so a boot that never returns names which part of it stood still.
static volatile unsigned boot_log_tick_calls, boot_log_super_runs, boot_log_tick_elapsed, boot_log_super_returns, boot_log_super_elapsed;
// Which call inside boot_log_tick it last entered. The monitor was measured stopping with its own step at 4 (inside this
// function), so this counter names the call inside it: 1 in, 2 after the one-time start, 3 before DpuProbe, 4 after it,
// 5 after the GPU frequency tick, 6 after the phase heartbeat, 7 before the deadline.
static volatile unsigned boot_log_tick_step;
static volatile uint64_t boot_log_tick_ctr, boot_log_tick_freq;
static volatile int boot_log_super_start = -1;
/* Which step of a round the writer is in, for a round that never finishes. Every wait on that path
 * is supposed to be bounded (the SCSI commands carry deadlines and poison the controller on
 * timeout, which shows up as write_errors), so a writer that stops with write_errors still 0 is
 * stuck somewhere that is not a command. This says which call it entered and never left; the
 * board monitor prints it, because the writer cannot report on itself. */
static volatile unsigned log_phase;
enum {ResetSyncGrace = 10};
static uint8_t *boot_log_buffer;   // LogMsgBlocks blocks: the message buffer, then reused for a slot and the header

// The export: boot_log_export_put takes the blob's pieces in order (any thread) and writes them through a staging buffer straight into
// the half of the export region that does not hold the last complete blob; once the last piece is on the medium that blob becomes the
// complete one, and the writer's next header names it. The lock serialises pieces and guards the counters.
static IOLock *export_lock;
static uint8_t *export_stage;
static uint32_t export_region;   // blocks of the export region, 0 = none
static uint32_t export_total, export_filled, export_generation, export_target, export_ready, export_ready_bytes, export_ready_first;
// The diagnostic file export takes the half the ready blob does not hold; from then on the plugin's exports are refused (-7), so neither
// overwrites the other. diag_status: 0 not asked, 1 running, 2 done, negative failed.
static uint32_t diag_first, diag_bytes;
static int diag_owned;
volatile int boot_diag_status;
extern volatile uint32_t boot_diag_files __attribute__((weak_import));
extern volatile uint32_t boot_diag_skipped __attribute__((weak_import));
extern volatile uint32_t boot_diag_errors __attribute__((weak_import));
extern uint64_t boot_diag_scan(uint64_t) __attribute__((weak_import));
extern int boot_diag_stream(int (*)(const void *, uint32_t, void *), void *) __attribute__((weak_import));
int boot_log_export_put(uint32_t generation, uint32_t total, uint32_t offset, const void *data, uint32_t size) {
  if (!export_lock || !generation || !total || !data || !size || offset > total || size > total - offset) return -1;
  IOLockLock(export_lock);
  int status = 0;
  const uint32_t half = export_region / 2, stage = LogExportStage * LogBlock;
  if (!half || !&native_ufs_export_write) status = -4;
  else if (total > (uint64_t)half * LogBlock) status = -5;
  else if (diag_owned) status = -7;
  else if (export_ready & 0x80000000u) status = -10;   // a generation with the top bit set (a device's RAM dump) is kept
  if (!status && !export_stage) export_stage = (uint8_t *)IOMallocAligned(stage, LogBlock);
  if (!status && !export_stage) status = -2;
  if (!status && offset == 0) {
    export_generation = generation; export_total = total; export_filled = 0;
    export_target = export_ready && export_ready_first == 0 ? half : 0;
  }
  if (!status && (generation != export_generation || total != export_total || offset != export_filled)) status = -3;   // only in order
  const uint8_t *from = (const uint8_t *)data;
  while (!status && size) {
    const uint32_t at = export_filled % stage, n = size < stage - at ? size : stage - at;
    memcpy(export_stage + at, from, n);
    export_filled += n; from += n; size -= n;
    const int full = at + n == stage, done = export_filled == export_total;
    if (full || done) {
      const uint32_t used = at + n, blocks = (used + LogBlock - 1) / LogBlock;
      if (used % LogBlock) memset(export_stage + used, 0, blocks * LogBlock - used);
      if (!native_ufs_export_write(export_target + (export_filled - used) / LogBlock, blocks, export_stage)) {
        status = -6; export_generation = 0;   // the next generation starts over
      }
    }
    if (!status && done) {export_ready = export_generation; export_ready_bytes = export_total; export_ready_first = export_target;}
  }
  IOLockUnlock(export_lock);
  return status;
}

// The diagnostic export's writer: the same staging as boot_log_export_put, into its own half.
struct diag_sink {uint8_t *stage; uint32_t target, filled;};
static int diag_flush(struct diag_sink *k) {
  const uint32_t stage = LogExportStage * LogBlock, used = k->filled % stage ? k->filled % stage : stage;
  const uint32_t blocks = (used + LogBlock - 1) / LogBlock;
  if (used % LogBlock) memset(k->stage + used, 0, blocks * LogBlock - used);
  return native_ufs_export_write(k->target + (k->filled - used) / LogBlock, blocks, k->stage) ? 0 : -6;
}
static int diag_emit(const void *data, uint32_t size, void *arg) {
  struct diag_sink *k = arg;
  const uint32_t stage = LogExportStage * LogBlock;
  const uint8_t *from = data;
  while (size) {
    const uint32_t at = k->filled % stage, n = size < stage - at ? size : stage - at;
    memcpy(k->stage + at, from, n);
    k->filled += n; from += n; size -= n;
    if (at + n == stage && diag_flush(k)) return -6;
  }
  return 0;
}
static void diag_thread(void *arg, wait_result_t wait) {
  (void)arg; (void)wait;
  IOLockLock(export_lock);
  const uint32_t half = export_region / 2;
  struct diag_sink k = {NULL, export_ready && export_ready_first == 0 ? half : 0, 0};
  diag_owned = half != 0;
  IOLockUnlock(export_lock);
  int status = -4;
  if (half && &native_ufs_export_write && (k.stage = (uint8_t *)IOMallocAligned(LogExportStage * LogBlock, LogBlock))) {
    const uint64_t stream = boot_diag_scan((uint64_t)half * LogBlock - 16);
    const uint32_t head[4] = {0, 0, (uint32_t)stream, 0};
    if (!stream) status = -8;
    else {
      status = diag_emit("SHEXR001", 8, &k);
      if (!status) status = diag_emit(&head[2], 8, &k);
      if (!status) status = boot_diag_stream(diag_emit, &k);
      if (!status && k.filled % (LogExportStage * LogBlock)) status = diag_flush(&k);
      if (!status && k.filled != stream + 16) status = -9;
    }
    IOFreeAligned(k.stage, LogExportStage * LogBlock);
  }
  IOLockLock(export_lock);
  if (!status) {diag_first = k.target; diag_bytes = k.filled;}
  IOLockUnlock(export_lock);
  boot_diag_status = status ? status : 2;
}

static uint64_t log_ticks(void) {uint64_t v; __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(v)); return v;}

// Appends a text row without its trailing blanks; returns the new length (never past cap - 1).
static size_t put(char *out, size_t at, size_t cap, const char *text, size_t max) {
  size_t n = strnlen(text, max);
  while (n && (text[n - 1] == ' ' || text[n - 1] == 0)) --n;
  for (size_t i = 0; i < n && at + 2 < cap; ++i) {char c = text[i]; out[at++] = (c >= 32 && c < 127) ? c : '?';}
  if (at + 1 < cap) out[at++] = '\n';
  return at;
}

static size_t compose(char *out, size_t cap, uint32_t slot, unsigned now) {
  size_t at = 0;
  char head[96];
  snprintf(head, sizeof head, "#slot %u t=%us tag=%s", slot, now, &boot_progress_tag ? boot_progress_tag() : "");
  at = put(out, at, cap, head, sizeof head);
  if (&boot_progress_page_text) {
    at = put(out, at, cap, "[page]", 8);
    for (unsigned i = 0; i < 48; ++i) if (boot_progress_page_text[i][0]) at = put(out, at, cap, boot_progress_page_text[i], 120);
  }
  if (&boot_progress_banner_text && &boot_progress_banner_rows) {
    unsigned rows = boot_progress_banner_rows; if (rows > 49) rows = 49;
    at = put(out, at, cap, "[banner]", 10);
    for (unsigned i = 0; i < rows; ++i) at = put(out, at, cap, boot_progress_banner_text[i], 120);
  }
  if (&boot_progress_ext) {
    at = put(out, at, cap, "[ext]", 8);
    for (unsigned i = 0; i < 8; ++i) {char row[56]; snprintf(row, sizeof row, "X%u %.47s", i, boot_progress_ext[i]); at = put(out, at, cap, row, sizeof row);}
  }
  // The Data store's counters (NativeRamBlock), a row of their own: the ext slots they once shared are taken by other kits.
  if (&native_data_persist_state && native_data_persist_state) {
    char row[400];
    snprintf(row, sizeof row, "D st=%u hdr=%u gpt=%x/%x lba=%llx pres=%llu ld=%llu wr=%llu cm=%llu er=%llu pages=%llu", native_data_persist_state,
             native_data_persist_header, native_data_persist_gpt, native_data_persist_gpt_after, (unsigned long long)native_data_persist_first_lba,
             (unsigned long long)native_data_persist_present, (unsigned long long)native_data_persist_loaded, (unsigned long long)native_data_persist_writes,
             (unsigned long long)native_data_persist_syncs, (unsigned long long)native_data_persist_errors, (unsigned long long)native_data_persist_pages);
    at = put(out, at, cap, "[data]", 8);
    at = put(out, at, cap, row, sizeof row);
  }
  // The payload's GPU addon (kern_exec.c native_gpu_addon_install): files in it (-1 none), written, already equal, failed, last error; then
  // the stage/status of the boot-time addon mounts (stage 5 = mounted and both entries visible): plugin, launchd job, wallpaper.
  if (&native_gpu_addon_files && &native_gpu_addon_stage) {
    char row[120];
    snprintf(row, sizeof row, "A files=%d wr=%d same=%d fail=%d err=%d mount=%d/%d mount2=%d/%d mount3=%d/%d", native_gpu_addon_files,
             native_gpu_addon_written, native_gpu_addon_same, native_gpu_addon_failed, native_gpu_addon_error, native_gpu_addon_stage,
             native_gpu_addon_status, native_gpu_addon2_stage, native_gpu_addon2_status, &native_gpu_addon3_stage ? native_gpu_addon3_stage : 0,
             &native_gpu_addon3_status ? native_gpu_addon3_status : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  // sheng-wallpaper-reset=1: how far removing PosterBoard's stored wallpaper configuration got. [0] candidates, [i+1] each unlink
  // (0 removed, 2 absent, else errno). A red test image in the mounted .ca changed nothing on screen (fix142), so the next question
  // is whether the desktop reuses a stored configuration instead of reading the collection.
  if (&sheng_wallpaper_reset && sheng_wallpaper_reset[0]) {
    char wrow[160];
    size_t n = (size_t)snprintf(wrow, sizeof wrow, "[wallpaper reset] n=%d", sheng_wallpaper_reset[0]);
    for (unsigned i = 1; i <= (unsigned)sheng_wallpaper_reset[0] && i < 8 && n < sizeof wrow; ++i)
      n += (size_t)snprintf(wrow + n, sizeof wrow - n, " %d", sheng_wallpaper_reset[i]);
    // The container scan behind those unlinks: lookup result, last READDIR result, entries walked, containers matched. A row of
    // "n=3 2 2 2" says only that the three fixed paths were absent; these four say why the dynamic ones were never tried.
    if (&sheng_wallpaper_scan && n < sizeof wrow)
      n += (size_t)snprintf(wrow + n, sizeof wrow - n, " | ent=%d dir=%d ctr=%d grp=%d rm=%u",
                            sheng_wallpaper_scan[2], sheng_wallpaper_scan[3], sheng_wallpaper_scan[0],
                            sheng_wallpaper_scan[1] & 0xff, &sheng_wallpaper_walked ? sheng_wallpaper_walked : 0u);
    at = put(out, at, cap, wrow, sizeof wrow);
  }
  // What PosterBoard stored as the desktop's wallpaper: the plist names the collection and the variant, so no experiment has to
  // guess which .ca is drawn. Printed as text with non-printables as '.', 96 bytes to a row.
  if (&sheng_wallpaper_cfg && sheng_wallpaper_cfg[0] == 0 && sheng_wallpaper_cfg[1] > 0) {
    const int32_t total = sheng_wallpaper_cfg[1];
    char crow[110];
    for (int32_t at2 = 0; at2 < total; at2 += 96) {
      int n = snprintf(crow, sizeof crow, "[wallpaper cfg] %03d ", at2);
      for (int32_t i = at2; i < at2 + 96 && i < total && n < 100; ++i) {
        const uint32_t w = (uint32_t)sheng_wallpaper_cfg[2 + i / 4];
        const unsigned char c = (unsigned char)((w >> ((i % 4) * 8)) & 0xff);
        crow[n++] = (c >= 0x20 && c < 0x7f) ? (char)c : '.';
      }
      crow[n] = 0;
      /* This is the only row that says which collection PosterBoard stored, and it was persistent
       * log only until 2026-11-06, which is why a round could see bytes=148 and no content. */
      if (&qcom_usb_debug_write) {char cl[112]; const int cn = snprintf(cl, sizeof cl, "%s\n", crow);
        if (cn > 0) qcom_usb_debug_write(cl, (size_t)cn < sizeof cl ? (size_t)cn : sizeof cl - 1);}
      at = put(out, at, cap, crow, sizeof crow);
    }
  }
  {
    /* The three addon mounts the kernel performs, with their verdicts. The wallpaper overlay is
     * the third; a status other than 0 there means the mount the wallpaper depends on failed, and
     * a status of 0 with no picture means the mount landed somewhere nothing renders. */
    extern volatile int native_gpu_addon_stage __attribute__((weak_import));
    extern volatile int native_gpu_addon_status __attribute__((weak_import));
    extern volatile int native_gpu_addon2_stage __attribute__((weak_import));
    extern volatile int native_gpu_addon2_status __attribute__((weak_import));
    extern volatile int native_gpu_addon3_stage __attribute__((weak_import));
    extern volatile int native_gpu_addon3_status __attribute__((weak_import));
    if (&native_gpu_addon_status && &native_gpu_addon2_status && &native_gpu_addon3_status) {
      char crow[120];
      snprintf(crow, sizeof crow, "[addon] 1=%d/%d 2=%d/%d 3=%d/%d",
        native_gpu_addon_stage, native_gpu_addon_status,
        native_gpu_addon2_stage, native_gpu_addon2_status,
        native_gpu_addon3_stage, native_gpu_addon3_status);
      if (&qcom_usb_debug_write) {char cl[126]; const int cn = snprintf(cl, sizeof cl, "%s\n", crow);
        if (cn > 0) qcom_usb_debug_write(cl, (size_t)cn < sizeof cl ? (size_t)cn : sizeof cl - 1);}
      at = put(out, at, cap, crow, sizeof crow);
    }
  }
  if (&sheng_wallpaper_db_ran && sheng_wallpaper_db_ran && &sheng_wallpaper_db) {
    /* Page 1 of the poster store holds the schema, so these runs are the table names. */
    char crow[420];
    snprintf(crow, sizeof crow, "[wallpaper db] %.390s", sheng_wallpaper_db);
    if (&qcom_usb_debug_write) {char cl[426]; const int cn = snprintf(cl, sizeof cl, "%s\n", crow);
      if (cn > 0) qcom_usb_debug_write(cl, (size_t)cn < sizeof cl ? (size_t)cn : sizeof cl - 1);}
    at = put(out, at, cap, crow, sizeof crow);
  }
  if (&sheng_wallpaper_list_ran && sheng_wallpaper_list_ran && &sheng_wallpaper_names) {
    /* The entry names under the two directories that can hold the chosen poster. A plist in
     * Library/Preferences turned out to be CoreAnimation settings, so this is the next place to
     * look, and it is a listing rather than another guessed file. */
    char crow[300];
    snprintf(crow, sizeof crow, "[wallpaper list] %.270s", sheng_wallpaper_names);
    if (&qcom_usb_debug_write) {char cl[306]; const int cn = snprintf(cl, sizeof cl, "%s\n", crow);
      if (cn > 0) qcom_usb_debug_write(cl, (size_t)cn < sizeof cl ? (size_t)cn : sizeof cl - 1);}
    at = put(out, at, cap, crow, sizeof crow);
  }
  if (&sheng_wallpaper_scan_ran && sheng_wallpaper_scan_ran) {
    /* How many containers name PosterBoard in their metadata, from the two container roots. A zero
     * here means the app never made one, which is a different fault from a stale container id. */
    char crow[80];
    snprintf(crow, sizeof crow, "[wallpaper scan] app=%d group=%d",
             (int)sheng_wallpaper_scan_app, (int)sheng_wallpaper_scan_group);
    if (&qcom_usb_debug_write) {char cl[80]; const int cn = snprintf(cl, sizeof cl, "%s\n", crow);
      if (cn > 0) qcom_usb_debug_write(cl, (size_t)cn < sizeof cl ? (size_t)cn : sizeof cl - 1);}
    at = put(out, at, cap, crow, sizeof crow);
  }
  if (0) {
  } else if (&sheng_wallpaper_probe_ran && sheng_wallpaper_probe_ran) {
    /* The probe ran and said nothing: status 0 with no bytes is a file that opened empty, a
     * positive status is the errno of the open. Either way the row must print, or a stale
     * container UUID in the probe path looks exactly like a probe that never ran. */
    char crow[80];
    snprintf(crow, sizeof crow, "[wallpaper cfg] ran status=%d bytes=%d",
             &sheng_wallpaper_cfg ? (int)sheng_wallpaper_cfg[0] : -1,
             &sheng_wallpaper_cfg ? (int)sheng_wallpaper_cfg[1] : -1);
    /* put() writes the persistent log, not the console. This row is the only thing that says
     * whether the probe path still resolves, and the console is the only channel a round can be
     * read without booting the reader, so it goes to both. */
    if (&qcom_usb_debug_write) {char cl[80]; const int cn = snprintf(cl, sizeof cl, "%s\n", crow);
      if (cn > 0) qcom_usb_debug_write(cl, (size_t)cn < sizeof cl ? (size_t)cn : sizeof cl - 1);}
    at = put(out, at, cap, crow, sizeof crow);
  } else if (&sheng_wallpaper_cfg && sheng_wallpaper_cfg[0]) {
    char crow[60];
    snprintf(crow, sizeof crow, "[wallpaper cfg] unreadable status=%d", sheng_wallpaper_cfg[0]);
    at = put(out, at, cap, crow, sizeof crow);
  }
  // The GPU frequency the governor holds and what it saw: a boot that keeps level 1 while frames are drawn means the policy is
  // reading no submissions, and a boot that never leaves 7 means it never got a quiet second.
  if (&native_gpu_freq && native_gpu_freq[3]) {
    char frow[120];
    snprintf(frow, sizeof frow, "[gpu freq] level=%u changes=%u subs=%u ticks=%u want=%u vote=%u hfi=%u oob=%u err=%u",
             native_gpu_freq[0], native_gpu_freq[1], native_gpu_freq[2], native_gpu_freq[3], native_gpu_freq[5], native_gpu_freq[4],
             native_gpu_freq[6], native_gpu_freq[7], native_gpu_freq[8]);
    at = put(out, at, cap, frow, sizeof frow);
  }
  // The OS cryptex path (kern_exec.c native_cryptex_os_view; stage 5 = a WebKit extension found through /System/Cryptexes/OS) and the
  // removed MobileGestalt cache (0 removed, 2 none there).
  if (&native_cryptex_os_stage && &native_gestalt_cache_status) {
    char row[80];
    snprintf(row, sizeof row, "[cryptex os] stage=%d status=%d gestalt-cache=%d ls-store=%d", native_cryptex_os_stage, native_cryptex_os_status,
             native_gestalt_cache_status, &native_ls_store_status ? native_ls_store_status : -9);
    at = put(out, at, cap, row, sizeof row);
  }
  // The one-time copy of the OS volume's staged system apps into Data (kern_exec.c native_staged_apps_prepare): stage (2 copied, 3 copied
  // before, -1 no source), directories, files, MiB, failures and the last error.
  if (&native_staged_apps_stage) {
    char row[120];
    snprintf(row, sizeof row, "S st=%d dirs=%d files=%d mib=%d fail=%d err=%d", native_staged_apps_stage, native_staged_apps_dirs,
             native_staged_apps_files, native_staged_apps_mib, native_staged_apps_failed, native_staged_apps_error);
    at = put(out, at, cap, row, sizeof row);
    if (native_staged_apps_stage == 1 && &native_staged_apps_path) {
      snprintf(row, sizeof row, "S at %.100s", native_staged_apps_path);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // Power and volume buttons (ButtonProbe.cpp): stage (3 armed, negative the failed step), interrupts, reports, pressed mask, the power and
  // GPIO APIDs, interrupts of other APIDs cleared, PDC version, last error, spurious interrupts.
  if (&native_buttons_stats) {
    char row[120];
    snprintf(row, sizeof row, "B st=%d irq=%d rep=%d mask=%x apid=%d/%d off=%d pdc=%x err=%d spur=%d", native_buttons_stats[0], native_buttons_stats[1],
             native_buttons_stats[2], native_buttons_stats[3], native_buttons_stats[4], native_buttons_stats[5], native_buttons_stats[6],
             native_buttons_stats[7], native_buttons_stats[8], native_buttons_stats[9]);
    {
      char brow[110];
      snprintf(brow, sizeof brow, "Braw pon=%02x gpio=%02x pr=%d gr=%d", native_buttons_stats[10], native_buttons_stats[11],
               native_buttons_stats[12], native_buttons_stats[13]);
      at = put(out, at, cap, brow, sizeof brow);
    }
    at = put(out, at, cap, row, sizeof row);
  }
  // Panel brightness (AppleARMBacklight.cpp): stage (3 published, negative the failed step), requests, last level, the levels read back from
  // both drivers at start, transfer errors, last failed M_IRQ status, both engines' protocol and clock configuration, last error.
  if (&native_backlight_stats) {
    char row[150];
    snprintf(row, sizeof row, "L st=%d req=%d lvl=%d a=%d b=%d xerr=%d irq=%x ea=%x eb=%x err=%d goal=%d fade=%d", native_backlight_stats[0],
             native_backlight_stats[1], native_backlight_stats[2], native_backlight_stats[3], native_backlight_stats[4], native_backlight_stats[5],
             native_backlight_stats[6], native_backlight_stats[7], native_backlight_stats[8], native_backlight_stats[9],
             &native_backlight_request ? native_backlight_request[0] : -1, &native_backlight_request ? native_backlight_request[1] : -1);
    at = put(out, at, cap, row, sizeof row);
    // The request path and the engines: request timer runs, fades begun, interrupts from a and b, interrupt mode, timeouts, transfers.
    snprintf(row, sizeof row, "L path runs=%d fades=%d irq=%d/%d mode=%d tmo=%d done=%d empty=%d last=%x", &native_backlight_request ? native_backlight_request[2] : -1,
             &native_backlight_request ? native_backlight_request[3] : -1, native_backlight_stats[10], native_backlight_stats[11],
             native_backlight_stats[12], native_backlight_stats[13], native_backlight_stats[15],
             &native_backlight_request ? native_backlight_request[4] : -1, &native_backlight_request ? (unsigned)native_backlight_request[5] : 0u);
    at = put(out, at, cap, row, sizeof row);
    // How transfers complete: interrupts from each engine, interrupt mode (1 attached, 0 polling, else the attach result), timeouts,
    // attach tries and transfers done.
    snprintf(row, sizeof row, "L2 ia=%d ib=%d mode=%d to=%d tries=%d done=%d", native_backlight_stats[10], native_backlight_stats[11],
             native_backlight_stats[12], native_backlight_stats[13], native_backlight_stats[14], native_backlight_stats[15]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&boot_progress_ext_wide)
    for (unsigned i = 0; i < 2; ++i) {char row[120]; snprintf(row, sizeof row, "W%u %.111s", i, boot_progress_ext_wide[i]); at = put(out, at, cap, row, sizeof row);}
  // Every traced process's stacks (the banner shows only what fits the panel): role 0..3 are the boot-ui-focus slots, main thread in full
  // (symbolicate with unslid = address - cache slide), then its thread groups.
  if (&boot_ui_trace_role_rows) {
    static char rows[64][120];
    at = put(out, at, cap, "[stacks]", 10);
    for (unsigned role = 0; role < 4; ++role) {
      unsigned n = boot_ui_trace_role_rows(role, rows, 64, 2);   // 2: with each thread group's deeper frames
      for (unsigned i = 0; i < n; ++i) at = put(out, at, cap, rows[i], 120);
    }
  }
  /* Where this kernel's own long-running threads sit. boot_kthread_report() (BootUiTraceProbe.c) looks a
   * kernel thread up by name and prints its saved frames, so a boot whose touch loop stopped names the call
   * it stopped in, and a boot whose process-capture worker stopped shows that as well. Addresses are kernel
   * virtual, already unslid against the kernel: symbolicate them on the built kernel binary. */
  if (&boot_kthread_report) {
    static const char *const kthreads[] = {"alioth-touch", "boot_process_probe", "alioth-bt"};
    for (unsigned i = 0; i < sizeof kthreads / sizeof kthreads[0]; ++i) {
      char krow[200];
      if (boot_kthread_report(kthreads[i], krow, sizeof krow) != 0) continue;
      char row[224];
      snprintf(row, sizeof row, "KTH %s %s", kthreads[i], krow);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  if (&native_legacy_state_words) {
    static char words[240];
    if (native_legacy_state_words(words, sizeof words)) {at = put(out, at, cap, "[display state]", 16); at = put(out, at, cap, words, sizeof words);}
  }
  if (&native_legacy_prepare_report) {
    char row[220];
    native_legacy_prepare_report(row, sizeof row);
    at = put(out, at, cap, "[scanout prepare]", 18); at = put(out, at, cap, row, sizeof row);
  }
  if (&native_legacy_swap_timeline) {
    static char timeline[4096];
    if (native_legacy_swap_timeline(timeline, sizeof timeline)) {
      at = put(out, at, cap, "[swap timeline]", 16);
      for (char *line = timeline; *line && at + 130 < cap;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        at = put(out, at, cap, line, 120);
        if (!end) break;
        line = end + 1;
      }
    }
  }
  if (&native_adreno_metal_rows && native_adreno_metal_rows[0][0]) {
    at = put(out, at, cap, "[metal table]", 14);
    for (unsigned r = 0; r < 48 && at + 130 < cap; ++r)
      if (native_adreno_metal_rows[r][0]) at = put(out, at, cap, (const char *)native_adreno_metal_rows[r], 96);
  }
  // The framebuffer user clients' notification slots (is the render server's vsync registered?).
  if (&native_legacy_clients_dump) {
    static char dump[4096];
    unsigned n = native_legacy_clients_dump(dump, sizeof dump);
    if (n) {
      at = put(out, at, cap, "[mfb clients]", 14);
      for (char *line = dump; *line && at + 130 < cap;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        at = put(out, at, cap, line, 120);
        if (!end) break;
        line = end + 1;
      }
    }
  }
  // The display object's properties now (refresh intervals, timing, scale): the lines the original driver published.
  if (&native_legacy_properties_now) {
    static char props[7168];
    unsigned n = native_legacy_properties_now(props, sizeof props);
    if (n) {
      at = put(out, at, cap, "[display]", 10);
      // The XML is one long line; 116 characters per row with a prefix, and a tag per key on its own row so refresh/timing keys stand out.
      for (unsigned i = 0; i < n && at + 130 < cap; i += 116) {
        char row[124];
        snprintf(row, sizeof row, "D%03u %.116s", i / 116, props + i);
        at = put(out, at, cap, row, sizeof row);
      }
    }
  }
  // The traced processes' threads with the highest context-switch rate (they are idle in every stack sample but woken hundreds of times a second).
  if (&boot_ui_trace_hot_rows) {
    static char hot[96][120];
    unsigned n = boot_ui_trace_hot_rows(hot, 96);
    if (n) {char head[64]; snprintf(head, sizeof head, "[hot] kernel slide=%lx", (unsigned long)vm_kernel_slide); at = put(out, at, cap, head, sizeof head);}
    for (unsigned i = 0; i < n; ++i) at = put(out, at, cap, hot[i], 120);
  }
  // The four newest ring submissions (fence, ring range, then the words the CP reads) and the first words of their IBs: all that the
  // logs keep, so a stalled fence behind newer submissions is still shown.
  if (&native_adreno_ring_log_count && native_adreno_ring_log_count) {
    const uint32_t newest = native_adreno_ring_log_count - 1;
    for (unsigned back = 0; back < 4 && back < native_adreno_ring_log_count; ++back) {
      const volatile uint32_t *r = native_adreno_ring_log[(newest - back) & 3];
      char row[130];
      snprintf(row, sizeof row, "[ring] fence=%u ring %x..%x words=%u", r[0], r[1], r[2], r[3]);
      at = put(out, at, cap, row, sizeof row);
      const uint32_t words = r[3] < 112 ? r[3] : 112;
      for (uint32_t i = 0; i < words; i += 8) {
        int n = snprintf(row, sizeof row, "R%03x", i);
        for (uint32_t k = i; k < i + 8 && k < words && n > 0 && n < 110; ++k) n += snprintf(row + n, sizeof row - n, " %08x", r[4 + k]);
        at = put(out, at, cap, row, sizeof row);
      }
    }
  }
  if (&native_adreno_ib_log_count && native_adreno_ib_log_count) {
    const uint32_t newest = native_adreno_ib_log_count - 1;
    for (unsigned back = 0; back < 4 && back < native_adreno_ib_log_count; ++back) {
      const volatile uint32_t *r = native_adreno_ib_log[(newest - back) & 3];
      char row[130];
      snprintf(row, sizeof row, "[ib] fence=%u iova=%x%08x bytes=%u base=%x%08x off=%llx%s", r[0], r[2], r[1], r[3], r[5], r[4],
               (unsigned long long)(((uint64_t)r[2] << 32 | r[1]) - ((uint64_t)r[5] << 32 | r[4])),
               &native_adreno_coherence[4] ? (native_adreno_coherence[4] ? " wc-distrusted" : " wc-trusted") : "");
      at = put(out, at, cap, row, sizeof row);
      for (uint32_t i = 0; i < 96; i += 8) {
        int n = snprintf(row, sizeof row, "I%02x", i);
        for (uint32_t k = i; k < i + 8 && k < 96 && n > 0 && n < 110; ++k) n += snprintf(row + n, sizeof row - n, " %08x", r[6 + k]);
        at = put(out, at, cap, row, sizeof row);
      }
      // The same words through the kernel's CPU view: a client IB that reads as zeros one way and
      // real packets the other names the reader, not the GPU.
      snprintf(row, sizeof row, "[ib view] status hook=%x view=%x differ=%u", r[198] & 0xffff, r[198] >> 16, r[199]);
      at = put(out, at, cap, row, sizeof row);
      for (uint32_t i = 0; i < 96; i += 8) {
        int n = snprintf(row, sizeof row, "V%02x", i);
        for (uint32_t k = i; k < i + 8 && k < 96 && n > 0 && n < 110; ++k) n += snprintf(row + n, sizeof row - n, " %08x", r[102 + k]);
        at = put(out, at, cap, row, sizeof row);
      }
      // The IB's last 16 dwords: whether the fence write is even present at its end.
      {
        int n = snprintf(row, sizeof row, "T%02x", 0);
        for (uint32_t k = 0; k < 16 && n > 0 && n < 110; ++k) n += snprintf(row + n, sizeof row - n, " %08x", r[200 + k]);
        at = put(out, at, cap, row, sizeof row);
      }
      // Words 96..183: real packets at the head and zeros here would mean the client's write stopped.
      for (uint32_t i = 0; i < 80; i += 8) {
        int n = snprintf(row, sizeof row, "M%02x", 96 + i);
        for (uint32_t k = i; k < i + 8 && k < 80 && n > 0 && n < 110; ++k) n += snprintf(row + n, sizeof row - n, " %08x", r[216 + k]);
        at = put(out, at, cap, row, sizeof row);
      }
    }
  }
  if (&native_adreno_coherence[0] && native_adreno_coherence[0]) {
    char row[130];
    snprintf(row, sizeof row, "[coherence] samples=%u cached=%08x before=%08x after=%08x distrust=%u first %08x/%08x/%08x",
             native_adreno_coherence[0], native_adreno_coherence[1], native_adreno_coherence[2], native_adreno_coherence[3],
             native_adreno_coherence[4], native_adreno_coherence[5], native_adreno_coherence[6], native_adreno_coherence[7]);
    at = put(out, at, cap, row, sizeof row);
  }
  // The fence page, word by word as DRAM holds it and as the CPU's cacheable alias reads it (the CP writes the timestamps with a GPU-side write).
  if (&native_adreno_fence_phys && native_adreno_fence_phys && &native_adreno_dram_read32 && native_adreno_dram_read32 && &native_adreno_cached_read32 &&
      native_adreno_cached_read32) {
    char row[130];
    int n = snprintf(row, sizeof row, "[fence page] pa=%llx dram:", (unsigned long long)native_adreno_fence_phys);
    for (unsigned w = 0; w < 8 && n > 0 && n < 110; ++w) {uint32_t v = 0xdead; native_adreno_dram_read32(native_adreno_fence_phys + 4 * w, &v); n += snprintf(row + n, sizeof row - n, " %x", v);}
    at = put(out, at, cap, row, sizeof row);
    n = snprintf(row, sizeof row, "[fence page] cached:");
    for (unsigned w = 0; w < 8 && n > 0 && n < 110; ++w) {uint32_t v = 0xdead; native_adreno_cached_read32(native_adreno_fence_phys + 4 * w, &v); n += snprintf(row + n, sizeof row - n, " %x", v);}
    at = put(out, at, cap, row, sizeof row);
  }
  // Kernel GPU methods by number: calls, failures, last status and total time (the calls of the last 2 s are the difference of two slots).
  if (&native_adreno_method_stats) {
    char row[130];
    int n = snprintf(row, sizeof row, "[gpu methods] n:calls/fail/us");
    for (unsigned m = 1; m < 20; ++m) {
      if (!native_adreno_method_stats[m][0]) continue;
      if (n > 100) {at = put(out, at, cap, row, sizeof row); n = snprintf(row, sizeof row, "[gpu methods+]");}
      // calls/failures/last status/us. The status is the one number that says why a call failed; it was recorded and never printed,
      // so every round that saw failures could say how many but not what.
      n += snprintf(row + n, sizeof row - n, " %u:%u/%u/%x/%u", m, native_adreno_method_stats[m][0], native_adreno_method_stats[m][1],
                    native_adreno_method_stats[m][2], native_adreno_method_stats[m][3]);
    }
    at = put(out, at, cap, row, sizeof row);
  }
  // Where a submission's time goes. Ticks are converted with cntfrq_el0, as the [wc] row does.
  if (&native_adreno_submit_phase && native_adreno_submit_phase[6]) {
    uint64_t f = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (!f) f = 19200000;
    const uint64_t n = native_adreno_submit_phase[6];
    char row[200];
    snprintf(row, sizeof row,
             "[submit phase] n=%llu us/call valid=%llu sweep=%llu wcaudit=%llu publish=%llu coherence=%llu ring=%llu",
             (unsigned long long)n,
             (unsigned long long)(native_adreno_submit_phase[0] * 1000000ull / f / n),
             (unsigned long long)(native_adreno_submit_phase[1] * 1000000ull / f / n),
             (unsigned long long)(native_adreno_submit_phase[2] * 1000000ull / f / n),
             (unsigned long long)(native_adreno_submit_phase[3] * 1000000ull / f / n),
             (unsigned long long)(native_adreno_submit_phase[4] * 1000000ull / f / n),
             (unsigned long long)(native_adreno_submit_phase[5] * 1000000ull / f / n));
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_vm_phase_us && (native_adreno_vm_phase_us[0][0] || native_adreno_vm_phase_us[1][0])) {
    char row[170];
    for (unsigned k = 0; k < 2; ++k) {
      const volatile uint64_t *p = native_adreno_vm_phase_us[k];
      if (!p[0]) continue;
      snprintf(row, sizeof row, "[vm %s] n=%llu us: observe1=%llu %s=%llu batch=%llu observe2=%llu %s=%llu max=%llu MiB=%llu", k ? "unmap" : "map",
               (unsigned long long)p[0], (unsigned long long)p[1], k ? "acquire" : "publish", (unsigned long long)p[2], (unsigned long long)p[3],
               (unsigned long long)p[4], k ? "retire" : "-", (unsigned long long)p[5], (unsigned long long)p[6], (unsigned long long)(p[7] >> 20));
      at = put(out, at, cap, row, sizeof row);
    }
  }
  if (&native_adreno_vm_kind_us) {
    char row[170];
    static const char *const kinds[3] = {"owned", "import-exclusive", "import-checked"};
    size_t used = (size_t)snprintf(row, sizeof row, "[vm kinds]");
    for (unsigned k = 0; k < 3 && used < sizeof row; ++k) {
      const volatile uint64_t *p = native_adreno_vm_kind_us[k];
      if (!p[0]) continue;
      /* A mean is what a comparison between two builds reads; the raw total hides a small kind behind a large one. */
      const uint64_t mean_us = p[0] ? p[1] / p[0] : 0;
      used += (size_t)snprintf(row + used, sizeof row - used, " %s n=%llu mean=%lluus spans=%llu", kinds[k],
                               (unsigned long long)p[0], (unsigned long long)mean_us, (unsigned long long)p[2]);
    }
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_pool_stats) {
    char row[200];
    snprintf(row, sizeof row, "[bo pool] %u MiB (last reserve status %x, wc %x): %u BOs from it, %u fell back, in use %u MiB (peak %u) | scattered: %u BOs %u spans | import spans %u",
             native_adreno_pool_stats[0], native_adreno_pool_status, &native_adreno_pool_wc ? native_adreno_pool_wc : 0, native_adreno_pool_stats[1], native_adreno_pool_stats[2], native_adreno_pool_stats[3],
             native_adreno_pool_stats[4], native_adreno_pool_stats[5], native_adreno_pool_stats[6], native_adreno_pool_stats[7]);
    at = put(out, at, cap, row, sizeof row);
  }
  // GPU clients: name opens/refused/closes, live slot (- none), highest GPU address its mappings reached (MiB), last refusal.
  if (&native_adreno_client_log) {
    char row[400];
    size_t used = (size_t)snprintf(row, sizeof row, "[gpu clients]");
    // 16 entries, wrapped onto as many rows as it takes. At 8 the wallpaper extension (CollectionsPoster, the one that draws the
    // wallpaper) fell past the end and every reading of this row said it had never opened the device.
    for (unsigned i = 0; i < 16 && native_adreno_client_log[i].name[0]; ++i) {
      const struct native_adreno_client_record *r = &native_adreno_client_log[i];
      char slot[8];
      if (r->slot < 8) snprintf(slot, sizeof slot, "%u", r->slot); else snprintf(slot, sizeof slot, "-");
      const int wrote = snprintf(row + used, sizeof row - used, " %.19s %u/%u/%u s%s %uM r%x", r->name, r->opens, r->refused, r->closes,
                                 slot, r->peak_mib, r->last_refusal);
      if (wrote <= 0 || (size_t)wrote >= sizeof row - used) {at = put(out, at, cap, row, sizeof row); used = (size_t)snprintf(row, sizeof row, "[gpu clients+]"); continue;}
      used += (size_t)wrote;
    }
    at = put(out, at, cap, row, sizeof row);
  }
  // The plugin's own progress markers (0x68xx/0x6Axx, NativeAdrenoMetalPlugin.mm). They were recorded from the start and never
  // printed, so a process that read MetalPluginName but never opened the device could only be described as "reads 1" -- not where
  // inside the plugin it stopped. The wallpaper extension is exactly that case.
  if (&native_adreno_metal_marks && native_adreno_metal_marks[0]) {
    char mrow[220];
    size_t n = (size_t)snprintf(mrow, sizeof mrow, "[metal marks] n=%u", native_adreno_metal_marks[0]);
    for (unsigned i = 1; i < 17 && n < sizeof mrow; ++i) n += (size_t)snprintf(mrow + n, sizeof mrow - n, " %x", native_adreno_metal_marks[i]);
    at = put(out, at, cap, mrow, sizeof mrow);
  }
  // [gpu windows]: per client window the mappings that fell outside it (count, last size KiB, last end MiB), the latest failed mount (stage 1 no free
  // slot, 2 core open, 3 window scope; status; slot; count), then one line per process other than the render server: why its Metal device failed.
  if (&native_adreno_window_overflow && &native_adreno_mount_fail) {
    char row[300];
    snprintf(row, sizeof row, "[gpu windows] ovf %u/%uK/%uM %u/%uK/%uM %u/%uK/%uM %u/%uK/%uM | mount fail stage %u st %x slot %u n %u",
             native_adreno_window_overflow[0][0], native_adreno_window_overflow[0][1], native_adreno_window_overflow[0][2],
             native_adreno_window_overflow[1][0], native_adreno_window_overflow[1][1], native_adreno_window_overflow[1][2],
             native_adreno_window_overflow[2][0], native_adreno_window_overflow[2][1], native_adreno_window_overflow[2][2],
             native_adreno_window_overflow[3][0], native_adreno_window_overflow[3][1], native_adreno_window_overflow[3][2],
             native_adreno_mount_fail[0], native_adreno_mount_fail[1], native_adreno_mount_fail[2], native_adreno_mount_fail[3]);
    at = put(out, at, cap, row, sizeof row);
    if (&native_adreno_client_text)
      for (unsigned i = 0; i < 8 && native_adreno_client_text[i].name[0]; ++i) {
        if (!native_adreno_client_text[i].text[0]) continue;
        snprintf(row, sizeof row, "[gpu client text] %.19s: %.63s", native_adreno_client_text[i].name, native_adreno_client_text[i].text);
        at = put(out, at, cap, row, sizeof row);
      }
  }
  if (&native_adreno_metal_askers) {
    char row[400];
    size_t used = (size_t)snprintf(row, sizeof row, "[metal askers]");
    for (unsigned i = 0; i < 12 && native_adreno_metal_askers[i].name[0] && used < sizeof row; ++i)
      used += (size_t)snprintf(row + used, sizeof row - used, " %.19s:%u", native_adreno_metal_askers[i].name, native_adreno_metal_askers[i].reads);
    at = put(out, at, cap, row, sizeof row);
  }
  // The IO-coherence proof (GpuPowerProbe coherenceProbe) and the maintenance passes it saved: map, unmap, submission, completion.
  if (&native_gpu_coherence_row && native_gpu_coherence_row[0]) {
    char row[260];
    snprintf(row, sizeof row, "[coherence] %.110s | on=%u skipped map=%u unmap=%u submit=%u complete=%u | fence pauses slept=%llu spun=%llu",
             native_gpu_coherence_row,
             &native_adreno_io_coherent ? native_adreno_io_coherent : 0, &native_adreno_coherent_skips ? native_adreno_coherent_skips[0] : 0,
             &native_adreno_coherent_skips ? native_adreno_coherent_skips[1] : 0, &native_adreno_coherent_skips ? native_adreno_coherent_skips[2] : 0,
             &native_adreno_coherent_skips ? native_adreno_coherent_skips[3] : 0,
             (unsigned long long)(&native_gpu_idle_sleeps ? native_gpu_idle_sleeps : 0), (unsigned long long)(&native_gpu_idle_spins ? native_gpu_idle_spins : 0));
    at = put(out, at, cap, row, sizeof row);
  }
  // Edge contacts (drivers/ShengHidInput edge::): wrapper installed, contacts begun, of them from an edge, events skipped.
  if (&native_touch_edge_stats) {
    char row[100];
    snprintf(row, sizeof row, "[touch edge] installed=%u contacts=%u from_edge=%u skipped=%u", native_touch_edge_stats[0],
             native_touch_edge_stats[1], native_touch_edge_stats[2], native_touch_edge_stats[3]);
    at = put(out, at, cap, row, sizeof row);
  }
  // Multi-finger reports (drivers/ShengHidInput): reports with two or more fingers down, the most at once, lifts of vanished fingers.
  if (&native_touch_hid_multi) {
    char row[80];
    snprintf(row, sizeof row, "[touch multi] reports=%u max=%u lifts=%u", native_touch_hid_multi[0], native_touch_hid_multi[1],
             native_touch_hid_multi[2]);
    at = put(out, at, cap, row, sizeof row);
  }
  /* What the panel itself returned. frames/valid are the probe's own reads, decoded counts publishes
   * that carried a contact: a run where someone touched and decoded stays 0 says the panel response
   * never reached the decoder, which is a different fault from a HID report that was not delivered.
   * raw is the last frame's first eight bytes as read, so 0xff everywhere means "no response". */
  if (&native_touch_decoded) {
    char row[170];
    snprintf(row, sizeof row, "[touch raw] frames=%u valid=%u decoded=%u seq=%u count=%u loop=%u fail=%u raw=%02x%02x%02x%02x%02x%02x%02x%02x",
             native_touch_frames, native_touch_valid, native_touch_decoded, native_touch_seq, native_touch_count,
             native_touch_loop_step, native_touch_fail_step,
             native_touch_frame_raw[0], native_touch_frame_raw[1], native_touch_frame_raw[2], native_touch_frame_raw[3],
             native_touch_frame_raw[4], native_touch_frame_raw[5], native_touch_frame_raw[6], native_touch_frame_raw[7]);
    at = put(out, at, cap, row, sizeof row);
  }
  // IBs per GPU submission: submissions, IBs, the most in one, and those with more than 16 (a frame used to be split at 16).
  if (&native_adreno_submit_ibs) {
    char row[120];
    snprintf(row, sizeof row, "[gpu ibs] submissions=%u ibs=%u max=%u over16=%u", native_adreno_submit_ibs[0], native_adreno_submit_ibs[1],
             native_adreno_submit_ibs[2], native_adreno_submit_ibs[3]);
    at = put(out, at, cap, row, sizeof row);
  }
  // The submissions' write-combine work: audits, IB lines compared and found dirty, pages set back to write-combined, the re-assertion's
  // total time, and the CPU cache maintenance passes (count, total ms, MiB).
  if (&native_adreno_wc_audit && &native_adreno_reassert_ticks && &native_adreno_cache_stats) {
    uint64_t f = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    if (!f) f = 19200000;
    char row[200];
    // wc_pages is how many pages are write-combined right now (cache_stats[4]). Read against [gpu objects]: when it is 0 the
    // board is maintaining every BO on every submission because GpuProbe set native_adreno_wc_enabled=0 (a0ff4a7); when it
    // tracks the live BO pages, the write-combine path is the one running and passes/ms say what the audit costs.
    snprintf(row, sizeof row, "[wc] audited=%u lines=%u dirty=%u reset=%u reassert_ms=%llu | maintenance passes=%llu ms=%llu MiB=%llu | wc_pages=%llu",
             native_adreno_wc_audit[0], native_adreno_wc_audit[1], native_adreno_wc_audit[2], native_adreno_wc_audit[5],
             (unsigned long long)(native_adreno_reassert_ticks * 1000 / f), (unsigned long long)native_adreno_cache_stats[3],
             (unsigned long long)(native_adreno_cache_stats[0] * 1000 / f), (unsigned long long)(native_adreno_cache_stats[1] >> 20),
             (unsigned long long)native_adreno_cache_stats[4]);
    at = put(out, at, cap, row, sizeof row);
  }
  // CPU clusters and L3 as voted (CpuFreqProbe, refreshed every round) and the GPU's initial HFI perf/bandwidth vote.
  if (&native_cpufreq_row && native_cpufreq_row[0]) {
    char row[200];
    snprintf(row, sizeof row, "[clocks] %.118s | gpu perf %u bw %u", native_cpufreq_row, &native_gpu_perf_vote ? native_gpu_perf_vote[0] : 0,
             &native_gpu_perf_vote ? native_gpu_perf_vote[1] : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_export_stats && (native_adreno_export_stats[0] || native_adreno_export_stats[1])) {
    char row[120];
    snprintf(row, sizeof row, "[export] pieces=%u refused=%u generation=%u bytes=%u | complete generation=%u bytes=%u", native_adreno_export_stats[0],
             native_adreno_export_stats[1], native_adreno_export_stats[2], native_adreno_export_stats[3], export_ready, export_ready_bytes);
    at = put(out, at, cap, row, sizeof row);
  }
  // WiFi bring-up (WifiProbe.cpp): stage, GCC votes, PCIe0 GDSC, halted branches, WLAN_EN/SW_CTRL/PERST# pins, LTSSM, root port ID, link.
  if (&native_wifi_stats && native_wifi_stats[0]) {
    char row[400];
    snprintf(row, sizeof row, "[wifi] st=%d v8=%x v0=%x gdsc=%x off=%x gpio=%06x ltssm=%x id=%08x lnk=%08x phy=%x", (int)native_wifi_stats[0],
             native_wifi_stats[1], native_wifi_stats[2], native_wifi_stats[3], native_wifi_stats[4], native_wifi_stats[5], native_wifi_stats[6],
             native_wifi_stats[7], native_wifi_stats[8], native_wifi_stats[9]);
    if (native_wifi_stats[0] >= 10 || (int)native_wifi_stats[0] <= -10)
      snprintf(row + strlen(row), sizeof row - strlen(row), " pins=%06x pwr=%x pcs=%x dbg0=%x dbg1=%x ep=%08x ms=%u wlan=%08x perst=%08x clkreq=%08x rails=%x rsc=%08x tcs=%x/%x/%x/%x rsctest=%x/%x/%x", native_wifi_stats[10],
               native_wifi_stats[11], native_wifi_stats[12], native_wifi_stats[13], native_wifi_stats[14], native_wifi_stats[15], native_wifi_stats[16],
               native_wifi_stats[17], native_wifi_stats[18], native_wifi_stats[19], native_wifi_stats[20], native_wifi_stats[21], native_wifi_stats[22], native_wifi_stats[23], native_wifi_stats[24], native_wifi_stats[25],
               native_wifi_stats[26], native_wifi_stats[27], native_wifi_stats[28]);
    at = put(out, at, cap, row, sizeof row);
    if (&native_wifi_dump && native_wifi_stats[20])
      for (unsigned s = 0; s < 6; ++s) {
        char dump[200];
        int len = snprintf(dump, sizeof dump, "[wifi-tcs%u]", s);
        for (unsigned w = 0; w < 18; ++w) len += snprintf(dump + len, sizeof dump - (unsigned)len, " %x", native_wifi_dump[s * 18 + w]);
        at = put(out, at, cap, dump, sizeof dump);
      }
    if (&native_wifi_pmic && native_wifi_pmic[0]) {
      char row[260];
      snprintf(row, sizeof row, "[wifi-pmic] arb=%x apids=%u regs=%u/%u gpio3=%x before=%08x.%08x.%08x write=%x after=%08x.%08x.%08x", native_wifi_pmic[0],
               native_wifi_pmic[1], native_wifi_pmic[2] & 0xffff, native_wifi_pmic[2] >> 16, native_wifi_pmic[3], native_wifi_pmic[4], native_wifi_pmic[5], native_wifi_pmic[6],
               native_wifi_pmic[10], native_wifi_pmic[7], native_wifi_pmic[8], native_wifi_pmic[9]);
      at = put(out, at, cap, row, sizeof row);
      snprintf(row, sizeof row, "[wifi-rails] cmd-db addresses ldob15 smpg6 smpg4 smpe4 smpg5 smpg2: %x %x %x %x %x %x", native_wifi_pmic[11], native_wifi_pmic[12],
               native_wifi_pmic[13], native_wifi_pmic[14], native_wifi_pmic[15], native_wifi_pmic[16]);
      at = put(out, at, cap, row, sizeof row);
      snprintf(row, sizeof row, "[wifi-rails] enabled-by-pmic-read=%x looked-at=%x last-wait-ms=%u total-ms=%u", native_wifi_pmic[22] & 0xff, native_wifi_pmic[22] >> 8 & 0xff,
               native_wifi_pmic[22] >> 16, native_wifi_pmic[23]);
      at = put(out, at, cap, row, sizeof row);
      if (&native_wifi_ldo && (native_wifi_ldo[0] || native_wifi_ldo[12])) {
        snprintf(row, sizeof row, "[wifi-1v8] before: %08x/%08x %08x/%08x %08x/%08x %08x/%08x %08x/%08x %08x/%08x", native_wifi_ldo[0], native_wifi_ldo[1], native_wifi_ldo[2], native_wifi_ldo[3],
                 native_wifi_ldo[4], native_wifi_ldo[5], native_wifi_ldo[6], native_wifi_ldo[7], native_wifi_ldo[8], native_wifi_ldo[9], native_wifi_ldo[10], native_wifi_ldo[11]);
        at = put(out, at, cap, row, sizeof row);
        snprintf(row, sizeof row, "[wifi-1v8] after: %08x/%08x %08x/%08x %08x/%08x %08x/%08x %08x/%08x %08x/%08x", native_wifi_ldo[12], native_wifi_ldo[13], native_wifi_ldo[14], native_wifi_ldo[15],
                 native_wifi_ldo[16], native_wifi_ldo[17], native_wifi_ldo[18], native_wifi_ldo[19], native_wifi_ldo[20], native_wifi_ldo[21], native_wifi_ldo[22], native_wifi_ldo[23]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_cfg && (native_wifi_cfg[0] || native_wifi_cfg[8])) {
        snprintf(row, sizeof row, "[wifi-cfg] first(at %u): ep=%08x cmd=%08x bar0=%08x bar1=%08x lnk=%08x dbg0=%08x mem=%08x rcmd=%08x | end: ep=%08x cmd=%08x bar0=%08x bar1=%08x lnk=%08x dbg0=%08x mem=%08x rcmd=%08x restored=%u",
                 native_wifi_cfg[16], native_wifi_cfg[0], native_wifi_cfg[1], native_wifi_cfg[2], native_wifi_cfg[3], native_wifi_cfg[4], native_wifi_cfg[5], native_wifi_cfg[6], native_wifi_cfg[7],
                 native_wifi_cfg[8], native_wifi_cfg[9], native_wifi_cfg[10], native_wifi_cfg[11], native_wifi_cfg[12], native_wifi_cfg[13], native_wifi_cfg[14], native_wifi_cfg[15], native_wifi_cfg[17]);
        at = put(out, at, cap, row, sizeof row);
        snprintf(row, sizeof row, "[wifi-smmu] gfsr=%x gfsynr0=%x cb-fsr=%x far=%x.%x fsynr0=%x sctlr=%x tcr=%x ttbr0=%x cbar=%x", native_wifi_cfg[18], native_wifi_cfg[19], native_wifi_cfg[20], native_wifi_cfg[22], native_wifi_cfg[21], native_wifi_cfg[23], native_wifi_cfg[24], native_wifi_cfg[25], native_wifi_cfg[26], native_wifi_cfg[27]);
        at = put(out, at, cap, row, sizeof row);
        snprintf(row, sizeof row, "[wifi-img] sum32(first 512K)=%u after=%u first-word=%08x size=%u", native_wifi_cfg[28], native_wifi_cfg[30], native_wifi_cfg[29], native_wifi_cfg[31]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_ev) for (unsigned k = 0; k < 2; ++k) {
        volatile uint32_t *e = native_wifi_ev + 24 * k;
        if (!e[8] && !e[0] && !e[12] && !e[13] && !e[18]) continue;
        snprintf(row, sizeof row, "[wifi-ev%u] er0: %08x %08x %08x %08x | %08x %08x %08x %08x er1: %08x %08x %08x %08x ctx rp/wp: %x/%x %x/%x sink: %08x %08x bhie: addrlo=%x hi=%x size=%x db=%x status=%x", k,
                 e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8], e[9], e[10], e[11], e[12], e[13], e[14], e[15], e[16], e[17], e[18], e[19], e[20], e[22], e[23]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_chip && native_wifi_chip[0]) {
        snprintf(row, sizeof row, "[wifi-chip] ltssm=%x hotrst=%x cookie=%x warm=%x warm-after=%x cause=%x global-before=%x after=%x mhistatus=%x->%x ee=%x aspm-ctl=%x msi-ctl=%x",
                 native_wifi_chip[0], native_wifi_chip[1], native_wifi_chip[2], native_wifi_chip[3], native_wifi_chip[4], native_wifi_chip[5], native_wifi_chip[6], native_wifi_chip[7],
                 native_wifi_chip[8], native_wifi_chip[9], native_wifi_chip[10], native_wifi_chip[11], native_wifi_chip[12]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_pon && native_wifi_pon[0]) {
        snprintf(row, sizeof row, "[wifi-pon] ppid/owner=%x bytes08..27=%08x %08x %08x %08x %08x %08x %08x %08x sn7=%08x sid=%08x", native_wifi_pon[0], native_wifi_pon[1], native_wifi_pon[2],
                 native_wifi_pon[3], native_wifi_pon[4], native_wifi_pon[5], native_wifi_pon[6], native_wifi_pon[7], native_wifi_pon[8], native_wifi_pon[9], native_wifi_pon[10]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_ep && native_wifi_ep[0]) {
        snprintf(row, sizeof row, "[wifi-ep] st=%u bar0-mask=%08x bar0=%08x cmd=%08x rp-mem=%08x mhi: len=%x ver=%x cfg=%x bhioff=%x bhieoff=%x ctrl=%x status=%x bhi: ver=%x exec=%x st=%x err=%x",
                 native_wifi_ep[0], native_wifi_ep[1], native_wifi_ep[2], native_wifi_ep[3], native_wifi_ep[4], native_wifi_ep[5], native_wifi_ep[6], native_wifi_ep[7],
                 native_wifi_ep[8], native_wifi_ep[9], native_wifi_ep[10], native_wifi_ep[11], native_wifi_ep[12], native_wifi_ep[13], native_wifi_ep[14], native_wifi_ep[15]);
        at = put(out, at, cap, row, sizeof row);
      }
      if (&native_wifi_mhi && native_wifi_mhi[0]) {
        snprintf(row, sizeof row, "[wifi-mhi] step=%d smmu=%x part=%x amss=%u segs=%u boot=%x ee=%x state=%x bhi-status=%x bhi-err=%x dbg=%x.%x.%x mhistatus=%x ms=%u bhie=%x",
                 (int)native_wifi_mhi[0], native_wifi_mhi[1], native_wifi_mhi[2], native_wifi_mhi[3], native_wifi_mhi[4], native_wifi_mhi[5], native_wifi_mhi[6], native_wifi_mhi[7],
                 native_wifi_mhi[8], native_wifi_mhi[9], native_wifi_mhi[10], native_wifi_mhi[11], native_wifi_mhi[12], native_wifi_mhi[13], native_wifi_mhi[14], native_wifi_mhi[15]);
        at = put(out, at, cap, row, sizeof row);
        if (&native_wifi_mhi_ms) {
          snprintf(row, sizeof row, "[wifi-mhi-ms] smmu-substep=%u last-smmu-access=%x buffers=%u found=%u read=%u smmu=%u sbl=%u ctx=%u end=%u", native_wifi_mhi[12], native_wifi_mhi_ms[7], native_wifi_mhi_ms[0], native_wifi_mhi_ms[1],
                   native_wifi_mhi_ms[2], native_wifi_mhi_ms[3], native_wifi_mhi_ms[4], native_wifi_mhi_ms[5], native_wifi_mhi_ms[6]);
          at = put(out, at, cap, row, sizeof row);
        }
      }
      if (&native_wifi_text && native_wifi_text_n) {
        if (&nw_wifi_stats) {
          snprintf(row, sizeof row, "[wifi-nw] stage=%u scans=%u frames=%u beacons=%u networks=%u err=%d lines=%u", nw_wifi_stats.stage, nw_wifi_stats.scans, nw_wifi_stats.frames,
                   nw_wifi_stats.beacons, nw_wifi_stats.bss_seen, nw_wifi_stats.last_err, native_wifi_text_n);
          at = put(out, at, cap, row, sizeof row);
          if (&nw_wifi_where) {
            snprintf(row, sizeof row, "[wifi-where] kind=%llu fn=%llx depth=%llu ms=%llu", (unsigned long long)nw_wifi_where[0], (unsigned long long)nw_wifi_where[1],
                     (unsigned long long)nw_wifi_where[2], (unsigned long long)nw_wifi_where[3]);
            at = put(out, at, cap, row, sizeof row);
          }
          uint32_t f[10] = {0};
          if (&native_ufs_extra_fault && native_ufs_extra_fault(f)) {
            snprintf(row, sizeof row, "[wifi-smmu] gfsr=%08x cb-fsr=%08x far=%08x%08x fsynr0=%08x", f[0], f[2], f[4], f[3], f[5]);
            at = put(out, at, cap, row, sizeof row);
          }
        }
        // the lines added since the last slot, at most 100 per slot; the rest wait for the next one while the ring still holds them
        static uint32_t shown;
        const uint32_t n = native_wifi_text_n;
        uint32_t i = shown;
        if (n - i > 512) i = n - 512;
        for (uint32_t k = 0; i < n && k < 100; ++i, ++k) {
          char line[124];
          snprintf(line, sizeof line, "[wifi-log] %.110s", native_wifi_text[i % 512]);
          at = put(out, at, cap, line, sizeof line);
        }
        shown = i;
        // the notices among them, which the debug lines cannot push out of their own ring: at most 40 new ones per slot
        if (&native_wifi_note) {
          static uint32_t noted;
          const uint32_t m = native_wifi_note_n;
          uint32_t j = noted;
          if (m - j > 128) j = m - 128;
          for (uint32_t k = 0; j < m && k < 40; ++j, ++k) {
            char line[124];
            snprintf(line, sizeof line, "[wifi-msg] %.110s", native_wifi_note[j % 128]);
            at = put(out, at, cap, line, sizeof line);
          }
          noted = j;
        }
      }
      snprintf(row, sizeof row, "[wifi-pcie] pipe-halt=%08x perst=%08x dbg0@20ms=%x dbg1@20ms=%x pipe-mux=%x", native_wifi_pmic[17], native_wifi_pmic[18], native_wifi_pmic[19],
               native_wifi_pmic[21], native_wifi_pmic[20]);
      at = put(out, at, cap, row, sizeof row);
      // every candidate regulator peripheral that exists, before and after the votes; v is the 0x41 0x40 pair
      const unsigned nb = native_wifi_pmic[2] & 0xffff, na = native_wifi_pmic[2] >> 16;
      for (unsigned i = 0; i < nb && i < 112; ++i) {
        const uint32_t k0 = native_wifi_pmic[24 + 2 * i], v0 = native_wifi_pmic[25 + 2 * i];
        uint32_t v1 = v0;
        if (i < na && native_wifi_pmic[248 + 2 * i] == k0) v1 = native_wifi_pmic[249 + 2 * i];
        char line[120];
        snprintf(line, sizeof line, "[wifi-reg] ppid=%03x type=%x/%x before st=%02x en=%02x v=%02x%02x after st=%02x en=%02x", k0 >> 16, k0 >> 8 & 0xff, k0 & 0xff,
                 v0 >> 24, v0 >> 16 & 0xff, v0 & 0xff, v0 >> 8 & 0xff, v1 >> 24, v1 >> 16 & 0xff);
        at = put(out, at, cap, line, sizeof line);
      }
    }
  }
  // What the boot loader left in the display controller (DpuProbe.cpp, boot argument native-dpu=1). The question this row answers
  // is which SSPP base pair is live on THIS board: the SM8250 pipes sit at 0x5000/0x7000 and the SM8550 ones at 0x4000/0x6000, and
  // both map inside the block, so writing the wrong pair touches unrelated registers without faulting. The live pair is the one
  // whose source address equals the boot framebuffer. Format is printed because the direct scanout path only rewrites address and
  // stride: every frame it flips inherits the firmware's source format, and a BGRA scanout with an RGB format programmed would show
  // swapped channels with no other symptom.
  if (&native_dpu_stats && &native_dpu_pipes && native_dpu_stats[0]) {
    char row[240];
    const volatile uint32_t *d = native_dpu_stats, *p = native_dpu_pipes;
    snprintf(row, sizeof row, "[dpu] st=%d hw=%x fb=%x%08x | dispcc ahb=%x mdp=%x (bit1 set = gated) | gdscr=%x (bit31 PWR_ON, bit0 SW_COLLAPSE)",
             (int)d[0], d[1], p[41], p[40], d[18], d[19], d[2]);
    at = put(out, at, cap, row, sizeof row);
    // size / src0 / stride0 / format / unpack, one line per candidate base.
    static const char *const names[4] = {"0x4000(vig0)", "0x6000(vig1)", "0x3000(vig2?)", "0x5000(vig3?)"};
    for (int i = 0; i < 4; ++i) {
      // src1 and opmode decide whether rewriting SRC0_ADDR alone can change the picture: a pipe left in multirect keeps scanning
      // its other rect from src1 (dpu_hw_sspp.c:472-496). opmode 0 means SOLO, which is what mainline clears it to.
      snprintf(row, sizeof row, "[dpu pipe] %s size=%x src0=%x stride=%x fmt=%x unpack=%x src1=%x opmode=%x%s", names[i], p[4 * i],
               p[4 * i + 1], p[4 * i + 2], p[4 * i + 3], p[16 + i], p[20 + i], p[24 + i],
               p[4 * i + 1] && p[4 * i + 1] == p[40] ? " LIVE" : "");
      at = put(out, at, cap, row, sizeof row);
    }
    snprintf(row, sizeof row, "[dpu ctl] 0x1000(ctl0) flush=%x top=%x | 0x1200(ctl1) flush=%x top=%x", p[42], p[43], p[44], p[45]);
    at = put(out, at, cap, row, sizeof row);
    // Every CTL of the SM8250 bank: CTL_FLUSH clears itself once a flush latches, so a zero there only means nothing was pending;
    // CTL_TOP (0x14) is the word that names the layer mixers the CTL drives, which is what identifies a programmed CTL.
    snprintf(row, sizeof row, "[dpu ctl6] top %x/%x/%x/%x/%x/%x flush %x/%x/%x/%x/%x/%x",
             d[12], d[13], d[14], d[15], d[16], d[17], d[6], d[7], d[8], d[9], d[10], d[11]);
    at = put(out, at, cap, row, sizeof row);
  }
  /* The display bank's stage-1 window. Printed whenever the probe or the window ran, because the two questions -- which streams the
   * SMR table routes, and whether the bank took our table -- are only separable with both halves in one row. */
  if (&native_ufs_dpu_smmu && (native_ufs_dpu_smmu[0] || (&native_ufs_dpu_state && native_ufs_dpu_state))) {
    char row[240];
    snprintf(row, sizeof row, "[dpu window] state=%x idr0=%x groups=%u valid=%u banks=%u | stream0 slot/bank=%x smr=%x s2cr=%x | stream1 slot/bank=%x smr=%x s2cr=%x",
             &native_ufs_dpu_state ? native_ufs_dpu_state : 0, native_ufs_dpu_smmu[2], native_ufs_dpu_smmu[0] & 0xff,
             (native_ufs_dpu_smmu[0] >> 8) & 0xff, (native_ufs_dpu_smmu[0] >> 16) & 0xff,
             native_ufs_dpu_smmu[4], native_ufs_dpu_smmu[5], native_ufs_dpu_smmu[6],
             native_ufs_dpu_smmu[13], native_ufs_dpu_smmu[14], native_ufs_dpu_smmu[15]);
    at = put(out, at, cap, row, sizeof row);
  }
  // Touch noise before the fuzz filter: resting or slow frames, mean step (x + y) and largest single-axis step, in 0.1 px.
  if (&native_touch_hold && native_touch_hold[7]) {
    char row[200];
    snprintf(row, sizeof row, "[touch hold] bridged=%u lifts none=%u far=%u weak=%u palm=%u taken=%u frames t2=%u t4=%u merges=%u peaks=%u full=%u ref=%u",
             native_touch_hold[0], native_touch_hold[1], native_touch_hold[2], native_touch_hold[3], native_touch_hold[4], native_touch_hold[5],
             native_touch_hold[6], native_touch_hold[7], native_touch_hold[10], native_touch_hold[11], native_touch_hold[12], native_touch_hold[13]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_touch_jitter && native_touch_jitter[0]) {
    char row[128];
    snprintf(row, sizeof row, "[touch jitter] frames=%u mean=%u max=%u smoothed mean=%u max=%u (0.1 px)", native_touch_jitter[0],
             native_touch_jitter[1] / native_touch_jitter[0], native_touch_jitter[2], &native_touch_hold ? native_touch_hold[8] / native_touch_jitter[0] : 0,
             &native_touch_hold ? native_touch_hold[9] : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  // Distinct path lookups that failed for MobileSafari (S) and lsd (L): errno and path, in the order they first failed.
  if (&native_lookup_count && native_lookup_count) {
    char row[128];
    const unsigned n = native_lookup_count < 160 ? native_lookup_count : 160;
    snprintf(row, sizeof row, "[safari lookups] failed=%u", native_lookup_count);
    at = put(out, at, cap, row, sizeof row);
    for (unsigned i = 0; i < n; ++i) {
      snprintf(row, sizeof row, "SL%u %s", i, native_lookup_log[i]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // Audio DSP bring-up (AdspProbe.cpp): stage and error, IPCC interrupts, the DSP's slave-kernel bits, SMEM items, AOSS mailbox, RSC id,
  // adsp.mbn KiB, ms to ready, GLINK messages in/out, channels up, opens nobody serves, last SCM result, rail votes; then its note.
  if (&native_adsp_stats && native_adsp_stats[0]) {
    char row[200];
    snprintf(row, sizeof row, "[adsp] st=%d err=%d irq=%d slave=%x items=%x qmp=%x rsc=%x fw=%dK rdy=%dms rx=%d tx=%d ch=%x unk=%d scm=%d/%x votes=%x",
             native_adsp_stats[0], native_adsp_stats[1], native_adsp_stats[2], native_adsp_stats[3], native_adsp_stats[4], native_adsp_stats[5],
             native_adsp_stats[6], native_adsp_stats[7], native_adsp_stats[8], native_adsp_stats[9], native_adsp_stats[10], native_adsp_stats[11],
             native_adsp_stats[12], native_adsp_stats[13], native_adsp_stats[14], native_adsp_stats[15]);
    at = put(out, at, cap, row, sizeof row);
    if (&native_adsp_text && native_adsp_text[0]) {
      snprintf(row, sizeof row, "[adsp] %.95s", native_adsp_text);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // Battery power source (ShengBattery.cpp): stage, percent, mV, mA, temperature (raw), flags (present, external, charging, full,
  // notifications on), refresh cycles, answers, timeouts|errors<<16, notifications, USB type, full/design mAh, cycles, dropped|ignored<<16,
  // send refusals.
  if (&native_battery_stats && native_battery_stats[0]) {
    char row[200];
    snprintf(row, sizeof row, "[battery] st=%d pct=%d mv=%d ma=%d temp=%d fl=%x cyc=%d ans=%d to=%x ntf=%d usb=%d full=%d design=%d cycles=%d drop=%x refuse=%d",
             native_battery_stats[0], native_battery_stats[1], native_battery_stats[2], native_battery_stats[3], native_battery_stats[4],
             native_battery_stats[5], native_battery_stats[6], native_battery_stats[7], native_battery_stats[8], native_battery_stats[9],
             native_battery_stats[10], native_battery_stats[11], native_battery_stats[12], native_battery_stats[13], native_battery_stats[14],
             native_battery_stats[15]);
    at = put(out, at, cap, row, sizeof row);
  }
  // Audio framework readiness over the packet router channel (AudioProbe.cpp): stage (5 ready), queries, answers, last status, refusals,
  // timeouts, ignored packets, ms from channel up to ready, channel ups/downs, last opcode, ring drops, send refusals, mode.
  if (&native_audio_stats && native_audio_stats[0]) {
    char row[200];
    snprintf(row, sizeof row, "[audio] st=%d q=%d ans=%d status=%d refused=%d to=%d ign=%d rdy=%dms up=%d down=%d op=%x drop=%d sendfail=%d mode=%d",
             native_audio_stats[0], native_audio_stats[1], native_audio_stats[2], native_audio_stats[3], native_audio_stats[4], native_audio_stats[5],
             native_audio_stats[6], native_audio_stats[7], native_audio_stats[8], native_audio_stats[9], native_audio_stats[10], native_audio_stats[11],
             native_audio_stats[12], native_audio_stats[13]);
    at = put(out, at, cap, row, sizeof row);
  }
  // Touch strokes in panel pixels as the digitizer published them (y mirrored, 0 = the edge the UI calls top): where each finger came
  // down and lifted, how long and over how many frames, and the extent of every contact so far (edge gestures need the outer pixels).
  if (&native_touch_stroke_count && &native_touch_strokes && &native_touch_extent && native_touch_stroke_count) {
    char row[160];
    const uint32_t n = native_touch_stroke_count;
    snprintf(row, sizeof row, "[touch] strokes=%u extent x %d..%d y %d..%d (px of 3048x2032)", n, native_touch_extent[0] / 10,
             native_touch_extent[1] / 10, native_touch_extent[2] / 10, native_touch_extent[3] / 10);
    at = put(out, at, cap, row, sizeof row);
    for (uint32_t k = n > 16 ? n - 16 : 0; k < n; ++k) {
      const volatile int32_t *r = native_touch_strokes[k & 15];
      snprintf(row, sizeof row, "TS%u t=%u.%03us %ums (%d,%d)->(%d,%d) f%d", k, (uint32_t)r[0] / 1000, (uint32_t)r[0] % 1000, (uint32_t)r[1],
               r[2] / 10, r[3] / 10, r[4] / 10, r[5] / 10, r[6]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  if (&native_swipe_stats && native_swipe_stats[3]) {
    char row[200];
    snprintf(row, sizeof row, "[swipe] done=%u first=%u.%03us last=%u.%03us frames=%u tap=%u.%03us home=%u.%03us library=%u.%03us..%u.%03us", native_swipe_stats[0],
             native_swipe_stats[1] / 1000, native_swipe_stats[1] % 1000, native_swipe_stats[2] / 1000, native_swipe_stats[2] % 1000,
             native_swipe_stats[3], native_swipe_stats[4] / 1000, native_swipe_stats[4] % 1000, native_swipe_stats[5] / 1000,
             native_swipe_stats[5] % 1000, native_swipe_stats[6] / 1000, native_swipe_stats[6] % 1000, native_swipe_stats[7] / 1000,
             native_swipe_stats[7] % 1000);
    at = put(out, at, cap, row, sizeof row);
    snprintf(row, sizeof row, "[swipe2] heavy=%u.%03us controlcenter..%u.%03us notification..%u.%03us apps..%u.%03us", native_swipe_stats[8] / 1000,
             native_swipe_stats[8] % 1000, native_swipe_stats[9] / 1000, native_swipe_stats[9] % 1000, native_swipe_stats[10] / 1000,
             native_swipe_stats[10] % 1000, native_swipe_stats[11] / 1000, native_swipe_stats[11] % 1000);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_map_fail && native_adreno_map_fail[0][1]) {
    char row[170];
    int n = snprintf(row, sizeof row, "[map fails]");
    for (unsigned k = 0; k < 5 && native_adreno_map_fail[k][1]; ++k) n += snprintf(row + n, sizeof row - n, " %x x%u", native_adreno_map_fail[k][0], native_adreno_map_fail[k][1]);
    snprintf(row + n, sizeof row - n, " | at last: spans=%u objects=%u iova_pfn=%x handle=%x", native_adreno_map_fail[5][0], native_adreno_map_fail[5][1], native_adreno_map_fail[6][0], native_adreno_map_fail[6][1]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_client_stats) {
    // per client slot: live objects/peak, refused BoAllocs, last refusal status and bytes, highest mapped end in the window (MiB)
    char row[200];
    int n = snprintf(row, sizeof row, "[gpu objects]");
    for (unsigned k = 0; k < 5; ++k)
      if (native_adreno_client_stats[k][1] || native_adreno_client_stats[k][2])
        // subs/ibs: what this slot actually submitted. [gpu clients] says a process opened the device; this says whether it rendered.
        n += snprintf(row + n, sizeof row - n, " s%u %u/%u r%u %x/%u w%uM subs=%u ibs=%u", k, native_adreno_client_stats[k][0],
                      native_adreno_client_stats[k][1], native_adreno_client_stats[k][2], native_adreno_client_stats[k][3],
                      native_adreno_client_stats[k][4], native_adreno_client_stats[k][5], native_adreno_client_stats[k][6],
                      native_adreno_client_stats[k][7]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_fence_wait && native_adreno_fence_wait[0]) {
    char row[160];
    snprintf(row, sizeof row, "[fence wait] calls=%u timeouts=%u last timed-out fence=%u gpu wrote=%u submitted=%u completed=%u no-job=%u refused fence=%u",
             native_adreno_fence_wait[0], native_adreno_fence_wait[1], native_adreno_fence_wait[2], native_adreno_fence_wait[3],
             native_adreno_fence_wait[4], native_adreno_fence_wait[5], native_adreno_fence_wait[6], native_adreno_fence_wait[7]);
    at = put(out, at, cap, row, sizeof row);
    // The callers' waits (int-147): the polls above run under the connection lock, the pauses between them outside it.
    if (&native_adreno_wait_outer && native_adreno_wait_outer[0]) {
      snprintf(row, sizeof row, "[fence wait] outer waits=%llu us=%llu pauses outside the lock=%llu longest=%lluus",
               (unsigned long long)native_adreno_wait_outer[0], (unsigned long long)native_adreno_wait_outer[1],
               (unsigned long long)native_adreno_wait_outer[2], (unsigned long long)native_adreno_wait_outer[3]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // The CP's registers once a fence stopped advancing: RBBM status 0-3, interrupt, CP fault, status 1, protect, ring rptr/wptr,
  // IB1 base/rem, IB2 base/rem, BV rptr, BV IB1 base/rem, BV IB2 rem, BV IB2 base.
  if (&native_adreno_unstick && native_adreno_unstick[0]) {
    char row[200];
    // [0] write-pointer retries, [1] fence of the last, [2] translation drops, [4] bus drains with ack [5]/[6], [9..11] stalls that ended after
    // the write pointer / translations / drain stage, [12] the most timeouts one stall took, [13] a wait's timeout in us.
    snprintf(row, sizeof row, "[gpu unstick] kick=%u last=%u tlb=%u drain=%u ack=%x/%x ended: kick=%u tlb=%u drain=%u longest=%u unit=%uus",
             native_adreno_unstick[0], native_adreno_unstick[1], native_adreno_unstick[2], native_adreno_unstick[4],
             native_adreno_unstick[5], native_adreno_unstick[6], native_adreno_unstick[9], native_adreno_unstick[10],
             native_adreno_unstick[11], native_adreno_unstick[12], native_adreno_unstick[13]);
    at = put(out, at, cap, row, sizeof row);
  }
  // The probe's own rows, into the persistent log. The USB console node on this board is the
  // node that enumerated at boot, so console-capture.py's "mtime newer than this run" test never
  // passes and the GPU rows were never captured; the log is the only channel that survives the
  // round. The mode (0), the publication and user-client counters (23), the live counters (25)
  // The mode (0), the publication and user-client counters (23), the Metal registration (24),
  // the live counters (25) and the CP hardware-error row (31) are what a stall needs. Row 24
  // names the Metal registration or, when the opt-in self-test is on, its diagnostic verdict.
  // Following the reference board, the self-test does not gate registration: the published
  // IOAcceleratorES service is what lets user clients open the driver.
  if (&native_gpu_rows && native_gpu_rows[0][0]) {
    static const int gpuRows[] = {0, 3, 6, 7, 13, 14, 15, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 31};
    char row[300];   // a GPU row is now up to 256 bytes, plus "GPUROWnn " and the NUL
    for (unsigned r = 0; r < sizeof gpuRows / sizeof gpuRows[0]; ++r) {
      const char *text = native_gpu_rows[gpuRows[r]];
      if (!text[0]) continue;
      snprintf(row, sizeof row, "GPUROW%d %s", gpuRows[r], text);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  // The last step a user client close reached. The driver's own IOLog lines for these steps fall out of
  // the message ring before the log is read (the probe closes at about 5 s, the ring holds seconds of a
  // verbose boot), so the step is carried as a value and printed by every round that follows.
  // Which mechanism puts a client address space in front of the CP: 2 the CP switches the
  // table in the ring (needs the aperture SCM call to be available), 1 the CPU writes TTBR0
  // with the GPU idle. A client IB in its own address space is only translatable if the
  // table the CP runs under is that client's.
  if (&native_adreno_cp_aperture && native_adreno_cp_aperture[3]) {
    char aprow[96];
    // switches[1] counts the CPU's TTBR0 writes: zero after client submissions means the table never moved.
    snprintf(aprow, sizeof aprow, "[gpu aperture] avail=%x res=%x check=%x mode=%u tbbr0_writes=%u",
             native_adreno_cp_aperture[0], native_adreno_cp_aperture[1], native_adreno_cp_aperture[2], native_adreno_cp_aperture[3],
             &native_adreno_table_switches ? native_adreno_table_switches[1] : 0u);
    at = put(out, at, cap, aprow, sizeof aprow);
    // hangCheck runs every 500 ms on its own thread under the device lock: a restart there, mid self-test,
    // would explain a run whose first submission times out and whose second never starts.
    if (&native_adreno_recovery && native_adreno_recovery[0]) {
      char rrow[96];
      snprintf(rrow, sizeof rrow, "[gpu recovery] hangs=%u restarts=%u failed=%u fence=%u last=%x replayed=%u",
               native_adreno_recovery[0], native_adreno_recovery[1], native_adreno_recovery[2],
               native_adreno_recovery[3], native_adreno_recovery[4], native_adreno_recovery[5]);
      at = put(out, at, cap, rrow, sizeof rrow);
      if (&native_adreno_recover_trace) {
        char tr[88];
        snprintf(tr, sizeof tr, "[gpu recover trace] step=%u value=%x fence=%u last=%u",
                 native_adreno_recover_trace[0], native_adreno_recover_trace[1],
                 native_adreno_recover_trace[2], native_adreno_recover_trace[3]);
        at = put(out, at, cap, tr, sizeof tr);
        char rs[48];
        snprintf(rs, sizeof rs, "[gpu restart step] %u", native_gpu_restart_step);
        at = put(out, at, cap, rs, sizeof rs);
      }
    }
    if (&native_adreno_ttbr0_tlb && native_adreno_ttbr0_tlb[0]) {
      char trow[72];
      snprintf(trow, sizeof trow, "[gpu ttbr0] writes=%u asid=%u last=%x", native_adreno_ttbr0_tlb[0],
               native_adreno_ttbr0_tlb[2], native_adreno_ttbr0_tlb[1]);
      at = put(out, at, cap, trow, sizeof trow);
    }
  }
  if (&native_adreno_wfi_try && native_adreno_wfi_try[0]) {
    char wrow[96];
    snprintf(wrow, sizeof wrow, "[gpu wfi] scan=%u nomap=%u noview=%u dropped=%u last=%x",
             native_adreno_wfi_try[0], native_adreno_wfi_try[1], native_adreno_wfi_try[2],
             &native_adreno_wfi_dropped ? native_adreno_wfi_dropped : 0u, native_adreno_wfi_try[4]);
    at = put(out, at, cap, wrow, sizeof wrow);
  }
  if (&native_adreno_close_step && native_adreno_close_step[0]) {
    char closerow[64];
    snprintf(closerow, sizeof closerow, "[gpu close] step=%u pid=%u stage=%u dev=%u prep=%u sys=%u spans=%u", native_adreno_close_step[0],
             native_adreno_close_step[1], &native_adreno_trace_stage ? native_adreno_trace_stage : 0u,
             native_adreno_close_step[2], native_adreno_close_step[3], native_adreno_close_step[4], native_adreno_close_step[5]);
    at = put(out, at, cap, closerow, sizeof closerow);
  }
  if (&native_adreno_wptr_fence && native_adreno_wptr_fence[0]) {
    char row[128];
    snprintf(row, sizeof row, "[gpu wptr] writes=%u retried=%u repeats=%u unfenced=%u", native_adreno_wptr_fence[0],
             native_adreno_wptr_fence[1], native_adreno_wptr_fence[2], native_adreno_wptr_fence[3]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_stall && native_adreno_stall[0]) {
    char row[240];    int n = snprintf(row, sizeof row, "[gpu stall] fence=%u n=%u", native_adreno_stall[0], native_adreno_stall[1]);
    for (unsigned k = 2; k < 24 && n > 0 && (size_t)n < sizeof row; ++k) n += snprintf(row + n, sizeof row - n, " %x", native_adreno_stall[k]);
    at = put(out, at, cap, row, sizeof row);
    // SMMU FSR, FAR lo/hi, FSYNR0, then for the BR IB1, BV IB1 and BV IB2: the dword index in the IB where the 16 words start
    // (ffff00NN: NN words before the fetch address, the IB start being unknown), then 16 words; deadbeef = not readable.
    if (&native_adreno_stall_mem)
      for (unsigned part = 0; part < 4; ++part) {
        static const char *const names[4] = {"smmu", "br ib1", "bv ib1", "bv ib2"};
        n = snprintf(row, sizeof row, "[gpu stall] %s", names[part]);
        if (part) n += snprintf(row + n, sizeof row - n, " @%x:", native_adreno_stall_mem[4 + 17 * (part - 1)]);
        const unsigned first = part ? 5 + 17 * (part - 1) : 0, count = part ? 16 : 4;
        for (unsigned k = first; k < first + count && n > 0 && (size_t)n < sizeof row; ++k) n += snprintf(row + n, sizeof row - n, " %x", native_adreno_stall_mem[k]);
        at = put(out, at, cap, row, sizeof row);
      }
    // Map handles: next at the stalled job's submission, then under the BR IB1, BV IB1, BV IB2 (>= the first: mapped after it).
    if (&native_adreno_stall_mem) {
      snprintf(row, sizeof row, "[gpu stall] handles job>=%u br=%u bv=%u bv2=%u", native_adreno_stall_mem[55], native_adreno_stall_mem[56],
               native_adreno_stall_mem[57], native_adreno_stall_mem[58]);
      at = put(out, at, cap, row, sizeof row);
    }
  }
  if (&native_gop_wait_stats && native_gop_wait_stats[0]) {
    char row[160];
    snprintf(row, sizeof row, "[scanout wait] calls=%llu timeouts=%llu asked=%llums last=%lluus min=%lluus max=%lluus mean=%lluus", native_gop_wait_stats[0],
             native_gop_wait_stats[1], native_gop_wait_stats[2], native_gop_wait_stats[3] / 1000, native_gop_wait_stats[4] / 1000,
             native_gop_wait_stats[5] / 1000, native_gop_wait_stats[1] ? native_gop_wait_stats[6] / native_gop_wait_stats[1] / 1000 : 0);
    at = put(out, at, cap, row, sizeof row);
  }
  /* Direct scanout, the zero-copy path. Absent when the display never entered it (startDirect needs sheng-dpu-scanout>=2 and a display
   * bank window), so this row's presence is itself the answer to "did the direct path run". The upstream display guide asks for a
   * "[direct scanout]" row carrying flips, mapped surfaces, latch time and the wait for the GPU; this is that row. */
  // Print when either path moved at all: with no flip the counters are the evidence, and a
  // gate on flips alone hides exactly the boot that needs reading.
  if (&native_legacy_direct && (native_legacy_direct[0] || (&native_legacy_direct_stage && native_legacy_direct_stage) ||
                                (&native_legacy_swapstat && native_legacy_swapstat[0]))) {
    char row[200];
    const volatile uint32_t *d = native_legacy_direct;
    snprintf(row, sizeof row,
             "[direct scanout] stage=%u branch=%u flips=%u mapped=%u refused=%u recycle=%u noslot=%u boot=%u"
             " | flipstep=%u ctl_top=%x ctl_flush=%x | latch late=%u max=%uus sum=%uus | gpu wait=%u sum=%uus max=%uus slow8ms=%u gaveup=%u",
             &native_legacy_direct_stage ? native_legacy_direct_stage : 0,
             &native_legacy_direct_flip_branch ? native_legacy_direct_flip_branch : 0,
             d[0], d[1], d[2], d[6], d[7], d[8],
             &native_legacy_direct_flip_stage ? native_legacy_direct_flip_stage : 0,
             &native_legacy_direct_ctl ? native_legacy_direct_ctl[0] : 0,
             &native_legacy_direct_ctl ? native_legacy_direct_ctl[1] : 0,
             d[3], d[4], d[5], d[9], d[10], d[11], d[13], d[12]);
    at = put(out, at, cap, row, sizeof row);
    if (&native_legacy_direct_vig) {
      char vrow[130];
      snprintf(vrow, sizeof vrow, "[direct vig] src0_0=%x took0=%u src0_1=%x took1=%u", native_legacy_direct_vig[0],
               native_legacy_direct_vig[2], native_legacy_direct_vig[1], native_legacy_direct_vig[3]);
      at = put(out, at, cap, vrow, sizeof vrow);
    }
    /* The queue's gates. accept/enter/transferred/precond/no-request/latched/flipped/retired/deepest, so a stalled flip count names
     * the gate that stopped it instead of leaving "flips stopped growing" unexplained. */
    if (&native_legacy_swapstat) {
      char qrow[170];
      const volatile uint32_t *s = native_legacy_swapstat;
      snprintf(qrow, sizeof qrow,
               "[swap queue] accept=%u enter=%u transferred=%u precond=%u no-request=%u latched=%u flipped=%u retired=%u deepest=%u",
               s[0], s[1], s[2], s[3], s[4], s[5], s[9], s[11], s[15]);
      at = put(out, at, cap, qrow, sizeof qrow);
    }
  }
  if (&native_adreno_view_diag && native_adreno_view_diag[0]) {
    char row[120];
    snprintf(row, sizeof row, "[bo views] calls=%u new=%u cached=%u refused=%u last=%x inside=%u", native_adreno_view_diag[0], native_adreno_view_diag[1],
             native_adreno_view_diag[2], native_adreno_view_diag[3], native_adreno_view_diag[4], native_adreno_view_diag[5]);
    at = put(out, at, cap, row, sizeof row);
  }
  if (&native_adreno_import_diag && native_adreno_import_diag[0]) {
    char row[160];
    at = put(out, at, cap, "[import]", 9);
    snprintf(row, sizeof row, "calls=%llu refused=%llu last: st=%llx stage=%llu bytes=%llx addr=%llx | first: st=%llx stage=%llu bytes=%llx addr=%llx",
             native_adreno_import_diag[0], native_adreno_import_diag[1], native_adreno_import_diag[2], native_adreno_import_diag[3],
             native_adreno_import_diag[4], native_adreno_import_diag[5], native_adreno_import_diag[6], native_adreno_import_diag[7],
             native_adreno_import_diag[8], native_adreno_import_diag[9]);
    at = put(out, at, cap, row, sizeof row);
    snprintf(row, sizeof row, "stage_now=%u refused_page pa=%llx offset=%llx", native_adreno_import_stage, native_adreno_import_fail_pa,
             native_adreno_import_fail_offset);
    at = put(out, at, cap, row, sizeof row);
    if (&native_adreno_import_live) {
      snprintf(row, sizeof row, "alive=%llu free-refused=%llu last=%llx persistent=%u plain=%u", native_adreno_import_live[0],
               native_adreno_import_live[1], native_adreno_import_live[2], &native_adreno_import_persistent ? native_adreno_import_persistent[0] : 0,
               &native_adreno_import_persistent ? native_adreno_import_persistent[1] : 0);
      at = put(out, at, cap, row, sizeof row);
      for (unsigned i = 0; i < 8; i += 2) {
        const volatile uint64_t *a = &native_adreno_import_live[3 + i * 4], *b = a + 4;
        snprintf(row, sizeof row, "I p%llu %llx+%llx h%llu s%llu | p%llu %llx+%llx h%llu s%llu", a[1] >> 40, a[0], a[1] & 0xffffffffffull, a[2],
                 a[3], b[1] >> 40, b[0], b[1] & 0xffffffffffull, b[2], b[3]);
        at = put(out, at, cap, row, sizeof row);
      }
    }
  }
  if (boot_log_reset_probe_text[0]) {
    at = put(out, at, cap, "[reset]", 8);
    at = put(out, at, cap, boot_log_reset_probe_text, sizeof boot_log_reset_probe_text);
    if (boot_log_reset_mark[0]) at = put(out, at, cap, boot_log_reset_mark, sizeof boot_log_reset_mark);
    if (boot_log_reset_watchdog[0]) at = put(out, at, cap, boot_log_reset_watchdog, sizeof boot_log_reset_watchdog);
    if (boot_log_reset_result[0]) at = put(out, at, cap, boot_log_reset_result, sizeof boot_log_reset_result);
  }
  if (boot_diag_status) {
    char row[120];
    snprintf(row, sizeof row, "[diag] status=%d files=%u skipped=%u errors=%u bytes=%u first=%u", boot_diag_status, &boot_diag_files ? boot_diag_files : 0,
             &boot_diag_skipped ? boot_diag_skipped : 0, &boot_diag_errors ? boot_diag_errors : 0, diag_bytes, diag_first);
    at = put(out, at, cap, row, sizeof row);
  }
  // The signed-text fixups given as text-fixupN boot arguments: how many parsed and how many page writes they made.
  if (&cs_text_fixup_parsed && cs_text_fixup_parsed) {
    char row[64];
    snprintf(row, sizeof row, "[text fixup] parsed=%u applied=%u", cs_text_fixup_parsed, cs_text_fixup_applied);
    at = put(out, at, cap, row, sizeof row);
  }
  // Whether this writer is still getting its turns. A frozen slots_written alone cannot tell a stalled
  // writer from a stalled system; these three numbers can. phase 12 is a round the storage path made us
  // stand down for, and skips counts those rounds since boot.
  {
    char row[72];
    snprintf(row, sizeof row, "[log] skips=%u phase=%u state=%u rounds=%u",
             &native_ufs_log_skips ? native_ufs_log_skips : 0, (unsigned)log_phase, boot_log_state, boot_log_rounds);
    at = put(out, at, cap, row, sizeof row);
  }
  // The autoreset clock itself: how many times boot_log_tick ran, the seconds it computes from cntpct, the raw counter it read, whether the
  // supervisor thread started and how many times its loop ran. A boot that never resets is one of these five numbers standing still, and
  // without them every reading of "the deadline did not fire" is a guess.
  {
    char row[128];
    snprintf(row, sizeof row, "[tick] calls=%u mon=%llu step=%u tstep=%u super=%u/%u t=%u/%u start=%d f=%llu",
             boot_log_tick_calls, &boot_progress_monitor_ticks ? (unsigned long long)__atomic_load_n(&boot_progress_monitor_ticks, __ATOMIC_RELAXED) : 0ULL,
             &boot_progress_monitor_step ? boot_progress_monitor_step : 0, boot_log_tick_step,
             boot_log_super_runs, boot_log_super_returns,
             boot_log_tick_elapsed, boot_log_super_elapsed, boot_log_super_start,
             (unsigned long long)boot_log_tick_freq);
    at = put(out, at, cap, row, sizeof row);
  }
  // Which processes were started and ended (BootProgress.c census) and the throw site of the last uncaught exception (kits BootFaultProbe.c).
  if (&boot_progress_spawn_rows) {
    static char spawn[16384];
    const size_t n = boot_progress_spawn_rows(spawn, sizeof spawn);
    if (n) {
      at = put(out, at, cap, "[spawn]", 8);
      for (char *line = spawn; *line;) {
        char *end = strchr(line, '\n');
        const size_t len = end ? (size_t)(end - line) : strlen(line);
        at = put(out, at, cap, line, len);
        if (!end) break;
        line = end + 1;
      }
    }
  }
  // The sampling profile (BootProgress.c): which processes the interrupts landed in and where, at most 6 KiB of the slot.
  if (&boot_progress_profile_rows) {
    static char profile[6144];
    const size_t n = boot_progress_profile_rows(profile, sizeof profile);
    if (n) {
      at = put(out, at, cap, "[profile]", 10);
      for (char *line = profile; *line;) {
        char *end = strchr(line, '\n');
        const size_t len = end ? (size_t)(end - line) : strlen(line);
        at = put(out, at, cap, line, len);
        if (!end) break;
        line = end + 1;
      }
    }
  }
  if (&boot_progress_panic_text && boot_progress_panic_text[0]) {
    at = put(out, at, cap, "[panic]", 8);
    at = put(out, at, cap, boot_progress_panic_text, 2048);
  }
  return at;
}

/* The message buffer, NUL-padded to LogMsgBlocks blocks; returns the bytes copied.
 *
 * The newest bytes, not the oldest. The region holds 128 KiB while the kernel's msgbuf is larger, so
 * every read-back stored exactly 131072 bytes and it was always the start of the ring: the copy ran
 * from msg_bufx, the oldest byte, and stopped at the limit. A crash in the last seconds of a boot
 * therefore left nothing behind, which is precisely the window an abort needs (u250 to u255 all
 * reported msg_bytes 131072). Starting at msg_bufx + size - cap keeps the tail instead. */
static uint32_t copy_msgbuf(uint8_t *out) {
  memset(out, 0, LogMsgBlocks * LogBlock);
  struct boot_log_msgbuf *m = msgbufp;
  if (!m || !m->msg_bufc || m->msg_size <= 0) return 0;
  int size = m->msg_size, x = m->msg_bufx;
  if (x < 0 || x >= size) x = 0;
  const uint32_t cap = LogMsgBlocks * LogBlock;
  const uint32_t keep = (uint32_t)size < cap ? (uint32_t)size : cap;
  const int start = (int)(((uint32_t)x + (uint32_t)size - keep) % (uint32_t)size);
  uint32_t n = 0;
  for (uint32_t i = 0; i < keep; ++i) {
    char c = m->msg_bufc[(start + (int)i) % size];
    if (c) out[n++] = (uint8_t)c;
  }
  return n;
}

// One line on the USB console. The medium cannot report its own silence, so the writer says what it
// is doing on the only channel that is still live when it stops: an open refusal, a round it stood
// down for, and every tenth round that landed.
static void log_say(int kind, unsigned a, unsigned b)
{
  if (!&qcom_usb_debug_write) return;
  char line[96];
  const int n = kind == 0 ? snprintf(line, sizeof line, "BOOTLOG skip %u phase %u\n", a, b)
              : kind == 1 ? snprintf(line, sizeof line, "BOOTLOG round %u slots %u\n", a, b)
                          : snprintf(line, sizeof line, "BOOTLOG beat %u phase %u\n", a, b);
  if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

static void writer(void *arg, wait_result_t result) {
  (void)arg; (void)result;
  thread_set_thread_name(current_thread(), "boot_log");
  uint64_t first = 0, blocks = 0;
  const char *why = "";
  boot_log_state = 1;
  // The window exists once NativeUfs has read the root layout; until then the open is refused ("no root layout").
  // The refusal is also said on the USB console: fix157 ran a whole boot with working touch and wrote no header at
  // all, and the medium cannot explain an absence, so the reason has to leave the kernel another way.
  while (!native_ufs_log_open(&first, &blocks, &why)) {
    if (strcmp(why, "no root layout")) {
      boot_log_state = 3;
      if (&qcom_usb_debug_write) {
        char line[96];
        const int n = snprintf(line, sizeof line, "BOOTLOG open refused: %s\n", why ? why : "?");
        if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
      }
      return;
    }
    IOSleep(1000);
  }
  if (&qcom_usb_debug_write) {
    char line[80];
    const int n = snprintf(line, sizeof line, "BOOTLOG open first=%llu blocks=%llu\n",
                           (unsigned long long)first, (unsigned long long)blocks);
    if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
  }
  boot_log_first_lba = first;
  /* Put the previous run's log back on the console before the new header replaces it. An
   * unattended round ends with the USB link gone, so the medium is the only record, and the next
   * boot is the only chance to show it. The tail is what matters: that is the state the reset
   * left behind.
   * Every read of this window fills whole 4096-byte blocks, so each destination must hold
   * blocks*4096 bytes. The header goes through the byte-granular call, which stages the block
   * itself. Passing &previous to the block call is what faulted boot-usb99: 4096 bytes landed in
   * a 152-byte stack object and ran on into the guard page (pc in memcpy, lr in hostRead4k,
   * far = sp + 0x3b0, x20 = 0x1000). */
  if (&native_ufs_log_read && &native_ufs_log_read_bytes &&
      blocks > (uint64_t)LogSlotFirst + LogSlotBlocks) {
    _Static_assert(sizeof(struct boot_log_header) <= LogBlock, "the header must fit in one block");
    struct boot_log_header previous;
    if (native_ufs_log_read_bytes(0, sizeof previous, &previous) && memcmp(previous.magic, "SHLOG001", 8) == 0 &&
        previous.slot_count && previous.slots_written && previous.slot_blocks == LogSlotBlocks) {
      uint32_t slot = (previous.slots_written - 1u) % previous.slot_count;
      /* Sized to exactly what the block read writes: LogSlotBlocks * LogBlock bytes. */
      static char previous_slot[LogSlotBlocks * LogBlock];
      if (native_ufs_log_read(LogSlotFirst + (uint64_t)slot * LogSlotBlocks, LogSlotBlocks, previous_slot)) {
        size_t end = sizeof previous_slot;
        while (end && previous_slot[end - 1] == 0) --end;
        size_t begin = end > 1536 ? end - 1536 : 0;
        if (&qcom_usb_debug_write) {
          char head[96];
          int n = snprintf(head, sizeof head, "PREVLOG run=%llu slots=%u err=%u tail=%zu bytes\n",
                           (unsigned long long)previous.boot_id, previous.slots_written,
                           previous.write_errors, end);
          if (n > 0) qcom_usb_debug_write(head, (size_t)n);
          if (end > begin) qcom_usb_debug_write(previous_slot + begin, end - begin);
        }
      }
    }
  }
  const uint32_t slot_count = (uint32_t)((blocks - LogSlotFirst) / LogSlotBlocks);
  if (&native_ufs_export_blocks && &native_ufs_export_write) {
    IOLockLock(export_lock);
    export_region = (uint32_t)native_ufs_export_blocks();
    IOLockUnlock(export_lock);
  }
  struct boot_log_header h;
  memset(&h, 0, sizeof h);
  memcpy(h.magic, "SHLOG001", 8);
  h.version = 5; h.header_bytes = sizeof h;
  h.export_blocks = export_region; h.export_below = export_region; h.boot_id = log_ticks(); h.first_lba = first; h.window_blocks = (uint32_t)blocks;
  h.msg_first = LogMsgFirst; h.msg_blocks = LogMsgBlocks; h.slot_first = LogSlotFirst; h.slot_blocks = LogSlotBlocks; h.slot_count = slot_count;
  h.gpt_open = h.gpt_last = &native_ufs_store_gpt_check ? native_ufs_store_gpt_check() : 0xff;
  if (&boot_progress_tag) strlcpy(h.tag, boot_progress_tag(), sizeof h.tag);
  uint64_t f = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); if (!f) f = 19200000;
  const uint64_t start = log_ticks();
  // The reboot-into-fastboot path is not here any more: boot_log_tick owns it, because this loop
  // can stop (state 3 on three failed rounds, state 4 on a GPT change, or blocked in a UFS write,
  // measured on boot 43ba9162) and the board still has to come back. This loop only reports that a
  // round landed, through log_sync_seq.
  // sheng-diag-export=<seconds of log time>: export the diagnostic files once then (set it shortly before sheng-autoreset).
  uint32_t diag_at = 0;
  if (&boot_diag_scan && &boot_diag_stream) PE_parse_boot_argn("sheng-diag-export", &diag_at, sizeof diag_at);
  uint32_t errors = 0;
  boot_log_state = 2;
  for (;;) {
    const unsigned now = (unsigned)((log_ticks() - start) / f);
    int ok = 1;
    unsigned bad = 0;
    // The header goes out FIRST, before the slot and the message buffer, and it names the round
    // that is about to run. A round that hangs inside one of the writes after this point leaves
    // the medium saying "round N started, uptime T" instead of the last round that happened to
    // finish -- which is the only way a reader can tell a stalled round from a stalled system.
    // slots_written and rounds are not incremented here, so a reader still sees the last complete
    // round; uptime_s is the round's own start time, and that is what names the stall.
    h.uptime_s = now; h.state = boot_log_state;
    memset(boot_log_buffer, 0, LogBlock);
    memcpy(boot_log_buffer, &h, sizeof h);
    log_phase = 5;
    if (h.rounds < 3) log_say(2, h.rounds, 5);   // before the header write
    { const int w = native_ufs_log_write(0, 1, boot_log_buffer);
      if (w == 2) {++boot_log_skips; log_phase = 12; if (boot_log_skips <= 5 || !(boot_log_skips % 20)) log_say(0, boot_log_skips, (unsigned)log_phase); IOSleep(LogPeriodMs); continue;}
      ok &= w != 0; if (!ok && !bad) bad = 5; }
    // Text slot, then the message buffer. `bad` keeps its numbering: 1 slot, 3 message buffer,
    // 5 header, 7 sync, so a failed round can name it on the console.
    if (h.rounds < 3) log_say(2, h.rounds, 1);   // before compose
    memset(boot_log_buffer, 0, LogSlotBlocks * LogBlock);
    compose((char *)boot_log_buffer, LogSlotBlocks * LogBlock, h.slots_written, now);
    log_phase = 1;
    if (h.rounds < 3) log_say(2, h.rounds, 2);   // before the slot write
    { const int w = native_ufs_log_write(LogSlotFirst + (uint64_t)(h.slots_written % slot_count) * LogSlotBlocks, LogSlotBlocks, boot_log_buffer);
      if (w == 2) {++boot_log_skips; log_phase = 12; if (boot_log_skips <= 5 || !(boot_log_skips % 20)) log_say(0, boot_log_skips, (unsigned)log_phase); IOSleep(LogPeriodMs); continue;}
      ok &= w != 0; if (!ok && !bad) bad = 1; }
    log_phase = 2;
    if (h.rounds < 3) log_say(2, h.rounds, 3);   // before the message-buffer copy
    h.msg_bytes = copy_msgbuf(boot_log_buffer);
    log_phase = 3;
    if (h.rounds < 3) log_say(2, h.rounds, 4);   // before the message-buffer write
    { const int w = native_ufs_log_write(LogMsgFirst, LogMsgBlocks, boot_log_buffer);
      if (w == 2) {++boot_log_skips; log_phase = 12; if (boot_log_skips <= 5 || !(boot_log_skips % 20)) log_say(0, boot_log_skips, (unsigned)log_phase); IOSleep(LogPeriodMs); continue;}
      ok &= w != 0; if (!ok && !bad) bad = 3; }
    // The header lands again after the slot and the message buffer are in, so the round that
    // finished is durable too; the first write above only named the round that started.
    // The counters move before this write so the header carries the finished round, but a busy
    // host lock takes the `continue` below, which would leave them advanced with no header on the
    // medium. Roll them back on that path. Measured 2026-11-07 on of6: BLPHASE read round=113
    // slots=113 err=0 while a direct read of block 0 returned version 4 / tag usb190 / slots 509,
    // so the reader saw the previous boot's header and reported diag_bytes 0.
    const uint32_t roundsBefore = boot_log_rounds;
    const uint32_t slotsBefore = h.slots_written;
    const uint32_t errorsBefore = h.write_errors;
    h.rounds = ++boot_log_rounds;
    if (ok) ++h.slots_written;
    else ++h.write_errors;
    __atomic_store_n(&boot_log_slots_hint, h.slots_written, __ATOMIC_RELAXED);
    memset(boot_log_buffer, 0, LogBlock);
    memcpy(boot_log_buffer, &h, sizeof h);
    { const int w = native_ufs_log_write(0, 1, boot_log_buffer);
      if (w == 2) {
        boot_log_rounds = roundsBefore; h.slots_written = slotsBefore; h.write_errors = errorsBefore;
        __atomic_store_n(&boot_log_slots_hint, slotsBefore, __ATOMIC_RELAXED);
        ++boot_log_skips; log_phase = 12; if (boot_log_skips <= 5 || !(boot_log_skips % 20)) log_say(0, boot_log_skips, (unsigned)log_phase); IOSleep(LogPeriodMs); continue;
      }
      ok &= w != 0; if (!ok && !bad) bad = 5; }
    /* Take the frozen early-boot snapshot exactly once, while the boot-time IOLog is still inside
     * the kernel ring. copy_msgbuf always returns the newest 131072 bytes, so a copy taken at
     * t=LogFrozenAtS covers t=0..LogFrozenAtS. Writing it to LogFrozenFirst and never touching that
     * block again is what survives a long run; rotating the working block cannot, because the bytes
     * that fall out of the kernel ring are gone.
     *
     * Trigger on the writer's own uptime, not on a round count. A round is 2 s of sleep plus a UFS
     * write, a GPT check every 30 rounds and any diagnostic export, so rounds and seconds drift apart:
     * at round 60 a slow run could already be past the ~132 s ring and the snapshot would begin
     * mid-boot, which reads as "the freeze did not work". LogFrozenAtS = 100 leaves 32 s of margin
     * and frozen_round still records which round actually took it. frozen_first is published only
     * after the block landed, so a reader never sees a block number for a write that failed. */
    if (!h.frozen_first && now >= LogFrozenAtS) {
      log_phase = 11;
      memset(boot_log_buffer, 0, LogFrozenBlocks * LogBlock);
      (void)copy_msgbuf(boot_log_buffer);
      if (native_ufs_log_write(LogFrozenFirst, LogFrozenBlocks, boot_log_buffer)) {
        h.frozen_first = LogFrozenFirst;
        h.frozen_round = h.slots_written;
      }
      log_phase = 3;
    }
    log_phase = 4;
    // The newest complete export is already on the medium; the header names it.
    IOLockLock(export_lock);
    h.export_generation = export_ready; h.export_bytes = export_ready_bytes; h.export_first = export_ready_first;
    h.diag_first = diag_first; h.diag_bytes = diag_bytes;
    IOLockUnlock(export_lock);
    if (diag_at && now >= diag_at && !boot_diag_status) {
      thread_t diag;
      boot_diag_status = 1;
      if (kernel_thread_start_priority(diag_thread, NULL, MINPRI_KERNEL, &diag) == KERN_SUCCESS) thread_deallocate(diag);
      else boot_diag_status = -10;
    }
    // Every 30 rounds the GPT is read back; any change stops the writer for good.
    if (&native_ufs_store_gpt_check && !(h.rounds % 30)) h.gpt_last = native_ufs_store_gpt_check();
    log_phase = 6;
    log_phase = 7;
    if (h.rounds < 3) log_say(2, h.rounds, 6);   // before the flush
    { const int w = native_ufs_log_sync();
      if (w == 2) {++boot_log_skips; log_phase = 12; if (boot_log_skips <= 5 || !(boot_log_skips % 20)) log_say(0, boot_log_skips, (unsigned)log_phase); IOSleep(LogPeriodMs); continue;}
      ok &= w != 0; if (!ok && !bad) bad = 7; }
    log_phase = 8;
    if (h.rounds < 3) log_say(2, h.rounds, 7);   // the round landed
    if (ok) __atomic_fetch_add(&log_sync_seq, 1, __ATOMIC_RELAXED);
    // A round that fails says so on the console, where the console is the only channel left once
    // the medium has stopped taking writes: the step that failed (1 slot, 3 message buffer,
    // 5 header, 7 sync) and the consecutive-failure count.
    if (!ok) {
      boot_log_errors = ++errors;
      if (&qcom_usb_debug_write) {
        char line[72];
        const int n = snprintf(line, sizeof line, "BLLOGFAIL round=%u step=%u errors=%u\n", boot_log_rounds, bad, errors);
        if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
      }
    } else errors = 0;
    // Three consecutive failures used to end the writer for the rest of the boot. A medium that is
    // busy while the Data store attaches is exactly the case that recovers, so the writer keeps
    // trying at a slower cadence and records state 3 for as long as it is failing.
    if (errors >= LogMaxErrors) {
      boot_log_state = 3;
      IOSleep(LogRetryMs);
      errors = 0;
      continue;
    }
    boot_log_state = 2;
    if (h.gpt_last != h.gpt_open) {boot_log_state = 4; return;}
    log_phase = 9;
    IOSleep(LogPeriodMs);
    log_phase = 10;   // woke; anything still at 9 is inside the sleep, anything at 10 is compose
  }
}

// An independent kernel thread driving the autoreset supervisor so the board returns to
// fastboot even if monitor() yields or stalls after display handoff.
// The reset, once the deadline is reached. Called from the deadline owners below, which are the board monitor and the supervisor.
static void boot_log_fire_reset(unsigned elapsed) {
  tick_reset_tried = 1;
  reset_pet_stopped = 1;   // let the hardware bark: every software path below returns 99 here
  // The deadline is reached, and that fact has to leave the kernel even when the reset then works or is
  // refused silently: the console is the only channel that survives both.
  if (&qcom_usb_debug_write) {
    char line[72];
    const int n = snprintf(line, sizeof line, "AUTORESET deadline %us reached at %us\n", tick_autoreset, elapsed);
    if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
  }
  if (!&boot_reset_bootloader) {
    strlcpy(boot_log_reset_result, "autoreset skipped: no reset entry point", sizeof boot_log_reset_result);
    return;
  }
  const int rc = boot_reset_bootloader(boot_log_reset_result, sizeof boot_log_reset_result);
  char tail[24]; snprintf(tail, sizeof tail, " (returned %d)", rc);
  strlcat(boot_log_reset_result, tail, sizeof boot_log_reset_result);
  if (&qcom_usb_debug_write) {
    char line[160];
    const int n = snprintf(line, sizeof line, "AUTORESET refused: %s\n", boot_log_reset_result);
    if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
  }
  if (rc != 0) {
    IOSleep(2000);
    tick_reset_tried = 0;
  }
}

// The deadline's own thread. It does NOT call boot_log_tick: that function runs the per-second probes
// (DpuProbe, the GPU frequency reader) which were written for one caller, and two threads inside them at
// once is what a boot with the monitor and this thread both ticking can do. The whole point of this
// thread is to survive a monitor that stops, so it must not share the monitor's work; it measures its own
// time from the hardware counter and fires the reset itself.
static void autoreset_supervisor(void *arg, wait_result_t result) {
  (void)arg; (void)result;
  thread_set_thread_name(current_thread(), "autoreset");
  uint64_t f = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); if (!f) f = 19200000;
  const uint64_t start = log_ticks();
  unsigned reported = 0;
  while (!tick_reset_tried) {
    // Before the first sleep: this line alone says the thread was entered. Without it "never
    // started" and "started and stuck in its first IOSleep" produce the same console.
    if (boot_log_super_runs == 0 && &qcom_usb_debug_write) {
      const char *entered = "SUP entered\n";
      qcom_usb_debug_write(entered, 12);
    }
    IOSleep(1000);
    boot_log_super_runs = boot_log_super_runs + 1;
    boot_log_super_returns = boot_log_super_returns + 1;
    const unsigned elapsed = (unsigned)((log_ticks() - start) / f);
    boot_log_super_elapsed = elapsed;
    // Beacons around the first two pets. A boot that loses its supervisor loses the round to the
    // watchdog, so the difference between "the thread never started", "it started and the pet's
    // register write hung" and "it is running and petting" has to be readable from the console.
    if (boot_log_super_runs <= 2 && &qcom_usb_debug_write) {
      char b[64];
      const int bn = snprintf(b, sizeof b, "SUPRUN n=%u t=%us pre-pet\n", boot_log_super_runs, elapsed);
      if (bn > 0) qcom_usb_debug_write(b, (size_t)bn < sizeof b ? (size_t)bn : sizeof b - 1);
    }
    /* The image's own name on the console, once. Until now sheng-build-tag reached only the
       persistent-log header (BootProgress.c:947) and the boot page, so 0 of 298 saved console
       captures could say which image they came from, and the 2026-10-30 u268/u269 rounds ran an
       older slot image while the operators believed they were testing a new build. The Darwin
       version string is no substitute: it is byte-identical across images that print different
       row sets. porting.toml's packaging area asks for exactly this reading. */
    if (boot_log_super_runs == 1 && &qcom_usb_debug_write) {
      char tag[32] = {0};
      PE_parse_boot_argn("sheng-build-tag", tag, sizeof tag);
      tag[sizeof tag - 1] = 0;
      char b[64];
      const int bn = snprintf(b, sizeof b, "BUILD tag=%s\n", tag[0] ? tag : "none");
      if (bn > 0) qcom_usb_debug_write(b, (size_t)bn < sizeof b ? (size_t)bn : sizeof b - 1);
    }
    // One register write a second. Its absence is the signal: a supervisor that stops, and a
    // deadline whose software reset failed, both end in the same hardware bark.
    if (!reset_pet_stopped && &boot_reset_pet) boot_reset_pet();
    if (boot_log_super_runs <= 2 && &qcom_usb_debug_write) {
      char b[64];
      const int bn = snprintf(b, sizeof b, "SUPRUN n=%u t=%us pet=%d\n", boot_log_super_runs, elapsed, boot_reset_wdt_stats[1]);
      if (bn > 0) qcom_usb_debug_write(b, (size_t)bn < sizeof b ? (size_t)bn : sizeof b - 1);
    }
    if (boot_log_super_runs - reported >= 10) {
      reported = boot_log_super_runs;
      char line[224];
      // Everything the answer to "did the desktop come up" needs, from a thread that does not share the monitor's or the
      // writer's work: the display's swap counters (a client submitted / acceptance), the boot page's ownership flag, the
      // pre-write hook, and the Data store's state (3 fresh, 2 resumed, 7 kept). A boot that draws no frame has swap=0
      // here; a boot that draws has it climbing.
      /* tf/lp are the touch loop's frame count and the step it is in. A boot that stops
       * compositing also stops that loop, and the two readings on one line are what say whether
       * they are the same event without needing the persistent log, which stalls on its own.
       * tf alone cannot say why it stopped: step 0 covers the 6 ms sleep, and native_touch_frames
       * is incremented only after a successful read, so "frozen at lp=0" is equally "every read
       * failed" (FocalTouchProbe.cpp:614-626). tfrc is the last read's negative status, tfs the
       * first failure's step, and tlow the IRQ line's low count, which climbs on every iteration
       * whether the read succeeds or not. A frozen tf with tlow climbing is a dead controller; a
       * frozen tf with tlow frozen is a thread that is no longer scheduled. */
      // wdt is the arm flag and the pet count: a boot whose supervisor runs has pets climbing once
      // a second, so a pet count that stops while this row keeps printing is a watchdog write that
      // no longer lands, and a row that stops printing is a supervisor that stopped.
      const int n = snprintf(line, sizeof line, "SUPER t=%us runs=%u calls=%u mon=%llu sw=%u/%u/%u own=%u prep=%u data=%u tf=%u lp=%u tc=%u tfrc=%u tfs=%u tlow=%u vr=%u wdt=%d/%d u=%08x d=%08x bg=%08x ns=%u\n", elapsed,
                             boot_log_super_runs, boot_log_tick_calls,
                             &boot_progress_monitor_ticks ? (unsigned long long)__atomic_load_n(&boot_progress_monitor_ticks, __ATOMIC_RELAXED) : 0ULL,
                             &native_legacy_swap_validation_calls ? native_legacy_swap_validation_calls : 0u,
                             &native_legacy_swap_accepted_count ? native_legacy_swap_accepted_count : 0u,
                             &native_legacy_swap_rejected_count ? native_legacy_swap_rejected_count : 0u,
                             &boot_progress_display_owned ? boot_progress_display_owned : 0u,
                             &native_legacy_prepare_calls ? native_legacy_prepare_calls : 0u,
                             &native_data_persist_state ? (unsigned)__atomic_load_n(&native_data_persist_state, __ATOMIC_RELAXED) : 0u,
                             &native_touch_frames ? __atomic_load_n(&native_touch_frames, __ATOMIC_RELAXED) : 0u,
                             &native_touch_loop_step ? __atomic_load_n(&native_touch_loop_step, __ATOMIC_RELAXED) : 0u,
                             &native_touch_core ? __atomic_load_n(&native_touch_core, __ATOMIC_RELAXED) : 0u,
                             &native_touch_frame_rc ? __atomic_load_n(&native_touch_frame_rc, __ATOMIC_RELAXED) : 0u,
                             &native_touch_fail_step ? __atomic_load_n(&native_touch_fail_step, __ATOMIC_RELAXED) : 0u,
                             &native_touch_irq_low ? __atomic_load_n(&native_touch_irq_low, __ATOMIC_RELAXED) : 0u,
                             &native_legacy_swap_validation_reason ? native_legacy_swap_validation_reason : 0u,
                             boot_reset_wdt_stats[0], boot_reset_wdt_stats[1],
                             &native_legacy_swap_req ? native_legacy_swap_req[0] : 0u,
                             &native_legacy_swap_req ? native_legacy_swap_req[1] : 0u,
                             &native_legacy_swap_req ? native_legacy_swap_req[2] : 0u,
                             &native_legacy_swap_req ? native_legacy_swap_req[3] : 0u);
      if (n > 0 && &qcom_usb_debug_write) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
      /* Per-core interrupt health, next to the row it explains. On this board two different timed
       * waits stop about six seconds into the boot while the supervisor's own one keeps ticking:
       * the boot-progress monitor stops after seven iterations in every boot, and the touch loop
       * freezes at about 840 frames with native_touch_frame_rc, native_touch_fail_step and
       * native_touch_irq_low all at zero, i.e. asleep in clock_delay_until after a successful read
       * and never woken (2026-11-06, console-s903542: tf=839 lp=0 tfrc=0 tfs=0 tlow=0 while the
       * desktop was up and sw=8). ArmCpu counts each core's architectural timer interrupts and
       * SGI 0 in armcpu_core_timer[] and armcpu_core_sgi[], and both were printed nowhere. A core
       * whose timer count freezes while another's climbs is a core that stopped taking its timer,
       * which is the one failure that would leave a thread blocked in a deadline wait for ever
       * without failing a single read. irq/ipi/sent are the totals and what the kernel asked for. */
      if (&armcpu_core_timer) {
        char core[320];
        int cn = snprintf(core, sizeof core, "CPUT t=%us irq=%llu ipi=%llu sent=%llu |", elapsed,
                          (unsigned long long)(&armcpu_timer_interrupts ? __atomic_load_n(&armcpu_timer_interrupts, __ATOMIC_RELAXED) : 0ull),
                          (unsigned long long)(&armcpu_ipi_interrupts ? __atomic_load_n(&armcpu_ipi_interrupts, __ATOMIC_RELAXED) : 0ull),
                          (unsigned long long)(&armcpu_ipi_sent ? __atomic_load_n(&armcpu_ipi_sent, __ATOMIC_RELAXED) : 0ull));
        /* The boot core is not one of the running secondaries, so the old bound (armcpu_smp_running) printed
         * c0..c6 on an eight-core board and never c7. The row is the boot core plus every secondary that
         * reported running, capped at the eight logical CPUs the device tree declares. */
        unsigned cores = (&armcpu_smp_running ? (unsigned)__atomic_load_n(&armcpu_smp_running, __ATOMIC_RELAXED) : 0u) + 1u;
        if (cores > 8) cores = 8;
        for (unsigned c = 0; c < cores && cn > 0 && (unsigned)cn < sizeof core - 24; ++c)
          cn += snprintf(core + cn, sizeof core - (unsigned)cn, " c%u=%u/%u",
                         c, __atomic_load_n(&armcpu_core_timer[c], __ATOMIC_RELAXED),
                         __atomic_load_n(&armcpu_core_sgi[c], __ATOMIC_RELAXED));
        /* A core whose timer count has not moved for two consecutive samples is deaf: it is taking no
         * interrupt at all, not missing a deadline. XNU keeps it registered and can place a thread there,
         * and that thread's wakeup never arrives. The mask says which cores a wakeup must avoid, and it
         * costs nothing when every core is healthy (deaf=00 forever). */
        static uint32_t lastTimer[8];
        static uint8_t unmoved[8];
        unsigned deaf = 0;
        for (unsigned c = 0; c < cores; ++c) {
          const uint32_t now = __atomic_load_n(&armcpu_core_timer[c], __ATOMIC_RELAXED);
          if (now == lastTimer[c]) {if (unmoved[c] < 255) ++unmoved[c];}
          else {unmoved[c] = 0; lastTimer[c] = now;}
          if (unmoved[c] >= 2) deaf |= 1u << c;
        }
        /* What each core says about itself, from ArmCpu's own arrays: a step below 7 is a core that never
         * reached running, and a nonzero fail says which check stopped it. Printed as masks so the row
         * stays one line; both are zero on a healthy boot. */
        unsigned badStep = 0, failed = 0;
        if (&armcpu_smp_step)
          for (unsigned c = 0; c < 8; ++c) {
            const uint32_t st = __atomic_load_n(&armcpu_smp_step[c], __ATOMIC_RELAXED);
            if (st && st != 7) badStep |= 1u << c;
          }
        if (&armcpu_smp_fail)
          for (unsigned c = 0; c < 8; ++c)
            if (__atomic_load_n(&armcpu_smp_fail[c], __ATOMIC_RELAXED)) failed |= 1u << c;
        if (cn > 0 && (unsigned)cn < sizeof core - 32)
          cn += snprintf(core + cn, sizeof core - (unsigned)cn, " run=%u deaf=%02x st=%02x fl=%02x",
                         &armcpu_smp_running ? (unsigned)__atomic_load_n(&armcpu_smp_running, __ATOMIC_RELAXED) : 0u,
                         deaf, badStep, failed);
        if (cn > 0) {core[cn++] = '\n'; if (&qcom_usb_debug_write) qcom_usb_debug_write(core, (size_t)cn);}
        // Same cores again, from the deadline side: how often the architectural timer was already past due and
        // how far ahead the last one was. A thread blocked in a timed wait whose deadline is skipped here never
        // wakes, so this is the row that turns "the touch loop stopped" into a timer question.
        if (&armcpu_timer_short) {
          char miss[192];
          int mn = snprintf(miss, sizeof miss, "CPUTM t=%us q=%u h=%u irqph=%u |", elapsed,
                            &armcpu_quiesce_calls ? __atomic_load_n(&armcpu_quiesce_calls, __ATOMIC_RELAXED) : 0u,
                            &armcpu_halt_calls ? __atomic_load_n(&armcpu_halt_calls, __ATOMIC_RELAXED) : 0u,
                            &armcpu_irq_phase ? __atomic_load_n(&armcpu_irq_phase, __ATOMIC_RELAXED) : 0u);
          for (unsigned c = 0; c < cores && mn > 0 && (unsigned)mn < sizeof miss - 22; ++c)
            mn += snprintf(miss + mn, sizeof miss - (unsigned)mn, " c%u=%u/%d",
                           c, __atomic_load_n(&armcpu_timer_short[c], __ATOMIC_RELAXED),
                           __atomic_load_n(&armcpu_timer_ahead[c], __ATOMIC_RELAXED));
          if (mn > 0) {miss[mn++] = '\n'; if (&qcom_usb_debug_write) qcom_usb_debug_write(miss, (size_t)mn);}
        }
        /* The boot-ui-trace worker, on the console. The stack capture needs boot-ui-trace=1 and runs on
           the worker's 44th..48th call, about 90 s; on 2026-11-06 the console showed six "STK none" rows
           and there was no way to tell whether the worker never got that far or a capture started and
           never returned. a= entered, p= captures started, s= processes seen, b= one still in flight. */
        if (&boot_process_attempts) {
          char row[96];
          const int rn = snprintf(row, sizeof row, "PROC t=%us a=%u p=%u s=%u b=%u\n", elapsed,
                                  __atomic_load_n(&boot_process_attempts, __ATOMIC_RELAXED),
                                  &boot_process_passes ? __atomic_load_n(&boot_process_passes, __ATOMIC_RELAXED) : 0u,
                                  &boot_process_seen ? __atomic_load_n(&boot_process_seen, __ATOMIC_RELAXED) : 0u,
                                  &boot_process_busy ? __atomic_load_n(&boot_process_busy, __ATOMIC_RELAXED) : 0u);
          if (rn > 0 && &qcom_usb_debug_write) qcom_usb_debug_write(row, (size_t)rn);
        }
      }
      // The refresh tick and the direct-scanout window, on the console. The six vsync counters
      // existed but nothing printed them (they were "kept for a future reader"), and the DPU
      // window's state only reached the UFS slot -- which is exactly the half a boot that stops
      // before the desktop never writes. Together they answer the one question left on a boot
      // that draws nothing: was the render server's refresh tick armed and firing, and was the
      // window that lets the display read the compositor's surface installed.
      //   arm/tick/idle/late/start/pub are native_legacy_vsync_observed's six events.
      //   ds is NativeLegacyDisplay's direct-stage: 1 no window, 2 not primary, 3 armed and
      //   waiting for NativeUfs, 5 armed but the MDP map failed, 0 taken.
      if (&native_legacy_vsync_counts) {
        uint64_t v[6] = {0, 0, 0, 0, 0, 0};
        unsigned vhz = 0;
        native_legacy_vsync_counts(v, &vhz);
        char r[208];
        const int rn = snprintf(r, sizeof r,
                                "VSYNC arm=%llu tick=%llu idle=%llu late=%llu start=%llu pub=%llu hz=%u dpu=%08x ds=%u\n",
                                (unsigned long long)v[0], (unsigned long long)v[1],
                                (unsigned long long)v[2], (unsigned long long)v[3],
                                (unsigned long long)v[4], (unsigned long long)v[5], vhz,
                                &native_ufs_dpu_state ? native_ufs_dpu_state : 0u,
                                &native_legacy_direct_stage ? native_legacy_direct_stage : 0u);
        if (rn > 0 && &qcom_usb_debug_write)
          qcom_usb_debug_write(r, (size_t)rn < sizeof r ? (size_t)rn : sizeof r - 1);
      }
      // What the panel's buffer holds right now, sampled here rather than in the monitor loop that stops early.
      // nonblack/1000 and luma are the same measure the GPUFB row uses, so a black screen and a drawn desktop differ
      // by orders of magnitude instead of by opinion. off/comp/fail/aban are the GPU hand-off counters: an offer that
      // never completes is a frame the render server sent and the display never showed.
      if (&boot_progress_panel_sample) {
        unsigned p[6] = {0, 0, 0, 0, 0, 0};
        boot_progress_panel_sample(p);
        {extern void surfaceLateObserved(void); surfaceLateObserved();}
        // The two mappings both claim to be the firmware's frame buffer and both were checked against PE_Video's
        // v_baseAddr: the presenter's (what the GPU copy writes into) and the display driver's (what this row
        // samples). If they differ, the copy is landing in memory the panel never scans out.
        extern volatile uint64_t native_gop_physical;
        extern volatile uint64_t native_legacy_gop_physical;
        extern volatile uint32_t native_legacy_fence_probe[4];
        extern volatile uint64_t native_adreno_readonly_imports;
        extern volatile uint64_t native_adreno_readonly_address;
        extern volatile uint64_t native_adreno_readonly_bytes;
        // The read-only import's first physical page: compared with gop= above, this says whether the
        // range that could not be wired for writing is the framebuffer itself or something else.
        extern volatile uint64_t native_adreno_readonly_phys;
        // The three most recent frame-sized imports' first physical pages, newest first. The framebuffer
        // is one of them; if none of these is 9c000000, the GPU's copy never had the panel's pages.
        extern volatile uint64_t native_adreno_large_imports[11];
        // The process whose range had to be imported read-only: a render server's own surfaces and a
        // sampler's read-only texture look identical in the counters, and only one of them is a fault.
        extern char native_adreno_readonly_proc[17];
        // FSR/FAR/FSYNR0 of the SMMU context the GPU runs on. stall_on_fault=0 and no fault interrupt mean a
        // refused store is dropped silently, so this is the only place the refusal shows.
        extern bool native_adreno_fault_read(uint32_t *);
        // The page-table class the last imported range was programmed with: permissions, cache index, shareability.
        extern volatile uint32_t native_adreno_map_attrs[4];
        uint32_t fault[4] = {0, 0, 0, 0};
        if (&native_adreno_fault_read) native_adreno_fault_read(fault);
        extern volatile uint32_t native_adreno_import_plain_writable;
        extern volatile uint64_t native_adreno_readonly_large[3];
        extern volatile uint64_t native_adreno_import_diag[10];
        char q[640];
        const int qn = snprintf(q, sizeof q,
                                "PANEL nonblack=%u/1000 luma=%u rgb=%u/%u/%u sum=%08x gpu off=%llu comp=%llu fail=%llu aban=%llu gop=%llx/%llx fen=%u/%u/%u/%u ro=%llu roa=%llx/%llu pw=%u imp=%llu/%llu rlg=%llu/%llx/%llu rop=%llx lip=%llx,%llx,%llx lsp=%llu/%llx ropn=%s fs=%x/%x/%x/%x ma=%u/%u/%u cp=%u/%x tlb=%u/%u/%x\n",
                                p[0], p[1], p[2], p[3], p[4], p[5],
                                &native_gop_gpu_offers ? (unsigned long long)native_gop_gpu_offers : 0ULL,
                                &native_gop_gpu_completions ? (unsigned long long)native_gop_gpu_completions : 0ULL,
                                &native_gop_gpu_failures ? (unsigned long long)native_gop_gpu_failures : 0ULL,
                                &native_gop_gpu_abandoned ? (unsigned long long)native_gop_gpu_abandoned : 0ULL,
                                (unsigned long long)native_gop_physical, (unsigned long long)native_legacy_gop_physical,
                                native_legacy_fence_probe[0], native_legacy_fence_probe[1], native_legacy_fence_probe[2],
                                native_legacy_fence_probe[3],
                                (unsigned long long)native_adreno_readonly_imports,
                                (unsigned long long)native_adreno_readonly_address,
                                (unsigned long long)native_adreno_readonly_bytes,
                                (unsigned)native_adreno_import_plain_writable,
                                (unsigned long long)native_adreno_import_diag[0],
                                (unsigned long long)native_adreno_import_diag[1],
                                (unsigned long long)native_adreno_readonly_large[0],
                                (unsigned long long)native_adreno_readonly_large[1],
                                (unsigned long long)native_adreno_readonly_large[2],
                                (unsigned long long)native_adreno_readonly_phys,
                                (unsigned long long)native_adreno_large_imports[1],
                                (unsigned long long)native_adreno_large_imports[3],
                                (unsigned long long)native_adreno_large_imports[5],
                                (unsigned long long)native_adreno_large_imports[7],
                                (unsigned long long)native_adreno_large_imports[9],
                                native_adreno_readonly_proc[0] ? native_adreno_readonly_proc : "-",
                                fault[0], fault[1], fault[2], fault[3],
                                native_adreno_map_attrs[0], native_adreno_map_attrs[1], native_adreno_map_attrs[2],
                                &native_adreno_cp_aperture ? native_adreno_cp_aperture[3] : 0u,
                                &native_adreno_cp_aperture ? native_adreno_cp_aperture[2] : 0u,
                                &native_adreno_ttbr0_tlb ? native_adreno_ttbr0_tlb[0] : 0u,
                                &native_adreno_ttbr0_tlb ? native_adreno_ttbr0_tlb[1] : 0u,
                                &native_adreno_ttbr0_tlb ? native_adreno_ttbr0_tlb[2] : 0u);
        if (qn > 0 && &qcom_usb_debug_write)
          qcom_usb_debug_write(q, (size_t)qn < sizeof q ? (size_t)qn : sizeof q - 1);
      }
      // What the last verdict was made on: layer masks, surface count, the request geometry and the
      // display it was compared with. One line, only when a swap has been seen.
      if (&native_legacy_swap_req && native_legacy_swap_req[4]) {
        char q[200];
        const int m = snprintf(q, sizeof q,
                 "SWAPREQ n=%u u=%08x d=%08x bg=%08x ns=%u dims=%ux%u rect=%u,%u,%u,%u disp=%ux%u\n",
                 native_legacy_swap_req[4], native_legacy_swap_req[0], native_legacy_swap_req[1],
                 native_legacy_swap_req[2], native_legacy_swap_req[3], native_legacy_swap_req[9],
                 native_legacy_swap_req[10], native_legacy_swap_req[5], native_legacy_swap_req[6],
                 native_legacy_swap_req[7], native_legacy_swap_req[8], native_legacy_swap_req[11] >> 16,
                 native_legacy_swap_req[11] & 0xffff);
        if (m > 0) qcom_usb_debug_write(q, (size_t)m < sizeof q ? (size_t)m : sizeof q - 1);
      }
      // The GPU's client side, on the console. [gpu clients] and [gpu client text] live in the UFS
      // slot, which is exactly what is unreachable on the boots that stop before the desktop: the
      // question there is whether any process opened the accelerator, what the last mount refusal
      // was, and what the first process that reached the bridge said. Answering it needs a reader.
      {
        unsigned named = 0;
        const char *who = "-", *txt = "-";
        if (&native_adreno_client_text)
          for (unsigned i = 0; i < 8; ++i)
            if (native_adreno_client_text[i].name[0]) {
              if (!named) {
                who = native_adreno_client_text[i].name;
                txt = native_adreno_client_text[i].text[0] ? native_adreno_client_text[i].text : "-";
              }
              ++named;
            }
        unsigned askers = 0;
        // ...and which processes they are. A count alone cannot say whether the difference
        // between a boot that composites and one that does not is "fewer processes reached Metal"
        // or "the same processes reached it in a different order", and the names are the row the
        // UFS slot holds but a boot that stops early never writes.
        char names[176];
        size_t nn = 0;
        names[0] = 0;
        if (&native_adreno_metal_askers)
          for (unsigned i = 0; i < 12; ++i)
            if (native_adreno_metal_askers[i].name[0]) {
              ++askers;
              if (nn + 22 < sizeof names)
                nn += (size_t)snprintf(names + nn, sizeof names - nn, "%s%.19s",
                                       nn ? "," : "", native_adreno_metal_askers[i].name);
            }
        // The plugin's own progress markers (0x68xx: entered, Turnip found, device made, no Turnip, no
        // device). Nonzero means the bundle loaded and createDevice ran; zero with askers > 0 means the
        // system found the plugin and never got an entry point out of it.
        // The whole marker ring, not the last two slots: the ring's tail is empty until it wraps,
        // so "last two" printed 0/0 on every boot that stopped early — exactly the case worth
        // reading. Ring order is slot order; marks_total says which slot is newest.
        char ring[96];
        size_t rn = 0;
        ring[0] = 0;
        if (&native_adreno_metal_marks)
          for (unsigned i = 1; i < 17 && rn + 8 < sizeof ring; ++i)
            rn += (size_t)snprintf(ring + rn, sizeof ring - rn, " %x", native_adreno_metal_marks[i]);
        // The other processes' lines. One text row per process was printed and the rest were unreachable:
        // a boot has more than one Metal client and the one that matters (the compositor) is not always the
        // first to arrive, so the row carries up to three.
        char rest[260];
        size_t xn = 0;
        rest[0] = 0;
        if (&native_adreno_client_text)
          for (unsigned i = 0; i < 8 && xn + 78 < sizeof rest; ++i)
            if (native_adreno_client_text[i].name[0] && strcmp(native_adreno_client_text[i].name, who) != 0)
              xn += (size_t)snprintf(rest + xn, sizeof rest - xn, " %s:%.52s |", native_adreno_client_text[i].name,
                                     native_adreno_client_text[i].text[0] ? native_adreno_client_text[i].text : "-");
        char g[960];
        int gn = snprintf(g, sizeof g, "GPUCLI clients=%u mountfail=%u/%x/%u d=%u%u%u/%u/%u/%u ps=%u/%u,%u/%u rc=%u/%x/%u/%u askers=%u[%s] disp=%u/%d/%x marks=%u%s txt=%s:%.60s%s\n",
                          named,
                          &native_adreno_mount_fail ? native_adreno_mount_fail[0] : 0u,
                          &native_adreno_mount_fail ? native_adreno_mount_fail[1] : 0u,
                          &native_adreno_mount_fail ? native_adreno_mount_fail[3] : 0u,
                          &native_adreno_mount_fail ? native_adreno_mount_fail[4] : 0u,
                          &native_adreno_mount_fail ? native_adreno_mount_fail[5] : 0u,
                          &native_adreno_mount_fail ? native_adreno_mount_fail[6] : 0u,
                          &native_adreno_mount_fail ? native_adreno_mount_fail[7] : 0u,
                          &native_adreno_mount_fail ? native_adreno_mount_fail[8] : 0u,
                          &native_adreno_mount_fail ? native_adreno_mount_fail[9] : 0u,
                          &native_adreno_submit_poison ? native_adreno_submit_poison[0] : 0u,
                          &native_adreno_submit_poison ? native_adreno_submit_poison[1] : 0u,
                          &native_adreno_submit_poison ? native_adreno_submit_poison[2] : 0u,
                          &native_adreno_submit_poison ? native_adreno_submit_poison[3] : 0u,
                          &native_adreno_recover_trace ? native_adreno_recover_trace[0] : 0u,
                          &native_adreno_recover_trace ? native_adreno_recover_trace[1] : 0u,
                          &native_adreno_recovery ? native_adreno_recovery[0] : 0u,
                          &native_adreno_recovery ? native_adreno_recovery[2] : 0u,
                          askers, names,
                          native_legacy_client_calls,
                          native_legacy_client_pid,
                          (unsigned)native_legacy_client_status,
                          &native_adreno_metal_marks ? native_adreno_metal_marks[0] : 0u,
                          ring,
                          who, txt, rest);
        if (gn > 0 && &qcom_usb_debug_write)
          qcom_usb_debug_write(g, (size_t)gn < sizeof g ? (size_t)gn : sizeof g - 1);
      }
      // The first GPU hang of the boot, once: what the CP was executing and where its pointers were. A steady state of one hang
      // per submission says less than the hang that started it.
      static int hang1_printed = 0;
      if (!hang1_printed && &native_adreno_hang_first && native_adreno_hang_first[0] && &qcom_usb_debug_write) {
        char h1[240];
        const int hn = snprintf(h1, sizeof h1,
            "HANG1 n=%u fence=%u ring=%u pid=%d wc=%u xvm=%u rp=%x wp=%x cps=%x rbbm=%x ib1=%x rem=%x act=%u w0=%08x d=%u st=%x\n",
            native_adreno_hang_first[0], native_adreno_hang_first[1], native_adreno_hang_first[2],
            (int)native_adreno_hang_first[3], native_adreno_hang_first[4], native_adreno_hang_first[5],
            native_adreno_hang_first[6], native_adreno_hang_first[7], native_adreno_hang_first[8],
            native_adreno_hang_first[9], native_adreno_hang_first[10], native_adreno_hang_first[11],
            native_adreno_hang_first[12], native_adreno_hang_first[13], native_adreno_hang_first[14],
            native_adreno_hang_first[15]);
        if (hn > 0) qcom_usb_debug_write(h1, (size_t)hn < sizeof h1 ? (size_t)hn : sizeof h1 - 1);
        hang1_printed = 1;
      }
      // The words the command processor stopped on, once: three rows of eight, read at the read pointer it never left.
      static int stall_printed = 0;
      if (!stall_printed && &native_adreno_ring_stall && native_adreno_ring_stall[24] && &qcom_usb_debug_write) {
        for (unsigned b = 0; b < 3; ++b) {
          char rs[200];
          const int rn = snprintf(rs, sizeof rs, "RINGSTALL at=%x wp=%x %u %08x %08x %08x %08x %08x %08x %08x %08x\n",
              native_adreno_ring_stall[24], native_adreno_ring_stall[25], b,
              native_adreno_ring_stall[b * 8 + 0], native_adreno_ring_stall[b * 8 + 1],
              native_adreno_ring_stall[b * 8 + 2], native_adreno_ring_stall[b * 8 + 3],
              native_adreno_ring_stall[b * 8 + 4], native_adreno_ring_stall[b * 8 + 5],
              native_adreno_ring_stall[b * 8 + 6], native_adreno_ring_stall[b * 8 + 7]);
          if (rn > 0) qcom_usb_debug_write(rs, (size_t)rn < sizeof rs ? (size_t)rn : sizeof rs - 1);
        }
        stall_printed = 1;
      }
      // One row for the GPU's own health, every tenth second: what hung, whether the restart took, what was replayed,
      // how many address spaces are resident, and whether an SMMU fault was left latched before a restart.
      if (elapsed % 10 == 0 && &qcom_usb_debug_write) {
        char h[220];
        const int hn = snprintf(h, sizeof h,
                                "GPUHANG h=%u r=%u f=%u fence=%u st=%x rp=%u g=%u kv=%u vm=%u/%u/%u tst=%x/%x flt=%u/%x rb=%x/%x sc=%u/%u\n",
                                &native_adreno_recovery ? native_adreno_recovery[0] : 0u,
                                &native_adreno_recovery ? native_adreno_recovery[1] : 0u,
                                &native_adreno_recovery ? native_adreno_recovery[2] : 0u,
                                &native_adreno_recovery ? native_adreno_recovery[3] : 0u,
                                &native_adreno_recovery ? native_adreno_recovery[4] : 0u,
                                &native_adreno_recovery ? native_adreno_recovery[5] : 0u,
                                &native_adreno_recovery ? native_adreno_recovery[6] : 0u,
                                &native_adreno_recovery ? native_adreno_recovery[7] : 0u,
                                &native_adreno_table_switches ? native_adreno_table_switches[4] : 0u,
                                &native_adreno_table_switches ? native_adreno_table_switches[5] : 0u,
                                &native_adreno_table_switches ? native_adreno_table_switches[2] : 0u,
                                &native_adreno_table_switches ? native_adreno_table_switches[7] : 0u,
                                &native_adreno_table_switches ? native_adreno_table_switches[6] : 0u,
                                &native_adreno_recovery_fault ? native_adreno_recovery_fault[0] : 0u,
                                &native_adreno_recovery_fault ? native_adreno_recovery_fault[1] : 0u,
                                &native_adreno_ring_pos ? native_adreno_ring_pos[0] : 0u,
                                &native_adreno_ring_pos ? native_adreno_ring_pos[1] : 0u,
                                &native_adreno_ring_pos ? native_adreno_ring_pos[2] : 0u,
                                &native_adreno_ring_pos ? native_adreno_ring_pos[3] : 0u);
        if (hn > 0) qcom_usb_debug_write(h, (size_t)hn < sizeof h ? (size_t)hn : sizeof h - 1);
      }
      // The probe's LIVE counters (row 25) and its HANG record (row 26), on the console. They carry the readings no
      // other row has: the read pointer the CP actually wrote into the memptrs shadow (not the CP_RB_RPTR register,
      // which this A650 does not keep authoritative), the SQE microcode's program counter, the three ROQ occupancies,
      // the ring's base and control words and the AHB/AON registers. Until now these rows went only to the persistent
      // UFS log, which on this board stops after about four seconds, so every GPU stall since 2026-11-03 was invisible.
      if (elapsed % 10 == 0 && &native_gpu_rows && &qcom_usb_debug_write) {
        char gr[300];
        for (unsigned r = 25; r <= 26; ++r) {
          if (!native_gpu_rows[r][0]) continue;
          const int gn = snprintf(gr, sizeof gr, "GPUROW%d %s\n", r, native_gpu_rows[r]);
          if (gn > 0) qcom_usb_debug_write(gr, (size_t)gn < sizeof gr ? (size_t)gn : sizeof gr - 1);
        }
      }
      // The Metal bridge's own telemetry table, on the console. It lives in the UFS slot ([metal table]),
      // and the slot is what a boot that stops early never writes — but this table is filled by the render
      // server's scanout loop, which is running on exactly those boots. Print the rows that have text,
      // every tenth second, so the pipeline, pass, draw and dispatch counts and the refusal texts are
      // readable without a reader.
      if (&native_adreno_metal_rows && elapsed % 10 == 0 && &qcom_usb_debug_write) {
        unsigned printed = 0;
        for (unsigned r = 0; r < 48 && printed < 6; ++r)
          if (native_adreno_metal_rows[r][0]) {
            char s[112];
            const int sn = snprintf(s, sizeof s, "MT%d %.94s\n", r, (const char *)native_adreno_metal_rows[r]);
            if (sn > 0) qcom_usb_debug_write(s, (size_t)sn < sizeof s ? (size_t)sn : sizeof s - 1);
            ++printed;
          }
      }
      // Owned qualification reserves rows 18..47; emit raw bytes as well as its verdict.
      if (&native_adreno_metal_rows && elapsed % 10 == 0 && &qcom_usb_debug_write) {
        for (unsigned r = 18; r < 48; ++r) if (native_adreno_metal_rows[r][0] &&
            (r >= 46 || !strncmp((const char *)native_adreno_metal_rows[r], "PP ", 3) ||
             !strncmp((const char *)native_adreno_metal_rows[r], "BC ", 3))) {
          char line[112];
          const int n = snprintf(line, sizeof line, "PIX%u %.94s\n", r, native_adreno_metal_rows[r]);
          if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
        }
      }
      if (&native_gpu_rows && elapsed % 10 == 0 && &qcom_usb_debug_write) {
        unsigned pixelCheck = 0;
        PE_parse_boot_argn("alioth-pixel-check", &pixelCheck, sizeof pixelCheck);
        if (pixelCheck) for (unsigned r = 29; r <= 31; ++r) if (native_gpu_rows[r][0]) {
          char line[288];
          const int n = snprintf(line, sizeof line, "PREG%u %.255s\n", r, native_gpu_rows[r]);
          if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
        }
      }
      if (&native_adreno_boundary_log && native_adreno_boundary_log && &qcom_usb_debug_write) {
        static uint32_t boundaryPrinted;
        const uint32_t total=__atomic_load_n(&native_adreno_boundary_count,__ATOMIC_ACQUIRE);
        if (elapsed % 10 == 0) {
          char line[80];const int n=snprintf(line,sizeof line,"BNS rows=%u lost=%u\n",total,native_adreno_boundary_lost);
          if(n>0)qcom_usb_debug_write(line,(size_t)n<sizeof line?(size_t)n:sizeof line-1);
        }
        for (;boundaryPrinted<total && boundaryPrinted<128;++boundaryPrinted) {
          const volatile uint64_t *r=native_adreno_boundary_log+boundaryPrinted*64;
          char line[256];
          int n=snprintf(line,sizeof line,"BND%u s=%llu a=%llu pid=%llu f=%llu h=%llx io=%llx bytes=%llu pa=%llx v=%llx ps=%llx\n",
            boundaryPrinted,r[1],r[0],r[2],r[3],r[4],r[5],r[6],r[7],r[8],r[9]);
          if(n>0)qcom_usb_debug_write(line,(size_t)n<sizeof line?(size_t)n:sizeof line-1);
          n=snprintf(line,sizeof line,"BNT%u vm=%llx table_at_note=%llx cpu=%llx cw=%llu dm=%llx lost=%u\n",
            boundaryPrinted,r[12],r[13],r[14],r[11],r[10],native_adreno_boundary_lost);
          if(n>0)qcom_usb_debug_write(line,(size_t)n<sizeof line?(size_t)n:sizeof line-1);
          for(unsigned first=0;first<16;first+=8)for(unsigned view=0;view<2;++view) {
            n=snprintf(line,sizeof line,"BN%c%u w%u",view?'P':'C',boundaryPrinted,first);
            for(unsigned k=0;k<8;++k)n+=snprintf(line+n,sizeof line-(size_t)n," %08x",(uint32_t)r[(view?32:16)+first+k]);
            if(n>0 && (size_t)n+1<sizeof line){line[n++]='\n';qcom_usb_debug_write(line,(size_t)n);}
          }
        }
      }
      // Where the four traced processes sit, on the console: [stacks] lives in the slot, and the slot
      // is what freezes on the boots that stop before the desktop. Two rows per role is the main
      // thread's own frames.
      // A round with no STK row at all reads two ways: the tracer never started, or it started and
      // found nothing. Say which, instead of leaving the absence silent.
      if (elapsed % 30 == 0 && &qcom_usb_debug_write && !&boot_ui_trace_role_rows) {
        const char *nohook = "STK nohook\n";
        qcom_usb_debug_write(nohook, 11);
      }
      if (elapsed % 30 == 0 && &boot_ui_trace_role_rows && &qcom_usb_debug_write) {
        static char srows[64][120];
        unsigned total = 0;
        for (unsigned role = 0; role < 4; ++role) {
          // Detail 0 is the only mode that prints the process's own name; detail 2 prints the
          // registers, the peer and one row per thread group. Two calls, three rows: without the
          // name the pids read like the xpcproxy that last held them.
          unsigned printed = 0;
          const unsigned named = boot_ui_trace_role_rows(role, srows, 64, 0);
          if (named) {
            char s[140];
            const int sn = snprintf(s, sizeof s, "STK %.120s\n", srows[0]);
            if (sn > 0) qcom_usb_debug_write(s, (size_t)sn < sizeof s ? (size_t)sn : sizeof s - 1);
            printed = 1;
          }
          const unsigned n = boot_ui_trace_role_rows(role, srows, 64, 2);
          for (unsigned i = 0; i < n && printed < 4; ++i) {
            // "STKx NN ..." is a frame row: 8 addresses, no name and no peer.
            if (srows[i][4] == ' ' && srows[i][5] >= '0' && srows[i][5] <= '9') continue;
            char s[140];
            const int sn = snprintf(s, sizeof s, "STK %.120s\n", srows[i]);
            if (sn > 0) qcom_usb_debug_write(s, (size_t)sn < sizeof s ? (size_t)sn : sizeof s - 1);
            ++printed;
          }
          total += printed;
        }
        if (!total) {
          const char *none = "STK none\n";
          qcom_usb_debug_write(none, 9);
        }
      }
    }
    if (!tick_autoreset || elapsed < tick_autoreset + ResetSyncGrace) continue;
    boot_log_fire_reset(elapsed);
  }
}

/* Core health from a thread of its own. Every other row on this console comes from the supervisor,
 * so a boot whose supervisor is the victim of a wedged core prints nothing at all -- and that is
 * exactly the shape of the 2026-11-06 rounds w11114382 and g21019511, where SUPRUN stops at one
 * second and the rest of the capture is userspace exits. This thread does nothing but sleep and
 * read ArmCpu's counters, so it can outlive the supervisor and still name the core.
 *
 * The mask names cores whose architectural-timer count has not moved for two consecutive samples.
 * A merely idle core also stops arming its timer, so the mask alone is not proof: the series is.
 * A core that counted thousands of timer interrupts in the first seconds and then stops for the
 * rest of the boot is wedged; one that was never busy was only idle. Print the row, not a verdict. */
static void core_health_thread(void *arg, wait_result_t result)
{
  (void)arg; (void)result;
  if (!&armcpu_core_timer) return;
  thread_set_thread_name(current_thread(), "corehealth");
  uint64_t f = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); if (!f) f = 19200000;
  const uint64_t start = log_ticks();
  static uint32_t last[8];
  static uint8_t unmoved[8];
  unsigned runs = 0;
  for (;;) {
    IOSleep(10000);
    ++runs;
    if (!&qcom_usb_debug_write) continue;
    const unsigned elapsed = (unsigned)((log_ticks() - start) / f);
    unsigned cores = (&armcpu_smp_running ? (unsigned)__atomic_load_n(&armcpu_smp_running, __ATOMIC_RELAXED) : 0u) + 1u;
    if (cores > 8) cores = 8;
    char row[384];
    int n = snprintf(row, sizeof row, "CORES t=%us runs=%u |", elapsed, runs);
    for (unsigned c = 0; c < cores && n > 0 && (unsigned)n < sizeof row - 32; ++c)
      n += snprintf(row + n, sizeof row - (unsigned)n, " c%u=%u/%u",
                    c, __atomic_load_n(&armcpu_core_timer[c], __ATOMIC_RELAXED),
                    __atomic_load_n(&armcpu_core_sgi[c], __ATOMIC_RELAXED));
    unsigned deaf = 0, badStep = 0, failed = 0;
    for (unsigned c = 0; c < cores; ++c) {
      const uint32_t now = __atomic_load_n(&armcpu_core_timer[c], __ATOMIC_RELAXED);
      if (now == last[c]) {if (unmoved[c] < 255) ++unmoved[c];}
      else {unmoved[c] = 0; last[c] = now;}
      if (unmoved[c] >= 2) deaf |= 1u << c;
      /* Declaring a core takes one more still sample than showing it: a core that merely lost one
       * timer tick to a long mask-off section recovers on the next sample, and boot would stop
       * poking it for good. Three still samples in a row is a core that stopped taking interrupts. */
      if (unmoved[c] >= 3 && &armcpu_core_note_deaf) armcpu_core_note_deaf(c);
    }
    if (&armcpu_smp_step)
      for (unsigned c = 0; c < 8; ++c) {
        const uint32_t st = __atomic_load_n(&armcpu_smp_step[c], __ATOMIC_RELAXED);
        if (st && st != 7) badStep |= 1u << c;
      }
    if (&armcpu_smp_fail)
      for (unsigned c = 0; c < 8; ++c)
        if (__atomic_load_n(&armcpu_smp_fail[c], __ATOMIC_RELAXED)) failed |= 1u << c;
    if (n > 0 && (unsigned)n < sizeof row - 96)
      n += snprintf(row + n, sizeof row - (unsigned)n, " run=%u deaf=%02x st=%02x fl=%02x wd=%u/%02x skip=%llu",
                    &armcpu_smp_running ? (unsigned)__atomic_load_n(&armcpu_smp_running, __ATOMIC_RELAXED) : 0u,
                    deaf, badStep, failed,
                    &armcpu_core_deaf_events ? (unsigned)__atomic_load_n(&armcpu_core_deaf_events, __ATOMIC_RELAXED) : 0u,
                    &armcpu_core_deaf_mask ? (unsigned)__atomic_load_n(&armcpu_core_deaf_mask, __ATOMIC_RELAXED) : 0u,
                    &armcpu_ipi_skipped_deaf ? (unsigned long long)__atomic_load_n(&armcpu_ipi_skipped_deaf, __ATOMIC_RELAXED) : 0ull);
    if (n > 0) {row[n++] = '\n'; qcom_usb_debug_write(row, (size_t)n < sizeof row ? (size_t)n : sizeof row - 1);}
    /* One line for the suspect cores only, so the CORES row stays readable: the frame's own affinity, then
     * pending, enabled, active and WAKER. A core that is pending but never takes the interrupt is masked or
     * halted; one whose enables are clear had its GIC turned off; one with nothing pending is not targeted. */
    if (deaf != 0 && &armcpu_core_gicr_probe) {
      char gr[512];
      /* The two step counters belong to the threads that most often go deaf, so they sit in the
       * header (they are global, not per core) where this row can still print them from a healthy
       * core. ms is the page monitor's step when it last moved, ts the log tick's: a monitor that
       * ran seven times and then stopped leaves ms at the call it never returned from, which is
       * the only reading that separates "it is spinning inside X" from "it is parked". */
      int m = snprintf(gr, sizeof gr, "GICR t=%us deaf=%02x ms=%u ts=%u mon=%llu", elapsed, deaf,
                       &boot_progress_monitor_step ? (unsigned)__atomic_load_n(&boot_progress_monitor_step, __ATOMIC_RELAXED) : 0u,
                       (unsigned)boot_log_tick_step,
                       &boot_progress_monitor_ticks ? (unsigned long long)__atomic_load_n(&boot_progress_monitor_ticks, __ATOMIC_RELAXED) : 0ULL);
      for (unsigned c = 0; c < cores && m > 0 && (unsigned)m < sizeof gr - 96; ++c) {
        if (!((deaf >> c) & 1u)) continue;
        uint32_t p[5] = {0, 0, 0, 0, 0};
        const int ok = armcpu_core_gicr_probe(c, p);
        /* How long it has been inside the handler it last entered, in seconds. A phase that never
         * changes and an age that keeps climbing is a core stuck in that one handler. */
        unsigned ph = 0, age = 0;
        if (&armcpu_core_phase && &armcpu_core_irq_enter) {
          uint64_t now; __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(now));
          const uint32_t entered = __atomic_load_n(&armcpu_core_irq_enter[c], __ATOMIC_RELAXED);
          ph = __atomic_load_n(&armcpu_core_phase[c], __ATOMIC_RELAXED);
          const uint64_t hz = f / 1024u;
          if (hz && entered && (uint32_t)(now >> 10) > entered) age = (unsigned)(((uint32_t)(now >> 10) - entered) / hz);
        }
        /* What that core was running when it stopped taking interrupts. The idle thread answers "it
         * went to sleep and never came back"; a real thread answers "it is stuck in kernel code", and
         * its id names the process. Read without a lock: this is a diagnostic snapshot, not a use. */
        unsigned long long ac = 0, acstate = 0, cont = 0;
        char nm[24] = "-";
        unsigned pri = 0;
        cpu_data_t *cd = cpu_datap((int)c);
        thread_t at = cd ? cd->cpu_active_thread : NULL;
        if (at) {
          ac = (unsigned long long)at->thread_id; acstate = (unsigned long long)at->state;
          /* XNU names its own kernel threads, and thread_get_thread_name reads that name without
           * taking a lock: it is a memcpy off a field set at creation. "kernel-high" is the
           * thread-call group that IOTimerEventSource callbacks run on, so this one field separates
           * a wedged timer callback from every other kernel thread. */
          if (&thread_get_thread_name) thread_get_thread_name(at, nm);
          /* The kernel routine a continuation thread is parked on, and the process it belongs to.
           * A thread whose state is TH_RUN with a continuation is inside kernel code, not asleep;
           * the address resolves against the kernel image's own symbols, the name needs no resolver.
           * proc_name is declared here because bsd/sys/proc.h drags in a second set of the exported
           * bsd headers and -Werror rejects the macro redefinitions. bsd_info is deliberately not
           * dereferenced: struct proc is not a kernel-internal layout this file may name. */
          cont = (unsigned long long)(uintptr_t)at->continuation;
          pri = (unsigned)at->sched_pri;
          /* arm64 keeps the task behind thread_ro (there is no thread->t_task), and the read-only
           * zone is exactly what makes this dereference safe while the thread is running. */
          const int pid = at->t_tro ? task_pid(at->t_tro->tro_task) : 0;
          if (pid > 0 && &proc_name) {
            char pn[24] = "-";
            proc_name(pid, pn, (int)sizeof pn);
            /* The kernel thread's name wins only if it exists; otherwise the process owns the row. */
            if (nm[0] == '-' || nm[0] == 0) snprintf(nm, sizeof nm, "%s", pn);
          }
        }
        m += snprintf(gr + m, sizeof gr - (unsigned)m, " c%u=%s%x/%x/%x/%x/%x ph%u age%us ac=%llx/%llx nm=%s k=%llx pr=%u",
                      c, ok == 0 ? "" : "?",
                      (unsigned)p[0], (unsigned)p[1], (unsigned)p[2], (unsigned)p[3], (unsigned)p[4],
                      ph, age, ac, acstate, nm, cont, pri);
      }
      if (m > 0 && (unsigned)m < sizeof gr) {gr[m++] = '\n'; qcom_usb_debug_write(gr, (size_t)m < sizeof gr ? (size_t)m : sizeof gr - 1);}
    }
  }
}

// Called by the board page's monitor once a second; starts the writer the first time when sheng-log=1.
void boot_log_tick(unsigned now) {
  uint64_t f = 0; __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f)); if (!f) f = 19200000;
  const uint64_t cur_ticks = log_ticks();
  boot_log_tick_freq = f;
  boot_log_tick_ctr = cur_ticks;
  boot_log_tick_calls = boot_log_tick_calls + 1;
  boot_log_tick_step = 1;
  if (!boot_log_started) {
    boot_log_started = 1;
    tick_start_cntpct = cur_ticks;
    tick_seen_at = 0;
    // An unattended run must come back however it ends: on the desktop past its deadline (int-229 first suspected) or in a panic seconds
    // after the start, before UFS and this log exist (int-229: a duplicate class at PE_lockdown_iokit). So the first tick, independent of
    // sheng-log, marks the cell (any restart is fastboot) and arms the hypervisor's watchdog, which bites half a minute after the pings stop:
    // after the autoreset deadline, or when a panic parks the cores.
    /* The core-health thread is independent of every switch: it is the only reading left on a boot
     * whose supervisor stalls, and it costs one sleeping thread that reads seven counters. */
    if (&armcpu_core_timer) {
      thread_t cth;
      if (kernel_thread_start_priority(core_health_thread, 0, BASEPRI_KERNEL, &cth) == KERN_SUCCESS)
        thread_deallocate(cth);
    }
    uint32_t autoreset = 0;
    // The probe is called for its text, not for a verdict: what the cell held at boot is worth
    // recording, but it must not decide whether the cell may be rewritten. It used to be the gate
    // and refused every run: it returns 0 only when the cell already holds a reason br_known()
    // accepts, this handset leaves 0x4ab2b0e2 in it, so the mark, the PON write and the reset were
    // skipped on every boot (boot-usb101, 273 rounds, known=0). Whether the cell held an old reason
    // says nothing about whether it can be overwritten.
    if (PE_parse_boot_argn("sheng-autoreset", &autoreset, sizeof autoreset) && autoreset) {
      tick_autoreset = autoreset;
      if (&boot_reset_probe) (void)boot_reset_probe(boot_log_reset_probe_text, sizeof boot_log_reset_probe_text);
      if (&boot_reset_mark_bootloader) boot_reset_mark_bootloader(boot_log_reset_mark, sizeof boot_log_reset_mark);
      if (&boot_reset_watchdog)
        boot_reset_watchdog(autoreset + 60, boot_log_reset_watchdog, sizeof boot_log_reset_watchdog);
      thread_t rth;
      const kern_return_t sr = kernel_thread_start_priority(autoreset_supervisor, 0, BASEPRI_KERNEL, &rth);
      boot_log_super_start = (int)sr;
      if (sr == KERN_SUCCESS)
        thread_deallocate(rth);
      // The arm's readback and the supervisor's start result on one line: without superstart a boot
      // with no SUPER row cannot be told apart from a boot whose pet's register write hung, and
      // both now cost the whole round to a 30 s bite.
      if (&qcom_usb_debug_write && boot_log_reset_watchdog[0]) {
        char w[300];
        const int wn = snprintf(w, sizeof w, "WDT %s superstart=%d\n", boot_log_reset_watchdog, (int)sr);
        if (wn > 0) qcom_usb_debug_write(w, (size_t)wn < sizeof w ? (size_t)wn : sizeof w - 1);
      }
    }
    uint32_t mode = 0;
    if (PE_parse_boot_argn("sheng-log", &mode, sizeof mode) && mode == 1 &&
        &native_ufs_log_open && &native_ufs_log_write && &native_ufs_log_sync) {
      boot_log_buffer = (uint8_t *)IOMallocAligned(LogMsgBlocks * LogBlock, LogBlock);
      if (boot_log_buffer) {
        export_lock = IOLockAlloc();
        thread_t thread;
        if (kernel_thread_start_priority(writer, 0, BASEPRI_KERNEL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
      }
    }
  }
  boot_log_tick_step = 2;
  const unsigned elapsed = cur_ticks >= tick_start_cntpct ? (unsigned)((cur_ticks - tick_start_cntpct) / f) : 0;
  boot_log_tick_elapsed = elapsed;
  const unsigned cur_now = now ? now : elapsed;
  /* The display controller as the boot loader left it (DpuProbe.cpp). It runs from this tick rather than from the display's start
   * because the MDP block is unclocked at display start and reads return zero there; by the time the log has run for a few seconds
   * the panel is powered and the registers answer. */
  boot_log_tick_step = 3;
  if (&alioth_dpu_probe_tick) alioth_dpu_probe_tick(cur_now);
  boot_log_tick_step = 4;
  /* The GPU frequency sample used to be called here, once a second. It is not any more: it reads
   * GMU registers, and this tick is not a context that can promise their clock is on. On 2026-11-06
   * that read stopped the core running this loop about seven seconds into every boot (GICR ms=4
   * ts=4 mon=7), and every thread asleep on that core was lost with it. The sample now requires its
   * caller to declare the GPU powered (native_gpu_freq_powered), which no diagnostic path may do. */
  boot_log_tick_step = 5;
  /* Phase heartbeat: once a second is too much, once on every change plus every tenth second
   * otherwise. A writer that stops leaves its last phase on the console and nothing else. */
  static unsigned last_phase = ~0U, last_at;
  {
    const unsigned phase = __atomic_load_n(&log_phase, __ATOMIC_RELAXED);
    if (phase != last_phase || elapsed - last_at >= 10) {
      last_phase = phase; last_at = elapsed;
      char row[128];
      // Round and slots as well as the phase: a writer that stops leaves its phase behind, and a
      // writer that keeps running but cannot write leaves the round count still while the phase
      // cycles. Without both numbers those two read the same on the console.
      const int n = snprintf(row, sizeof row, "BLPHASE t=%us phase=%u round=%u slots=%u err=%u state=%u\n", elapsed, phase,
                             boot_log_rounds, __atomic_load_n(&boot_log_slots_hint, __ATOMIC_RELAXED), boot_log_errors, boot_log_state);
      if (n > 0 && &qcom_usb_debug_write) qcom_usb_debug_write(row, (size_t)n < sizeof row ? (size_t)n : sizeof row - 1);
    }
  }
  boot_log_tick_step = 6;
  if (!tick_autoreset || tick_reset_tried) return;
  // A round that landed recently means the medium is keeping up; one that did not means the writer
  // stopped, and it will not start again (boot_log_started is never cleared, and the loop has
  // returned on every exit path). Either way the board still has to come back.
  const unsigned seq = __atomic_load_n(&log_sync_seq, __ATOMIC_RELAXED);
  if (seq != tick_seen_seq) {tick_seen_seq = seq; tick_seen_at = elapsed;}
  if (elapsed < tick_autoreset) return;
  // log-capture section 6: the timeout covers the measured checkpoint and the log sync. Restart at
  // the deadline when a round has landed within the grace, and at the deadline plus the grace when
  // none has, so an in-flight round gets its moment and a stopped writer costs only the grace. The
  // header still names the last complete round either way.
  if (elapsed - tick_seen_at > ResetSyncGrace && elapsed < tick_autoreset + ResetSyncGrace) return;
  boot_log_fire_reset(elapsed);
}
