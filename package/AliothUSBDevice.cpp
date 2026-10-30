// SPDX-License-Identifier: BSD-2-Clause
// Alioth (SM8250) IOUSBDeviceController — A-3, first method: createIOBuffer.
//
// The slot layout in IOUSBDeviceController.h is measured from this board's own firmware
// (iPadOS 17.7.11 / 21H461), not copied from another version. See the header for the evidence
// per slot. This file implements only what A-3 needs so far; the other pure slots still return
// fixed values and are filled in order.
#include "IOUSBDeviceController.h"
#include "AliothUsbEndpoint.h"

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
    (void)interfaces; (void)ids;
    return STUB_UNSUPPORTED;
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
