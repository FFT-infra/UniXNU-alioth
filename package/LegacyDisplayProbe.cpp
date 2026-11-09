// SPDX-License-Identifier: BSD-2-Clause-Patent
// The board half of the GopFramebuffer port: the observation hooks that
// NativeLegacyDisplay.cpp declares but does not define, and the two entries the patched kernel
// calls. Ported from UniXNU-sheng's package/LegacyDisplayProbe.cpp with every sheng-only
// peripheral removed (touch, Wi-Fi, audio DSP, backlight, battery, buttons, CPU frequency, GPU
// power, HID, key store, the UFS log writer and the reset probe), so the archive holds the
// display path alone. What survives is the part the display lifecycle itself needs:
//   - native_legacy_display_probe(): registers the two classes and, after the init phase, keeps
//     the instance until bsd_autoconf has published the IOBSD resource.
//   - native_legacy_display_late_probe(): binds the instance to the real firmware framebuffer
//     once IOKitBSDInit() returned.
//   - the *_observed hooks: counters only. Nothing here changes a verdict or an ABI.
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSerialize.h>
#include <libkern/c++/OSData.h>
// What the screenshot collector answered when the frame buffer was offered. -1 not tried.
static int native_screenshot_register_status = -1;
#include <kern/task.h>
#include <sys/proc.h>
// allproc and struct proc's list entry, for the process-table walk in the state dump.
#include <sys/proc_internal.h>
#include <pexpert/pexpert.h>
#include <console/video_console.h>
#include <mach/kmod.h>
#include <machine/machine_routines.h>

// The console's own record of the frame buffer. initialize_screen() fills v_baseaddr with the
// kernel mapping it made; PE_state.video keeps the firmware's physical address, so a store
// through that one would fault.
extern "C" struct vc_info vinfo;

extern "C" kmod_info_t g_kernel_kmod_info;
// The static initializers of the two owned classes leave __mod_init_func for this reason: they
// must run between preModLoad and postModLoad, not from the kernel's own init section.
extern "C" void native_legacy_display_static_init(void) __asm__("__GLOBAL__sub_I_NativeLegacyDisplay.cpp");
extern "C" IOService *native_legacy_display_allocate(void);
extern "C" void native_gop_presenter_static_init(void) __asm__("__GLOBAL__sub_I_NativeGOPPresenter.cpp");
extern "C" IOService *native_gop_presenter_create(void);
// The display controller's state as the boot loader left it (DpuProbe.cpp). Read before the display driver takes the controller
// over: the direct scanout path inherits the SSPP base, source format and stride from those registers.
static IOService *pendingLegacyDisplay;
static IOService *activeGOPPresenter, *activeGOPNub;

extern "C" {
volatile uint32_t native_legacy_stage;
volatile int native_legacy_meta_status = -1;
volatile uint32_t native_legacy_init_result, native_legacy_start_result, native_legacy_probe_match;
volatile uint32_t native_legacy_parent_match, native_legacy_object_size, native_legacy_released;
volatile uint64_t native_legacy_object, native_legacy_nub, native_legacy_workloop;
volatile uint32_t native_legacy_guard_slot;
volatile uint64_t native_legacy_guard_object, native_legacy_guard_caller;
volatile int native_legacy_surface_status = -1;
volatile uint32_t native_legacy_surface_found;
volatile uint64_t native_legacy_video_base, native_legacy_video_bytes;
volatile uint64_t native_legacy_gop_map, native_legacy_gop_virtual, native_legacy_gop_physical, native_legacy_gop_bytes;
volatile uint32_t native_legacy_gop_width, native_legacy_gop_height;
volatile uint32_t native_legacy_registration_calls, native_legacy_properties_length;
volatile uint32_t native_legacy_dt_attached;
volatile int native_legacy_unit_scale_status = -1;
volatile uint32_t native_legacy_unit_scale_calls, native_legacy_chosen_scale, native_legacy_chosen_rotation;
volatile uint32_t native_legacy_pm_stage, native_legacy_pm_calls, native_legacy_pm_callbacks, native_legacy_pm_ordinal;
volatile int native_legacy_pm_status = -1;
volatile uint32_t native_legacy_swap_validation_calls, native_legacy_swap_validation_result;
volatile uint32_t native_legacy_swap_validation_reason;
volatile uint64_t native_legacy_swap_request, native_legacy_swap_surface;
volatile uint32_t native_legacy_transfer_calls;
volatile int native_legacy_transfer_status = -1;
volatile uint64_t native_legacy_transfer_bytes, native_legacy_transfer_request;
volatile uint32_t native_legacy_retirement_calls, native_legacy_retired_generation;
volatile uint32_t native_legacy_presented_generation, native_legacy_retired_frames;
volatile uint64_t native_legacy_retired_current, native_legacy_retired_pending;
char native_legacy_properties[4096];
volatile uint32_t native_legacy_client_calls, native_legacy_client_type;
volatile int native_legacy_client_pid, native_legacy_client_status = -1;
volatile uint64_t native_legacy_client_task, native_legacy_client_object;
char native_legacy_client_class[64];
volatile uint32_t native_legacy_swap_accepted_count, native_legacy_swap_rejected_count, native_legacy_rej_reason;
volatile uint64_t native_legacy_rej_surfaces[4];
volatile uint32_t native_legacy_rej_state[3], native_legacy_rej_options[3];
volatile uint32_t native_legacy_rej_size[2], native_legacy_rej_rects[2][4];
char native_legacy_rej_class[32];
volatile uint64_t native_legacy_rej_surface[7];
volatile uint32_t native_legacy_frame_count;
volatile uint32_t native_legacy_frame_kind[32];
volatile uint32_t native_legacy_frame_time[32];
volatile uint64_t native_legacy_frame_stats[32][5];
volatile uint32_t native_legacy_frame_colors[8][4];
volatile uint64_t native_legacy_frame_color_counts[8][4], native_legacy_frame_color_other[8];
volatile uint64_t native_legacy_copy_n, native_legacy_copy_last, native_legacy_copy_max;
volatile uint64_t native_legacy_copy_sum, native_legacy_copy_bytes_last, native_legacy_copy_bytes_sum;
volatile uint32_t native_legacy_prepare_calls, native_legacy_prepare_reason, native_legacy_prepare_attempts;
volatile uint32_t native_legacy_prepare_failures, native_legacy_prepare_retried, native_legacy_prepare_max_attempts;
volatile uint64_t native_legacy_prepare_wait_us;

// Everything the owned class calls is declared extern "C" in NativeLegacyDisplay.cpp, so every
// definition below keeps C linkage. This block stays open to the end of the file on purpose: a
// definition left outside it gets a C++ mangled name, the owned object's import stays undefined,
// and only the final link would report it.

static uint64_t probeNowUs(void) {
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

// The boot progress layer owns the console-versus-scanout arbitration, and libBootProgress.a is
// not linked here. A weak_import is not an option: a static kernel link refuses to leave one
// undefined, which is the same rule that keeps CONFIG_XNU_BOOT_PROGRESS off. A weak definition
// resolves the reference now and still yields to the real one if that archive is ever linked.
__attribute__((weak)) int boot_progress_prepare_scanout(void);
__attribute__((weak)) int boot_progress_prepare_scanout(void) {
	return 1;
}
__attribute__((weak)) void boot_progress_cancel_scanout(void);
__attribute__((weak)) void boot_progress_cancel_scanout(void) {
}

// The touch kit is C++, so its entry point is the mangled __Z15touchProbeStartv. An extern "C"
// declaration asks for _touchProbeStart instead and can never resolve; that mismatch is why an
// earlier attempt failed at link time. This file sits inside an extern "C" block to the end, so
// C++ linkage has to be asked for explicitly here, or the declaration inherits C linkage again.
//
// A strong reference is deliberate. A static archive member is pulled in only when something
// references it, and a weak definition here would win over the archive rather than yield to it,
// leaving the driver out of the kernel. The kit is always built by build-alioth-touch.py, which
// board.toml lists as a kernel archive, so the reference always resolves.
extern "C++" {
bool touchProbeStart(void);
}

// The USB console, when the board links it. Weak, so a board without it still links and simply
// loses the line; the same contract BootProgress.c uses for the same symbol.
extern "C" size_t qcom_usb_debug_write(const void *, size_t) __attribute__((weak_import));

// Called from the owned display before it writes a frame into the firmware framebuffer.
bool native_legacy_prepare_scanout(void) {
	native_legacy_prepare_calls = native_legacy_prepare_calls + 1;
	native_legacy_prepare_reason = 0;
	native_legacy_prepare_attempts = 0;
	if (!boot_progress_prepare_scanout) {
		return true;
	}
	if (ml_at_interrupt_context()) {
		native_legacy_prepare_reason = 1;
		return false;
	}
	if (!ml_get_interrupts_enabled()) {
		native_legacy_prepare_reason = 2;
		return false;
	}
	const uint64_t began = probeNowUs();
	for (unsigned attempt = 0; attempt < 100; ++attempt) {
		native_legacy_prepare_attempts = attempt + 1;
		if (boot_progress_prepare_scanout()) {
			if (attempt) {
				native_legacy_prepare_retried = native_legacy_prepare_retried + 1;
				native_legacy_prepare_wait_us = native_legacy_prepare_wait_us + (probeNowUs() - began);
				if (attempt + 1 > native_legacy_prepare_max_attempts) {
					native_legacy_prepare_max_attempts = attempt + 1;
				}
			}
			return true;
		}
		IOSleep(1);
	}
	native_legacy_prepare_failures = native_legacy_prepare_failures + 1;
	native_legacy_prepare_wait_us = native_legacy_prepare_wait_us + (probeNowUs() - began);
	native_legacy_prepare_reason = 3;
	if (boot_progress_cancel_scanout) {
		boot_progress_cancel_scanout();
	}
	return false;
}

// The three checkpoints the component calls are barriers for an external observer: they mark a
// point the harness may stop at (a class state, a mapping, a user client). Offline they are empty.
__attribute__((noinline)) void native_legacy_display_checkpoint(void) { __asm__ volatile("" ::: "memory"); }
// Defined with the other band painters below; the swap and transfer hooks above it need the name.
void native_legacy_paint_userspace_band(unsigned stage);
__attribute__((noinline)) void native_legacy_gop_mapping_checkpoint(void) { __asm__ volatile("" ::: "memory"); }
__attribute__((noinline)) void native_legacy_client_checkpoint(void) { __asm__ volatile("" ::: "memory"); }

void native_legacy_pm_observed(unsigned stage, IOReturn status, unsigned long ordinal) {
	native_legacy_pm_stage = stage;
	native_legacy_pm_status = status;
	native_legacy_pm_ordinal = (uint32_t)ordinal;
	native_legacy_pm_calls = native_legacy_pm_calls + 1;
	if (stage == 3) {
		native_legacy_pm_callbacks = native_legacy_pm_callbacks + 1;
	}
}

__attribute__((noinline)) void native_legacy_swap_validation_checkpoint(bool, unsigned, const void *, const void *) {
	__asm__ volatile("" ::: "memory");
}

// Last rejected swap: copied from the request layout the validator in the component reads.
// Observation only; nothing here changes the verdict.
static void recordRejectedSwap(unsigned reason, const void *request, const void *surface) {
	const unsigned char *bytes = reinterpret_cast<const unsigned char *>(request);
	if (!bytes) {
		return;
	}
	native_legacy_rej_reason = reason;
	uint64_t surfaces[4] = {};
	memcpy(surfaces, bytes + 2624, sizeof(surfaces));
	for (unsigned i = 0; i < 4; ++i) {
		native_legacy_rej_surfaces[i] = surfaces[i];
	}
	const unsigned stateOffsets[3] = {792, 796, 800}, optionOffsets[3] = {804, 808, 812};
	for (unsigned i = 0; i < 3; ++i) {
		uint32_t a = 0, b = 0;
		memcpy(&a, bytes + stateOffsets[i], 4);
		memcpy(&b, bytes + optionOffsets[i], 4);
		native_legacy_rej_state[i] = a;
		native_legacy_rej_options[i] = b;
	}
	uint32_t w = 0, h = 0;
	memcpy(&w, bytes + 816, 4);
	memcpy(&h, bytes + 820, 4);
	native_legacy_rej_size[0] = w;
	native_legacy_rej_size[1] = h;
	const unsigned rectOffsets[2] = {17, 81};
	for (unsigned r = 0; r < 2; ++r) {
		uint32_t rect[4] = {};
		memcpy(rect, bytes + rectOffsets[r], sizeof(rect));
		for (unsigned i = 0; i < 4; ++i) {
			native_legacy_rej_rects[r][i] = rect[i];
		}
	}
	char name[32] = {};
	if (surface) {
		const OSObject *object = reinterpret_cast<const OSObject *>(surface);
		const OSMetaClass *meta = object->getMetaClass();
		const char *cls = meta ? meta->getClassName() : nullptr;
		if (cls) {
			strlcpy(name, cls, sizeof(name));
		}
	}
	memcpy(native_legacy_rej_class, name, sizeof(name));
}

// File scope: a linkage specification cannot sit inside a function body in C++.
extern "C" size_t qcom_usb_debug_write(const void *, size_t) __attribute__((weak_import));
void native_legacy_surface_rejected(uint64_t width, uint64_t height, uint32_t format, uint32_t planes,
    uint64_t stride, uint64_t allocation, uint64_t bytes) {
	native_legacy_rej_surface[0] = width;
	native_legacy_rej_surface[1] = height;
	native_legacy_rej_surface[2] = format;
	native_legacy_rej_surface[3] = planes;
	native_legacy_rej_surface[4] = stride;
	native_legacy_rej_surface[5] = allocation;
	native_legacy_rej_surface[6] = bytes;
	// Say it once, on the console, at the moment it happens: the persistent log that also carries
	// these was a one-round snapshot on every boot this ran, and the progress thread dies with it.
	{
		static unsigned said;
		if (!said && &qcom_usb_debug_write) {
			char line[200];
			int n = snprintf(line, sizeof line,
			    "GPUREJ surface %llux%llu fmt %x planes %llu stride %llu alloc %llu bytes %llu\n",
			    (unsigned long long)width, (unsigned long long)height, format, (unsigned long long)planes,
			    (unsigned long long)stride, (unsigned long long)allocation, (unsigned long long)bytes);
			if (n > 0) qcom_usb_debug_write(line, (size_t)n);
			said = 1;
		}
	}
}

// Content statistics of presented frames (copy = kind 0, blank fill = kind 1): the first 32 are
// kept, with the time in seconds since the first frame.
static uint64_t frameEpoch;
void native_legacy_frame_observed(unsigned kind, uint64_t count, uint64_t minx, uint64_t miny,
    uint64_t maxx, uint64_t maxy) {
	uint32_t n = native_legacy_frame_count;
	uint64_t now, frequency;
	__asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now));
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
	if (!frameEpoch) {
		frameEpoch = now;
	}
	if (n < 32) {
		native_legacy_frame_kind[n] = kind;
		native_legacy_frame_time[n] = frequency ? (uint32_t)((now - frameEpoch) / frequency) : 0;
		native_legacy_frame_stats[n][0] = count;
		native_legacy_frame_stats[n][1] = minx;
		native_legacy_frame_stats[n][2] = miny;
		native_legacy_frame_stats[n][3] = maxx;
		native_legacy_frame_stats[n][4] = maxy;
	}
	native_legacy_frame_count = n + 1;
}

