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

void native_legacy_surface_rejected(uint64_t width, uint64_t height, uint32_t format, uint32_t planes,
    uint64_t stride, uint64_t allocation, uint64_t bytes) {
	native_legacy_rej_surface[0] = width;
	native_legacy_rej_surface[1] = height;
	native_legacy_rej_surface[2] = format;
	native_legacy_rej_surface[3] = planes;
	native_legacy_rej_surface[4] = stride;
	native_legacy_rej_surface[5] = allocation;
	native_legacy_rej_surface[6] = bytes;
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

void native_legacy_swap_validation_observed(bool accepted, unsigned reason, const void *request, const void *surface) {
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

// The raster hand-off lives in sheng's Adreno port. This board has no GPU client yet, so the
// owned display keeps its CPU copy path: the offer is declined and the component falls back.
bool native_legacy_gpu_offer(void *, IOMemoryDescriptor *, uint64_t, uint32_t, uint32_t) {
	return false;
}

void native_legacy_gpu_fallback_observed(int status) {
	(void)status;
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
					started = display->start(nub);
					native_legacy_start_result = started;
				}
				if (started && phase == 4) {
					activeGOPNub = nub;
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
