/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * Public API exposed by power.c for the S2RAM-with-SMP handshake.
 *
 * Only CONFIG_SMP builds define cpu1_pm_sem/cpu1_pm_ctrl_restart - a
 * single-core AM62L build has nothing to signal here, see power.c.
 */

#ifndef ZEPHYR_SOC_TI_K3_AM62LX_POWER_H_
#define ZEPHYR_SOC_TI_K3_AM62LX_POWER_H_

#include <zephyr/kernel.h>
#include <zephyr/pm/pm.h>

/* Forced S2RAM state shared by both CPUs. */
extern const struct pm_state_info s2ram_state;

#ifdef CONFIG_SMP
/* Give to wake CPU1's control thread and start its PSCI CPU_OFF sequence. */
extern struct k_sem cpu1_pm_sem;

/*
 * Bounded, sleepable poll confirming CPU1 reached PSCI AFF_STATE_OFF.
 * Must be called from a sleepable context (not from pm_state_set()).
 */
bool am62l_wait_cpu1_off(void);

/* (Re)creates and starts the CPU1 control thread; see power.c. */
void cpu1_pm_ctrl_restart(void *arg);
#endif /* CONFIG_SMP */

#endif /* ZEPHYR_SOC_TI_K3_AM62LX_POWER_H_ */
