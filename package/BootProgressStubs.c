
// BootProgress.c and BootLog.c reference sheng-only diagnostics as weak_import. A static kernel
// link refuses to leave a weak_import undefined, so CONFIG_XNU_BOOT_PROGRESS cannot be enabled
// until every one of them resolves. Each is a plain zero-initialised global: the progress code
// only reads them, so a zero reads as "that component did nothing", which is the truth on this
// board. Generated from the extern declarations in package/BootProgress.c; if that file gains a
// diagnostic, this list has to gain the same name or the link fails loudly.
#include <stdint.h>

__attribute__((weak)) uint64_t armcpu_ipi_interrupts;
__attribute__((weak)) uint64_t armcpu_timer_interrupts;
__attribute__((weak)) unsigned boot_process_attempts;
__attribute__((weak)) unsigned boot_process_busy;
__attribute__((weak)) unsigned boot_process_seen;
__attribute__((weak)) unsigned boot_ui_trace_index;
__attribute__((weak)) unsigned boot_ui_trace_phase;
__attribute__((weak)) unsigned boot_ui_trace_ready;
__attribute__((weak)) uint32_t native_adreno_audit_attr;
__attribute__((weak)) uint64_t native_adreno_audit_ticks;
__attribute__((weak)) uint64_t native_adreno_reassert_ticks;
__attribute__((weak)) uint32_t native_adreno_wc_distrusted;
__attribute__((weak)) uint32_t native_cpufreq_stage;
__attribute__((weak)) uint64_t native_data_persist_errors;
__attribute__((weak)) uint64_t native_data_persist_first_lba;
__attribute__((weak)) uint32_t native_data_persist_gpt;
__attribute__((weak)) uint32_t native_data_persist_gpt_after;
__attribute__((weak)) uint32_t native_data_persist_header;
__attribute__((weak)) uint64_t native_data_persist_loaded;
__attribute__((weak)) uint64_t native_data_persist_pages;
__attribute__((weak)) uint64_t native_data_persist_present;
__attribute__((weak)) uint32_t native_data_persist_state;
__attribute__((weak)) uint64_t native_data_persist_syncs;
__attribute__((weak)) uint64_t native_data_persist_writes;
__attribute__((weak)) uint64_t native_gop_gpu_abandoned;
__attribute__((weak)) uint64_t native_gop_gpu_completions;
__attribute__((weak)) uint64_t native_gop_gpu_failures;
__attribute__((weak)) uint64_t native_gop_gpu_offers;
__attribute__((weak)) uint32_t native_legacy_client_calls;
__attribute__((weak)) int native_legacy_client_pid;
__attribute__((weak)) int native_legacy_client_status;
__attribute__((weak)) uint32_t native_legacy_client_type;
__attribute__((weak)) uint64_t native_legacy_copy_bytes_last;
__attribute__((weak)) uint64_t native_legacy_copy_bytes_sum;
__attribute__((weak)) uint64_t native_legacy_copy_last;
__attribute__((weak)) uint64_t native_legacy_copy_max;
__attribute__((weak)) uint64_t native_legacy_copy_n;
__attribute__((weak)) uint64_t native_legacy_copy_sum;
__attribute__((weak)) uint64_t native_legacy_early_failures;
__attribute__((weak)) uint64_t native_legacy_early_retires;
__attribute__((weak)) uint32_t native_legacy_frame_count;
__attribute__((weak)) uint64_t native_legacy_gop_bytes;
__attribute__((weak)) uint64_t native_legacy_gop_physical;
__attribute__((weak)) uint64_t native_legacy_gop_virtual;
__attribute__((weak)) uint32_t native_legacy_gpu_fallbacks;
__attribute__((weak)) uint32_t native_legacy_gpu_last_status;
__attribute__((weak)) uint32_t native_legacy_guard_slot;
__attribute__((weak)) uint32_t native_legacy_pm_callbacks;
__attribute__((weak)) uint32_t native_legacy_pm_calls;
__attribute__((weak)) uint32_t native_legacy_pm_stage;
__attribute__((weak)) uint32_t native_legacy_prepare_attempts;
__attribute__((weak)) uint32_t native_legacy_prepare_calls;
__attribute__((weak)) uint32_t native_legacy_prepare_reason;
__attribute__((weak)) uint32_t native_legacy_rej_reason;
__attribute__((weak)) uint32_t native_legacy_retirement_calls;
__attribute__((weak)) uint32_t native_legacy_stage;
__attribute__((weak)) uint32_t native_legacy_swap_accepted_count;
__attribute__((weak)) uint32_t native_legacy_swap_rejected_count;
__attribute__((weak)) uint32_t native_legacy_swap_validation_calls;
__attribute__((weak)) uint32_t native_legacy_swap_validation_reason;
__attribute__((weak)) uint32_t native_legacy_swap_validation_result;
__attribute__((weak)) uint64_t native_legacy_transfer_bytes;
__attribute__((weak)) uint32_t native_legacy_transfer_calls;
__attribute__((weak)) int native_legacy_transfer_status;
__attribute__((weak)) uint64_t native_ram_block_resident_bytes;
__attribute__((weak)) uint32_t native_ram_block_sparse;
__attribute__((weak)) uint32_t native_touch_count;
__attribute__((weak)) uint32_t native_touch_loop_step;
__attribute__((weak)) unsigned native_touch_stage_req;
__attribute__((weak)) uint32_t native_ufs_matched;
__attribute__((weak)) uint32_t native_ufs_probe_calls;
__attribute__((weak)) uint64_t native_ufs_read_ticks;
__attribute__((weak)) uint32_t native_ufs_root_step;
__attribute__((weak)) uint64_t native_ufs_verify_ticks;
__attribute__((weak)) unsigned native_wi_count;
__attribute__((weak)) uint32_t boot_diag_errors;
__attribute__((weak)) uint32_t boot_diag_files;
__attribute__((weak)) uint32_t boot_diag_skipped;
__attribute__((weak)) unsigned boot_progress_banner_rows;
__attribute__((weak)) uint32_t cs_text_fixup_applied;
__attribute__((weak)) uint32_t cs_text_fixup_parsed;
__attribute__((weak)) uint64_t native_adreno_fence_phys;
__attribute__((weak)) uint32_t native_adreno_ib_log_count;
__attribute__((weak)) uint64_t native_adreno_import_fail_offset;
__attribute__((weak)) uint64_t native_adreno_import_fail_pa;
__attribute__((weak)) uint32_t native_adreno_import_stage;
__attribute__((weak)) uint32_t native_adreno_io_coherent;
__attribute__((weak)) uint32_t native_adreno_pool_status;
__attribute__((weak)) uint32_t native_adreno_pool_wc;
__attribute__((weak)) uint32_t native_adreno_ring_log_count;
__attribute__((weak)) int native_cryptex_os_stage;
__attribute__((weak)) int native_cryptex_os_status;
__attribute__((weak)) int native_gestalt_cache_status;
__attribute__((weak)) uint64_t native_gop_virtual;
__attribute__((weak)) int native_gpu_addon2_stage;
__attribute__((weak)) int native_gpu_addon2_status;
__attribute__((weak)) int native_gpu_addon3_stage;
__attribute__((weak)) int native_gpu_addon3_status;
__attribute__((weak)) int native_gpu_addon_error;
__attribute__((weak)) int native_gpu_addon_failed;
__attribute__((weak)) int native_gpu_addon_files;
__attribute__((weak)) int native_gpu_addon_same;
__attribute__((weak)) int native_gpu_addon_stage;
__attribute__((weak)) int native_gpu_addon_status;
__attribute__((weak)) int native_gpu_addon_written;
__attribute__((weak)) uint64_t native_gpu_idle_sleeps;
__attribute__((weak)) uint64_t native_gpu_idle_spins;
__attribute__((weak)) unsigned native_lookup_count;
__attribute__((weak)) int native_ls_store_status;
__attribute__((weak)) int native_staged_apps_dirs;
__attribute__((weak)) int native_staged_apps_error;
__attribute__((weak)) int native_staged_apps_failed;
__attribute__((weak)) int native_staged_apps_files;
__attribute__((weak)) int native_staged_apps_mib;
__attribute__((weak)) int native_staged_apps_stage;
__attribute__((weak)) uint32_t native_touch_stroke_count;
__attribute__((weak)) uint32_t native_wifi_note_n;
__attribute__((weak)) uint32_t native_wifi_text_n;

