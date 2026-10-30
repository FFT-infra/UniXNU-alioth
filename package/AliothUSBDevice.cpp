// SPDX-License-Identifier: BSD-2-Clause
// Alioth (SM8250) IOUSBDeviceController — A-3, first method: createIOBuffer.
//
// The slot layout in IOUSBDeviceController.h is measured from this board's own firmware
// (iPadOS 17.7.11 / 21H461), not copied from another version. See the header for the evidence
// per slot. This file implements only what A-3 needs so far; the other pure slots still return
// fixed values and are filled in order.
#include "IOUSBDeviceController.h"
#include "AliothUsbEndpoint.h"
#include "AliothUsbInterface.h"

#define STUB_UNSUPPORTED kIOReturnUnsupported

// AppleNubSynopsysOTGDevice::createIOBuffer (slot 189) rejects a capacity above 1 MiB and
// otherwise forwards to the IOBufferMemoryDescriptor factory with an alignment mask taken from
// its own ivar. This port keeps the same ceiling: a request larger than that is a caller bug,
// and returning null makes it fail at the caller instead of here.
enum { kAliothIOBufferMax = 1024u * 1024u, kAliothIOBufferAlign = 4096u };

class AliothUSBDevice : public IOUSBDeviceController {
    OSDeclareDefaultStructors(AliothUSBDevice)
public:
    bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
    void free() APPLE_KEXT_OVERRIDE;

    IOReturn goOnBus(bool) APPLE_KEXT_OVERRIDE;
    bool connectedWithAccessoryCable() APPLE_KEXT_OVERRIDE;
    void handleStallEndpoint(int, bool) APPLE_KEXT_OVERRIDE;
    void handleAbortEndpoint(int) APPLE_KEXT_OVERRIDE;
    void handleSetAddressPreStatusPhase(int) APPLE_KEXT_OVERRIDE;
    void handleSetAddressPostStatusPhase(int) APPLE_KEXT_OVERRIDE;
    UInt32 getConnectionSpeed() APPLE_KEXT_OVERRIDE;
    IOReturn provideEndpointIDsForConfiguration(OSArray *, UInt8 *) APPLE_KEXT_OVERRIDE;
    void prepareConfiguration(OSArray *) APPLE_KEXT_OVERRIDE;
    IOReturn activateEndpoint(int, int, int, int, tIOUSBDeviceInterfacePipeProperty) APPLE_KEXT_OVERRIDE;
    void deactivateEndpoint(int) APPLE_KEXT_OVERRIDE;
    IOBufferMemoryDescriptor *createIOBuffer(UInt32, UInt64) APPLE_KEXT_OVERRIDE;
    bool isEndpointStalled(int) APPLE_KEXT_OVERRIDE;
    void resetEndpointDataToggle(int) APPLE_KEXT_OVERRIDE;
    IOReturn doRemoteWakeGated() APPLE_KEXT_OVERRIDE;
    IOReturn ioGated(_IOUSBDeviceIORequestTag *) APPLE_KEXT_OVERRIDE;

    void nativeSlot175(UInt32) APPLE_KEXT_OVERRIDE;
    void nativeSlot199() APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AliothUSBDevice, IOUSBDeviceController)

bool AliothUSBDevice::start(IOService *provider)
{
    if (!IOUSBDeviceController::start(provider)) return false;
    readyForHost();          // instead of registerService(); see IOUSBDeviceController.h
    return true;
}

void AliothUSBDevice::stop(IOService *provider) { IOUSBDeviceController::stop(provider); }
void AliothUSBDevice::free() { IOUSBDeviceController::free(); }

/* The buffer the Family hands to a transfer. Apple's own DWC2 driver caps the request at 1 MiB
 * and takes its alignment from the controller; this port caps at the same 1 MiB and aligns to
 * one page, which is what the DWC3 DMA arena and the SMMU mapping already require.
 *
 * The KMA_LOMEM trap ios_sagit hit does not apply here, and that is measured, not assumed:
 * this board's USB DMA is a static array (QcomXnuUsbDebug/kernel/QcomXnuUsbDebug.c:37,
 * `static uint8_t usb_dma[DWC3_DMA_BYTES]`) whose physical address comes from ml_static_vtop
 * (validate_static_pages, same file :705), never from kmem_alloc_contig. The kernel's
 * vm_lopage_needed is FALSE (osfmk/vm/vm_resident.c:163), so an IOBufferMemoryDescriptor with a
 * numeric physical mask would be the allocation that falls back to unrestricted pages. Keeping
 * the Family's own buffers inside one contiguous arena is what avoids that; this method only
 * allocates the small per-transfer descriptors, which the Family frees itself.
 */
IOBufferMemoryDescriptor *AliothUSBDevice::createIOBuffer(UInt32 options, UInt64 capacity)
{
    if (capacity == 0 || capacity > kAliothIOBufferMax) return nullptr;
    return IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task,
                                                       options,
                                                       (vm_size_t)capacity,
                                                       (vm_offset_t)kAliothIOBufferAlign);
}

