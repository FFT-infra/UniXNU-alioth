/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef PONG_LINUX_SUSPEND_H
#define PONG_LINUX_SUSPEND_H
#include "lx_os.h"
/* This XNU board profile has no Linux system-suspend notifier chain. Match
 * Linux's CONFIG_PM_SLEEP=n behavior; MHI power-up/down is still real. */
#define PM_HIBERNATION_PREPARE 0x0001
#define PM_POST_HIBERNATION 0x0002
#define PM_SUSPEND_PREPARE 0x0003
#define PM_POST_SUSPEND 0x0004
#define PM_RESTORE_PREPARE 0x0005
#define PM_POST_RESTORE 0x0006
#ifndef register_pm_notifier
#define register_pm_notifier(n) ((void)(n), 0)
#endif
#ifndef unregister_pm_notifier
#define unregister_pm_notifier(n) ((void)(n), 0)
#endif
#endif
