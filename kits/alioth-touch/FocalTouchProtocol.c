// SPDX-License-Identifier: BSD-2-Clause-Patent
// FocalTech FTS5652 SPI protocol implementation. Wire format and register map
// follow the FocalTech FTS driver of the alioth Android kernel (ArrowOS
// android_kernel_xiaomi_alioth, focaltech_3658u) and the UniXNU Fts8203 port.
#include "FocalTouchProtocol.h"
#include <string.h>

// Kermit CRC16 (poly 0x8408, init 0xFFFF). Trailer is stored little-endian.
static uint16_t wire_crc(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < length; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0x8408 : 0);
        }
    }
    return crc;
}

static void stage(struct Fts5652 *chip, const char *name, unsigned value, int result)
{
    if (chip->io.stage) {
        chip->io.stage(chip->io.context, name, value, result);
    }
}

static int exchange(struct Fts5652 *chip, size_t length, int read, size_t payload)
{
    int result = -5;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        memset(chip->rx, 0, length);
        result = chip->io.transfer(chip->io.context, chip->tx, chip->rx, length);
        chip->io.delay_us(chip->io.context, 150);
        chip->last_status = chip->rx[3];
        if (result) {
            continue;
        }
        if (chip->last_status & 0xa0) {
            result = -5;
            continue;
        }
        if (read) {
            // Data follows the 7-byte header (4 header + 3 dummy); CRC trailer is
            // two bytes after the payload, little-endian.
            uint16_t received = (uint16_t)chip->rx[7 + payload] |
                                ((uint16_t)chip->rx[8 + payload] << 8);
            if (wire_crc(chip->rx + 7, payload) != received) {
                chip->last_crc_fail = 1;
                /* Keep what the check compared: without it a CRC refusal cannot be told
                 * apart from a bus that returned garbage, and the two need opposite fixes.
                 * boot-usb44 returned -74 on every read with no way to see either number. */
                chip->last_crc_expected = wire_crc(chip->rx + 7, payload);
                chip->last_crc_received = received;
                chip->last_header[0] = chip->rx[0];
                chip->last_header[1] = chip->rx[1];
                chip->last_header[2] = chip->rx[2];
                chip->last_header[3] = chip->rx[3];
                chip->last_data0 = chip->rx[7];
                chip->last_data1 = chip->rx[8];
                chip->last_data2 = chip->rx[9];
                result = -74;
                continue;
            }
            chip->last_crc_fail = 0;
            /* Record the pair on success too. "The frame passed its CRC" is the premise the
             * whole read path rests on, and until these two bytes are visible it is an
             * inference from a counter that could also be zero because the check never ran.
             * An all-0xFF payload has CRC 0xF11C, so a trailer of 0xFFFF here would mean the
             * bus returned nothing rather than a valid empty frame, and the two need opposite
             * fixes. */
            chip->last_crc_expected = wire_crc(chip->rx + 7, payload);
            chip->last_crc_received = received;
        }
        return 0;
    }
    return result;
}

int fts5652_write(struct Fts5652 *chip, uint8_t command, const uint8_t *data, size_t length)
{
    if (!chip || !chip->io.transfer || length > FTS5652_CHUNK || (length && !data)) {
        return -22;
    }
    size_t total = length ? 7 + length : 4;
    memset(chip->tx, 0, total);
    chip->tx[0] = command;
    chip->tx[2] = length >> 8;
    chip->tx[3] = length;
    if (length) {
        memcpy(chip->tx + 7, data, length);
    }
    return exchange(chip, total, 0, length);
}

static int fts5652_read(struct Fts5652 *chip, uint8_t command, uint8_t *data, size_t length)
{
    if (!chip || !data || !length || length > FTS5652_CHUNK) {
        return -22;
    }
    memset(chip->tx, 0, length + 9);
    chip->tx[0] = command;
    chip->tx[1] = 0xa0;
    chip->tx[2] = length >> 8;
    chip->tx[3] = length;
    int result = exchange(chip, length + 9, 1, length);
    if (!result) {
        memcpy(data, chip->rx + 7, length);
    }
    return result;
}

