// SPDX-License-Identifier: BSD-2-Clause-Patent
// No-op definitions of the probe hooks the patched XNU references while alioth_defconfig leaves their component out.
// Each hook runs only on its boot argument, which board.toml does not pass; the kernel link needs the symbol anyway.

// offline-component-load-probes.patch, bsd_init: called on offline-legacy-register=1.
// CONFIG_XNU_NATIVE_LEGACY_DISPLAY is now on, so libNativeLegacyDisplay.late.a defines this
// entry and the kernel calls the real one. The stub is gone: keeping it was a duplicate symbol
// at the link. Set the displayed switch back to n and this definition has to come back with it.
#include <stdint.h>

#if !XNU_NATIVE_LEGACY_DISPLAY
void native_legacy_display_probe(void);
void native_legacy_display_probe(void)
{
}
#endif

// ShengUfs imports this as a weak reference, which a kernel link still has to resolve.
// alioth leaves CONFIG_XNU_BOOT_PROGRESS unset, so libBootProgress.a is not linked.
// Weak here, so enabling that option later keeps the real definition.
__attribute__((weak)) void boot_progress_poll(void);
__attribute__((weak)) void boot_progress_poll(void)
{
}

// offline-owner-systemos-mount.patch, kern_exec: native_owner_data_prepare() calls
// native_gpu_addon_prepare(), which tests native_ram_block_gpu_addon before calling it. The patch
// declares that symbol weak_import, and ld64 still refuses to leave a weak_import undefined in a
// static kernel link, so CONFIG_XNU_OWNER_DATA_MOUNT does not link until this name resolves.
// libRamBlock.a owns the real definition, but alioth leaves CONFIG_XNU_NATIVE_RAM_BLOCK unset: the
// Data mount reads the DarwinBoot RAMDisk through mdevlookup(0), not a virtio provider.
// The hook runs only on sheng-gpu-addon=1, which board.toml does not pass. Weak here, so
// selecting the RamBlock transport later keeps the real definition.
__attribute__((weak)) int native_ram_block_gpu_addon(const unsigned char **, unsigned long long *);
__attribute__((weak)) int native_ram_block_gpu_addon(const unsigned char **addon, unsigned long long *bytes)
{
	(void)addon;
	(void)bytes;
	return 0;
}

// GopFramebuffer's compositor-scanout path imports the UFS DPU window as weak_import, and a
// static kernel link refuses to leave a weak_import undefined. NativeUfs defines both; this board
// selects ShengUfs, so neither exists here. NativeLegacyDisplay.cpp:136 tests `&native_ufs_dpu_map`
// and `!(native_legacy_dpu_state >> 31)` before using either, so a zero state and a reporter that
// maps nothing make the path decline itself, which is the correct answer on a board whose DPU is
// not published. Weak, so a component that later defines the real symbols wins.
__attribute__((weak)) volatile uint32_t native_ufs_dpu_state;
__attribute__((weak)) uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages,
    unsigned count);
__attribute__((weak)) uint32_t native_ufs_dpu_map(unsigned slot, const uint64_t *pages,
    unsigned count)
{
	(void)slot;
	(void)pages;
	(void)count;
	return 0;
}
/* The compositor's surface reports 1088 as its stride for a 1080-wide window whose allocation is
 * four bytes a pixel a row, so the IOSurface field the display reads holds pixels here while
 * native_legacy_surface_bounds compares it with width*4. Every swap was refused for that reason
 * (GPUREJ: stride 1088, stride*4 4352, need 4320). Take the field as pixels when the allocation
 * and the row count prove it, and leave a getter that returns bytes alone. */
extern unsigned long long native_legacy_surface_stride_raw(void *);
extern unsigned long long native_legacy_surface_allocation(void *);
extern unsigned long long native_legacy_surface_height(void *);
unsigned long long native_legacy_surface_stride(void *surface) {
    unsigned long long stride = native_legacy_surface_stride_raw(surface);
    unsigned long long allocation = native_legacy_surface_allocation(surface);
    unsigned long long height = native_legacy_surface_height(surface);
    if (stride && allocation && height && stride <= allocation / height / 4) return stride * 4;
    return stride;
}
