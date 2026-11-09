/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PONG_ATH11K_OF_IRQ_H
#define PONG_ATH11K_OF_IRQ_H
#include "lx_pci.h"
/* The PCI path obtains MSI vectors from nw_board; no Linux OF IRQ domain. */
static inline int of_irq_count(struct device_node *node) { (void)node; return 0; }
static inline int of_irq_get(struct device_node *node, int i) { (void)node; (void)i; return -ENODEV; }
#endif