/* One register, for callers outside this file. The reader below is static here, and the probe's
 * health check needs it to inspect the power-mode and identity registers while the frame loop
 * runs. 0 = read. */
int fts5652_read_reg(struct Fts5652 *chip, uint8_t reg, uint8_t *value)
{
    if (!chip || !value) {
        return -22;
    }
    return fts5652_read(chip, reg, value, 1);
}

/* There is deliberately no sleep or wake helper here. Both would write 0xA5, and nothing on
 * this board documents what that register accepts: the board's own driver only ever writes
 * 0x03 (sleep) there, on suspend, and never on the path that brings the part up. The previous
 * pair of helpers wrote 0x03 and 0x00 and had no caller; keeping them was a hazard, because
 * the first display-sleep path wired up would have sent the part to sleep through a value we
 * cannot justify. Reading 0xA5 stays: it is what tells us whether the part is answering. */

static int supported_app(unsigned app_id)
{
    switch (app_id) {
    case 0x5652:
    case 0x565a:
    case 0x565b:
        return 1;
    default:
        return 0;
    }
}

/* Poll the two identity registers until they stop reading as an unpopulated bus.
 *
 * Measured 2026-10-27 on boot-usb56: every register, including the firmware version, read back
 * as 0xef immediately after reset was released, which is what a controller that has not finished
 * coming up looks like. The board's own touch driver allows a full second, polling every 200 ms
 * (reference/k40-kernel/drivers/input/touchscreen/focaltech_spi/focaltech_core.c:52-53), while
 * this probe read once after 9 ms. Poll here instead, and report how long the part took so the
 * number is on the console rather than guessed.
 *
 * A value of 0x00 or 0xff means the bus is floating; anything else is taken as the part talking,
 * because the two identity bytes of a real FTS part are never those. */
int fts5652_wait_ready(struct Fts5652 *chip, unsigned timeout_ms, unsigned *waited_ms)
{
    const unsigned step_ms = 20;
    unsigned waited = 0;
    while (waited <= timeout_ms) {
        uint8_t high = 0, low = 0;
        if (!fts5652_read(chip, 0xa3, &high, 1) && !fts5652_read(chip, 0x9f, &low, 1) &&
            high != 0x00 && high != 0xff && low != 0xef && (high != 0xef || low != 0xef)) {
            chip->app[0] = high;
            chip->app[1] = low;
            if (waited_ms) *waited_ms = waited;
            return 0;
        }
        chip->io.delay_us(chip->io.context, step_ms * 1000u);
        waited += step_ms;
    }
    if (waited_ms) *waited_ms = waited;
    return -1;
}

int fts5652_state(struct Fts5652 *chip, int probe_boot)
{
    uint8_t high = 0, low = 0;
    int app_result = fts5652_read(chip, 0xa3, &high, 1);
    if (!app_result) {
        app_result = fts5652_read(chip, 0x9f, &low, 1);
    }
    unsigned app = ((unsigned)high << 8) | low;
    stage(chip, "state-app", app, app_result);
    chip->app[0] = high;
    chip->app[1] = low;
    int rom_result = 0;
    if (probe_boot) {
        uint8_t rom[2] = {0};
        rom_result = fts5652_read(chip, 0x90, rom, sizeof(rom));
        stage(chip, "state-rom", ((unsigned)rom[0] << 8) | rom[1], rom_result);
        chip->rom[0] = rom[0];
        chip->rom[1] = rom[1];
    }
    int ver_result = fts5652_read(chip, 0xa6, &chip->version, 1);
    stage(chip, "state-version", chip->version, ver_result);
    /* How each identity read ended, so a refused app id names the value it got instead of
     * only "step 8". boot-usb55 reached the chip for the first time (rc=0 on every read) and
     * still refused, which can only be the app id. */
    chip->app_result = app_result;
    chip->rom_result = rom_result;
    chip->version_result = ver_result;
    if (!app_result && supported_app(app)) {
        return 0;
    }
    return app_result ? app_result : -19;
}

