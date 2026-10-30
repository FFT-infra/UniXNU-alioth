/* SPDX-License-Identifier: BSD-2-Clause
 * Opaque read-only views of the Family's interface and pipe objects.
 *
 * IOUSBDeviceController::provideEndpointIDsForConfiguration is handed an OSArray of interfaces,
 * and each interface answers with its alternative-setting count and, per setting, the pipes in
 * it. Those three accessors plus the pipe's direction are the whole vocabulary this port needs
 * to assign endpoint numbers.
 *
 * These are NOT subclass headers. The Family owns the real objects with their own metaclasses
 * and vtables; this file declares only the storage size and the audited non-virtual calls, so
 * that a pointer obtained from the OSArray can be used without inventing a vtable.
 *
 * Evidence, all measured on this board's own kernelcache (iPadOS 17.7.11 / 21H461):
 *
 *   instance size   IOUSBDeviceInterface = 312   OSMetaClass ctor `mov w3,#0x138`
 *                                                (0xfffffff00616f300, 0xfffffff00616f3e0,
 *                                                 0xfffffff0061726d0)
 *                   IOUSBDevicePipe      = 120   OSMetaClass ctor `mov w3,#0x78`
 *                                                (0xfffffff006172728, 0xfffffff0061727f8)
 *   accessor bytes  each of the four below was found by exact byte match in our image, so the
 *                   instruction sequences are identical to the ones the working MSM8998 port
 *                   audited; only their addresses differ between versions:
 *                     getDirection                   0xfffffff006173e34
 *                     getAlternateSettingCount       0xfffffff00616fa0c
 *                     getPipeCountForAlternateSetting 0xfffffff00616fa58
 *                     getPipeForAlternateSetting     0xfffffff00616faa8
 *
 * Read-only, and that was checked rather than assumed: getDirection is two instructions
 * (`ldr w0,[x0,#0x1c]; ret`), and the other three read the object and dispatch through a vtable
 * slot with w3..w5 = 0, storing only to their own stack slot. None writes a controller register,
 * so using them cannot collide with qcom-usb-debug's ownership of the controller.
 */
#ifndef ALIOTH_USB_DEVICE_INTERFACE_H
#define ALIOTH_USB_DEVICE_INTERFACE_H

#include <libkern/c++/OSObject.h>

class IOUSBDevicePipe {
    class MetaClass : public OSMetaClass { OSObject *alloc() const APPLE_KEXT_OVERRIDE; };
    alignas(UInt64) [[maybe_unused]] UInt8 opaque[120];
    IOUSBDevicePipe() = delete;
    IOUSBDevicePipe(const IOUSBDevicePipe &) = delete;
    IOUSBDevicePipe &operator=(const IOUSBDevicePipe &) = delete;
    ~IOUSBDevicePipe() = delete;
public:
    static const MetaClass gMetaClass;
    int getDirection();          /* native 0 = OUT, 1 = IN */
};

class IOUSBDeviceInterface {
    class MetaClass : public OSMetaClass { OSObject *alloc() const APPLE_KEXT_OVERRIDE; };
    alignas(UInt64) [[maybe_unused]] UInt8 opaque[312];
    IOUSBDeviceInterface() = delete;
    IOUSBDeviceInterface(const IOUSBDeviceInterface &) = delete;
    IOUSBDeviceInterface &operator=(const IOUSBDeviceInterface &) = delete;
    ~IOUSBDeviceInterface() = delete;
public:
    static const MetaClass gMetaClass;
    int getAlternateSettingCount() const;
    int getPipeCountForAlternateSetting(UInt8 alternative);
    IOUSBDevicePipe *getPipeForAlternateSetting(int pipe, UInt8 alternative);
    static IOUSBDeviceInterface *fromObject(OSObject *object) {
        return reinterpret_cast<IOUSBDeviceInterface *>(
            OSMetaClassBase::safeMetaCast(object, &gMetaClass));
    }
};

static_assert(sizeof(IOUSBDevicePipe) == 120, "21H461 IOUSBDevicePipe instance size");
static_assert(sizeof(IOUSBDeviceInterface) == 312, "21H461 IOUSBDeviceInterface instance size");

#endif /* ALIOTH_USB_DEVICE_INTERFACE_H */
