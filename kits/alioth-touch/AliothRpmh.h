// SPDX-License-Identifier: BSD-2-Clause-Patent
// Board hooks the RPMh vote needs. The vote itself (AliothRpmh.c) is pure register
// code so the host test can drive it against a software model.
#ifndef ALIOTH_RPMH_H
#define ALIOTH_RPMH_H
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Map a physical window. 0 = failure, matching the kernel's ml_io_map contract.
uintptr_t rh_port_map(uintptr_t physical, size_t bytes);
uint32_t rh_port_read(uintptr_t base, uint32_t offset);
void rh_port_write(uintptr_t base, uint32_t offset, uint32_t value);
// Spin for at least this long. Called only inside bounded retry loops.
void rh_port_delay_us(unsigned microseconds);
// One finished line, no newline required.
void rh_port_log(const char *text);

// Enables one rail through an ACTIVE TCS and waits for the command to complete.
// 0 = voted. A non-zero result leaves the rail as it was and prints why.
int sweet_rpmh_vreg_enable(const char *resource, unsigned millivolts);

#ifdef __cplusplus
}
#endif
#endif
