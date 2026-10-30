/* SPDX-License-Identifier: BSD-2-Clause
 * The endpoint-ID assignment the Apple USB device Family asks a controller subclass for.
 *
 * provideEndpointIDsForConfiguration(OSArray *interfaces, UInt8 *ids) hands this method the
 * interface list of the configuration the host selected and expects, for every pipe in it, one
 * endpoint number written into `ids`. The Family then builds its pipes from those numbers. A
 * wrong number, a wrong direction bit or a wrong count does not fail here: it fails later, as a
 * host that cannot open the configuration, which is why this is a pure function with a host test.
 *
 * The rules, taken from AppleNubSynopsysOTGDevice's own implementation and from the working
 * MSM8998 port that was checked against it:
 *
 *   - IN and OUT share one 1..15 space per direction, counted separately.
 *   - Every alternative setting of one interface draws from the same starting number, and the
 *     interface then advances the start by the largest number it used, so two alternatives of the
 *     same interface never claim the same endpoint.
 *   - A number above 15, or one whose physical endpoint is beyond the controller's endpoint
 *     count, is NoResources.
 *   - The direction bit is the number's high bit: 0x80 for IN.
 *
 * This header has no IOKit dependency so a host compiler can include it. AliothUSBDevice.cpp
 * walks the real OSArray and calls these functions.
 */
#ifndef ALIOTH_USB_ENDPOINT_H
#define ALIOTH_USB_ENDPOINT_H

#include <stdint.h>

/* The Family's `ids` array is indexed by physical endpoint, so this is the DWC3 core's endpoint
 * count: DWC3_ENDPOINTS_NUM is 32 (Dwc3Regs.h:55), which is what the hardware exposes.
 *
 * Do not confuse it with EP_COUNT = 6 in QcomXnuUsbDebug/usb/Dwc3.c: that is the size of that
 * driver's own per-endpoint arrays for the CDC console (EP0 at 0..1, notify 3, out 4, in 5),
 * not a hardware limit. A controller subclass must use the hardware number, because the Family
 * allocates its pipes against it and a host may ask for more endpoints than the console uses. */
#define ALIOTH_USB_ENDPOINT_COUNT 32

/* Pipe counts above this are refused rather than clamped: Apple's implementation returns
 * kIOReturnNoResources at 31 and this port keeps the same ceiling. */
#define ALIOTH_USB_MAX_PIPES_PER_SETTING 30

typedef struct {
    uint32_t pipe_count;      /* pipes in this alternative setting */
    uint32_t in_count;        /* of those, how many are IN */
    uint32_t out_count;       /* the rest; in_count + out_count must equal pipe_count */
} alioth_usb_setting_summary;

typedef struct {
    uint32_t first_in;        /* next IN number this interface may use */
    uint32_t first_out;       /* next OUT number this interface may use */
    uint32_t written;          /* how many ids have been written so far */
    uint32_t limit;            /* physical endpoint count */
} alioth_usb_endpoint_cursor;

static inline void alioth_usb_cursor_init(alioth_usb_endpoint_cursor *cursor, uint32_t limit)
{
    cursor->first_in = 1;
    cursor->first_out = 1;
    cursor->written = 0;
    cursor->limit = limit;
}

/* Assign the endpoints of one alternative setting. Returns 0 on success.
 *
 * On failure it writes nothing and leaves `cursor->written` where it was: a configuration the
 * Family cannot open must not be published half-filled. The checks therefore run over the whole
 * setting before any byte is stored, which is why this loops twice rather than once.
 *
 * `in_first` is where IN numbering starts for this setting, `out_first` likewise; `used_in` and
 * `used_out` come back as how many this setting consumed, which is what advances the interface. */
