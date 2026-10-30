// SPDX-License-Identifier: BSD-2-Clause
// Alioth (SM8250) IOUSBDeviceController skeleton — A-2 first cut.
//
// The slot layout below is MEASURED from this board's own firmware
// (iPadOS 17.7.11 / 21H461), not copied from any other version:
//
//   tmp/usb-rootcause/kexts/com_apple_iokit_IOUSBDeviceFamily  (IOUSBDeviceController)
//   vtable @ 0xfffffff006e81da0, 234 slots, slot 0..167 inherited from IOService
//
// Cross-checks that fix the layout:
//   slot 167  __ZN9IOService15powerChangeDoneEm      (last inherited)
//   slot 168  createPipe                             (first own)
//   slot 175  PURE
//   slot 177  PURE
//   slot 179..189 PURE (11 consecutive)
//   slot 192,193 PURE
//   slot 199,202 PURE
//   slot 227  PURE                                   (ioGated)
//   slot 234+ is the OSMetaClass table, not this vtable
//
// Behavioural identification used for the non-pure tail:
//   207 returns 2                  -> getMaxSpeedCapability
//   208 returns 0                  -> nativeSlot208 (Synopsys overrides to 1 = FS)
//   209 ldrb [x0,#0x17c]           -> getDeviceAddress
//
// A 235-slot layout exists in iPadOS 18 (22H374); it has ONE extra virtual
// between slot 209 and ioGated. Using that numbering on 21H461 misaligns the
// tail, so every slot here is declared from our own measurement.
#pragma once
#include <IOKit/IOService.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

class IOUSBDevicePipe;
class IOUSBDeviceControlRequest;
class IOTimerEventSource;
struct _IOUSBDeviceIORequestTag;
struct tCreatePipeParameters;
enum tIOUSBDeviceInterfacePipeProperty : UInt32;
enum tUSBDeviceConnectionSpeed : UInt32;
enum tUSBLinkState : UInt32;

class IOUSBDeviceController : public IOService {
    OSDeclareAbstractStructors(IOUSBDeviceController)
public:
    // ---- inherited overrides we rely on (do not change slot order) ----
    void free() APPLE_KEXT_OVERRIDE;
    IOReturn setProperties(OSObject *) APPLE_KEXT_OVERRIDE;
    bool start(IOService *) APPLE_KEXT_OVERRIDE;
    void stop(IOService *) APPLE_KEXT_OVERRIDE;
    IOReturn callPlatformFunction(const OSSymbol *, bool, void *, void *, void *, void *) APPLE_KEXT_OVERRIDE;

    // ---- 168..174 : pipe plumbing (7, none pure) ----
    virtual IOUSBDevicePipe *createPipe(tCreatePipeParameters &);                 // 168
    virtual IOReturn activatePipe(IOUSBDevicePipe *);                             // 169
    virtual IOReturn deactivatePipe(IOUSBDevicePipe *);                           // 170
    virtual UInt16 getAddress() const;                                            // 171
    virtual IOReturn createPipeGated(tCreatePipeParameters &, IOUSBDevicePipe *&); // 172
    virtual IOReturn activatePipeGated(IOUSBDevicePipe *);                        // 173
    virtual IOReturn deactivatePipeGated(IOUSBDevicePipe *);                      // 174

    // ---- 175..189 : the device contract ----
    virtual void nativeSlot175(UInt32) = 0;                                       // 175 PURE
    virtual void nativeSlot176();                                                 // 176
    virtual IOReturn goOnBus(bool) = 0;                                           // 177 PURE
    virtual IOReturn handleUserlandCommand(OSString *, OSObject *);               // 178
    virtual bool connectedWithAccessoryCable() = 0;                               // 179 PURE
    virtual void handleStallEndpoint(int, bool) = 0;                              // 180 PURE
    virtual void handleAbortEndpoint(int) = 0;                                    // 181 PURE
    virtual void handleSetAddressPreStatusPhase(int) = 0;                         // 182 PURE
    virtual void handleSetAddressPostStatusPhase(int) = 0;                        // 183 PURE
    virtual UInt32 getConnectionSpeed() = 0;                                      // 184 PURE
    virtual IOReturn provideEndpointIDsForConfiguration(OSArray *, UInt8 *) = 0;  // 185 PURE
    virtual void prepareConfiguration(OSArray *) = 0;                             // 186 PURE
    virtual IOReturn activateEndpoint(int, int, int, int, tIOUSBDeviceInterfacePipeProperty) = 0; // 187 PURE
    virtual void deactivateEndpoint(int) = 0;                                     // 188 PURE
    virtual IOBufferMemoryDescriptor *createIOBuffer(UInt32, UInt64) = 0;         // 189 PURE

