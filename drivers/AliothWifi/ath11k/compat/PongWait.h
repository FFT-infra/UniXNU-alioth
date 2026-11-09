/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef ZIZHAN_PONG_WIFI_WAIT_H
#define ZIZHAN_PONG_WIFI_WAIT_H
/* zizhan uses the cooperative dispatcher with polled IRQs. A busy pump
 * still consumes time: never make the deadline depend on becoming idle.
 * WMI send conditions transfer skb ownership and must run only once on
 * the successful iteration. Completion waits use the same bounded rule. */
#undef wait_event_timeout
#undef wait_event_timeout_once
#define wait_event_timeout_once(w, cond, timeout) ({ \
    unsigned long _pong_end = jiffies + (timeout); long _pong_result; (void)(w); \
    for (;;) { \
        if (cond) { \
            _pong_result = time_before(jiffies, _pong_end) ? (long)(_pong_end - jiffies) : 1; \
            break; \
        } \
        if (!time_before(jiffies, _pong_end)) { _pong_result = 0; break; } \
        if (!lx_pump()) lx_idle(1); \
    } \
    _pong_result; })
#define wait_event_timeout(w, cond, timeout) wait_event_timeout_once(w, cond, timeout)
#endif
