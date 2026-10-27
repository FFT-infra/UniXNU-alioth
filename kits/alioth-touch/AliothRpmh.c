// SPDX-License-Identifier: BSD-2-Clause-Patent
// RPMh ACTIVE-only regulator vote, enough to power the touch controller.
//
// Why this file exists: the FTS5652 never answers on SPI until its supply rail is
// powered, and on this board nothing powers it at boot. Measured on the same phone
// with the ios-sweet port (ARCHIVE/ios-sweet/NOTES.md:29): "0x5d and 0x38 both time
// out -> touch IC unpowered (L11C LDO needs an RPMh vote)". Adding that vote is what
// made touch work there (NOTES.md:24, boot 0063u).
//
// The implementation is the one proven on this board:
// ARCHIVE/UniXNU/out/alioth/ios-sweet-0081/diag-markers-v30/src/bsd/dev/sweet_rpmh.c,
// whose ACTIVE vote completed in boot v36. It is reproduced here rather than linked
// because that tree is 16 KiB-page and this is 4 KiB, and because that file is
// behind ARM64_BOARD_CONFIG_SWEET.
//
// Scope: one ACTIVE TCS command set per call, no sleep/wake programming, no SPMI, no
// interrupt handler, no retry after an uncertain write. Every refusal prints the
// register state that decided it.
#include "AliothRpmh.h"

// Everything the kernel provides comes through the port, so the host test can drive
// this file against a software model with no kernel headers.
static void rh_fmt(char *out, size_t cap, const char *fmt, ...);

#define RH_RSC_PA       0x18220000u
#define RH_TCS_PA       0x18220d00u
#define RH_TCS_SIZE     0x3000u
#define RH_DICT_PA      0x0c3f000cu
#define RH_DRV          2u
#define RH_ACTIVE_N     2u       /* DT order: ACTIVE 2, SLEEP 3, WAKE 3 */
#define RH_NORMAL_N     8u       /* hidden CONTROL TCS is not in this bank */
#define RH_TCS_STRIDE   0x2a0u
#define RH_CMD_STRIDE   0x14u
#define RH_IRQ_EN       0x00u
#define RH_IRQ_STATUS   0x04u
#define RH_IRQ_CLEAR    0x08u
#define RH_WAIT         0x10u
#define RH_CONTROL      0x14u
#define RH_STATUS       0x18u
#define RH_ENABLE       0x1cu
#define RH_MSGID        0x30u
#define RH_ADDR         0x34u
#define RH_DATA         0x38u
#define RH_CMD_STATUS   0x3cu
#define RH_AMC_EN       (1u << 16)
#define RH_TRIGGER      (1u << 24)
#define RH_ISSUED       (1u << 8)
#define RH_COMPLETE     (1u << 16)
#define RH_WRITE_ACK    (8u | (1u << 8) | (1u << 16))
#define RH_DB_MAGIC     0x0c0330dbu
#define RH_DB_HEADER    144u
#define RH_DB_ENTRY     24u
#define RH_DB_MAX       0x100000u /* policy bound, not a DT size */
#define RH_VRM          4u
#define RH_TIMEOUT_US   100000u   /* iterations of a 1 us wait: a 100 ms budget */

// The vote reports errno-style codes. Spelled here because the kit is freestanding and the
// host test builds it without <errno.h>.
enum { RH_ENOMEM = 12, RH_EIO = 5, RH_EINVAL = 22, RH_ENODEV = 19, RH_EBUSY = 16, RH_ETIMEDOUT = 110 };
#define ENOMEM    RH_ENOMEM
#define EIO       RH_EIO
#define EINVAL    RH_EINVAL
#define ENODEV    RH_ENODEV
#define EBUSY     RH_EBUSY
#define ETIMEDOUT RH_ETIMEDOUT

#define RH_LOG(...) do { char _b[192]; rh_fmt(_b, sizeof(_b), __VA_ARGS__); rh_port_log(_b); } while (0)

// Live downstream DT has two positional reg entries, no reg-names.
// SPDX-License-Identifier: BSD-2-Clause-Patent
// RPMh ACTIVE-only regulator vote, enough to power the touch controller.
//
// Why this file exists: the FTS5652 never answers on SPI until its supply rail is
// powered, and on this board nothing powers it at boot. Measured on the same phone
// with the ios-sweet port (ARCHIVE/ios-sweet/NOTES.md:29): "0x5d and 0x38 both time
// out -> touch IC unpowered (L11C LDO needs an RPMh vote)". Adding that vote is what
// made touch work there (NOTES.md:24, boot 0063u).
//
// The implementation is the one proven on this board: ARCHIVE/UniXNU/out/alioth/
// ios-sweet-0081/diag-markers-v30/src/bsd/dev/sweet_rpmh.c, verified in boot v36.
// It is reproduced here rather than linked because the sweet tree is 16 KiB-page
// and this is 4 KiB, and because that file is behind ARM64_BOARD_CONFIG_SWEET.
//
// Scope: one ACTIVE TCS command set per call, no sleep/wake programming, no SPMI,
// no interrupt handler, no retry after an uncertain write. Every refusal prints the
// register state that decided it.
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

