// SPDX-License-Identifier: BSD-2-Clause-Patent
// The packet and level arithmetic for this board's panel brightness, kept out of the driver so a
// host test can check it against the vendor kernel's own rules without a device.
//
// Every constant here is derived in BacklightProbe.cpp's header comment; this file only holds
// the arithmetic, with no MMIO and no IOKit, so package/tests/backlight_host.c can compile it
// with a plain host compiler.
//
// The panel is qcom,mdss_dsi_k11a_38_08_0a_dsc_cmd: dsi_cmd_mode, bl_ctrl_dcs, bl-min-level 1,
// bl-max-level 0x7ff, mi,mdss-dsi-bl-dcs-big-endian-type (uefi-alioth/Resources/DTBs/alioth.dts
// 27436-27489). Its brightness is DCS 0x51 with two bytes, high byte first.
#ifndef BACKLIGHT_CORE_H
#define BACKLIGHT_CORE_H
#include <stdint.h>

enum {
  BL_MAX_LEVEL = 0x7ff,      // qcom,mdss-dsi-bl-max-level
  BL_MIN_LEVEL = 1,          // qcom,mdss-dsi-bl-min-level
  BL_DEFAULT_LEVEL = 0x218,  // qcom,mdss-brightness-init-level
  BL_DCS_BRIGHTNESS = 0x51, BL_DCS_BRIGHTNESS_GET = 0x52,
  BL_DCS_DISPLAY_ON = 0x29, BL_DCS_DISPLAY_OFF = 0x28,
  BL_DCS_SLEEP_OUT = 0x11, BL_DCS_SLEEP_IN = 0x10,
};

// The four header bytes of a MIPI packet, in order, as dsi_ctrl_copy_and_pad_cmd lays them out
// (techpack/display/msm/dsi/dsi_ctrl.c:1061-1103): byte 0 at bits 7:0, byte 1 at 15:8, byte 2 (the
// data type) at 23:16, byte 3 (the flags) at 31:24. Flags: bit 6 payload present, bit 7 last
// command, bit 5 turn the lane around for a read.
enum {BL_FLAG_PAYLOAD = 1u << 30, BL_FLAG_LAST = 1u << 31, BL_FLAG_BTA = 1u << 29};

// A DCS long write: the word count is payload plus the command byte, in header byte 0.
// 0x39 with one payload word gives 0xC0390003, the word asteroids' device-verified port sends.
static inline uint32_t bl_long_write_header(uint32_t payload_bytes) {
  return BL_FLAG_PAYLOAD | BL_FLAG_LAST | (0x39u << 16) | (payload_bytes + 1u);
}
// A DCS short write with no parameter: 0x80050000 | command. The command byte is header byte 0.
static inline uint32_t bl_short_write(uint8_t command) {
  return BL_FLAG_LAST | (0x05u << 16) | (uint32_t)command;
}
// SET_MAXIMUM_RETURN_PACKET_SIZE (0x37) with the size as its one parameter.
static inline uint32_t bl_max_return_header(uint32_t size) {
  return BL_FLAG_PAYLOAD | BL_FLAG_LAST | (0x37u << 16) | 2u | ((size & 0xffu) << 8);
}
static inline uint32_t bl_max_return_payload(uint32_t size) {return (size >> 8) & 0xffu;}
// A DCS read: 0xA0060000 | command, with the BTA flag so the panel turns the lane around.
static inline uint32_t bl_read_header(uint8_t command) {
  return BL_FLAG_BTA | BL_FLAG_LAST | (0x06u << 16) | (uint32_t)command;
}

// The payload of a DCS 0x51 long write, two bytes high byte first, with the pad byte the
// controller needs to reach a whole DWORD (7 bytes round up to 8).
static inline uint32_t bl_brightness_payload(uint32_t level) {
  return (uint32_t)BL_DCS_BRIGHTNESS | ((level >> 8) & 0xffu) << 8 | (level & 0xffu) << 16 | 0xffu << 24;
}

// A level the panel accepts: clamped to the declared maximum, and a non-zero level below the
// declared minimum becomes that minimum. Zero is kept as zero, because the driver treats it as
// display off rather than a DCS level the panel rejects.
static inline uint32_t bl_clamp_level(uint32_t level) {
  if (level > (uint32_t)BL_MAX_LEVEL) return BL_MAX_LEVEL;
  if (level && level < (uint32_t)BL_MIN_LEVEL) return BL_MIN_LEVEL;
  return level;
}

// The two bytes a DCS 0x52 read returns, big endian, from the word the controller latched.
// get_cmd_read_data reads RDBK_DATA highest word first and byte-swaps (dsi_ctrl_hw_cmn.c:828-860),
// so the caller passes the already-swapped word and this takes the payload from it.
static inline uint32_t bl_decode_brightness(uint32_t word) {
  return ((word >> 8) & 0xffu) << 8 | (word & 0xffu);
}
#endif
