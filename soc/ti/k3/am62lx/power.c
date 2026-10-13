/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AM62L SoC PM backend.
 *
 * TF-A and TIFS handle DDR self-refresh and the LPM sequence. Zephyr saves
 * the CPU0 context and calls PSCI SYSTEM_SUSPEND.
 *
 * With SMP, CPU1 must be off before CPU0 suspends. The application runs
 * pm_state_set(PM_STATE_SUSPEND_TO_RAM) on CPU1, which issues PSCI CPU_OFF.
 * The CPU0 invocation then polls PSCI AFFINITY_INFO to confirm CPU1 is off,
 * because TF-A skips the suspend on a CPU1 timeout yet still reports success
 * to Zephyr.
 *
 * After resume CPU1 is restarted with k_smp_cpu_start(), not
 * k_smp_cpu_resume(): CPU_OFF resets its GIC redistributor group registers
 * and only the secondary GIC init in k_smp_cpu_start() restores them. CPU0
 * needs no such step, TF-A saves and restores its GIC around SYSTEM_SUSPEND.
 */

#include <zephyr/kernel.h>
#include <zephyr/pm/pm.h>
#include <zephyr/arch/arm64/cpu.h>
#include <zephyr/arch/common/pm_s2ram.h>
#include <zephyr/drivers/pm_cpu_ops/psci.h>
#include <zephyr/drivers/pm_cpu_ops.h>
#include <zephyr/kernel/smp.h>

#ifdef CONFIG_SMP
#define CPU1_MPIDR					0x1ULL
#define PSCI_AFFINITY_LEVEL_OFF		1
#define CPU1_OFF_POLL_TIMEOUT_MS	100

/* Poll (up to 100 ms) until CPU1 reports AFFINITY_INFO OFF. */
static bool am62l_cpu1_is_off(void)
{
	for (int i = 0; i < CPU1_OFF_POLL_TIMEOUT_MS; i++) {
		if (psci_affinity_info(CPU1_MPIDR, 0) == PSCI_AFFINITY_LEVEL_OFF) {
			return true;
		}
		k_busy_wait(USEC_PER_MSEC);
	}

	return false;
}
#endif /* CONFIG_SMP */

static int am62l_system_off(void)
{
	return psci_system_suspend((uintptr_t)arch_pm_s2ram_resume);
}

void pm_state_set(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(substate_id);

	switch (state) {
	case PM_STATE_SUSPEND_TO_RAM:
#ifdef CONFIG_SMP
		if (arch_curr_cpu()->id != 0U) {
			unsigned int key = arch_irq_lock();

			arch_cpu_off_prepare();
			(void)pm_cpu_off();
			arch_irq_unlock(key);
			break;
		}

		if (!am62l_cpu1_is_off()) {
			printk("CPU1 did not shut down in time, skipping S2RAM\n");
			break;
		}
#endif
		(void)arch_pm_s2ram_suspend(am62l_system_off);
#ifdef CONFIG_SMP
		k_smp_cpu_start(1, NULL, NULL);
#endif
		break;

	case PM_STATE_SUSPEND_TO_IDLE:
	case PM_STATE_RUNTIME_IDLE:
		k_cpu_idle();
		break;

	default:
		break;
	}
}

void pm_state_exit_post_ops(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(state);
	ARG_UNUSED(substate_id);

	irq_unlock(0);
}