// Called right after the matching native_legacy_frame_observed(); fills slot count-1.
void native_legacy_frame_colors_observed(const uint32_t *colors, const uint64_t *counts, uint64_t other) {
	uint32_t n = native_legacy_frame_count;
	if (!n || n > 8) {
		return;
	}
	n -= 1;
	for (unsigned i = 0; i < 4; ++i) {
		native_legacy_frame_colors[n][i] = colors[i];
		native_legacy_frame_color_counts[n][i] = counts[i];
	}
	native_legacy_frame_color_other[n] = other;
}

// The life of every accepted swap: submitted, offered to the GPU copy (or started on the CPU),
// retired. Times are microseconds since boot; the last 64 are kept.
struct SwapEvent {
	uint64_t submitted, offered, done;
	uint32_t number;
};
static SwapEvent swapEvents[64];
static uint32_t swapEventCount;
extern "C" void native_legacy_swap_stage(unsigned stage) {
	const uint64_t now = probeNowUs();
	uint32_t total = swapEventCount;
	if (stage == 0) {
		SwapEvent &e = swapEvents[total % 64];
		e = {now, 0, 0, total + 1};
		swapEventCount = total + 1;
		return;
	}
	uint32_t first = total > 64 ? total - 64 : 0;
	for (uint32_t i = first; i < total; ++i) {
		SwapEvent &e = swapEvents[i % 64];
		if (stage == 1 && !e.offered && !e.done) {
			e.offered = now;
			return;
		}
		if (stage == 2 && !e.done) {
			e.done = now;
			if (!e.offered) {
				e.offered = now;
			}
			return;
		}
	}
}

volatile uint32_t native_legacy_swap_reason_counts[8];
// The last request's layer masks, background word and surface count. A rejection is a verdict
// about these three, so they belong beside the reason code, not only in the request memory.
volatile uint32_t native_legacy_swap_req[12];
extern "C" void native_legacy_swap_request_state(uint32_t updated, uint32_t disabled,
                                                 uint32_t background, uint32_t surfaces) {
	native_legacy_swap_req[0] = updated;
	native_legacy_swap_req[1] = disabled;
	native_legacy_swap_req[2] = background;
	native_legacy_swap_req[3] = surfaces;
	native_legacy_swap_req[4] = native_legacy_swap_req[4] + 1;
}
// The geometry the verdict compared: the request's own dimensions and first rectangle, and the
// display it was compared with. A reason 3 says these disagree, and only the numbers say which.
extern "C" void native_legacy_swap_request_geometry(uint32_t rw, uint32_t rh, const uint32_t *rect,
                                                    uint32_t dw, uint32_t dh) {
	for (unsigned i = 0; i < 4; ++i) native_legacy_swap_req[5 + i] = rect ? rect[i] : 0;
	native_legacy_swap_req[9] = rw;
	native_legacy_swap_req[10] = rh;
	native_legacy_swap_req[11] = (dw << 16) | (dh & 0xffff);
}
// Frame pacing against touch. The compositor draws when something asks it to, so the number that
// matters is not the frame rate but the two gaps: to the previous swap, and to the last touch
// frame the touch driver published. One console row per swap, rate-limited to 50 a second, with
// the number of rows the limit skipped, so a burst is visible as a count instead of a hole.
extern volatile uint32_t native_touch_seq __attribute__((weak_import));
static void framePacingObserved(bool accepted, unsigned reason) {
	static uint64_t lastFrameUs, lastTouchUs;
	static uint32_t lastTouchSeq, rows, skipped;
	const uint64_t now = probeNowUs();
	const uint32_t seq = &native_touch_seq ? native_touch_seq : 0;
	if (seq != lastTouchSeq) {
		lastTouchSeq = seq;
		lastTouchUs = now;
	}
	const uint64_t gap = lastFrameUs ? now - lastFrameUs : 0;
	if (lastFrameUs && gap < 20000) {   // 20 ms: the limit is a burst guard, not a sample rate
		// lastFrameUs stays at the last printed row, so the next row's gap covers the burst.
		skipped = skipped + 1;
		return;
	}
	lastFrameUs = now;
	if (!&qcom_usb_debug_write) return;
	char line[144];
	int n = snprintf(line, sizeof line, "FRAME %u gap_us=%llu touch_us=%llu seq=%u a=%u r=%u skip=%u\n",
	                 ++rows, (unsigned long long)gap,
	                 (unsigned long long)(lastTouchUs ? now - lastTouchUs : 0), seq,
	                 accepted ? 1u : 0u, reason, skipped);
	if (n > 0) qcom_usb_debug_write(line, (size_t)n);
	skipped = 0;
}

// What the client's own surface holds, read before any copy runs. The IOSurface accessors are the
// driver's extern "C" ones; a surface that does not answer them is skipped rather than guessed at.
extern IOMemoryDescriptor *native_legacy_surface_descriptor(OSObject *) __attribute__((weak_import));
extern uint64_t native_legacy_surface_width(OSObject *) __attribute__((weak_import));
extern uint64_t native_legacy_surface_height(OSObject *) __attribute__((weak_import));
extern uint64_t native_legacy_surface_stride(OSObject *) __attribute__((weak_import));
// The kernel's data-cache invalidate, the same symbol NativeLegacyDisplay.cpp declares for its copy path.
extern "C" void flush_dcache64(uint64_t addr, unsigned count, int phys);

