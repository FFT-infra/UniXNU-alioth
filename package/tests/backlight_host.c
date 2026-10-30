// Host test of BacklightCore.h. It checks the packet words and the level arithmetic against the
// rules the vendor kernel and this board's devicetree state, with no device involved. Build:
//
//   cc -std=c11 -Wall -Wextra -Werror -o backlight_host package/tests/backlight_host.c
//   ./backlight_host
//
// What is NOT covered here: whether the DSI controller accepts the packets, whether the panel
// answers DCS 0x52, and whether the level is visible. Those need the phone.
#include <assert.h>
#include <stdio.h>
#include "../BacklightCore.h"

int main(void) {
  // The level range is the panel node's: bl-min-level 1, bl-max-level 0x7ff.
  assert(BL_MIN_LEVEL == 1 && BL_MAX_LEVEL == 0x7ff);

  // The DCS long write for one payload word. The header bytes are the MIPI packet's in order
  // (dsi_ctrl_copy_and_pad_cmd), so the data type 0x39 is in bits 23:16 and the word count in
  // bits 7:0; flags bit 6 says a payload follows and bit 7 marks the last command. 0xC0390003 is
  // the word the device-verified asteroids port sends for the same message.
  assert(bl_long_write_header(2) == 0xC0390003u);
  assert((bl_long_write_header(2) >> 16) == 0xC039u);   // data type 0x39 in byte 2
  assert((bl_long_write_header(2) & 0xffu) == 3u);      // command byte + two payload bytes

  // The brightness payload: DCS 0x51, then the level big endian, then copy_and_pad_cmd's 0xff
  // pad byte (seven bytes round up to eight).
  assert(bl_brightness_payload(0x218) == 0xff180251u);
  assert(bl_brightness_payload(0x7ff) == 0xffff0751u);
  assert((bl_brightness_payload(0x218) & 0xffu) == BL_DCS_BRIGHTNESS);

  // High byte first, which is what mi,mdss-dsi-bl-dcs-big-endian-type selects. A little-endian
  // panel would put 0x18 in byte 1 and 0x02 in byte 2; this asserts the opposite.
  assert(((bl_brightness_payload(0x218) >> 8) & 0xffu) == 0x02u);
  assert(((bl_brightness_payload(0x218) >> 16) & 0xffu) == 0x18u);

  // A DCS short write with no parameter: the command byte is header byte 0, so 0x80050000 | cmd.
  assert(bl_short_write(BL_DCS_DISPLAY_ON) == 0x80050029u);
  assert(bl_short_write(BL_DCS_SLEEP_OUT) == 0x80050011u);
  assert(bl_short_write(BL_DCS_DISPLAY_OFF) == 0x80050028u);
  assert(bl_short_write(BL_DCS_SLEEP_IN) == 0x80050010u);

  // SET_MAXIMUM_RETURN_PACKET_SIZE is type 0x37, not 0x15. With the size 2 as its one parameter
  // the word is 0xC0370202: data type 0x37 in byte 2, the size's low byte in byte 1, and the
  // word count 2 (the command byte plus the parameter) in byte 0. The size's high byte is the
  // separate payload word, zero for any size below 256.
  assert((bl_max_return_header(2) >> 16) == 0xC037u);
  assert((bl_max_return_header(2) & 0xffu) == 2u);          // the command byte plus one parameter
  assert(((bl_max_return_header(2) >> 8) & 0xffu) == 2u);   // size low byte
  assert(bl_max_return_payload(2) == 0u);
  assert(bl_max_return_payload(0x102) == 1u);

  // The DCS read: type 0x06, the command in byte 0, and the BTA flag (bit 5 of the flags byte,
  // i.e. bit 29 of the word) so the panel turns the lane around. The command must come through
  // untouched: an earlier draft OR'd 0x20 into the command byte, turning 0x52 into 0x72.
  assert(bl_read_header(BL_DCS_BRIGHTNESS_GET) == 0xA0060052u);
  assert((bl_read_header(BL_DCS_BRIGHTNESS_GET) & 0xffu) == 0x52u);
  assert((bl_read_header(BL_DCS_BRIGHTNESS_GET) & BL_FLAG_BTA) == BL_FLAG_BTA);
  assert((bl_read_header(BL_DCS_BRIGHTNESS_GET) >> 16 & 0xffu) == 0x06u);

  // Level clamping: over the maximum becomes the maximum; a non-zero level under the minimum
  // becomes the minimum; zero stays zero because the driver reads it as display off.
  assert(bl_clamp_level(0x800) == BL_MAX_LEVEL);
  assert(bl_clamp_level(0xffffffffu) == BL_MAX_LEVEL);
  assert(bl_clamp_level(0) == 0);
  assert(bl_clamp_level(5) == 5);
  assert(bl_clamp_level(BL_MAX_LEVEL) == BL_MAX_LEVEL);
  assert(bl_clamp_level(BL_MIN_LEVEL) == BL_MIN_LEVEL);

  // The read-back: the payload's two bytes, high byte first. Decoding 0x0218 gives 0x218.
  //
  // This is NOT a round trip through bl_brightness_payload: a write payload word also carries
  // the command byte (0x51) and the pad byte (0xff), which a read response does not, so feeding
  // one into the other would compare two different packets. The read response's own layout
  // (where in RDBK_DATA0..3 the two bytes land, and whether a header precedes them) is the one
  // thing here that could not be checked against the phone, because the board stopped before
  // the probe ran. It is written from get_cmd_read_data (dsi_ctrl_hw_cmn.c:828-860) and is the
  // first thing round 1's DCS 0x52 read would confirm.
  assert(bl_decode_brightness(0x00000218u) == 0x218u);
  assert(bl_decode_brightness(0x000007ffu) == 0x7ffu);
  assert(bl_decode_brightness(0x00000000u) == 0u);
  assert(bl_decode_brightness(0x00000200u) == 0x200u);   // high byte only
  assert(bl_decode_brightness(0x00000018u) == 0x018u);   // low byte only

  puts("backlight_host: all assertions passed");
  return 0;
}
