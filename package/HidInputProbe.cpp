// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Starts the ShengHidInput device on this board.
//
// NativeHidInput is an IOHIDDevice compiled into this kernel, not a kext, so IOKit has no
// personality to match it against and nothing would ever start it. The driver adaption guide
// names the two ways a service starts: one matched through its provider, which needs a
// personality and probe/start, and one the board starts explicitly once its resources and OS
// dependencies are ready. A statically linked driver is the second kind, and the same guide
// points at the board initialisation calls in native_legacy_display_late_probe as where those
// calls belong, which is where the touch core is started from.
//
// The sequence itself is the reference implementation's, not something the documentation
// describes: register the class (which is why its static initialiser was globalised when the
// object was built), then allocate, init, attach and start. UniXNU-sheng and UniXNU-pipa both
// do exactly this; the only board-specific parts here are the boot argument and the provider.
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSKext.h>
#include <libkern/c++/OSMetaClass.h>
#include <libkern/c++/OSSymbol.h>
#include <pexpert/pexpert.h>
#include <mach/kmod.h>

extern kmod_info_t g_kernel_kmod_info;
// The static initialiser of the publisher, globalised by build-sheng-hid-input.py so that a
// statically linked class can be registered from here instead of by a kext load.
extern "C" void native_hid_static_init(void) __asm__("__GLOBAL__sub_I_NativeHidInput.cpp");
extern "C" IOService *native_hid_input_allocate(void);
extern "C" IOService *native_hid_touch_allocate(void);
extern "C" kern_return_t OSKextLoadKextWithIdentifier(const char *);

extern "C" {
// Published for the diagnostic panel and the console report.
volatile int native_hid_matching_load_status = -1;
volatile uint32_t native_hid_stage, native_hid_resource_found;
volatile int native_hid_load_status = -1, native_hid_input_meta_status = -1;
volatile uint32_t native_hid_class_sizes[5];
volatile uint64_t native_hid_input_object;
// The publisher's tick asks whether it should run. The touch core feeds it directly through
// the sink, so nothing has to poll: answer yes once the board has asked for the digitizer.
volatile uint32_t native_hid_input_command;
}

bool native_hid_input_requested(void) {
  return true;
}

// Runs once, from native_legacy_display_late_probe, after the display and the touch core are up.
// Returns 0 when the device started. Idempotent: a second call returns the first result.
extern "C" int native_hid_input_probe(void) {
  static int result = -1;
  if (result >= 0) return result;
  unsigned enabled = 0;
  if (!PE_parse_boot_argn("offline-hid-input", &enabled, sizeof(enabled)) || enabled != 1) {
    result = 1;                       // not asked for on this boot
    return result;
  }
  native_hid_stage = 1;
  // IOHIDFamily must be loaded first: the device attaches to the IOHIDResource it publishes.
  native_hid_load_status = OSKextLoadKextWithIdentifier("com.apple.iokit.IOHIDFamily");
  /* Two kexts, not one. IOHIDFamily provides the device class and IOHIDResource, but the thing
   * that makes a report reach the system is IOHIDEventDriver, which is a separate kext. Without
   * it the publisher starts, publishes, and every report is dropped by its first line
   * (hasEventDriverCached() finds no child service), so native_touch_hid_reports stays zero
   * while contacts arrive. Measured on boot-usb92: stage reached 8 with zero reports. The
   * reference board loads both, in this order. */
  if (!native_hid_load_status)
    native_hid_matching_load_status = OSKext::loadKextWithIdentifier(
        "com.apple.iokit.IOHIDEventDriver", true, false, kOSKextExcludeNone, kOSKextExcludeNone, nullptr);
  const char *names[5] = {"IOHIDUserDevice", "IOHIDEventDriver", "IOHIDEventService",
                          "IOHIDResourceDeviceUserClient", "IOHIDDevice"};
  for (unsigned i = 0; i < 5; ++i) {
    const OSSymbol *name = OSSymbol::withCString(names[i]);
    const OSMetaClass *meta = name ? OSMetaClass::getMetaClassWithName(name) : nullptr;
    if (meta) native_hid_class_sizes[i] = meta->getClassSize();
    OSSafeReleaseNULL(name);
  }
  native_hid_stage = 2;
  OSDictionary *match = IOService::serviceMatching("IOHIDResource");
  IOService *resource = match ? IOService::waitForMatchingService(match, 5000000000ULL) : nullptr;
  OSSafeReleaseNULL(match);
  native_hid_resource_found = resource != nullptr;
  if (resource == nullptr) { native_hid_stage = 3; result = 2; return result; }
  // 208 is the IOHIDDevice instance size this build's headers describe. If it does not match,
  // the class layout and the original vtable disagree and starting it would corrupt memory.
  if (native_hid_class_sizes[4] != 208) { native_hid_stage = 4; result = 3; OSSafeReleaseNULL(resource); return result; }
  void *handle = OSMetaClass::preModLoad(g_kernel_kmod_info.name);
  if (handle == nullptr) { native_hid_stage = 5; result = 4; OSSafeReleaseNULL(resource); return result; }
  native_hid_static_init();
  native_hid_input_meta_status = OSMetaClass::postModLoad(handle);
  if (native_hid_input_meta_status != 0) { native_hid_stage = 6; result = 5; OSSafeReleaseNULL(resource); return result; }
  IOService *device = native_hid_input_allocate();
  native_hid_input_object = (uintptr_t) device;
  bool valid = device && device->metaCast("IOHIDDevice") && device->init();
  bool attached = valid && device->attach(resource);
  bool started = attached && device->start(resource);
  if (!started) {
    native_hid_stage = 7;
    if (attached) device->detach(resource);
    OSSafeReleaseNULL(device);
    native_hid_input_object = 0;
    result = 6;
    OSSafeReleaseNULL(resource);
    return result;
  }
  native_hid_stage = 8;
  result = 0;
  OSSafeReleaseNULL(resource);
  return result;
}