// Array and function types the regex above cannot express, found by the linker rather than by
// parsing: enabling CONFIG_XNU_BOOT_PROGRESS turned them into undefined symbols and the link
// named them. Same rule as the scalars, a zero-filled definition the progress code only reads.
__attribute__((weak)) char native_wifi_note[128][112];
__attribute__((weak)) volatile uint32_t native_wifi_pmic[472];
__attribute__((weak)) volatile uint32_t native_wifi_pon[12];
__attribute__((weak)) volatile uint32_t native_wifi_stats[29];
__attribute__((weak)) char native_wifi_text[512][112];
__attribute__((weak)) struct { uint32_t stage, scans, frames, beacons, bss_seen; int last_err; } nw_wifi_stats;
__attribute__((weak)) volatile uint64_t nw_wifi_where[4];
__attribute__((weak)) void sheng_cpufreq_poll(void);
__attribute__((weak)) void sheng_cpufreq_poll(void) { }

// Remaining diagnostics, gathered by scanning every package source for weak_import instead of
// by the linker one batch at a time. Scalars are uint64_t: the progress code only reads them
// and a zero is the truthful value on a board with none of these components.
__attribute__((weak)) uint64_t armcpu_boot_mpidr;
__attribute__((weak)) uint64_t armcpu_boot_sys[5];
__attribute__((weak)) uint64_t armcpu_gic_stride;
__attribute__((weak)) uint64_t armcpu_gic_typer_probe[2];
__attribute__((weak)) uint64_t armcpu_halt_calls;
__attribute__((weak)) uint64_t armcpu_irq_id;
__attribute__((weak)) uint64_t armcpu_irq_phase;
__attribute__((weak)) uint64_t armcpu_quiesce_calls;
__attribute__((weak)) uint64_t armcpu_smp_conduit;
__attribute__((weak)) uint64_t armcpu_smp_fail[8];
__attribute__((weak)) uint64_t armcpu_smp_psci_rc[8];
__attribute__((weak)) uint64_t armcpu_smp_requested;
__attribute__((weak)) uint64_t armcpu_smp_running;
__attribute__((weak)) uint64_t armcpu_smp_seen_mpidr[8];
__attribute__((weak)) uint64_t armcpu_smp_step[8];
__attribute__((weak)) uint64_t armcpu_smp_sys[8][5];
__attribute__((weak)) uint64_t boot_process_passes;
__attribute__((weak)) uint64_t boot_process_rows[10][120];
__attribute__((weak)) uint64_t boot_progress_banner_text[49][120];
__attribute__((weak)) uint64_t boot_progress_ext[8][48];
__attribute__((weak)) uint64_t boot_progress_ext_wide[2][112];
__attribute__((weak)) uint64_t boot_progress_page_text[48][120];
__attribute__((weak)) uint64_t boot_progress_panic_text[2048];
__attribute__((weak)) uint64_t boot_progress_throw_text[330];
__attribute__((weak)) uint64_t boot_ui_trace_label[120];
__attribute__((weak)) uint64_t boot_ui_trace_rows[14][120];
__attribute__((weak)) uint64_t debug_buf_base;
__attribute__((weak)) uint64_t kernel_debugger_entry_count;
__attribute__((weak)) uint64_t native_adreno_cache_stats[5];
__attribute__((weak)) uint64_t native_adreno_client_log[8];
__attribute__((weak)) uint64_t native_adreno_client_stats[5][6];
__attribute__((weak)) uint64_t native_adreno_client_text[4];
__attribute__((weak)) uint64_t native_adreno_coherence[8];
__attribute__((weak)) uint64_t native_adreno_coherent_skips[4];
__attribute__((weak)) uint64_t native_adreno_export_stats[4];
__attribute__((weak)) uint64_t native_adreno_fence_wait[8];
__attribute__((weak)) uint64_t native_adreno_ib_log[4][4 + 28];
__attribute__((weak)) uint64_t native_adreno_import_diag[10];
__attribute__((weak)) uint64_t native_adreno_import_live[3 + 8 * 4];
__attribute__((weak)) uint64_t native_adreno_import_persistent[2];
__attribute__((weak)) uint64_t native_adreno_map_fail[7][2];
__attribute__((weak)) uint64_t native_adreno_metal_askers[12];
__attribute__((weak)) uint64_t native_adreno_metal_rows[48][96];
__attribute__((weak)) uint64_t native_adreno_method_stats[20][4];
__attribute__((weak)) uint64_t native_adreno_mount_fail[4];
__attribute__((weak)) uint64_t native_adreno_pool_stats[8];
__attribute__((weak)) uint64_t native_adreno_ring_log[4][4 + 112];
__attribute__((weak)) uint64_t native_adreno_stall[24];
__attribute__((weak)) uint64_t native_adreno_stall_mem[59];
__attribute__((weak)) uint64_t native_adreno_submit_ibs[4];
__attribute__((weak)) uint64_t native_adreno_unstick[15];
__attribute__((weak)) uint64_t native_adreno_view_diag[6];
__attribute__((weak)) uint64_t native_adreno_vm_phase_us[2][8];
__attribute__((weak)) uint64_t native_adreno_wait_outer[4];
__attribute__((weak)) uint64_t native_adreno_wc_audit[6];
__attribute__((weak)) uint64_t native_adreno_window_overflow[4][3];
__attribute__((weak)) uint64_t native_adsp_stats[16];
__attribute__((weak)) uint64_t native_adsp_text[96];
__attribute__((weak)) uint64_t native_audio_stats[16];
__attribute__((weak)) uint64_t native_backlight_request[6];
__attribute__((weak)) uint64_t native_backlight_stats[16];
__attribute__((weak)) uint64_t native_battery_stats[16];
__attribute__((weak)) uint64_t native_buttons_stats[10];
__attribute__((weak)) uint64_t native_cpufreq_row[120];
__attribute__((weak)) uint64_t native_dpu_pipes[54];
__attribute__((weak)) uint64_t native_dpu_stats[20];
__attribute__((weak)) uint64_t native_gop_wait_stats[7];
__attribute__((weak)) uint64_t native_gpu_coherence_row[120];
__attribute__((weak)) uint64_t native_gpu_perf_vote[2];
__attribute__((weak)) uint64_t native_hid_input_events[8];
__attribute__((weak)) uint64_t native_hid_input_results[8];
__attribute__((weak)) uint64_t native_legacy_client_class[64];
__attribute__((weak)) uint64_t native_legacy_frame_color_counts[8][4];
__attribute__((weak)) uint64_t native_legacy_frame_color_other[8];
__attribute__((weak)) uint64_t native_legacy_frame_colors[8][4];
__attribute__((weak)) uint64_t native_legacy_frame_kind[32];
__attribute__((weak)) uint64_t native_legacy_frame_stats[32][5];
__attribute__((weak)) uint64_t native_legacy_frame_time[32];
__attribute__((weak)) uint64_t native_legacy_rej_class[32];
__attribute__((weak)) uint64_t native_legacy_rej_options[3];
__attribute__((weak)) uint64_t native_legacy_rej_rects[2][4];
__attribute__((weak)) uint64_t native_legacy_rej_size[2];
__attribute__((weak)) uint64_t native_legacy_rej_state[3];
__attribute__((weak)) uint64_t native_legacy_rej_surface[7];
__attribute__((weak)) uint64_t native_legacy_rej_surfaces[4];
__attribute__((weak)) uint64_t native_lookup_log[160][112];
__attribute__((weak)) uint64_t native_ram_block_bytes;
__attribute__((weak)) uint64_t native_staged_apps_path[96];
__attribute__((weak)) uint64_t native_swipe_stats[16];
__attribute__((weak)) uint64_t native_touch_edge_stats[4];
__attribute__((weak)) uint64_t native_touch_extent[4];
__attribute__((weak)) uint64_t native_touch_hid_multi[3];
__attribute__((weak)) uint64_t native_touch_hold[14];
__attribute__((weak)) uint64_t native_touch_ids[12];
__attribute__((weak)) uint64_t native_touch_jitter[3];
__attribute__((weak)) uint64_t native_touch_rows[4][120];
__attribute__((weak)) uint64_t native_touch_seq;
__attribute__((weak)) uint64_t native_touch_strokes[16][7];
__attribute__((weak)) uint64_t native_touch_xs[12];
__attribute__((weak)) uint64_t native_touch_ys[12];
__attribute__((weak)) uint64_t native_ufs_root_failure;
__attribute__((weak)) uint64_t native_wi_log[4][100];
__attribute__((weak)) uint64_t native_wifi_cfg[32];
__attribute__((weak)) uint64_t native_wifi_chip[16];
__attribute__((weak)) uint64_t native_wifi_dump[108];
__attribute__((weak)) uint64_t native_wifi_ep[16];
__attribute__((weak)) uint64_t native_wifi_ev[48];
__attribute__((weak)) uint64_t native_wifi_ldo[24];
__attribute__((weak)) uint64_t native_wifi_mhi[16];
__attribute__((weak)) uint64_t native_wifi_mhi_ms[8];