IOReturn AliothUSBDevice::goOnBus(bool) { return STUB_UNSUPPORTED; }
bool AliothUSBDevice::connectedWithAccessoryCable() { return false; }
void AliothUSBDevice::handleStallEndpoint(int, bool) {}
void AliothUSBDevice::handleAbortEndpoint(int) {}
void AliothUSBDevice::handleSetAddressPreStatusPhase(int) {}
void AliothUSBDevice::handleSetAddressPostStatusPhase(int) {}
UInt32 AliothUSBDevice::getConnectionSpeed() { return 1; }              // 1 = full speed, lowest
/* The Family hands the selected configuration's interface list and an `ids` array indexed by
 * physical endpoint. Every pipe gets a number here, and the Family builds its pipes from those
 * numbers, so this is the one place a wrong count turns into "the host cannot open the
 * configuration" much later. The arithmetic lives in AliothUsbEndpoint.h with a host test
 * (package/tests/alioth_usb_endpoint_host.c); this method only walks the OSArray.
 *
 * Not implemented yet: the pipe objects carry the per-pipe direction and the per-alternative
 * grouping, and IOUSBDeviceInterface's accessors are declared in the Family's private headers
 * that this port has not reconstructed. Until they are, the method reports the configuration as
 * unsupported rather than guessing an order: a guessed order produces wrong endpoint numbers
 * that enumerate far enough to look like progress. */
IOReturn AliothUSBDevice::provideEndpointIDsForConfiguration(OSArray *interfaces, UInt8 *ids)
{
    alioth_usb_endpoint_cursor cursor;
    uint32_t index, interfaces_count;

    if (!interfaces || !ids) return kIOReturnBadArgument;
    interfaces_count = interfaces->getCount();
    if (!interfaces_count) return kIOReturnBadArgument;
    /* The ids array is indexed by physical endpoint, so its capacity is the controller's. */
    alioth_usb_cursor_init(&cursor, ALIOTH_USB_ENDPOINT_COUNT);

    for (index = 0; index < interfaces_count; index++) {
        IOUSBDeviceInterface *interface = IOUSBDeviceInterface::fromObject(interfaces->getObject(index));
        alioth_usb_endpoint_cursor interface_start = cursor;
        int alternatives, alt;
        uint32_t reserve_in = 0, reserve_out = 0;
        if (!interface) return kIOReturnBadArgument;
        alternatives = interface->getAlternateSettingCount();
        if (alternatives <= 0 || alternatives > 256) return kIOReturnUnsupported;

        /* Every alternative setting of one interface draws from the same starting numbers, and
         * the interface then advances by the largest one used, so two alternatives never claim
         * the same endpoint.
         *
         * The ids array accumulates across alternatives: each alternative's pipes each append an
         * entry, so the count is the sum of all alternatives' pipe counts, not the maximum. That
         * is what the Family expects -- it indexes ids by the pipe it is building, and the same
         * number appearing under two alternatives is the point, since only one is active at a
         * time. Apple's own driver and the working MSM8998 port both append with one running
         * index; assigning into a per-alternative copy and keeping the maximum would drop every
         * alternative but the widest. */
        for (alt = 0; alt < alternatives; alt++) {
            int pipes = interface->getPipeCountForAlternateSetting((UInt8)alt);
            uint8_t directions[ALIOTH_USB_MAX_PIPES_PER_SETTING];
            uint32_t used_in = 0, used_out = 0, p;
            alioth_usb_endpoint_cursor probe = cursor;
            if (pipes <= 0 || pipes > ALIOTH_USB_MAX_PIPES_PER_SETTING) return kIOReturnNoResources;
            for (p = 0; p < (uint32_t)pipes; p++) {
                IOUSBDevicePipe *pipe = interface->getPipeForAlternateSetting((int)p, (UInt8)alt);
                int direction;
                if (!pipe) return kIOReturnBadArgument;
                direction = pipe->getDirection();
                if (direction != 0 && direction != 1) return kIOReturnBadArgument;
                directions[p] = (uint8_t)direction;
            }
            /* Assign into a copy first: if this alternative does not fit, the interface's
             * earlier alternatives must not be left half-written. */
            if (alioth_usb_assign_setting_directions(ids, &probe,
                                                     interface_start.first_in, interface_start.first_out,
                                                     directions, (uint32_t)pipes,
                                                     &used_in, &used_out) != 0) {
                return kIOReturnNoResources;
            }
            /* Append: this alternative's pipes go after the ones already written. */
            cursor.written = probe.written;
            if (used_in > reserve_in) reserve_in = used_in;
            if (used_out > reserve_out) reserve_out = used_out;
        }
        cursor.first_in = interface_start.first_in;
        cursor.first_out = interface_start.first_out;
        alioth_usb_advance(&cursor, reserve_in, reserve_out);
    }
    setProperty("USBDeviceEndpointIDs", ids, cursor.written);
    return kIOReturnSuccess;
}
/* Called when the host selects a configuration, before the endpoints are activated. Apple's own
 * DWC2 driver deactivates every enabled endpoint first and then restarts endpoint configuration;
 * that needs the controller, which qcom-usb-debug owns until A-5. Reporting nothing here is
 * correct while the driver has no controller: the Family still holds the configuration, and
 * activateEndpoint (also stubbed) is what would have used it. */
void AliothUSBDevice::prepareConfiguration(OSArray *interfaces)
{
    (void)interfaces;
}
IOReturn AliothUSBDevice::activateEndpoint(int, int, int, int, tIOUSBDeviceInterfacePipeProperty) { return STUB_UNSUPPORTED; }
void AliothUSBDevice::deactivateEndpoint(int) {}
bool AliothUSBDevice::isEndpointStalled(int) { return false; }
void AliothUSBDevice::resetEndpointDataToggle(int) {}
IOReturn AliothUSBDevice::doRemoteWakeGated() { return STUB_UNSUPPORTED; }
IOReturn AliothUSBDevice::ioGated(_IOUSBDeviceIORequestTag *) { return STUB_UNSUPPORTED; }

void AliothUSBDevice::nativeSlot175(UInt32) {}
void AliothUSBDevice::nativeSlot199() {}