// One reading of a surface's memory: every 4096th dword, bounded to 16384 samples. A 1080x2400 frame is
// 6.5 MiB, so this walks the whole picture without costing a frame. The caller owns the descriptor.
// The most saturated sample it found on the way, which SURFACEL prints.
static unsigned bestPixelR, bestPixelG, bestPixelB, bestPixelSpread;
// A coarse picture of the same frame the row numbers come from: 12 rows by 24 columns of luma
// levels, so the console shows the screen's shape (a status bar, a row of icons, a wall band)
// instead of one average. Written by surfaceMeasure, printed by surfaceGridPrint.
static unsigned char surfaceGridChars[12][24];

static bool surfaceMeasure(IOMemoryDescriptor *memory, uint64_t stride, uint64_t height, unsigned *rows,
                           unsigned *litOut, unsigned *lumaOut, unsigned *hashOut) {
	IOMemoryMap *map = memory->map(kIOMapAnywhere | kIOMapReadOnly | kIOMapCopybackCache);
	if (!map || !map->getVirtualAddress()) { OSSafeReleaseNULL(map); return false; }
	const uint64_t length = map->getLength();
	if (!stride || !height || stride > length) { OSSafeReleaseNULL(map); return false; }
	// The surfaces are write-combined (iPadOS 27 display surfaces are), so the CPU's view is only valid after the
	// cache lines are invalidated: without this every read returns the previous frame's, which is zero. The CPU
	// copy path in NativeLegacyDisplay.cpp flushes the same way before it reads a surface (line 829 there).
	flush_dcache64((uint64_t)(uintptr_t)map->getVirtualAddress(), (unsigned)length, false);
	const volatile uint32_t *base = (const volatile uint32_t *)(uintptr_t)map->getVirtualAddress();
	const uint64_t dwords = length / 4;
	uint64_t lit = 0, luma = 0, samples = 0;
	uint32_t hash = 2166136261u;
	// The most saturated sample, so one row answers "is the picture coloured at all, and which
	// channel leads". A neutral grey frame leaves spread 0; red and blue exchanged swaps R and B here.
	unsigned bestSpread = 0, bestR = 0, bestG = 0, bestB = 0;
	for (uint64_t i = 0; i < dwords && samples < 16384; i += 4096) {
		const uint32_t pixel = base[i];
		const uint32_t b = pixel & 0xff, g = (pixel >> 8) & 0xff, r = (pixel >> 16) & 0xff;
		if (r | g | b) lit = lit + 1;
		luma += (r * 299 + g * 587 + b * 114) / 1000;
		const unsigned maxc = r > g ? (r > b ? r : b) : (g > b ? g : b);
		const unsigned minc = r < g ? (r < b ? r : b) : (g < b ? g : b);
		if (maxc - minc > bestSpread) { bestSpread = maxc - minc; bestR = r; bestG = g; bestB = b; }
		hash ^= pixel; hash *= 16777619u;
		samples = samples + 1;
	}
	bestPixelR = bestR; bestPixelG = bestG; bestPixelB = bestB; bestPixelSpread = bestSpread;
	// The coarse picture: one sample per cell, taken from the same flushed mapping.
	{
		const uint64_t pixelsPerRow = stride / 4;
		for (unsigned gy = 0; gy < 12; ++gy) {
			const uint64_t y = (height * (2 * gy + 1)) / 24;
			for (unsigned gx = 0; gx < 24; ++gx) {
				const uint64_t x = (pixelsPerRow * (2 * gx + 1)) / 48;
				const uint64_t idx = y * pixelsPerRow + x;
				const uint32_t pixel = idx < dwords ? base[idx] : 0;
				const unsigned b = pixel & 0xff, g = (pixel >> 8) & 0xff, r = (pixel >> 16) & 0xff;
				const unsigned l = (r * 299 + g * 587 + b * 114) / 1000;
				surfaceGridChars[gy][gx] = (unsigned char)(l / 26 > 9 ? 9 : l / 26);
			}
		}
	}
	OSSafeReleaseNULL(map);
	if (!samples) return false;
	*rows = (*rows) + 1;
	*litOut = (unsigned)(lit * 1000 / samples);
	*lumaOut = (unsigned)(luma / samples);
	*hashOut = hash;
	return true;
}

static unsigned surfaceRows, surfaceLit, surfaceLuma, surfaceSum;
void surfaceSampleObserved(bool accepted, const void *surfaceObject) {
	// Once per second at most: the swap path can run ten times in half a second, and one sample of the
	// frame that got there is enough to say whether the client drew anything.
	static uint64_t lastAt;
	const uint64_t now = probeNowUs();
	if (lastAt && now - lastAt < 1000000) return;
	lastAt = now;
	OSObject *object = (OSObject *)surfaceObject;
	if (!object || !&native_legacy_surface_descriptor) return;
	IOMemoryDescriptor *memory = native_legacy_surface_descriptor(object);
	if (!memory) return;
	const uint64_t stride = &native_legacy_surface_stride ? native_legacy_surface_stride(object) : 0;
	const uint64_t height = &native_legacy_surface_height ? native_legacy_surface_height(object) : 0;
	if (!surfaceMeasure(memory, stride, height, &surfaceRows, &surfaceLit, &surfaceLuma, &surfaceSum)) return;
	if (!&qcom_usb_debug_write) return;
	char line[160];
	const int n = snprintf(line, sizeof line,
	                      "SURFACE n=%u a=%u nonblack=%u/1000 luma=%u sum=%08x stride=%llu h=%llu\n",
	                      surfaceRows, accepted ? 1u : 0u, surfaceLit, surfaceLuma, surfaceSum,
	                      (unsigned long long)stride, (unsigned long long)height);
	if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
	/* Once every thirty seconds, the same frame as characters: twelve rows of twenty-four luma
	 * levels. A row of '#' at the top and a bright band at the bottom reads as a status bar over a
	 * wallpaper; twelve blank rows read as a black screen, which no average can tell apart from a
	 * dark one. */
	{
		static uint64_t lastGridAt;
		if (!lastGridAt || now - lastGridAt >= 30000000) {
			static const char ramp[] = " .:-=+*#%@";
			lastGridAt = now;
			for (unsigned gy = 0; gy < 12; ++gy) {
				char grid[48];
				int gn = snprintf(grid, sizeof grid, "SURFACEG %02u |", gy);
				for (unsigned gx = 0; gx < 24 && gn > 0 && gn < (int)sizeof grid - 3; ++gx)
					grid[gn++] = ramp[surfaceGridChars[gy][gx] > 9 ? 9 : surfaceGridChars[gy][gx]];
				grid[gn++] = '|';
				grid[gn++] = '\n';
				qcom_usb_debug_write(grid, (size_t)gn);
			}
		}
	}
}

// The same reading, taken after the GPU copy finished. SURFACE reads the frame before anything writes it and
// says the compositor's surface is empty; this row says whether the copy put anything into it. A lit row here
// with a dark panel puts the break in the display path; an empty one means the GPU never wrote the frame the
// compositor swapped, and the draws went somewhere else.
static IOMemoryDescriptor *gpuCopyMemory;
static uint64_t gpuCopyStride, gpuCopyHeight;
static unsigned afterRows, afterLit, afterLuma, afterSum;
void surfaceAfterCopyObserved(void) {
	IOMemoryDescriptor *memory = gpuCopyMemory;
	if (!memory) return;
	gpuCopyMemory = nullptr;
	const uint64_t stride = gpuCopyStride, height = gpuCopyHeight;
	gpuCopyStride = gpuCopyHeight = 0;
	if (surfaceMeasure(memory, stride, height, &afterRows, &afterLit, &afterLuma, &afterSum) && &qcom_usb_debug_write) {
		char line[160];
		const int n = snprintf(line, sizeof line,
		                      "SURFACEB n=%u nonblack=%u/1000 luma=%u sum=%08x stride=%llu h=%llu\n",
		                      afterRows, afterLit, afterLuma, afterSum,
		                      (unsigned long long)stride, (unsigned long long)height);
		if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
	}
	OSSafeReleaseNULL(memory);
}