extern uint32_t rh_port_read(uintptr_t base, uint32_t off);
extern void rh_port_write(uintptr_t base, uint32_t off, uint32_t value);
extern void rh_port_delay_us(unsigned us);
extern void rh_port_log(const char *text);

#define RH_COMPLETE     (1u << 16)
#define RH_WRITE_ACK    (8u | (1u << 8) | (1u << 16))
#define RH_DB_MAGIC     0x0c0330dbu
#define RH_DB_HEADER    144u
#define RH_DB_ENTRY     24u
#define RH_DB_MAX       0x100000u /* policy bound, not a DT size */
#define RH_VRM          4u
#define RH_TIMEOUT_US   100000u   /* iterations of a 1 us wait: a 100 ms budget */

static uintptr_t rh_rsc, rh_tcs, rh_dict, rh_db;
static uint32_t rh_db_size;
static unsigned rh_ncpt;
static unsigned rh_lock;
static bool rh_poisoned;

static uint32_t
rh_read(uintptr_t base, uint32_t off)
{
	uint32_t v = *(volatile uint32_t *)(base + off);
	__asm__ volatile("dmb oshld" ::: "memory");
	return v;
}

static void
rh_write(uintptr_t base, uint32_t off, uint32_t value)
{
	__asm__ volatile("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(base + off) = value;
	__asm__ volatile("dsb osh" ::: "memory");
}

static bool
rh_sync(uint32_t off, uint32_t value)
{
	rh_write(rh_tcs, off, value);
	/* 100 ms of real polling. The original bound was 1000 iterations of a 1 us delay, which
	 * was meant as a millisecond; with the delay now a true busy wait that is too short for a
	 * readback that crosses the RPMh's own clock domain. */
	for (unsigned i = 0; i < 100000; ++i) {
		if (rh_read(rh_tcs, off) == value) {
			return true;
		}
		rh_port_delay_us(1);
	}
	RH_LOG("readback timeout off=%#x want=%#x got=%#x\n",
	    off, value, rh_read(rh_tcs, off));
	return false;
}

/* DB is little endian; avoid packed structs and unaligned Device loads. */
static uint16_t
rh_db16(uint32_t off)
{
	uint32_t v = rh_read(rh_db, off & ~3u);
	return (uint16_t)(v >> (8u * (off & 3u)));
}

static bool
rh_range(uint32_t off, uint32_t size)
{
	return off <= rh_db_size && size <= rh_db_size - off;
}

static int
rh_map(void)
{
	if (!rh_rsc) {
		rh_rsc = rh_port_map(RH_RSC_PA, 0x100);
	}
	if (!rh_tcs) {
		rh_tcs = rh_port_map(RH_TCS_PA, RH_TCS_SIZE);
	}
	if (!rh_dict) {
		rh_dict = rh_port_map(RH_DICT_PA, 8);
	}
	if (!rh_rsc || !rh_tcs || !rh_dict) {
		RH_LOG("RSC/locator mapping failed\n");
		return ENOMEM;
	}
	uint32_t id = rh_read(rh_rsc, 0);
	uint32_t solver = rh_read(rh_rsc, 4);
	uint32_t config = rh_read(rh_rsc, 0x0c);
	unsigned major = (id >> 16) & 0xffu;
	unsigned count = (config >> (6u * RH_DRV)) & 0x3fu;
	rh_ncpt = (config >> 27) & 0x1fu;
	RH_LOG("rsc=%#x tcs=%#x DRV=%u id=%#x solver=%#x config=%#x TCS=%u ncpt=%u\n",
	    RH_RSC_PA, RH_TCS_PA, RH_DRV, id, solver, config, count, rh_ncpt);
	/* This client implements the pre-v3 layout only; do not guess v3 offsets. */
	if ((major != 1 && major != 2) || config == 0xffffffffu ||
	    solver == 0xffffffffu || count < RH_NORMAL_N ||
	    count > RH_TCS_SIZE / RH_TCS_STRIDE || rh_ncpt < 3 || rh_ncpt > 16) {
		RH_LOG("unsupported RSC geometry/revision\n");
		return ENODEV;
	}
	if (!rh_db) {
		uint32_t pa = rh_read(rh_dict, 0);
		uint32_t size = rh_read(rh_dict, 4);
		RH_LOG("cmd-db locator=%#x pa=%#x size=%#x\n", RH_DICT_PA, pa, size);
		/* Firmware supplies the reserved RAM address; never scan physical RAM. */
		if (pa < 0x80000000u || (pa & 3u) || (size & 3u) ||
		    size < RH_DB_HEADER || size > RH_DB_MAX ||
		    (uint64_t)pa + size > 0x100000000ULL) {
			RH_LOG("invalid cmd-db locator\n");
			return EINVAL;
		}
		rh_db = rh_port_map(pa, size);
		if (!rh_db) {
			return ENOMEM;
		}
		rh_db_size = size;
	}
	uint32_t db_version = rh_read(rh_db, 0);
	uint32_t magic = rh_read(rh_db, 4);
	uint32_t flags = rh_read(rh_db, 140);
	RH_LOG("cmd-db version=%#x magic=%#x checksum=%#x flags=%#x\n",
	    db_version, magic, rh_read(rh_db, 136), flags);
	/* Linux only checks magic; version 1 is an intentionally stricter policy. */
	if (magic != RH_DB_MAGIC || db_version != 1 || (flags & 1u)) {
		RH_LOG("unsupported/standalone cmd-db\n");
		return EINVAL;
	}
	return 0;
}

static int
rh_lookup(const char *res, uint32_t *address)
{
	uint32_t key[2] = {0, 0};
	for (unsigned i = 0; i < 8 && res[i]; ++i) {
		key[i / 4] |= (uint32_t)(uint8_t)res[i] << (8u * (i % 4));
	}
	unsigned matches = 0;
	for (unsigned i = 0; i < 8; ++i) {
		uint32_t h = 8u + 16u * i;
		unsigned slave = rh_db16(h);
		if (!slave) {
			break;
		}
		uint32_t entries = RH_DB_HEADER + rh_db16(h + 2);
		uint32_t aux = RH_DB_HEADER + rh_db16(h + 4);
		unsigned count = rh_db16(h + 6);
		if (slave < 3 || slave > 5 || (entries & 3u) ||
		    !rh_range(entries, count * RH_DB_ENTRY) || !rh_range(aux, 0)) {
			RH_LOG("malformed cmd-db slave header %u\n", i);
			return EINVAL;
		}
		for (unsigned j = 0; j < count; ++j) {
			uint32_t e = entries + RH_DB_ENTRY * j;
			if (rh_read(rh_db, e) != key[0] || rh_read(rh_db, e + 4) != key[1]) {
				continue;
			}
			uint32_t addr = rh_read(rh_db, e + 16);
			if (slave != RH_VRM || (addr >> 16) != RH_VRM || (addr & 15u) ||
			    !rh_range(aux + rh_db16(e + 22), rh_db16(e + 20))) {
				RH_LOG("invalid VRM entry for %s\n", res);
				return EINVAL;
			}
			*address = addr;
			++matches;
			RH_LOG("%s addr=%#x slave=%u aux-version=%#x priority=%#x:%#x\n",
			    res, addr, slave, rh_db16(h + 8),
			    rh_read(rh_db, e + 8), rh_read(rh_db, e + 12));
		}
	}
	if (matches != 1) {
		RH_LOG("%s lookup matches=%u; refusing\n", res, matches);
		return ENODEV;
	}
	return 0;
}

static bool
rh_free(unsigned t)
{
	uint32_t b = t * RH_TCS_STRIDE;
	/* Idle alone is insufficient: a sleep/wake payload can be armed and idle. */
	return rh_read(rh_tcs, b + RH_STATUS) != 0 &&
	    rh_read(rh_tcs, b + RH_STATUS) != 0xffffffffu &&
	    rh_read(rh_tcs, b + RH_CONTROL) == 0 &&
	    rh_read(rh_tcs, b + RH_ENABLE) == 0 &&
	    rh_read(rh_tcs, b + RH_WAIT) == 0 &&
	    !(rh_read(rh_tcs, RH_IRQ_STATUS) & (1u << t));
}

static int
rh_send(uint32_t address, unsigned mv)
{
	int chosen = -1;
	for (unsigned t = 0; t < RH_ACTIVE_N; ++t) {
		uint32_t b = t * RH_TCS_STRIDE;
		uint32_t enabled = rh_read(rh_tcs, b + RH_ENABLE);
		RH_LOG("ACTIVE TCS%u idle=%#x control=%#x enable=%#x wait=%#x\n",
		    t, rh_read(rh_tcs, b + RH_STATUS), rh_read(rh_tcs, b + RH_CONTROL),
		    enabled, rh_read(rh_tcs, b + RH_WAIT));
		if (enabled & ~((1u << rh_ncpt) - 1u)) {
			return EIO;
		}
		for (unsigned c = 0; c < rh_ncpt; ++c) {
			if ((enabled & (1u << c)) &&
			    (rh_read(rh_tcs, b + RH_ADDR + c * RH_CMD_STRIDE) & ~15u) == address) {
				RH_LOG("conflicting inherited L11C request in TCS%u\n", t);
				return EBUSY;
			}
		}
		if (chosen < 0 && rh_free(t)) {
			chosen = (int)t;
		}
	}
	if (chosen < 0) {
		RH_LOG("no empty idle ACTIVE TCS; leaving inherited state intact\n");
		return EBUSY;
	}
	unsigned t = (unsigned)chosen;
	uint32_t b = t * RH_TCS_STRIDE, bit = 1u << t;
	uint32_t irq_en = rh_read(rh_tcs, RH_IRQ_EN);
	if (irq_en == 0xffffffffu || !rh_free(t)) {
		return EBUSY;
	}
	/* Only this TCS's source is masked. Other sources/CPU IRQs remain intact. */
	if (!rh_sync(RH_IRQ_EN, irq_en & ~bit)) {
		rh_poisoned = true;
		return EIO;
	}
	if (!rh_free(t)) {
		/* No payload was touched. Do not acknowledge somebody else's IRQ. */
		if (!rh_sync(RH_IRQ_EN, (rh_read(rh_tcs, RH_IRQ_EN) & ~bit) | (irq_en & bit))) {
			rh_poisoned = true;
		}
		return EBUSY;
	}
	/* From here an uncertain write/timeout quarantines our slot for this boot. */
	rh_poisoned = true;
	/* HPM (raw 7) is supported by this PMIC5 LDO; enable comes last. */
	const uint32_t offsets[3] = {0, 8, 4};
	const uint32_t values[3] = {mv, 7, 1};
	RH_LOG("TCS%u ACTIVE %u mV mode=7 enable=1 irq-enable=%#x\n", t, mv, irq_en);
	for (unsigned c = 0; c < 3; ++c) {
		uint32_t cmd = b + c * RH_CMD_STRIDE;
		if (!rh_sync(cmd + RH_MSGID, RH_WRITE_ACK) ||
		    !rh_sync(cmd + RH_ADDR, address + offsets[c]) ||
		    !rh_sync(cmd + RH_DATA, values[c])) {
			return EIO;
		}
	}
	if (!rh_sync(b + RH_WAIT, 7) || !rh_sync(b + RH_ENABLE, 7) ||
	    !rh_sync(b + RH_CONTROL, 0) || !rh_sync(b + RH_CONTROL, RH_AMC_EN)) {
		return EIO;
	}
	rh_write(rh_tcs, b + RH_CONTROL, RH_AMC_EN | RH_TRIGGER);
	uint32_t irq = 0, idle = 0, status[3] = {0, 0, 0};
	bool done = false, progress = false;
	for (unsigned us = 0; us < RH_TIMEOUT_US; ++us) {
		irq = rh_read(rh_tcs, RH_IRQ_STATUS);
		idle = rh_read(rh_tcs, b + RH_STATUS);
		bool complete = true;
		for (unsigned c = 0; c < 3; ++c) {
			status[c] = rh_read(rh_tcs, b + RH_CMD_STATUS + c * RH_CMD_STRIDE);
			complete &= status[c] != 0xffffffffu &&
			    (status[c] & (RH_ISSUED | RH_COMPLETE)) == (RH_ISSUED | RH_COMPLETE);
		}
		/* A fresh IRQ or observed execution transition excludes stale CMD_STATUS. */
		progress |= idle == 0 || !complete;
		if (idle && idle != 0xffffffffu && irq != 0xffffffffu &&
		    ((irq & bit) || progress) && complete) {
			done = true;
			break;
		}
		rh_port_delay_us(1);
	}
	RH_LOG("TCS%u done=%d irq=%#x idle=%#x control=%#x cmd-status=%#x/%#x/%#x\n",
	    t, done, irq, idle, rh_read(rh_tcs, b + RH_CONTROL), status[0], status[1], status[2]);
	if (!done) {
		RH_LOG("timeout; TCS quarantined, IRQ remains masked; no reset/retry\n");
		return ETIMEDOUT;
	}
	/* Proven completion: clear trigger before mode, then free only our commands. */
	if (!rh_sync(b + RH_CONTROL, RH_AMC_EN) || !rh_sync(b + RH_CONTROL, 0) ||
	    !rh_sync(b + RH_ENABLE, 0) || !rh_sync(b + RH_WAIT, 0)) {
		return EIO;
	}
	rh_write(rh_tcs, RH_IRQ_CLEAR, bit); /* W1C: never clear other TCS bits */
	if ((rh_read(rh_tcs, RH_IRQ_STATUS) & bit) ||
	    !rh_sync(RH_IRQ_EN, (rh_read(rh_tcs, RH_IRQ_EN) & ~bit) | (irq_en & bit))) {
		return EIO;
	}
	rh_poisoned = false;
	RH_LOG("L11C ACTIVE vote complete (rail voltage requires hardware verification)\n");
	return 0;
}

/*
 * The two rails this board's devices are on. A name not on this list is refused
 * rather than given PMIC5-LDO policy it may not want: the touch screen is on
 * ldoc10 (3.0 V) and ldoc1 (1.8 V), the panel's I/O on ldoc11 (3.2 V).
 */
static const struct {
	const char *name;
	unsigned min_mv, max_mv;
} rh_board_rails[] = {
	{ "ldoc11", 2950, 3400 },
	{ "ldoc10", 2950, 3400 },
	{ "ldoc1",  1700, 1900 },
};

int
sweet_rpmh_vreg_enable(const char *res, unsigned mv)
{
	bool known = false;

	for (unsigned i = 0; i < sizeof(rh_board_rails) / sizeof(rh_board_rails[0]); ++i) {
		if (res && strcmp(res, rh_board_rails[i].name) == 0) {
			if (mv < rh_board_rails[i].min_mv || mv > rh_board_rails[i].max_mv) {
				RH_LOG("%s outside %u..%u mV\n", res,
				    rh_board_rails[i].min_mv, rh_board_rails[i].max_mv);
				return EINVAL;
			}
			known = true;
			break;
		}
	}
	/* Board-specific helper: do not apply PMIC5-LDO policy to arbitrary resources. */
	if (!known) {
		RH_LOG("%s is not a rail this board declares\n", res ? res : "(null)");
		return EINVAL;
	}
	if (__atomic_exchange_n(&rh_lock, 1, __ATOMIC_ACQUIRE)) {
		return EBUSY;
	}
	int error = EIO;
	uint32_t address = 0;
	if (rh_poisoned) {
		RH_LOG("previous uncertain transaction; refusing further access\n");
		goto out;
	}
	error = rh_map();
	if (!error) {
		error = rh_lookup(res, &address);
	}
	if (!error) {
		error = rh_send(address, mv);
	}
out:
	if (error) {
		RH_LOG("enable failed errno=%d\n", error);
	}
	__atomic_store_n(&rh_lock, 0, __ATOMIC_RELEASE);
	return error;
}


// Minimal formatter: the calls below use only %s, %u, %d, %x and %#x. Written here
// because the kit is compiled freestanding and kernel printf is not available to it.
static void rh_fmt(char *out, size_t cap, const char *fmt, ...)
{
    __builtin_va_list ap;
    __builtin_va_start(ap, fmt);
    size_t at = 0;
    for (const char *p = fmt; *p && at + 1 < cap; ++p) {
        if (*p != '%') { out[at++] = *p; continue; }
        ++p;
        bool alt = false;
        if (*p == '#') { alt = true; ++p; }
        if (*p == 's') {
            const char *v = __builtin_va_arg(ap, const char *);
            if (!v) v = "(null)";
            for (; *v && at + 1 < cap; ++v) out[at++] = *v;
        } else if (*p == 'd') {
            long v = __builtin_va_arg(ap, int);
            char t[24]; int n = 0;
            if (v < 0) { if (at + 1 < cap) out[at++] = '-'; v = -v; }
            do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
            while (n && at + 1 < cap) out[at++] = t[--n];
        } else if (*p == 'u' || *p == 'x') {
            unsigned long v = __builtin_va_arg(ap, unsigned int);
            const char *d = (*p == 'x') ? "0123456789abcdef" : "0123456789";
            unsigned base = (*p == 'x') ? 16u : 10u;
            if (alt && *p == 'x') {
                if (at + 2 < cap) { out[at++] = '0'; out[at++] = 'x'; }
            }
            char t[24]; int n = 0;
            do { t[n++] = d[v % base]; v /= base; } while (v);
            while (n && at + 1 < cap) out[at++] = t[--n];
        } else {
            if (at + 1 < cap) out[at++] = *p;
        }
    }
    out[at] = 0;
    __builtin_va_end(ap);
}
