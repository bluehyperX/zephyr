// SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
// SPDX-License-Identifier: Apache-2.0

/*
 * AM62L SoC PM backend.
 *
 * TF-A handles DDR self-refresh and LPM HAL entirely (via TIFS).  Zephyr
 * saves CPU context and drives PSCI CPU_SUSPEND.
 *
 * S2RAM with SMP (CPU_OFF):
 *   1. CPU0 signals CPU1 via SGI3 to power itself off (PSCI CPU_OFF).
 *      TF-A sets aff_info_state = AFF_STATE_OFF on CPU_OFF, so
 *      psci_affinity_info() works correctly here.
 *   2. CPU0 polls psci_affinity_info until it returns non-zero (CPU1 off).
 *   3. CPU0 calls arch_pm_s2ram_suspend() -> psci_cpu_suspend(DEEPSLEEP).
 *   4. On resume CPU0 calls k_smp_cpu_start(1, ...) which reinitialises
 *      the idle thread ESF (z_init_cpu), then starts CPU1 via pm_cpu_on.
 *      arch_secondary_cpu_init reinits GIC and timer; scheduler follows.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/pm/pm.h>
#include <zephyr/arch/arm64/cpu.h>
#include <zephyr/arch/common/pm_s2ram.h>
#include <zephyr/drivers/pm_cpu_ops/psci.h>
#include <zephyr/drivers/pm_cpu_ops.h>
#include <zephyr/drivers/interrupt_controller/gic.h>
#include <zephyr/kernel/smp.h>

/* SGI3 is unallocated in smp.c (0=SCHED, 1=MMCFG, 2=FPU) */
#define SGI_S2RAM_IPI           3

/* CPU1 MPIDR: Aff0=1, all higher affinity levels 0 */
#define CPU1_MPIDR              0x1ULL

/*
 * PSCI AFFINITY_INFO raw return values (PSCI spec table 11).
 * psci_affinity_info() returns these directly (no psci_to_dev_err mapping).
 */
#define PSCI_AFF_STATE_ON       0
#define PSCI_AFF_STATE_OFF      1

#ifdef CONFIG_SMP
static atomic_t s2ram_cpu1_req;

static void am62l_s2ram_cpu1_ipi(const void *arg)
{
	ARG_UNUSED(arg);
	if (atomic_get(&s2ram_cpu1_req)) {
		/*
		 * Park _current so the scheduler can pick it up after resume.
		 * pm_cpu_off() bypasses z_arm64_context_switch (switch.S:99),
		 * the only path that sets switch_handle = thread.  Without this,
		 * _current->switch_handle stays NULL; on the next CPU1 boot,
		 * next_up() may return this (idle) thread and z_sched_switch_spin
		 * deadlocks.  callee_saved is valid from the last cooperative
		 * context switch and will be used to restart the idle loop.
		 */
		_current->switch_handle = _current;
		/*
		 * Power CPU1 off entirely.  TF-A sets aff_info_state = OFF
		 * on CPU_OFF, allowing CPU0 to use psci_affinity_info().
		 * This SMC never returns on success.
		 */
		(void)pm_cpu_off();
		/* Reached only on failure; disarm so CPU1 stays alive. */
		atomic_set(&s2ram_cpu1_req, 0);
	}
}

void soc_per_core_init_hook(void)
{
	/*
	 * On CPU0 this hook fires before GIC initialisation — skip.
	 * On CPU1 it fires after arm_gic_secondary_init() — safe to enable.
	 */
	if (arch_curr_cpu()->id != 0U) {
		irq_enable(SGI_S2RAM_IPI);
	}
}
#endif /* CONFIG_SMP */

static int am62l_system_off(void)
{
	uintptr_t resume_pa = (uintptr_t)arch_pm_s2ram_resume;

	return psci_system_suspend(resume_pa);
}

void pm_state_set(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(substate_id);

	switch (state) {
	case PM_STATE_SUSPEND_TO_RAM:
#ifdef CONFIG_SMP
		/*
		 * Signal CPU1 to power itself off (PSCI CPU_OFF).
		 * TF-A sets aff_info_state = AFF_STATE_OFF on CPU_OFF, so
		 * psci_affinity_info() returns non-zero once CPU1 is off.
		 */
		atomic_set(&s2ram_cpu1_req, 1);
		gic_raise_sgi(SGI_S2RAM_IPI, CPU1_MPIDR, 1U << 1);
		while (psci_affinity_info(CPU1_MPIDR, 0) == PSCI_AFF_STATE_ON) {
			k_busy_wait(100);
		}
#endif
		/*
		 * Save CPU0 context and call PSCI_SYSTEM_SUSPEND.
		 * Returns here after full resume and TURN_ON for all devices.
		 */
		arch_pm_s2ram_suspend(am62l_system_off);

#ifdef CONFIG_SMP
		/*
		 * Restart CPU1 via k_smp_cpu_start.  k_smp_cpu_resume would
		 * skip z_init_cpu(), leaving z_idle_threads[1].pInitCtx with a
		 * corrupted SPSR field: z_thread_entry's prologue (stp/str
		 * x23,[sp,#48]) overwrote it with 0 during the first boot.
		 * SPSR=0 causes z_arm64_exit_exc to ERET to EL0t, which
		 * immediately faults because SP_EL0 is 0.
		 *
		 * k_smp_cpu_start calls z_init_cpu(1) → init_idle_thread(1)
		 * → arch_new_thread, which writes a fresh pInitCtx ESF with
		 * SPSR = EL1h | DAIF before CPU1 is powered on.  It also sets
		 * reinit_timer=true and invoke_sched=true, matching the old
		 * k_smp_cpu_resume(1, NULL, NULL, true, true) behaviour.
		 *
		 * arch_cpu_start (smp.c resume path) detects cpu_map[1] !=
		 * INV_MPID and calls pm_cpu_on directly, so the cpu_node_list
		 * scan (which would k_panic on an exhausted iterator) is skipped.
		 */
		atomic_set(&s2ram_cpu1_req, 0);
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

#ifdef CONFIG_SMP
static int am62l_pm_init(void)
{
	IRQ_CONNECT(SGI_S2RAM_IPI, IRQ_DEFAULT_PRIORITY,
		    am62l_s2ram_cpu1_ipi, NULL, 0);
	return 0;
}
SYS_INIT(am62l_pm_init, PRE_KERNEL_2, 0);
#endif /* CONFIG_SMP */
