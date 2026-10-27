// SPDX-License-Identifier: BSD-2-Clause-Patent
// Kernel side of the RPMh vote: the four hooks AliothRpmh.c calls, plus the console line.
// Kept in its own file so the vote itself stays free of kernel headers and the host test can
// compile it unchanged.
#include "AliothRpmh.h"
#include <machine/machine_routines.h>
#include <IOKit/IOLib.h>

// ml_io_map hands out permanent io_submap mappings that ml_io_unmap cannot free, so nothing
// here unmaps: the windows live for the boot, which is what a one-shot vote needs.
// C linkage: the kernel exports ml_io_map unmangled, and this file is C++.
extern "C" vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern "C" size_t qcom_usb_debug_write(const void *, size_t) __attribute__((weak_import));

uintptr_t rh_port_map(uintptr_t physical, size_t bytes) {
    return (uintptr_t)ml_io_map((vm_offset_t)physical, (vm_size_t)bytes);
}

uint32_t rh_port_read(uintptr_t base, uint32_t offset) {
    return *(volatile uint32_t *)(base + offset);
}

void rh_port_write(uintptr_t base, uint32_t offset, uint32_t value) {
    *(volatile uint32_t *)(base + offset) = value;
}

// Spin, do not IODelay. Measured on this board: IODelay(1) costs a full timer tick, about
// 830 us, because delay_for_interval only spins when the interval is below the CPU's idle
// latency and this board's device tree carries no powergate-latency, so that latency is 0.
// The vote polls in loops bounded by 1000 and 100000 iterations, which at a tick each becomes
// 0.8 s and 83 s; boot-usb47 stopped after the second rail's vote for exactly this reason.
// A busy wait is what the register protocol wants here, and the whole vote still finishes in
// well under a millisecond of real time.
void rh_port_delay_us(unsigned microseconds) {
    uint64_t frequency = 0, start = 0, now = 0;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frequency));
    __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(start));
    const uint64_t ticks = (frequency ? frequency : 19200000) * microseconds / 1000000 + 1;
    do { __asm__ volatile("isb\n\tmrs %0, cntpct_el0" : "=r"(now)); } while (now - start < ticks);
}

// The console, when the board links it. The vote runs before the display is up, so this is
// the only channel that carries its refusals off the device.
void rh_port_log(const char *text) {
    if (!text || !text[0] || !&qcom_usb_debug_write) return;
    size_t n = 0;
    while (text[n]) ++n;
    char line[224];
    size_t at = 0;
    for (; at < n && at + 2 < sizeof(line); ++at) line[at] = text[at];
    line[at++] = '\n';
    qcom_usb_debug_write(line, at);
}