    // ---- 190..198 : state / suspend-resume ----
    virtual uintptr_t nativeSlot190();                                            // 190
    virtual uintptr_t nativeSlot191();                                            // 191
    virtual bool isEndpointStalled(int) = 0;                                      // 192 PURE
    virtual void resetEndpointDataToggle(int) = 0;                                // 193 PURE
    virtual uintptr_t nativeSlot194();                                            // 194
    virtual void handleUSBSuspend();                                              // 195
    virtual void handleUSBReset();                                                // 196
    virtual void handleUSBConnectionDone();                                       // 197
    virtual void handleUSBResume();                                               // 198

    // ---- 199..202 ----
    virtual void nativeSlot199() = 0;                                             // 199 PURE
    virtual uintptr_t nativeSlot200();                                            // 200
    virtual uintptr_t nativeSlot201();                                            // 201
    virtual IOReturn doRemoteWakeGated() = 0;                                     // 202 PURE

    // ---- 203..226 : the tail. 21H461 has ONE FEWER slot here than 22H374. ----
    virtual uintptr_t nativeSlot203();                                            // 203
    virtual void handleUSBCableConnect();                                         // 204
    virtual void handleUSBCableDisconnect();                                      // 205
    virtual uintptr_t nativeSlot206();                                            // 206
    virtual UInt32 getMaxSpeedCapability();                                       // 207  returns 2
    virtual uintptr_t nativeSlot208();                                            // 208  returns 0 (Synopsys: 1 = FS)
    virtual UInt8 getDeviceAddress();                                             // 209  ldrb [x0+0x17c]
    virtual uintptr_t nativeSlot210();                                            // 210
    virtual uintptr_t nativeSlot211();                                            // 211
    virtual uintptr_t nativeSlot212();                                            // 212
    virtual uintptr_t nativeSlot213();                                            // 213
    virtual uintptr_t nativeSlot214();                                            // 214
    virtual uintptr_t nativeSlot215();                                            // 215
    virtual void handleControlRequestStateChange(UInt32, UInt32);                 // 216
    virtual uintptr_t nativeSlot217();                                            // 217
    virtual uintptr_t nativeSlot218();                                            // 218
    virtual uintptr_t nativeSlot219();                                            // 219
    virtual uintptr_t nativeSlot220();                                            // 220
    virtual uintptr_t nativeSlot221();                                            // 221
    virtual uintptr_t nativeSlot222();                                            // 222
    virtual uintptr_t nativeSlot223();                                            // 223
    virtual IOUSBDeviceControlRequest *createControlRequestTracker();             // 224
    virtual uintptr_t nativeSlot225();                                            // 225
    virtual IOReturn io(int, IOMemoryDescriptor *, IOByteCount, void *, UInt64 *, UInt32); // 226

    // ---- 227..233 ----
    virtual IOReturn ioGated(_IOUSBDeviceIORequestTag *) = 0;                     // 227 PURE
    virtual _IOUSBDeviceIORequestTag *allocateRequest();                          // 228
    virtual void freeRequest(_IOUSBDeviceIORequestTag *);                         // 229
    virtual IODMACommand *getDMACommand();                                        // 230
    virtual uintptr_t nativeSlot231();                                            // 231
    virtual uintptr_t nativeSlot232();                                            // 232
    virtual uintptr_t nativeSlot233();                                            // 233

    // Non-virtual entry point the Family provides: it creates USBStringIndex, the
    // control-request tracker and the default device description, then publishes.
    // Hardware adapters must call this INSTEAD of registerService() (pitfall 1).
    void readyForHost();
};