// Functions, not variables. The progress code calls these to collect diagnostics; on a board with
// none of those components the truthful answer is "nothing to report", so the reporters write
// nothing and return zero. They were found by the linker, one batch at a time, because a scan of
// the declarations cannot tell a call target from a variable.
__attribute__((weak)) void boot_process_capture(void) { }
__attribute__((weak)) void boot_process_snapshot(void) { }
__attribute__((weak)) unsigned boot_ui_trace_hot_rows(char rows[][120], unsigned capacity) { (void)rows; (void)capacity; return 0; }
__attribute__((weak)) unsigned boot_ui_trace_role_rows(unsigned role, char rows[][120], unsigned capacity, unsigned limit) { (void)role; (void)rows; (void)capacity; (void)limit; return 0; }
__attribute__((weak)) void boot_ui_trace_snapshot(unsigned phase) { (void)phase; }
__attribute__((weak)) unsigned native_legacy_clients_dump(char *out, unsigned capacity) { (void)out; (void)capacity; return 0; }
__attribute__((weak)) unsigned native_legacy_prepare_report(char *out, unsigned capacity) { (void)out; (void)capacity; return 0; }
__attribute__((weak)) unsigned native_legacy_properties_now(char *out, unsigned capacity) { (void)out; (void)capacity; return 0; }
__attribute__((weak)) unsigned native_legacy_state_words(char *out, unsigned capacity) { (void)out; (void)capacity; return 0; }
__attribute__((weak)) unsigned native_legacy_swap_timeline(char *out, unsigned capacity) { (void)out; (void)capacity; return 0; }
__attribute__((weak)) int native_ufs_extra_fault(uint32_t *out) { (void)out; return 0; }
// These two are function pointers, not functions: the progress log calls through them only when
// a GPU probe installed one.
__attribute__((weak)) int (*native_adreno_cached_read32)(uint64_t, uint32_t *) = 0;
__attribute__((weak)) int (*native_adreno_dram_read32)(uint64_t, uint32_t *) = 0;
/* The USB status row in BootProgress.c calls this through weak_import, and a static kernel
 * link refuses to leave such a symbol undefined. The real definition lives in
 * libQcomXnuUsbDebug.a, which is force-loaded when CONFIG_XNU_QCOM_USB_DEBUG is on, and a
 * strong definition wins over this one. The sentinel matters: a zero-filled struct would print
 * phase=0 error=0, which is exactly what "the module never initialized" also prints. Writing
 * 0xff says "this stub answered, not the module", so the row can never be misread as a real
 * measurement. The struct's first two fields are phase and error, which is all this writes. */
__attribute__((weak)) void qcom_usb_debug_get_status(void *status)
{
  /* The real function copies 304 bytes; fill the same span so the caller's buffer is fully
   * defined either way. */
  volatile unsigned char *bytes = (volatile unsigned char *)status;
  unsigned i;
  if (!bytes) return;
  for (i = 0; i < 304u; ++i) bytes[i] = 0xffu;
}
