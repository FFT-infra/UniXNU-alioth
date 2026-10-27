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
                result = -74;
                continue;
            }
            chip->last_crc_fail = 0;
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

int fts5652_sleep(struct Fts5652 *chip)
{
    const uint8_t mode = 3;
    return fts5652_write(chip, 0xa5, &mode, 1);
}

int fts5652_wake(struct Fts5652 *chip)
{
    const uint8_t mode = 0;
    return fts5652_write(chip, 0xa5, &mode, 1);
}

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
    if (probe_boot) {
        uint8_t rom[2] = {0};
        int rom_result = fts5652_read(chip, 0x90, rom, sizeof(rom));
        stage(chip, "state-rom", ((unsigned)rom[0] << 8) | rom[1], rom_result);
        chip->rom[0] = rom[0];
        chip->rom[1] = rom[1];
    }
    int ver_result = fts5652_read(chip, 0xa6, &chip->version, 1);
    stage(chip, "state-version", chip->version, ver_result);
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
    // Read command 1, 62 bytes of touch data (frame buffer is 2 header + 60
    // contact bytes); the Android driver reads the same layout.
    uint8_t buf[FTS5652_FRAME_DATA + 9];
    memset(buf, 0, sizeof(buf));
    buf[0] = 1;
    int result = exchange(chip, FTS5652_FRAME_DATA + 9, 1, FTS5652_FRAME_DATA);
    if (result) {
        return result;
    }
    // buf[1..] hold the 62 data bytes; the frame header is:
    //   data[0] = 0x01 status, data[1] = contact count (bits 0..3)
    const uint8_t *d = buf + 1;
    if (d[0] == 0xef && d[1] == 0xef && d[2] == 0xef) {
        return -74;                       // panel in boot ROM, no frame out yet
    }
    unsigned points = d[1] & 0x0f;
    if (points > FTS5652_CONTACTS) {
        points = FTS5652_CONTACTS;
    }
    unsigned out = 0;
    for (unsigned i = 0; i < points; ++i) {
        unsigned base = i * 6;
        uint8_t xh = d[2 + base];
        uint8_t xl = d[3 + base];
        uint8_t yh = d[4 + base];
        uint8_t yl = d[5 + base];
        unsigned x = ((unsigned)(xh & 0x0f) << 8) | xl;
        unsigned y = ((unsigned)(yh & 0x0f) << 8) | yl;
        // The Android driver masks IDs with xID_MASK.. event field is bits 6..7.
        int event = xh >> 6;
        int id = yh >> 4;
        if (event == 0 && x == 0 && y == 0) {
            continue;
        }
        if (out < capacity) {
            contacts[out].id = id;
            contacts[out].x = (int)x;
            contacts[out].y = (int)y;
            contacts[out].event = event;
            contacts[out].pressure = d[6 + base];
            contacts[out].area = d[7 + base] >> 4;
            ++out;
        }
    }
    *count = out;
    return 0;
}