int fts5652_frame(struct Fts5652 *chip, unsigned *count, struct Fts5652Contact *contacts, unsigned capacity)
{
    *count = 0;
    if (!chip || !chip->io.transfer || !contacts || capacity < FTS5652_CONTACTS) {
        return -22;
    }
    // Frame read: command 1, 62 payload bytes, then a 2-byte CRC. exchange() sends whatever is
    // in chip->tx and nothing else, so the full four-byte header has to be built here: command,
    // ctrl (READ | CRC enable), payload length big-endian, then the three dummy bytes the part
    // inserts before its data. Without this the transfer went out carrying the previous
    // register read's command, so the panel never received a frame request and every read
    // failed its CRC (measured 2026-10-28 on boot-usb60: the probe started, TOUCHSTAT never
    // showed a frame).
    memset(chip->tx, 0, FTS5652_FRAME_DATA + 9);
    chip->tx[0] = 1;
    chip->tx[1] = 0xa0;
    chip->tx[2] = (uint8_t)(FTS5652_FRAME_DATA >> 8);
    chip->tx[3] = (uint8_t)FTS5652_FRAME_DATA;
    int result = exchange(chip, FTS5652_FRAME_DATA + 9, 1, FTS5652_FRAME_DATA);
    if (result) {
        return result;
    }
    // Payload begins after the four header bytes and the three dummy bytes. Layout, matching
    // the board's own touch driver: payload[0] status, payload[1] contact count in bits 0..3,
    // then six bytes per contact.
    const uint8_t *d = chip->rx + 7;
    /* A frame of all 0xFF is the part's "not ready" answer, not ten fingers. The board's own
     * driver detects point count 0xF with the following bytes all 0xFF and runs a recovery
     * sequence; publishing ours reported ten fingers at 32767,32767 a hundred and forty times
     * a second until the part woke up, which is what boot-usb61 showed in its first three
     * minutes. Publish no contacts for such a frame. */
    if (d[0] == 0xff && d[1] == 0xff) {
        return 0;
    }
    unsigned points = d[1] & 0x0f;
    if (points > FTS5652_CONTACTS) {
        points = FTS5652_CONTACTS;
    }
    unsigned out = 0;
    for (unsigned i = 0; i < points; ++i) {
        const unsigned base = i * 6;
        const uint8_t *r = d + 2 + base;
        /* Six bytes per contact. X and Y are not simple 16-bit big-endian words: the low three
         * bits of each live in the pressure byte, and the high four live in the X_H / Y_H
         * bytes, giving a 15-bit field (0..32767). event is X_H bits 7..6, id is Y_H bits
         * 7..4, area is the area byte's high nibble. Getting the packing wrong reports
         * coordinates an order of magnitude small and in the wrong place. */
        const unsigned pre = r[4];
        /* The vendor driver packs each axis into the full 16 bits (focaltech_core.c:712-720):
         * the high nibble of the pressure byte carries X's bits 7..4 and its low nibble Y's, the
         * low byte lands at bit 4, and the X_H / Y_H nibbles at bit 12. The firmware counts 16 per
         * unit and the device tree 10, so the result is scaled by 10/16.
         * The previous form shifted by 11 and 3 and took the low bits from PRE[7:5], which is
         * algebraically the vendor value halved: every touch landed in the top-left 80 % of the
         * panel, and the FromEdgeTip band at 97.6 % of the height could never be reached -- so a
         * swipe up from the bottom could not produce an edge gesture at all. */
        unsigned x = ((pre & 0xf0u) >> 4) + ((unsigned)r[1] << 4) + (((unsigned)(r[0] & 0x0fu)) << 12);
        unsigned y = (pre & 0x0fu) + ((unsigned)r[3] << 4) + (((unsigned)(r[2] & 0x0fu)) << 12);
        x = x * 10u / 16u;
        y = y * 10u / 16u;
        const int event = r[0] >> 6;
        const int id = r[2] >> 4;
        if (event == 0 && x == 0 && y == 0) {
            continue;
        }
        if (out < capacity) {
            contacts[out].id = id;
            contacts[out].x = (int)x;
            contacts[out].y = (int)y;
            contacts[out].event = event;
            contacts[out].pressure = pre & 0x03;
            contacts[out].area = r[5] >> 4;
            ++out;
        }
    }
    *count = out;
    return 0;
}