static inline int alioth_usb_assign_setting(uint8_t *ids,
                                            alioth_usb_endpoint_cursor *cursor,
                                            uint32_t in_first, uint32_t out_first,
                                            const alioth_usb_setting_summary *setting,
                                            uint32_t *used_in, uint32_t *used_out)
{
    uint32_t i, nin = 0, nout = 0;
    if (!cursor || !setting || !used_in || !used_out) return -1;
    if (setting->pipe_count > ALIOTH_USB_MAX_PIPES_PER_SETTING) return -1;
    if (setting->pipe_count && !ids) return -1;
    if (setting->in_count > setting->pipe_count) return -1;
    if (setting->out_count > setting->pipe_count - setting->in_count) return -1;
    if (cursor->written + setting->pipe_count > cursor->limit) return -1;
    *used_in = 0;
    *used_out = 0;
    /* Pass one: every number must fit. Nothing is written until all of them do. */
    for (i = 0; i < setting->pipe_count; i++) {
        const int is_in = i < setting->in_count;
        const uint32_t number = is_in ? in_first + nin : out_first + nout;
        if (is_in) nin++; else nout++;
        /* Endpoint numbers are 4 bits wide, so 15 is the last one. The physical index is
         * number*2 plus the direction bit, and that must stay inside the controller's count:
         * 15 IN is physical 31, which needs a limit of at least 32. */
        if (number > 15 || number * 2 + (uint32_t)(is_in ? 1 : 0) >= cursor->limit) return -1;
    }
    /* Pass two: commit. Same arithmetic, now known to fit. */
    nin = nout = 0;
    for (i = 0; i < setting->pipe_count; i++) {
        const int is_in = i < setting->in_count;
        const uint32_t number = is_in ? in_first + nin : out_first + nout;
        if (is_in) nin++; else nout++;
        ids[cursor->written++] = (uint8_t)(number | (is_in ? 0x80u : 0u));
    }
    *used_in = nin;
    *used_out = nout;
    return 0;
}

/* Assign a setting whose pipe directions are given one by one, in the order the interface
 * reports them. This is what a real configuration needs: the Family's pipe order is the
 * descriptor's, not IN-first, so a summary of counts cannot express it.
 *
 * `directions[i]` is 0 for OUT and 1 for IN, the same encoding IOUSBDevicePipe::getDirection
 * returns. Same two-pass rule as above: nothing is written unless every number fits. */
static inline int alioth_usb_assign_setting_directions(uint8_t *ids,
                                                       alioth_usb_endpoint_cursor *cursor,
                                                       uint32_t in_first, uint32_t out_first,
                                                       const uint8_t *directions,
                                                       uint32_t pipe_count,
                                                       uint32_t *used_in, uint32_t *used_out)
{
    uint32_t i, nin = 0, nout = 0;
    if (!cursor || !directions || !used_in || !used_out) return -1;
    if (pipe_count > ALIOTH_USB_MAX_PIPES_PER_SETTING) return -1;
    if (pipe_count && !ids) return -1;
    if (cursor->written + pipe_count > cursor->limit) return -1;
    *used_in = 0;
    *used_out = 0;
    for (i = 0; i < pipe_count; i++) {
        const uint32_t number = directions[i] ? in_first + nin : out_first + nout;
        if (directions[i] > 1) return -1;
        if (directions[i]) nin++; else nout++;
        if (number > 15 || number * 2 + (uint32_t)directions[i] >= cursor->limit) return -1;
    }
    nin = nout = 0;
    for (i = 0; i < pipe_count; i++) {
        const uint32_t number = directions[i] ? in_first + nin : out_first + nout;
        if (directions[i]) nin++; else nout++;
        ids[cursor->written++] = (uint8_t)(number | (directions[i] ? 0x80u : 0u));
    }
    *used_in = nin;
    *used_out = nout;
    return 0;
}

/* Advance one interface's cursors by what its alternatives consumed. The maximum, not the sum:
 * alternatives are mutually exclusive, so two of them may reuse the same numbers. */
static inline void alioth_usb_advance(alioth_usb_endpoint_cursor *cursor,
                                      uint32_t reserve_in, uint32_t reserve_out)
{
    if (!cursor) return;
    cursor->first_in += reserve_in;
    cursor->first_out += reserve_out;
}

#endif /* ALIOTH_USB_ENDPOINT_H */