// The same reading as SURFACE, taken after the CPU copy has finished. SURFACE runs at swap-validation time, which
// is when the compositor submits — before its command buffer has drawn (docs/zh/porting/display.md). If this row is
// lit while SURFACE is not, the copy simply reads too early and the fix is a wait; if both are dark, nothing ever
// writes the surface the display is handed.
static unsigned afterTransferRows, afterTransferLit, afterTransferLuma, afterTransferSum;
void surfaceAfterTransferObserved(void) {
	OSObject *object = (OSObject *)(uintptr_t)native_legacy_swap_surface;
	if (!object || !&native_legacy_surface_descriptor) return;
	IOMemoryDescriptor *memory = native_legacy_surface_descriptor(object);
	if (!memory) return;
	const uint64_t stride = &native_legacy_surface_stride ? native_legacy_surface_stride(object) : 0;
	const uint64_t height = &native_legacy_surface_height ? native_legacy_surface_height(object) : 0;
	if (!surfaceMeasure(memory, stride, height, &afterTransferRows, &afterTransferLit, &afterTransferLuma, &afterTransferSum)) return;
	if (!&qcom_usb_debug_write) return;
	char line[160];
	const int n = snprintf(line, sizeof line, "SURFACEC n=%u nonblack=%u/1000 luma=%u sum=%08x\n",
	                      afterTransferRows, afterTransferLit, afterTransferLuma, afterTransferSum);
	if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

// A reading of the last swapped surface, taken from the periodic tick rather than from the swap itself: it says
// whether that memory ever holds anything at all, independent of when the copy happens to run.
static unsigned lateRows, lateLit, lateLuma, lateSum;
extern "C" void surfaceLateObserved(void) {
	OSObject *object = (OSObject *)(uintptr_t)native_legacy_swap_surface;
	if (!object || !&native_legacy_surface_descriptor) return;
	IOMemoryDescriptor *memory = native_legacy_surface_descriptor(object);
	if (!memory) return;
	const uint64_t stride = &native_legacy_surface_stride ? native_legacy_surface_stride(object) : 0;
	const uint64_t height = &native_legacy_surface_height ? native_legacy_surface_height(object) : 0;
	if (!surfaceMeasure(memory, stride, height, &lateRows, &lateLit, &lateLuma, &lateSum)) return;
	if (!&qcom_usb_debug_write) return;
	// The surface's own first physical page and length, next to the import counters on the PANEL row:
	// the only reading that ties "this frame's surface" to "the range the GPU imported". A read-only
	// import of 2532 pages is a 1080x2400 unpadded buffer, while this surface's 4352 stride needs 2550,
	// so equal sizes do not prove equal memory and the two numbers have to be printed together.
	IOByteCount segment = 0;
	const addr64_t firstPhysical = memory->getPhysicalSegment(0, &segment, kIOMemoryMapperNone);
	// Does anything at all write this surface? On the first observation of a page it plants two words
	// through a writable mapping, and every later observation reports whether they are still there.
	// This opt-in intrusive probe observes only two words; survival does not prove the rest of the
	// surface was untouched. Normal pixel qualification leaves the compositor's memory alone.
	static addr64_t magicPage = 0;
	static bool magicPlanted = false;
	unsigned magic = 0;
	unsigned magicEnabled = 0;
	PE_parse_boot_argn("alioth-surface-magic", &magicEnabled, sizeof magicEnabled);
	if (magicEnabled) {
		// Uncached, so the plant reaches DRAM directly and the read sees what is really there: a
		// cacheable mapping would need a clean whose direction depends on the kernel's helper.
		IOMemoryMap *w = memory->map(kIOMapAnywhere | kIOMapInhibitCache);
		if (w && w->getVirtualAddress() && w->getLength() >= 8192) {
			volatile uint32_t *words = (volatile uint32_t *)w->getVirtualAddress();
			if (firstPhysical != magicPage) {
				words[0] = 0xdeadbeefu;
				words[1024] = 0xfeedfaceu;
				__asm__ volatile("dsb sy" ::: "memory");
				magicPage = firstPhysical;
				magicPlanted = true;
				magic = 2;   // planted now
			} else if (magicPlanted) {
				magic = (words[0] == 0xdeadbeefu && words[1024] == 0xfeedfaceu) ? 1u : 0u;
			}
		}
		OSSafeReleaseNULL(w);
	}
	char line[260];
	const int n = snprintf(line, sizeof line, "SURFACEL n=%u nonblack=%u/1000 luma=%u sum=%08x pa=%llx seg=%llx bytes=%llu stride=%llu magic=%u mx=%u/%u/%u sp=%u\n",
	                      lateRows, lateLit, lateLuma, lateSum,
	                      (unsigned long long)firstPhysical, (unsigned long long)segment,
	                      (unsigned long long)memory->getLength(), (unsigned long long)stride, magic,
	                      bestPixelR, bestPixelG, bestPixelB, bestPixelSpread);
	if (n > 0) qcom_usb_debug_write(line, (size_t)n < sizeof line ? (size_t)n : sizeof line - 1);
}

void native_legacy_swap_validation_observed(bool accepted, unsigned reason, const void *request, const void *surface) {
	framePacingObserved(accepted, reason);
	if (reason < 8) native_legacy_swap_reason_counts[reason] = native_legacy_swap_reason_counts[reason] + 1;
	// Band 8: a client reached the backend and submitted a swap. Painted before the verdict, so
	// the band reports the submission, not whether it was accepted.
	native_legacy_paint_userspace_band(8);
	if (accepted) {
		native_legacy_swap_stage(0);
		native_legacy_swap_accepted_count = native_legacy_swap_accepted_count + 1;
	} else {
		native_legacy_swap_rejected_count = native_legacy_swap_rejected_count + 1;
		recordRejectedSwap(reason, request, surface);
	}
	native_legacy_swap_validation_calls = native_legacy_swap_validation_calls + 1;
	native_legacy_swap_validation_result = accepted;
	native_legacy_swap_validation_reason = reason;
	native_legacy_swap_request = (uintptr_t)request;
	native_legacy_swap_surface = (uintptr_t)surface;
	// The frame the client submitted, measured where it is still only the client's surface: before any copy, so the
	// number cannot be confused with what reached the panel. The panel's own reading (BootProgress's PANEL row) says
	// the buffer ends up all black on every round so far, and this row separates the two causes: a black surface is a
	// render that produced nothing, a lit surface that still lands black is the copy. Samples every 64th dword: the
	// surface can be 10 MiB and this runs in the swap path.
	surfaceSampleObserved(accepted, surface);
	native_legacy_swap_validation_checkpoint(accepted, reason, request, surface);
}

void native_legacy_copy_timing_observed(uint64_t ticks, uint64_t bytes) {
	native_legacy_copy_bytes_last = bytes;
	native_legacy_copy_bytes_sum = native_legacy_copy_bytes_sum + bytes;
	native_legacy_copy_n = native_legacy_copy_n + 1;
	native_legacy_copy_last = ticks;
	native_legacy_copy_sum = native_legacy_copy_sum + ticks;
	if (ticks > native_legacy_copy_max) {
		native_legacy_copy_max = ticks;
	}
}

__attribute__((noinline)) void native_legacy_transfer_observed(IOReturn status, const void *request, uint64_t bytes) {
	native_legacy_transfer_calls = native_legacy_transfer_calls + 1;
	if (status == kIOReturnSuccess) {
		// Band 9: the copy into the scanout buffer succeeded, so a frame is in the memory the
		// panel reads. This is the last thing that can be true before the picture changes.
		native_legacy_paint_userspace_band(9);
		native_legacy_swap_stage(2);
	}
	native_legacy_transfer_status = status;
	native_legacy_transfer_request = (uintptr_t)request;
	native_legacy_transfer_bytes = bytes;
}

// The raster hand-off: offer each swapped frame to the GOP presenter, which hands it to the GPU
// client for the copy. This was a stub that declined every offer, so the display kept its CPU copy
// path and GPUFRAME read "gpu offered 0" no matter what sheng-fb-gpu said. Note the arity: the
// display driver calls this with six arguments, so a five-argument stub still linked under C
// linkage and silently dropped the format.
extern "C" IOService *native_gop_presenter_create(void);
extern "C" bool native_gop_gpu_offer(IOService *, void *display, IOMemoryDescriptor *, uint64_t stride, uint32_t width,
                                     uint32_t height, uint32_t format);
extern "C" void native_gop_presenter_set_source(IOService *presenter, IOService *nub);

static IOService *gpuHandoffPresenter;
static volatile uint32_t native_legacy_gpu_fallbacks;
static volatile int native_legacy_gpu_last_status;

bool native_legacy_gpu_offer(void *display, IOMemoryDescriptor *memory, uint64_t stride, uint32_t width, uint32_t height,
                             uint32_t format) {
	if (!gpuHandoffPresenter || !&native_gop_gpu_offer) return false;
	if (!native_gop_gpu_offer(gpuHandoffPresenter, display, memory, stride, width, height, format)) return false;
	// Keep this frame's memory for the after-copy reading (surfaceAfterCopyObserved); the completion gives it back.
	IOMemoryDescriptor *previous = gpuCopyMemory;
	if (memory) memory->retain();
	gpuCopyMemory = memory;
	gpuCopyStride = stride;
	gpuCopyHeight = height;
	OSSafeReleaseNULL(previous);
	return true;
}

void native_legacy_gpu_fallback_observed(int status) {
	native_legacy_gpu_last_status = status;
	native_legacy_gpu_fallbacks = native_legacy_gpu_fallbacks + 1;
}

__attribute__((noinline)) void native_legacy_retirement_observed(IOService *display) {
	const unsigned char *bytes = reinterpret_cast<const unsigned char *>(display);
	uint64_t current = 0, pending = 0;
	uint32_t completed = 0, presented = 0, frames = 0;
	memcpy(&current, bytes + 5472, 8);
	memcpy(&pending, bytes + 5480, 8);
	memcpy(&completed, bytes + 5820, 4);
	memcpy(&presented, bytes + 5824, 4);
	memcpy(&frames, bytes + 252, 4);
	native_legacy_retired_current = current;
	native_legacy_retired_pending = pending;
	native_legacy_retired_generation = completed;
	native_legacy_presented_generation = presented;
	native_legacy_retired_frames = frames;
	native_legacy_retirement_calls = native_legacy_retirement_calls + 1;
}

// The refresh tick, as counters: armes, messages sent, idle stops, late deadlines, starts, and
// refresh properties published at the requested rate. sheng prints these from its UFS log; here
// they are only kept for a future reader.
static volatile uint64_t vsyncEvents[6];
static volatile unsigned vsyncHzValue;
void native_legacy_vsync_observed(unsigned event, unsigned hz) {
	if (event < 6) {
		vsyncEvents[event] = vsyncEvents[event] + 1;
	}
	vsyncHzValue = hz;
}

// The counters above, for the console. Until now nothing read them: the comment below said they
// were "kept for a future reader", and the question they answer -- did the refresh tick arm, fire
// and publish on the boot that drew nothing -- stayed open with the data sitting in memory.
// Order is the argument to native_legacy_vsync_observed: 0 timer armed, 1 tick, 2 idle stop, 3
// late deadline, 4 start with a rate, 5 published at the requested rate.
unsigned native_legacy_vsync_counts(uint64_t *out, unsigned *hz) {
	if (out)
		for (unsigned i = 0; i < 6; ++i) out[i] = vsyncEvents[i];
	if (hz) *hz = vsyncHzValue;
	return 1;
}

void native_legacy_registration_observed(IOService *display) {
	native_legacy_registration_calls = native_legacy_registration_calls + 1;
	// Band 7: the display reached registerService. This is the only marker that reports the
	// service was actually published, and it sits right under the red band so a photograph
	// reads the boot path and the publication question in one column.
	native_legacy_paint_userspace_band(7);
	OSSerialize *xml = OSSerialize::withCapacity(2048);
	if (xml && display->serializeProperties(xml)) {
		unsigned size = xml->getLength();
		if (size >= sizeof(native_legacy_properties)) {
			size = sizeof(native_legacy_properties) - 1;
		}
		memcpy(native_legacy_properties, xml->text(), size);
		native_legacy_properties[size] = 0;
		native_legacy_properties_length = size;
	}
	OSSafeReleaseNULL(xml);
}

void native_legacy_client_enter(task_t task, uint32_t type) {
	native_legacy_client_calls = native_legacy_client_calls + 1;
	native_legacy_client_task = (uintptr_t)task;
	native_legacy_client_type = type;
	native_legacy_client_pid = task ? proc_pid((proc_t)get_bsdtask_info(task)) : 0;
	// No band here any more: band 15 now carries the exit hook's abnormal-exit marker, and a
	// client opening the device is already implied by band 8 (a swap was submitted), which is
	// the observation that actually matters. The pid is still recorded in
	// native_legacy_client_pid for a later reader.
}

// sheng interposes two entries of a copied vtable here to count every framebuffer user client.
// That is an offline instrument for its own qualification campaign, not part of the display
// path, so this board only records the last client it saw.
void native_legacy_client_observed(IOReturn status, IOUserClient *client) {
	native_legacy_client_status = status;
	native_legacy_client_object = (uintptr_t)client;
	if (client) {
		strlcpy(native_legacy_client_class, client->getMetaClass()->getClassName(),
		    sizeof(native_legacy_client_class));
	}
	native_legacy_client_checkpoint();
}

__attribute__((noinline)) bool native_legacy_gop_mapping_observed(IOService *object, IOMemoryMap *mapping,
    uint32_t width, uint32_t height) {
	if ((uintptr_t)object != native_legacy_object) {
		panic("NativeLegacyDisplay: wrong mapping owner");
	}
	native_legacy_gop_map = (uintptr_t)mapping;
	native_legacy_gop_virtual = mapping->getVirtualAddress();
	native_legacy_gop_physical = mapping->getPhysicalAddress();
	native_legacy_gop_bytes = mapping->getLength();
	native_legacy_gop_width = width;
	native_legacy_gop_height = height;
	native_legacy_gop_mapping_checkpoint();
	/* Publish this mapping to the USB console's screenshot provider, only when asked for.
	 * Measured 2026-10-27: the SCREENSHOT control verb returned "state=unavailable" because
	 * provider_state never reached 2 and nothing called qcom_usb_debug_register_framebuffer.
	 * It is opt-in because the build that always registered it also submitted no frame
	 * (SWAP calls=0 against 7 on the build before it), and that difference is not yet
	 * attributed. offline-screenshot=1 turns it on without touching the default path.
	 * Weak, so a kernel without that console still links. */
	unsigned screenshot = 0;
	if (PE_parse_boot_argn("offline-screenshot", &screenshot, sizeof(screenshot)) &&
	    screenshot == 1) {
		struct qcom_usb_framebuffer_abi {
			const volatile uint8_t *pixels;
			size_t bytes;
			uint32_t width, height, stride;
			uint32_t format;
		} framebuffer;
		extern int qcom_usb_debug_register_framebuffer(const void *) __attribute__((weak_import));
		if (&qcom_usb_debug_register_framebuffer) {
			framebuffer.pixels = (const volatile uint8_t *)mapping->getVirtualAddress();
			framebuffer.bytes = (size_t)mapping->getLength();
			framebuffer.width = width;
			framebuffer.height = height;
			/* The real stride comes from PE_Video, not from the mapping length: the
			 * GOP mapping can carry padding past the last row. */
			/* PE_current_console returns 0 on success and copies PE_state.video, so
			 * its v_rowBytes is the loader's real stride (4320 for this panel). */
			PE_Video published = {};
			framebuffer.stride = PE_current_console(&published) == 0 && published.v_rowBytes
			    ? (uint32_t)published.v_rowBytes : width * 4u;
			framebuffer.format = 1; /* QCOM_USB_BGRA8888 */
			/* The return value used to be discarded, so "registration refused" and "registered
			 * but never asked for a frame" looked identical from outside. Keep it and print it. */
			native_screenshot_register_status = qcom_usb_debug_register_framebuffer(&framebuffer);
		}
	}
	// Whether the screenshot collector accepted the frame buffer: 0 registered, otherwise the
	// refusal. Printed once, because a capture that never happens is otherwise unattributable.
	if (&qcom_usb_debug_write) {
		char shot[64];
		int n = snprintf(shot, sizeof shot, "SCREENSHOT register=%d\n", native_screenshot_register_status);
		if (n > 0) qcom_usb_debug_write(shot, (size_t)n);
	}
	// offline-legacy-unit-scale=<1..3> publishes /chosen display-scale as an offline UI policy:
	// that many actual scanout pixels per logical point. It changes only the published scale;
	// the scanout copy still moves one actual pixel per pixel.
	unsigned enabled = 0;
	PE_parse_boot_argn("offline-legacy-unit-scale", &enabled, sizeof(enabled));
	if (!enabled) {
		return true;
	}
	native_legacy_unit_scale_calls = native_legacy_unit_scale_calls + 1;
	native_legacy_unit_scale_status = 1;
	unsigned phase = 0;
	PE_parse_boot_argn("offline-legacy-phase", &phase, sizeof(phase));
	PE_Video video = {};
	if (enabled < 1 || enabled > 3 || phase != 4 || PE_current_console(&video) ||
	    video.v_width != width || video.v_height != height || video.v_rotate ||
	    mapping->getPhysicalAddress() != video.v_baseAddr) {
		return false;
	}
	IORegistryEntry *chosen = IORegistryEntry::fromPath("/chosen", gIODTPlane);
	bool scaleAdded = false, rotationAdded = false;
	bool ok = chosen != nullptr;
	if (ok) {
		OSData *scale = OSDynamicCast(OSData, chosen->getProperty("display-scale"));
		if (scale) {
			uint32_t word = 0;
			ok = scale->getLength() == sizeof(word) &&
			    (memcpy(&word, scale->getBytesNoCopy(), sizeof(word)), word == enabled);
		} else {
			OSData *data = OSData::withBytes(&enabled, sizeof(enabled));
			ok = data && chosen->setProperty("display-scale", data);
			scaleAdded = ok;
			OSSafeReleaseNULL(data);
		}
	}
	if (ok) {
		OSData *rotation = OSDynamicCast(OSData, chosen->getProperty("display-rotation"));
		if (rotation) {
			uint32_t word = 0;
			ok = rotation->getLength() == sizeof(word) &&
			    (memcpy(&word, rotation->getBytesNoCopy(), sizeof(word)), word == video.v_rotate);
		} else {
			OSData *data = OSData::withBytes(&video.v_rotate, sizeof(video.v_rotate));
			ok = data && chosen->setProperty("display-rotation", data);
			rotationAdded = ok;
			OSSafeReleaseNULL(data);
		}
	}
	if (ok) {
		OSData *scale = OSDynamicCast(OSData, chosen->getProperty("display-scale"));
		OSData *rotation = OSDynamicCast(OSData, chosen->getProperty("display-rotation"));
		uint32_t actualScale = 0, actualRotation = 0;
		ok = scale && rotation && scale->getLength() == 4 && rotation->getLength() == 4;
		if (ok) {
			memcpy(&actualScale, scale->getBytesNoCopy(), 4);
			memcpy(&actualRotation, rotation->getBytesNoCopy(), 4);
			native_legacy_chosen_scale = actualScale;
			native_legacy_chosen_rotation = actualRotation;
			native_legacy_unit_scale_status = actualScale == enabled && actualRotation == video.v_rotate ? 0 : 2;
			ok = native_legacy_unit_scale_status == 0;
		}
	}
	if (chosen && !ok) {
		if (scaleAdded) {
			chosen->removeProperty("display-scale");
		}
		if (rotationAdded) {
			chosen->removeProperty("display-rotation");
		}
	}
	OSSafeReleaseNULL(chosen);
	return ok;
}

// One of the seven unclassified backend leaves was reached. Stop here and name it: the component
// passes its own calibration state so the panic screen says which lifecycle point asked.
__attribute__((noreturn, noinline)) void native_legacy_backend_guard(unsigned slot, void *object, void *caller) {
	native_legacy_guard_slot = slot;
	native_legacy_guard_object = (uintptr_t)object;
	native_legacy_guard_caller = (uintptr_t)caller;
	panic("NativeLegacyDisplay: leaf=%u caller=%llx stage=%u PM=%u swap=%u/%u copy=%08x bytes=%llu retire=%u prepare=%u/%u",
	    slot, (uint64_t)(uintptr_t)caller, native_legacy_stage, native_legacy_pm_stage,
	    native_legacy_swap_validation_result, native_legacy_swap_validation_reason,
	    (unsigned)native_legacy_transfer_status, native_legacy_transfer_bytes, native_legacy_retirement_calls,
	    native_legacy_prepare_reason, native_legacy_prepare_attempts);
}

volatile uint32_t native_legacy_reached_band;
volatile uint64_t native_legacy_reached_base;
volatile uint32_t native_legacy_reached_geometry;
volatile uint32_t native_legacy_reached_mask;

// Paint band `index` of 16, in BGRA `colour`, across the full panel width. Bands 0 to 2 belong to
// the loader and the entry probe (green, blue, magenta); this file owns 3 and up, so a band that
// appears is proof the kernel reached that marker and can still write the frame buffer.
// The address comes from vinfo, not from PE_state.video: only vinfo carries the kernel mapping
// initialize_screen() made. It is the same source the panic screen reads.
// Every marker is drawn before the code it marks can fail, so a missing band means the kernel
// never got there, never mind why it stopped afterwards.
static void native_legacy_paint_band(unsigned index, uint32_t colour) {
	uint64_t base = vinfo.v_baseaddr;
	uint32_t rowbytes = vinfo.v_rowbytes, width = vinfo.v_width, height = vinfo.v_height;
	native_legacy_reached_base = base;
	native_legacy_reached_geometry = ((uint64_t)rowbytes << 32) | (width << 16) | (height & 0xFFFF);
	native_legacy_reached_mask |= 1u << index;
	if (!base || vinfo.v_depth != 32 || !rowbytes || !width || !height || rowbytes < width * 4u) {
		return;
	}
	uint64_t top = ((uint64_t)height * index) / 16u;
	uint64_t end = ((uint64_t)height * (index + 1u)) / 16u;
	if (end > height) {
		end = height;
	}
	volatile uint32_t *pixels = (volatile uint32_t *)base;
	for (uint64_t row = top; row < end; row++) {
		volatile uint32_t *line = (volatile uint32_t *)((volatile uint8_t *)pixels + row * rowbytes);
		for (uint32_t column = 0; column < width; column++) {
			line[column] = colour;
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	native_legacy_reached_band = index + 1u;
}

// Band 3, yellow: bsd_autoconf reached the late probe. Called unconditionally from its first
// statement, before any test can return.
void native_legacy_paint_reached_band(void) {
	unsigned enabled = 0;
	if (!PE_parse_boot_argn("offline-legacy-reached", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	native_legacy_paint_band(3, 0xFFFFFF00u);
}

// Bands 4 to 6, the userspace ladder. Each one answers a different question, so one photograph
// separates the cases that all look like "the panel never changed":
//   band 4 cyan   bsdinit_task ran: bsd_ast fired for the first time, so bsd_init() finished.
//   band 5 orange load_init_program was entered: the owner SystemOS and Data mounts returned.
//   band 6 red    the launchd execve returned 0: the image was actually loaded into process 1.
// No band 4 means the kernel never left bsd_init. Bands 4 and 5 but not 6 means exec failed,
// which is where AMFI, the trust cache and the union mount would each show up. All three means
// userspace started and the remaining silence is the compositor's, not the kernel's.
// These live here, in the board package, so the kernel patch that calls them needs no framebuffer
// code of its own and stays a single call site.
void native_legacy_paint_userspace_band(unsigned stage) {
	unsigned enabled = 0;
	if (!PE_parse_boot_argn("offline-legacy-reached", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	// Each pixel is a little-endian u32 in B,G,R,A byte order, so the value is
	// A<<24 | R<<16 | G<<8 | B. Yellow band 3 is 0xFFFFFF00 and the photograph confirms it,
	// which fixes that reading; these three use the same encoding.
	switch (stage) {
	case 4: native_legacy_paint_band(4, 0xFF00FFFFu); break;   // cyan:   B=FF G=FF R=00
	case 5: native_legacy_paint_band(5, 0xFFFFA500u); break;   // orange: B=00 G=A5 R=FF
	case 6: native_legacy_paint_band(6, 0xFFFF0000u); break;   // red:    B=00 G=00 R=FF
	// Bands 8 and 9 answer whether a display client ever reached this backend, which no earlier
	// band can: bands 3 to 6 all sit on the boot path and are painted by the kernel alone.
	//   band 8 purple  a client submitted a swap. The validator ran, whatever its verdict.
	//   band 9 white   a transfer completed and wrote the scanout buffer, so a frame reached
	//                  the panel's memory.
	// Band 8 without 9 means a client exists and its frames were refused or never copied; that
	// is a validator or transfer fault, not a missing client. Neither band means no client
	// submitted anything, so the gap is upstream of this backend.
	// Band 7 sits directly below the red band 6, so it is the easiest one to find in a
	// photograph. It is painted by native_legacy_registration_observed, i.e. only when the
	// original class's start() actually reaches its registerService call. A missing band 7
	// therefore means that start() returned before publishing the service, which is the
	// difference between "userspace cannot find the display" and "userspace found it and did
	// not present".
	case 7: native_legacy_paint_band(7, 0xFF808080u); break;   // grey
	// Band 8 was 0xFFFF00FF, which is the same value the entry probe paints for band 2, so the
	// two were indistinguishable in a photograph. Violet keeps the red channel at 0x80, which
	// separates it from magenta.
	case 8: native_legacy_paint_band(8, 0xFF8000FFu); break;   // violet: B=FF G=00 R=80
	case 9: native_legacy_paint_band(9, 0xFFFFFFFFu); break;   // white
	// Band 15, the last one, reports that a userspace process opened the display device.
	// Yellow-green keeps green at full and drops blue to 0x80, which separates it from band 4's
	// cyan (green and blue both full) and from the plain green the exec counter uses.
	case 15: native_legacy_paint_band(15, 0xFF00FF80u); break;  // yellow-green: B=80 G=FF R=00
	default: break;
	}
}

// Bands 10 to 13 are a four-bit counter, not four independent facts: each band is painted when
// that bit of the number of successful userspace execs is set. Read them as a binary number,
// bottom band = least significant. This answers how far launchd got, which a single yes/no band
// cannot: 0 means launchd never forked anything, a handful means it started a few daemons and
// then stalled, a dozen or more means the system was still bringing services up when the panel
// was photographed. backboardd appears early in a normal boot, so a count that stops below it
// says the stall is before the display server rather than in it.
// The counter saturates at 15 so a long-running system keeps a readable value instead of
// wrapping, and each band is painted only once per boot because repainting is the expensive part.
// The first exec is launchd itself, counted by band 6 at its own call site. So the count that
// matters starts at the second one: band 10 appears as soon as launchd has started any other
// process, which is the cheapest proof that it is running rather than merely loaded.
static volatile uint32_t execCount;

// Process exits, reported on the panel. A 2026-10-26 measurement found fewer than ten processes
// alive while the exec counter had passed fifteen, so processes are being created and then
// exiting almost at once. `panic_on_crash_disabled=1` is set, so a crashed daemon panics nothing
// and leaves no trace anywhere; this hook is the only channel that can report it.
//
// Two bands, painted on the first exit that matches each condition, so a photograph says whether
// early exits happened at all and how bad they were:
//
//   band 14 violet   some process other than launchd exited
//   band 15 white    some process exited with a reason that is not a plain exit
//
// "Not a plain exit" means an exit reason exists and its namespace is not the ordinary one, or
// the process was killed by a signal. Those are the interesting cases: a daemon that exits 0 and
// is respawned is normal, a daemon killed by the system is not.
static volatile uint32_t exitCount, lastExitPid, lastExitNamespace, lastExitCode, lastExitSignal;
void native_legacy_note_exit(uint32_t pid, const char *name, int namespace_value, uint32_t code,
    int signal) {
	unsigned enabled = 0;
	if (!PE_parse_boot_argn("offline-legacy-reached", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	exitCount = exitCount + 1u;
	lastExitPid = pid;
	lastExitNamespace = (uint32_t)namespace_value;
	lastExitCode = code;
	lastExitSignal = (uint32_t)signal;
	// launchd itself never exits without panicking the kernel, so anything counted here is a
	// process launchd started. Band 14 says exits happen at all.
	static bool sawOther;
	if (!sawOther) {
		native_legacy_paint_band(14, 0xFF8000FFu);  // violet
		sawOther = true;
	}
	// The pid of the FIRST abnormal exit, kept for a later reader. Painting it would need a row
	// that no other marker uses and every row is taken, so the value is recorded rather than
	// guessed at.
	static bool sawFirstAbnormalPid;
	if (!sawFirstAbnormalPid && namespace_value != -1 && namespace_value != 0) {
		lastExitPid = pid;
		sawFirstAbnormalPid = true;
	}
	(void)signal;
	// Band 15 carries the REASON as a colour. Photographs on 2026-10-26 showed band 14 lit with
	// fewer than ten processes alive and the exec counter past fifteen: processes are created and
	// then die. Band 15 was purple in one photograph and grey in another, so more than one reason
	// occurs and the last one painted wins; that is why the namespace is split further below.
	//
	// Namespace colours. OS_REASON_LIBSYSTEM gets its own codes as separate colours, because it
	// was the one observed and its code says which libsystem check fired:
	//   white    CODESIGNING 3   AMFI or a signature check rejected the binary
	//   orange   EXEC        9   the exec itself failed a policy check
	//   cyan     DYLD        6   the dynamic loader refused the image
	//   yellow-green JETSAM  1   the system killed it for memory
	//   grey     any other namespace
	//   dark     no reason recorded at all, i.e. a plain exit
	// libsystem codes, from libkern/os/reason_private.h:55-63:
	//   purple       LIBSYSTEM code 1   workloop ownership leak
	//   deep purple  LIBSYSTEM code 2   generic fault (the common one)
	//   blue         LIBSYSTEM code 3   secinit initializer
	//   teal         LIBSYSTEM code 4   pthread corruption
	//   magenta      LIBSYSTEM code 5   os_log_fault from libtrace
	//   violet       LIBSYSTEM code 6 or more, unclassified
	if (namespace_value == -1 || namespace_value == 0) {
		return;
	}
	uint32_t colour = 0xFF808080u;   // grey: unclassified namespace
	switch (namespace_value) {
	case 3:  colour = 0xFFFFFFFFu; break;   // codesigning
	case 9:  colour = 0xFFFFA500u; break;   // exec
	case 6:  colour = 0xFF00FFFFu; break;   // dyld
	case 1:  colour = 0xFF00FF80u; break;   // jetsam
	case 18:
		// libsystem: the code matters more than the namespace, so it picks the colour.
		switch (code) {
		case 1:  colour = 0xFFFF00FFu; break;   // workloop ownership leak   purple
		case 2:  colour = 0xFF800080u; break;   // generic fault              deep purple
		case 3:  colour = 0xFFFF0000u; break;   // secinit initializer        red
		case 4:  colour = 0xFF808000u; break;   // pthread corruption         olive
		case 5:  colour = 0xFF00FF00u; break;   // os_log_fault               green
		default: colour = 0xFF8000FFu; break;   // unclassified libsystem code violet
		}
		break;
	default: break;
	}
	native_legacy_paint_band(15, colour);
}

void native_legacy_count_exec(void) {
	unsigned enabled = 0;
	if (!PE_parse_boot_argn("offline-legacy-reached", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	execCount = execCount + 1u;
	// Process-count thresholds, one solid band each. A cell bitmap was tried first and abandoned:
	// reading 32 cells out of a phone photograph needs the sampling grid to line up, and
	// compression plus the panel's moire made the reading unreliable. A solid band is a yes or no
	// that survives any camera, so the count is reported as thresholds instead of a number.
	//
	//   band 10 cyan    any process at all exists
	//   band 11 purple  at least 10 processes exist
	//   band 12 teal    at least 20
	//   band 13 brown   at least 30
	//
	// A normal boot passes 30 well before SpringBoard, so a panel that stops at 10 or 20 says the
	// system stalled early and the display question is not yet the right one to ask.
	// Each threshold keeps its own flag: the process count grows over time, so a single latch
	// would paint the first threshold reached and then freeze the picture at that value.
	static uint32_t thresholdPainted;
	uint32_t live = 0;
	proc_t walk;
	LIST_FOREACH(walk, &allproc, p_list) {
		live = live + 1u;
	}
	if (live >= 1u && !(thresholdPainted & 1u)) {
		native_legacy_paint_band(10, 0xFF00FFFFu);   // cyan
		thresholdPainted |= 1u;
	}
	if (live >= 10u && !(thresholdPainted & 2u)) {
		native_legacy_paint_band(11, 0xFFFF00FFu);   // purple
		thresholdPainted |= 2u;
	}
	if (live >= 20u && !(thresholdPainted & 4u)) {
		native_legacy_paint_band(12, 0xFF808000u);   // teal
		thresholdPainted |= 4u;
	}
	if (live >= 30u && !(thresholdPainted & 8u)) {
		native_legacy_paint_band(13, 0xFF004080u);   // brown
		thresholdPainted |= 8u;
	}
}

// This board has no serial console and no other kernel output channel past the MMU, so a silent
// early return in this file looks exactly like "the probe never ran": the panel keeps the
// DarwinBoot frame in both cases. With offline-legacy-verdict=1 a failed path stops in the panic
// screen and names its stage, which is the only way one flash can report where the port died.
// Success does not panic; it is read from the panel itself.
__attribute__((noinline)) void native_legacy_verdict(const char *where) {
	unsigned enabled = 0;
	if (!PE_parse_boot_argn("offline-legacy-verdict", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	panic("NativeLegacyDisplay: %s stage=%u meta=%d init=%u probe=%u start=%u dt=%u surface=%d/%u video=%llx",
	    where, native_legacy_stage, native_legacy_meta_status, native_legacy_init_result,
	    native_legacy_probe_match, native_legacy_start_result, native_legacy_dt_attached,
	    native_legacy_surface_status, native_legacy_surface_found, native_legacy_video_base);
}

void native_legacy_display_probe(void) {
	// Two call sites reach this boot-thread entry. Run the class constructors once: a second
	// postModLoad would try to register the same metaclasses again.
	if (native_legacy_stage) {
		return;
	}
	native_legacy_stage = 1;
	void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
	if (!handle) {
		native_legacy_verdict("preModLoad");
		native_legacy_display_checkpoint();
		return;
	}
	native_legacy_display_static_init();
	native_gop_presenter_static_init();
	native_legacy_meta_status = OSMetaClass::postModLoad(handle);
	if (native_legacy_meta_status) {
		native_legacy_verdict("postModLoad");
		native_legacy_display_checkpoint();
		return;
	}
	PE_Video video = {};
	native_legacy_stage = 2;
	// The firmware console must be a plain 32-bit, unrotated, single-plane framebuffer that this
	// service can map one-to-one. Anything else is left to the original family's own handling.
	if (PE_current_console(&video) || !video.v_baseAddr || !video.v_width || !video.v_height ||
	    video.v_depth != 32 || video.v_rotate || video.v_width > 16384 || video.v_height > 16384 ||
	    video.v_rowBytes < video.v_width * 4 || video.v_rowBytes > UINT64_MAX / video.v_height) {
		native_legacy_verdict("console");
		native_legacy_display_checkpoint();
		return;
	}
	IOService *display = native_legacy_display_allocate();
	native_legacy_object = (uintptr_t)display;
	if (!display) {
		native_legacy_verdict("allocate");
		native_legacy_display_checkpoint();
		return;
	}
	native_legacy_object_size = display->getMetaClass()->getClassSize();
	native_legacy_parent_match = display->metaCast("IOMobileFramebufferLegacy") &&
	    display->metaCast("IOMobileFramebuffer") && display->metaCast("IOService");
	native_legacy_stage = 3;
	native_legacy_init_result = display->init();
	unsigned phase = 1;
	PE_parse_boot_argn("offline-legacy-phase", &phase, sizeof(phase));
	if (native_legacy_init_result && (phase == 2 || phase == 3 || phase == 4)) {
		// IOSurface matches the real IOBSD resource, published later by bsd_autoconf: hold the
		// instance until that normal lifecycle boundary.
		pendingLegacyDisplay = display;
		return;
	}
	native_legacy_stage = 7;
	display->release();
	native_legacy_released = 1;
	native_legacy_stage = 8;
	native_legacy_verdict("init");
	native_legacy_display_checkpoint();
}

void native_legacy_display_late_probe(void) {
	// Unconditional, and first: this band separates "bsd_autoconf reached the late probe" from
	// "the kernel stopped earlier", which the panel otherwise cannot tell apart, because both
	// leave the DarwinBoot frame in place.
	native_legacy_paint_reached_band();
	// sheng-ls-reset=1: empty the LaunchServices store before userland starts (BootDiagExport.c).
	// pkd rejects every /Applications plugin because the containing app has no LS record, and its
	// own message names 'mobile_install rebuild system user' as the remedy; there is no shell on
	// this board, so the store is invalidated instead and lsd builds a new one.
	extern void boot_diag_ls_reset(void) __attribute__((weak_import));
	if (&boot_diag_ls_reset) boot_diag_ls_reset();
	// The HID publisher. It is an IOHIDDevice compiled into this kernel, so IOKit has no
	// personality for it and nothing would start it: the board starts it explicitly, which the
	// driver adaption guide names as the second of the two start paths and places in this
	// function alongside the other board initialisation calls. Opt-in through
	// offline-hid-input=1. Inside this file's extern "C" block, so no linkage specifier here.
	extern int native_hid_input_probe(void);
	native_hid_input_probe();

	// The A650 probe, at the point the reference board calls it. Opt-in through pipa-gpu=<mode>.
	extern void native_gpu_probe(void);
	native_gpu_probe();

	// The panel brightness service and the physical buttons. Both are board services the porting
	// guide requires (display.md steps 3-4, input.md step 4), both opt-in through their own boot
	// argument, and both live in this archive because it is the one the kernel link force-loads.
	// Declared weak so a build without them still links.
	extern void alioth_backlight_probe(void) __attribute__((weak_import));
	if (&alioth_backlight_probe) alioth_backlight_probe();
	extern void alioth_buttons_probe(void) __attribute__((weak_import));
	if (&alioth_buttons_probe) alioth_buttons_probe();

	// The Wi-Fi controller's power and clock steps. Opt-in through pcie=<n>; step 1 only reads.
	extern void alioth_pcie_init(void);
	alioth_pcie_init();
	{
		/* The QCA6390 board layer (drivers/AliothWifi). It needs the trained link and the
		 * windows the step above leaves, so it runs right after it; the argument keeps the
		 * normal boot untouched until the first MHI round says otherwise. */
		extern void alioth_wifi_start(void) __attribute__((weak_import));
		unsigned wifi = 0;
		if (&alioth_wifi_start && PE_parse_boot_argn("alioth-wifi", &wifi, sizeof wifi) && wifi) alioth_wifi_start();
	}

	// The AppleKeyStore stand-in. Same reason as the HID publisher: a selected component that
	// nothing references is dropped from a static link, and without it there is no keybag to
	// answer the post-restart passcode prompt. Opt-in through offline-key-events=1.
	extern void native_keystore_probe(void);
	native_keystore_probe();
	// The CPU frequency vote (sheng-cpufreq=<n>) maps the EPSS domains and votes the top
	// non-turbo level; it reports its state through native_cpufreq_row on the panel.
	/* This file sits inside an extern "C" block to the end, so the declaration must not repeat
	 * the linkage specifier: an extern "C" inside a function body there is a syntax error. */
	extern void sheng_cpufreq_probe(void) __attribute__((weak_import));
	if (&sheng_cpufreq_probe) sheng_cpufreq_probe();
	// The touch controller is a separate part from the display and needs no framebuffer, so it
	// starts here rather than after the display path, which can refuse early. Opt-in through
	// native-touch=1: the default boot must stay the configuration that holds the lock screen,
	// and touchProbeStart maps six register windows, votes clocks and starts its own thread.
	// It returns after starting that thread, so it does not hold the boot here.
	{
		unsigned touch = 0;
		if (PE_parse_boot_argn("native-touch", &touch, sizeof(touch)) && touch == 1) {
			const bool started = touchProbeStart();
			if (&qcom_usb_debug_write) {
				/* The step number is the only diagnosis this driver has: fail() records it in
				 * native_touch_fail_step and nothing else reads that variable. 1 = a register
				 * window did not map, 2 = the GCC clocks did not come up, 3 = the GENI read
				 * failed, 4 = the SE reported FIFO disabled (the DMA path, not written), 5 =
				 * GENI init, 6 = GENI setup, 7 = the serial clock, 8 = the FTS5652 boot
				 * handshake, 9 = no thread. Without it a refusal is just "start=0". */
				extern volatile uint32_t native_touch_fail_step __attribute__((weak_import));
				extern volatile int native_touch_stage_result __attribute__((weak_import));
				extern volatile unsigned native_touch_stage_value __attribute__((weak_import));
				extern volatile unsigned native_touch_stage_total __attribute__((weak_import));
				extern char native_touch_stage_name[32] __attribute__((weak_import));
				unsigned step = &native_touch_fail_step ? native_touch_fail_step : 0;
				/* The last register read the protocol core made, with the bytes it got and the
				 * transfer result. Step 8 is the FTS5652 handshake, which reads 0xa3/0x9f for
				 * the application id, 0x90 for the ROM id and 0xa6 for the version; without
				 * this the failure names none of them. */
				extern volatile unsigned native_touch_crc_expected __attribute__((weak_import));
				extern volatile unsigned native_touch_crc_received __attribute__((weak_import));
				extern volatile unsigned native_touch_hdr0 __attribute__((weak_import));
				extern volatile unsigned native_touch_hdr1 __attribute__((weak_import));
				extern volatile unsigned native_touch_hdr2 __attribute__((weak_import));
				extern volatile unsigned native_touch_hdr3 __attribute__((weak_import));
				extern volatile unsigned native_touch_data0 __attribute__((weak_import));
				extern volatile unsigned native_touch_data1 __attribute__((weak_import));
				extern volatile unsigned native_touch_data2 __attribute__((weak_import));
				char text[256];
				int n = snprintf(text, sizeof(text),
				    "TOUCH start=%u step=%u last=%s value=%04x rc=%d reads=%u "
				    "crc=%04x/%04x hdr=%02x%02x%02x%02x data=%02x%02x%02x\n",
				    started ? 1u : 0u, step,
				    &native_touch_stage_name ? native_touch_stage_name : "-",
				    &native_touch_stage_value ? native_touch_stage_value : 0,
				    &native_touch_stage_result ? native_touch_stage_result : 0,
				    &native_touch_stage_total ? native_touch_stage_total : 0,
				    &native_touch_crc_expected ? native_touch_crc_expected : 0,
				    &native_touch_crc_received ? native_touch_crc_received : 0,
				    &native_touch_hdr0 ? native_touch_hdr0 : 0, &native_touch_hdr1 ? native_touch_hdr1 : 0,
				    &native_touch_hdr2 ? native_touch_hdr2 : 0, &native_touch_hdr3 ? native_touch_hdr3 : 0,
				    &native_touch_data0 ? native_touch_data0 : 0, &native_touch_data1 ? native_touch_data1 : 0,
				    &native_touch_data2 ? native_touch_data2 : 0);
				if (n > 0) qcom_usb_debug_write(text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
				/* The three SPI pins, as written and as read back. Equal values prove the pin
				 * write reached the controller; a mismatch or a zero says the window is wrong. */
				extern volatile unsigned native_touch_pin_cfg[3] __attribute__((weak_import));
				extern volatile unsigned native_touch_pin_want[3] __attribute__((weak_import));
				extern volatile unsigned native_touch_tlmm_probe[3] __attribute__((weak_import));
				extern volatile unsigned native_touch_ready __attribute__((weak_import));
				extern volatile unsigned native_touch_ready_ms __attribute__((weak_import));
				extern volatile unsigned native_touch_app_id __attribute__((weak_import));
				extern volatile unsigned native_touch_app_rc[3] __attribute__((weak_import));
				n = snprintf(text, sizeof(text), "TOUCH ready=%u after=%ums\n",
				    &native_touch_ready ? native_touch_ready : 0,
				    &native_touch_ready_ms ? native_touch_ready_ms : 0);
				if (n > 0) qcom_usb_debug_write(text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
				n = snprintf(text, sizeof(text), "TOUCH app=%04x rc=%u/%u/%u\n",
				    &native_touch_app_id ? native_touch_app_id : 0,
				    &native_touch_app_rc ? native_touch_app_rc[0] : 0,
				    &native_touch_app_rc ? native_touch_app_rc[1] : 0,
				    &native_touch_app_rc ? native_touch_app_rc[2] : 0);
				if (n > 0) qcom_usb_debug_write(text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
				n = snprintf(text, sizeof(text), "TOUCH tlmm north=%08x south=%08x\n",
				    &native_touch_tlmm_probe ? native_touch_tlmm_probe[0] : 0,
				    &native_touch_tlmm_probe ? native_touch_tlmm_probe[1] : 0);
				if (n > 0) qcom_usb_debug_write(text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
				n = snprintf(text, sizeof(text), "TOUCH pins cfg=%08x,%08x,%08x want=%08x,%08x,%08x\n",
				    &native_touch_pin_cfg ? native_touch_pin_cfg[0] : 0,
				    &native_touch_pin_cfg ? native_touch_pin_cfg[1] : 0,
				    &native_touch_pin_cfg ? native_touch_pin_cfg[2] : 0,
				    &native_touch_pin_want ? native_touch_pin_want[0] : 0,
				    &native_touch_pin_want ? native_touch_pin_want[1] : 0,
				    &native_touch_pin_want ? native_touch_pin_want[2] : 0);
				if (n > 0) qcom_usb_debug_write(text, (size_t)n < sizeof(text) ? (size_t)n : sizeof(text) - 1);
			}
		}
	}
	IOService *display = pendingLegacyDisplay;
	if (!display) {
		// Either the graphics kext load failed, so bsd_init never called the first entry, or the
		// first entry rejected the console. Both leave no instance here and neither reaches a
		// verdict of its own; this is the only place that can still name the outcome.
		native_legacy_verdict("no-pending-instance");
		return;
	}
	pendingLegacyDisplay = nullptr;
	// The original IOSurface personality needs the kext and the real IOCoreSurfaceRoot service,
	// not a substitute for either.
	native_legacy_surface_status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOSurface");
	OSDictionary *matching = IOService::nameMatching("IOCoreSurfaceRoot");
	IOService *surface = matching ? IOService::waitForMatchingService(matching, 5000000000ULL) : nullptr;
	native_legacy_surface_found = surface != nullptr;
	OSSafeReleaseNULL(surface);
	OSSafeReleaseNULL(matching);
	PE_Video video = {};
	if (native_legacy_surface_status || !native_legacy_surface_found || PE_current_console(&video) ||
	    !video.v_baseAddr || !video.v_width || !video.v_height || video.v_depth != 32 ||
	    video.v_rotate || video.v_width > 16384 || video.v_height > 16384 ||
	    video.v_rowBytes < video.v_width * 4 || video.v_rowBytes > UINT64_MAX / video.v_height) {
		native_legacy_stage = 7;
		display->release();
		native_legacy_released = 1;
		native_legacy_stage = 8;
		native_legacy_verdict("late-surface-or-console");
		native_legacy_display_checkpoint();
		return;
	}
	native_legacy_video_base = video.v_baseAddr;
	native_legacy_video_bytes = video.v_rowBytes * video.v_height;
	// A real firmware framebuffer resource, with the addresses and extent taken from the actual
	// GOP handoff. There is no invented compatible, panel timing, display list or vendor register.
	native_legacy_stage = 4;
	IOPlatformDevice *nub = new IOPlatformDevice;
	native_legacy_nub = (uintptr_t)nub;
	bool nubAttached = false, nubStarted = false, attached = false, started = false;
	IORegistryEntry *displayTreeParent = nullptr;
	bool dtAttached = false;
	IOService *parent = IOService::getResourceService();
	IODeviceMemory *memory = IODeviceMemory::withRange(video.v_baseAddr, video.v_rowBytes * video.v_height);
	OSArray *ranges = OSArray::withCapacity(1);
	if (nub && memory && ranges && parent && nub->init()) {
		ranges->setObject(memory);
		nub->setDeviceMemory(ranges);
		nub->setName("NativeGOPFramebuffer");
		nubAttached = nub->attach(parent);
		if (nubAttached) {
			nubStarted = nub->start(parent);
		}
		if (nubStarted) {
			unsigned phase = 0;
			PE_parse_boot_argn("offline-legacy-phase", &phase, sizeof(phase));
			if (phase == 4) {
				// /arm-io in the gIODTPlane is the ADT node that devicetree.py always writes at the
				// root, not a node of the Linux FDT. It carries the SoC children the display
				// subsystem belongs beside.
				displayTreeParent = IORegistryEntry::fromPath("/arm-io", gIODTPlane);
				const char type[] = "display-subsystem";
				OSData *data = OSData::withBytes(type, sizeof(type));
				if (displayTreeParent && data && nub->setProperty("device_type", data)) {
					nub->setName("gop-display", gIODTPlane);
					dtAttached = nub->attachToParent(displayTreeParent, gIODTPlane);
				}
				OSSafeReleaseNULL(data);
				native_legacy_dt_attached = dtAttached;
			}
			if (phase == 3) {
				// Presenter-only mode: publish the owned transport service, never the incomplete
				// original backend.
				IOService *presenter = native_gop_presenter_create();
				bool presenterAttached = false;
				if (presenter && presenter->init() && (presenterAttached = presenter->attach(nub)) &&
				    presenter->start(nub)) {
					activeGOPPresenter = presenter;
					activeGOPNub = nub;
					nub = nullptr;
					nubAttached = false;
					nubStarted = false;
				} else {
					if (presenterAttached) {
						presenter->detach(nub);
					}
					OSSafeReleaseNULL(presenter);
				}
			} else {
				SInt32 score = 0;
				native_legacy_probe_match = (phase != 4 || dtAttached) && display->probe(nub, &score) == display;
				if (native_legacy_probe_match) {
					attached = display->attach(nub);
				}
				if (attached) {
					native_legacy_stage = 5;
					// The display bank's stage-1 window, built here and not earlier: porting/ufs.md step 5 and drivers.md
					// section 2 put storage before display, and the display's own startDirect() checks this window once and
					// does not retry. Both symbols are weak, so a build without the storage driver or without the direct
					// path still links and simply skips this.
					extern uintptr_t native_ufs_smmu_base(void) __attribute__((weak_import));
					extern int native_ufs_dpu_window_build(uintptr_t) __attribute__((weak_import));
					if (&native_ufs_smmu_base && &native_ufs_dpu_window_build) {
						uintptr_t smmu = native_ufs_smmu_base();
						if (smmu) native_ufs_dpu_window_build(smmu);
					}
					started = display->start(nub);
					native_legacy_start_result = started;
				}
				if (started && phase == 4) {
					activeGOPNub = nub;
					/* The GPU hand-off presenter. It hangs off the resource service, not off this
					 * display's node: a presenter attached under the display made SpringBoard find no
					 * display and crash (sheng recorded that), so the placement is deliberate. */
					unsigned gpu = 0;
					if (PE_parse_boot_argn("sheng-fb-gpu", &gpu, sizeof gpu) && gpu == 1 && &native_gop_presenter_create) {
						IOService *handoff = native_gop_presenter_create();
						IOService *home = IOService::getResourceService();
						bool attached = false;
						if (handoff && home) native_gop_presenter_set_source(handoff, nub);
						if (handoff && home && handoff->init() && (attached = handoff->attach(home)) && handoff->start(home))
							gpuHandoffPresenter = handoff;
						else {
							if (attached) handoff->detach(home);
							OSSafeReleaseNULL(handoff);
						}
					}
					native_legacy_stage = 10;
					native_legacy_display_checkpoint();
					return;
				}
				// The nub was published and the backend did not come up. The teardown below reuses
				// nub, so record which arm stopped it before the local state goes away.
				if (phase == 4) {
					native_legacy_verdict(native_legacy_probe_match ? "attach" : "probe");
				}
			}
		}
	}
	native_legacy_stage = 6;
	if (started) {
		display->stop(nub);
	}
	if (attached) {
		display->detach(nub);
	}
	if (dtAttached) {
		nub->detachFromParent(displayTreeParent, gIODTPlane);
	}
	if (nubStarted) {
		nub->stop(parent);
	}
	if (nubAttached) {
		nub->detach(parent);
	}
	OSSafeReleaseNULL(ranges);
	OSSafeReleaseNULL(memory);
	OSSafeReleaseNULL(nub);
	OSSafeReleaseNULL(displayTreeParent);
	native_legacy_stage = 7;
	display->release();
	native_legacy_released = 1;
	native_legacy_stage = 8;
	// The nub never started: name what the device tree nub needed and did not get.
	native_legacy_verdict("nub");
	native_legacy_display_checkpoint();
}
}
