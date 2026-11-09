/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PONG_ATH11K_DMI_H
#define PONG_ATH11K_DMI_H
#include "lx_ath.h"
/* ARM Pong has no x86 DMI board table. Keep laptop-specific quirks unmatched. */
enum { DMI_NONE, DMI_BIOS_VENDOR, DMI_BIOS_VERSION, DMI_BOARD_VENDOR, DMI_BOARD_NAME,
       DMI_BOARD_VERSION, DMI_PRODUCT_NAME, DMI_PRODUCT_VERSION, DMI_SYS_VENDOR };
struct dmi_strmatch { int slot; const char *substr; };
struct dmi_system_id {
    int (*callback)(const struct dmi_system_id *);
    const char *ident;
    struct dmi_strmatch matches[4];
    void *driver_data;
};
#define DMI_MATCH(slot_, text_) { .slot = (slot_), .substr = (text_) }
#define DMI_EXACT_MATCH(slot_, text_) DMI_MATCH(slot_, text_)
static inline const struct dmi_system_id *dmi_first_match(const struct dmi_system_id *ids)
{ (void)ids; return NULL; }
#endif
