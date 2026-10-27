// SPDX-License-Identifier: BSD-2-Clause-Patent
// FocalTech FTS5652 (chip id 0x5652) SPI touch protocol, K40 (alioth, SM8250) panel.
// Wire format follows the FocalTech FTS SPI driver of the alioth Android kernel
// (read command 0x01, ctrl 0xA0, Kermit CRC16 trailer, 3-byte dummy) as verified
// on the device: IC ID 0x5652, 10 contacts, 12-bit coordinates.
#ifndef FOCAL_TOUCH_PROTOCOL_H
#define FOCAL_TOUCH_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

// The implementation is C and the only consumer is C++ (FocalTouchProbe.cpp). Without this
// guard the consumer mangles these names and the link reports the C definitions as undefined.
#ifdef __cplusplus
extern "C" {
#endif

#define FTS5652_CONTACTS 10
#define FTS5652_CHUNK 4096
// Frame read: 62 data bytes (2 header + 10 contacts * 6 bytes) + 1 command byte.
#define FTS5652_FRAME_DATA 62

// Coordinate axes of the published finger set (0.1 mm units, K40 1080x2400 panel).
// The HID layer scales these to its own 0..32767 range.
// The panel's own coordinate range, from this board's device tree
// (alioth.dts focaltech@0: focaltech,display-coords = <0 0 0x21c0 0x4b00>).
// The previous values (30480 x 20320) are not in the device tree and do not match the
// contact range the part reports.
#define FTS5652_AXIS_X 8640
#define FTS5652_AXIS_Y 19200
// The coordinate space the HID layer expects (NativeHidInput::touchSend transforms from here
// into the report's 0..32767). The sheng touch core's own axis maxima.
#define FTS5652_HID_AXIS_X 30480
#define FTS5652_HID_AXIS_Y 20320

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
    // What a refused CRC compared, and the bytes around it. A wrong expected value means the
    // command never reached the chip; matching-but-different data means the chip answered
    // something else.
    uint16_t last_crc_expected, last_crc_received;
    uint8_t last_header[4];
    uint8_t last_data0, last_data1, last_data2;
    // How each identity read ended: 0 answered.
    int app_result, rom_result, version_result;
};

// Read one register. 0 = read, and *value holds the byte.
int fts5652_read_reg(struct Fts5652 *chip, uint8_t reg, uint8_t *value);

// Poll the identity registers until the part answers, up to timeout_ms. 0 when it did, and
// *waited_ms receives how long that took.
int fts5652_wait_ready(struct Fts5652 *chip, unsigned timeout_ms, unsigned *waited_ms);

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

// No sleep or wake helper is exported: both would write the power-mode register 0xA5, and
// this board has no documentation for the values it accepts. Use reset for state recovery.

#ifdef __cplusplus
}
#endif

#endif
