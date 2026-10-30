// Host test of AliothUsbEndpoint.h. It checks the endpoint-number assignment the Apple USB
// device Family asks for, against the rules AppleNubSynopsysOTGDevice implements and the working
// MSM8998 port agrees with. No device involved. Build:
//
//   cc -std=c11 -Wall -Wextra -Werror -o endpoint_host package/tests/alioth_usb_endpoint_host.c
//   ./endpoint_host
//
// What is NOT covered: whether the Family accepts these ids, whether the DWC3 core actually has
// 16 usable endpoints on this board, and whether the host opens the configuration. Those need
// the phone. What IS covered is the arithmetic that decides every one of those outcomes, because
// a number off by one is invisible until enumeration fails.
#include <assert.h>
#include <stdio.h>
#include "../AliothUsbEndpoint.h"

static alioth_usb_setting_summary setting(uint32_t pipes, uint32_t ins)
{
    alioth_usb_setting_summary s;
    s.pipe_count = pipes;
    s.in_count = ins;
    s.out_count = pipes - ins;
    return s;
}

int main(void)
{
    // The Family's ids array is indexed by physical endpoint; the DWC3 core exposes 32.
    // EP_COUNT = 6 in the CDC driver is that driver's own array size, not the hardware limit.
    assert(ALIOTH_USB_ENDPOINT_COUNT == 32);
    assert(ALIOTH_USB_MAX_PIPES_PER_SETTING == 30);

    // One interface, one setting, one IN and one OUT: the mux configuration this board targets.
    // IN gets 0x81 and OUT gets 0x01, the same pair the working MSM8998 port reports.
    {
        uint8_t ids[ALIOTH_USB_ENDPOINT_COUNT];
        alioth_usb_endpoint_cursor c;
        uint32_t used_in = 99, used_out = 99;
        alioth_usb_setting_summary s = setting(2, 1);
        alioth_usb_cursor_init(&c, ALIOTH_USB_ENDPOINT_COUNT);
        assert(alioth_usb_assign_setting(ids, &c, 1, 1, &s, &used_in, &used_out) == 0);
        assert(used_in == 1 && used_out == 1);
        assert(c.written == 2);
        assert(ids[0] == 0x81);          // IN, number 1, direction bit set
        assert(ids[1] == 0x01);          // OUT, number 1
    }

    // Two alternatives of the SAME interface may reuse the same numbers, and the interface then
    // advances by the larger of the two, not the sum. This is the rule that keeps a host from
    // seeing two different endpoints under one address.
    {
        uint8_t ids[ALIOTH_USB_ENDPOINT_COUNT];
        alioth_usb_endpoint_cursor c;
        uint32_t ui, uo;
        alioth_usb_setting_summary alt0 = setting(2, 1);
        alioth_usb_setting_summary alt1 = setting(4, 2);
        alioth_usb_cursor_init(&c, ALIOTH_USB_ENDPOINT_COUNT);
        assert(alioth_usb_assign_setting(ids, &c, 1, 1, &alt0, &ui, &uo) == 0);
        assert(alioth_usb_assign_setting(ids, &c, 1, 1, &alt1, &ui, &uo) == 0);
        assert(ui == 2 && uo == 2);      // the second alternative's usage
        assert(c.written == 6);
        assert(ids[2] == 0x81 && ids[3] == 0x82);   // IN 1, IN 2 in alt1
        assert(ids[4] == 0x01 && ids[5] == 0x02);   // OUT 1, OUT 2 in alt1
        alioth_usb_advance(&c, 2, 2);               // advance by the max, which is alt1's 2
        assert(c.first_in == 3 && c.first_out == 3);
    }

    // A second interface starts where the first left off, so the two never collide.
    {
        uint8_t ids[ALIOTH_USB_ENDPOINT_COUNT];
        alioth_usb_endpoint_cursor c;
        uint32_t ui, uo;
        alioth_usb_setting_summary s1 = setting(2, 1);
        alioth_usb_cursor_init(&c, ALIOTH_USB_ENDPOINT_COUNT);
        assert(alioth_usb_assign_setting(ids, &c, c.first_in, c.first_out, &s1, &ui, &uo) == 0);
        alioth_usb_advance(&c, ui, uo);
        assert(c.first_in == 2 && c.first_out == 2);
        assert(alioth_usb_assign_setting(ids, &c, c.first_in, c.first_out, &s1, &ui, &uo) == 0);
        assert(ids[2] == 0x82 && ids[3] == 0x02);
    }

    // Number 15 is the last usable one; number 16 must be refused, not wrapped.
    {
        uint8_t ids[ALIOTH_USB_ENDPOINT_COUNT];
        alioth_usb_endpoint_cursor c;
        uint32_t ui, uo;
        alioth_usb_setting_summary s = setting(2, 1);
        alioth_usb_cursor_init(&c, ALIOTH_USB_ENDPOINT_COUNT);
        assert(alioth_usb_assign_setting(ids, &c, 15, 15, &s, &ui, &uo) == 0);
        assert(ids[0] == 0x8f && ids[1] == 0x0f);
        alioth_usb_cursor_init(&c, ALIOTH_USB_ENDPOINT_COUNT);
        assert(alioth_usb_assign_setting(ids, &c, 16, 16, &s, &ui, &uo) != 0);
    }

    // A pipe count above the ceiling is refused, and so is a null ids array with pipes.
    {
        uint8_t ids[ALIOTH_USB_ENDPOINT_COUNT];
        alioth_usb_endpoint_cursor c;
        uint32_t ui, uo;
        alioth_usb_setting_summary big = setting(31, 1);
        alioth_usb_cursor_init(&c, ALIOTH_USB_ENDPOINT_COUNT);
        assert(alioth_usb_assign_setting(ids, &c, 1, 1, &big, &ui, &uo) != 0);
        alioth_usb_setting_summary two = setting(2, 1);
        assert(alioth_usb_assign_setting(NULL, &c, 1, 1, &two, &ui, &uo) != 0);
        assert(alioth_usb_assign_setting(ids, NULL, 1, 1, &two, &ui, &uo) != 0);
    }

    // in_count above pipe_count is inconsistent input, not a direction guess.
    {
        uint8_t ids[ALIOTH_USB_ENDPOINT_COUNT];
        alioth_usb_endpoint_cursor c;
        uint32_t ui, uo;
        alioth_usb_setting_summary bad = setting(2, 3);
        alioth_usb_cursor_init(&c, ALIOTH_USB_ENDPOINT_COUNT);
        assert(alioth_usb_assign_setting(ids, &c, 1, 1, &bad, &ui, &uo) != 0);
    }

    // A failed setting writes nothing, so a partial configuration is never published.
    // The failure here is the 4-bit ceiling, not the physical one: starting at 15 with two IN
    // pipes needs number 16, and 16 does not fit in a 4-bit endpoint number.
    {
        uint8_t ids[ALIOTH_USB_ENDPOINT_COUNT];
        alioth_usb_endpoint_cursor c;
        uint32_t ui, uo;
        alioth_usb_setting_summary ok = setting(2, 1);
        alioth_usb_setting_summary two_in = setting(2, 2);
        alioth_usb_cursor_init(&c, ALIOTH_USB_ENDPOINT_COUNT);
        assert(alioth_usb_assign_setting(ids, &c, 1, 1, &ok, &ui, &uo) == 0);
        const uint32_t before = c.written;
        assert(alioth_usb_assign_setting(ids, &c, 15, 15, &two_in, &ui, &uo) != 0);
        assert(c.written == before);
    }

    // A real configuration's pipe order is the descriptor's, not IN-first. The directions
    // variant must number them by their own direction, and it must refuse a value that is
    // neither 0 nor 1 rather than treating it as OUT.
    {
        uint8_t ids[ALIOTH_USB_ENDPOINT_COUNT];
        alioth_usb_endpoint_cursor c;
        uint32_t ui, uo;
        const uint8_t order[3] = { 1, 0, 0 };   // IN, OUT, OUT
        alioth_usb_cursor_init(&c, ALIOTH_USB_ENDPOINT_COUNT);
        assert(alioth_usb_assign_setting_directions(ids, &c, 1, 1, order, 3, &ui, &uo) == 0);
        assert(ui == 1 && uo == 2);
        assert(ids[0] == 0x81);                  // the IN pipe takes IN 1
        assert(ids[1] == 0x01 && ids[2] == 0x02); // the two OUT pipes take OUT 1 and 2
        const uint8_t bad[2] = { 1, 2 };          // 2 is neither OUT nor IN
        alioth_usb_cursor_init(&c, ALIOTH_USB_ENDPOINT_COUNT);
        assert(alioth_usb_assign_setting_directions(ids, &c, 1, 1, bad, 2, &ui, &uo) != 0);
        assert(c.written == 0);
    }

    // in_count + out_count must not exceed pipe_count; the summary is input, not a guess.
    {
        uint8_t ids[ALIOTH_USB_ENDPOINT_COUNT];
        alioth_usb_endpoint_cursor c;
        uint32_t ui, uo;
        alioth_usb_setting_summary s;
        s.pipe_count = 2; s.in_count = 1; s.out_count = 2;   // 3 > 2
        alioth_usb_cursor_init(&c, ALIOTH_USB_ENDPOINT_COUNT);
        assert(alioth_usb_assign_setting(ids, &c, 1, 1, &s, &ui, &uo) != 0);
    }

    // Alternatives APPEND: the ids array grows by each alternative's pipe count, and the same
    // number appearing under two alternatives is intended, because only one is active at a time.
    // Keeping the maximum instead of the sum would silently drop every alternative but the widest,
    // and the Family indexes ids by the pipe it is building, so the later ones would read as unset.
    {
        uint8_t ids[ALIOTH_USB_ENDPOINT_COUNT];
        alioth_usb_endpoint_cursor c;
        uint32_t ui, uo;
        const uint8_t a0[2] = { 1, 0 };          // alt0: IN 1, OUT 1
        const uint8_t a1[4] = { 1, 0, 1, 0 };    // alt1: IN 1, OUT 1, IN 2, OUT 2
        alioth_usb_cursor_init(&c, ALIOTH_USB_ENDPOINT_COUNT);
        assert(alioth_usb_assign_setting_directions(ids, &c, 1, 1, a0, 2, &ui, &uo) == 0);
        assert(c.written == 2);
        assert(alioth_usb_assign_setting_directions(ids, &c, 1, 1, a1, 4, &ui, &uo) == 0);
        assert(c.written == 6);                  // 2 + 4, not max(2,4)
        assert(ids[0] == 0x81 && ids[1] == 0x01);
        assert(ids[2] == 0x81 && ids[3] == 0x01);   // alt1 reuses IN 1 and OUT 1
        assert(ids[4] == 0x82 && ids[5] == 0x02);   // and adds IN 2 and OUT 2
    }

    printf("alioth_usb_endpoint_host: all checks passed\n");
    return 0;
}
