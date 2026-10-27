// SPDX-License-Identifier: BSD-2-Clause-Patent
// FocalTech FTS5652 (chip id 0x5652) SPI touch protocol, K40 (alioth, SM8250) panel.
// Wire format follows the FocalTech FTS SPI driver of the alioth Android kernel
// (read command 0x01, ctrl 0xA0, Kermit CRC16 trailer, 3-byte dummy) as verified
// on the device: IC ID 0x5652, 10 contacts, 12-bit coordinates.
#ifndef FOCAL_TOUCH_PROTOCOL_H
#define FOCAL_TOUCH_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define FTS5652_CONTACTS 10
#define FTS5652_CHUNK 4096
// Frame read: 62 data bytes (2 header + 10 contacts * 6 bytes) + 1 command byte.
#define FTS5652_FRAME_DATA 62

// Coordinate axes of the published finger set (0.1 mm units, K40 1080x2400 panel).
// The HID layer scales these to its own 0..32767 range.
#define FTS5652_AXIS_X 30480
#define FTS5652_AXIS_Y 20320

struct Fts5652IO {
    void *context;
    // One full-duplex SPI exchange of txlen bytes (tx valid, rx receives txlen bytes).
    int (*transfer)(void *context, const uint8_t *tx, uint8_t *rx, size_t len);
    void (*reset)(void *context, int asserted);   // touch panel reset line
    void (*delay_us)(void *context, unsigned us);
    void (*stage)(void *context, const char *name, unsigned value, int result);
};

struct Fts5652Contact {
    int id;           // 0..9 touch ID
    int x;            // 12-bit raw coordinate 0..4095 (axis X)
    int y;            // 12-bit raw coordinate 0..4095 (axis Y)
    int event;        // 0 down, 1 up, 2 move (FTS event field, bits 6..7 of the X high byte)
    int pressure;     // 0..255
    int area;         // 0..15
};

struct Fts5652 {
    struct Fts5652IO io;
    uint8_t tx[FTS5652_FRAME_DATA + 9];
    uint8_t rx[FTS5652_FRAME_DATA + 9];
    uint8_t rom[2];
    uint8_t app[2];
    uint8_t version;
    uint8_t last_status;
    int last_crc_fail;
};

// Identify the panel: reads the application IDs (0xA3/0x9F) and the ROM ID (0x90,
// only when probe_boot). Returns 0 and fills chip->app when the panel answers
// with a supported FocalTech ID (0x5652 family), -ENODEV when the panel is absent.
int fts5652_state(struct Fts5652 *chip, int probe_boot);

// Read one touch frame. On success *count is the number of live contacts and
// contacts[0..count-1] are filled with raw 12-bit coordinates. A frame with no
// contact yields count 0. Returns 0, or negative errno.
int fts5652_frame(struct Fts5652 *chip, unsigned *count, struct Fts5652Contact *contacts, unsigned capacity);

// Generic single-register write (used by the probe for wakeup/sleep control).
int fts5652_write(struct Fts5652 *chip, uint8_t command, const uint8_t *data, size_t length);

// Sleep (deep sleep mode 3) and wakeup (mode 0).
int fts5652_sleep(struct Fts5652 *chip);
int fts5652_wake(struct Fts5652 *chip);

#endif
