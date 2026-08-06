// SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
// SPDX-License-Identifier: Apache-2.0

/*
 * AM62L SoC PM backend.
 *
 * TF-A handles DDR self-refresh and LPM HAL entirely (via TIFS).  Zephyr
 * saves CPU context and drives PSCI CPU_SUSPEND.
 *
 * S2RAM with SMP (CPU_OFF, default):
 *   1. CPU0 signals CPU1 via SGI3 to power itself off (PSCI CPU_OFF).
 *      TF-A sets aff_info_state = AFF_STATE_OFF on CPU_OFF, so
 *      psci_affinity_info() works correctly here.
 *   2. CPU0 polls psci_affinity_info until it returns non-zero (CPU1 off).
 *   3. CPU0 calls arch_pm_s2ram_suspend() -> psci_cpu_suspend(DEEPSLEEP).
 *   4. On resume CPU0 calls k_smp_cpu_resume(1, ...) which restarts CPU1
 *      via pm_cpu_on, runs arch_secondary_cpu_init, reinits GIC and timer,
 *      and rejoins the scheduler.
 *
 * S2RAM with SMP (CPU_SUSPEND, CONFIG_SOC_AM62L_S2RAM_CPU1_SUSPEND=y):
 *   1. CPU1 enters psci_cpu_suspend(LPM_PSTATE_SEC) triggered by SGI3.
 *   2. CPU0 polls s2ram_cpu1_in_psci until CPU1 sets it (at PSCI SMC).
 *      psci_affinity_info cannot be used: TF-A only sets aff_info_state=OFF
 *      for CPU_OFF, not CPU_SUSPEND, so it always returns AFF_STATE_ON.
 *   3. CPU0 calls arch_pm_s2ram_suspend() -> psci_cpu_suspend(LPM_PSTATE_RTC_DDR).
 *   4. On resume TF-A powers CPU1 back on; CPU1's pwr_domain_suspend_finish
 *      waits in EL3 WFI until it receives a NS G1 interrupt.
 *   5. CPU0 sends SGI3 to CPU1.  TF-A's WFI wakes, ERets CPU1 to
 *      am62l_cpu1_resume which restores context and returns from step 1.
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

/*
 * AM62L TF-A PSCI power state for S2RAM (deep sleep).
 * 0x2012235 = LPM_PSTATE_DEEPSLEEP → TI_K3_SLEEP_MODE_DEEP_SLEEP (mode 0).
 *
 * Zephyr passes this pstate and TF-A selects the actual sleep mode via
 * am62l_pwr_domain_suspend().  Both DEEP_SLEEP and RTC_PLUS_DDR put DDR
 * in self-refresh; the difference is whether the PMIC is cut (RTC_PLUS_DDR)
 * or not (DEEP_SLEEP).  With the current TF-A configuration, mode=0
 * (DEEP_SLEEP) is selected.
 */
#define LPM_PSTATE_DEEPSLEEP    0x2012235U

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

#if IS_ENABLED(CONFIG_SOC_AM62L_S2RAM_CPU1_SUSPEND)
/*
 * Set by CPU1's IPI handler just before calling psci_cpu_suspend.
 * CPU0 polls this instead of psci_affinity_info: TF-A never transitions
 * aff_info_state to OFF for CPU_SUSPEND (only for CPU_OFF), so
 * psci_affinity_info always returns AFF_STATE_ON for a suspended core.
 */
static atomic_t s2ram_cpu1_in_psci;

/*
 * _cpu1_context is in __noinit so it survives DDR self-refresh.
 * Shared with power_smp.S via GDATA(_cpu1_context).
 */
__noinit _cpu_context_t _cpu1_context;

extern int am62l_cpu1_s2ram_suspend(void);
extern void am62l_cpu1_resume(void);
#endif /* CONFIG_SOC_AM62L_S2RAM_CPU1_SUSPEND */

static void am62l_s2ram_cpu1_ipi(const void *arg)
{
	ARG_UNUSED(arg);
	if (atomic_get(&s2ram_cpu1_req)) {
#if IS_ENABLED(CONFIG_SOC_AM62L_S2RAM_CPU1_SUSPEND)
		/* Signal CPU0 that CPU1 is about to enter PSCI suspend. */
		atomic_set(&s2ram_cpu1_in_psci, 1);
		am62l_cpu1_s2ram_suspend();
		atomic_set(&s2ram_cpu1_in_psci, 0);
#else
		/*
		 * Power CPU1 off entirely.  TF-A sets aff_info_state = OFF
		 * on CPU_OFF, allowing CPU0 to use psci_affinity_info().
		 * This SMC never returns on success.
		 */
		(void)pm_cpu_off();
		/* Reached only on failure; disarm so CPU1 stays alive. */
		atomic_set(&s2ram_cpu1_req, 0);
#endif
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

	return psci_cpu_suspend(LPM_PSTATE_DEEPSLEEP, resume_pa);
}

void pm_state_set(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(substate_id);

	switch (state) {
	case PM_STATE_SUSPEND_TO_RAM:
#ifdef CONFIG_SMP
#if IS_ENABLED(CONFIG_SOC_AM62L_S2RAM_CPU1_SUSPEND)
		/*
		 * Signal CPU1 to enter PSCI CPU_SUSPEND.  The IPI handler
		 * calls am62l_cpu1_s2ram_suspend() and blocks inside
		 * psci_cpu_suspend() until TF-A resumes the system.
		 *
		 * We cannot use psci_affinity_info() here: TF-A only sets
		 * aff_info_state = OFF for CPU_OFF, never for CPU_SUSPEND.
		 * Polling psci_affinity_info() would spin forever.
		 * Instead, CPU1's IPI handler sets s2ram_cpu1_in_psci just
		 * before issuing the PSCI SMC, and we wait for that flag.
		 */
		atomic_set(&s2ram_cpu1_in_psci, 0);
		atomic_set(&s2ram_cpu1_req, 1);
		gic_raise_sgi(SGI_S2RAM_IPI, CPU1_MPIDR, 1U << 1);

		/*
		 * Wait for CPU1 to be at the PSCI SMC boundary.  By the time
		 * CPU0 saves its own context and issues psci_cpu_suspend,
		 * CPU1 will be fully inside TF-A.  TF-A then waits up to
		 * 10 ms for CPU1's PSC MDSTAT before proceeding.
		 */
		while (!atomic_get(&s2ram_cpu1_in_psci)) {
		}
#else
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
#endif
		/*
		 * Save CPU0 context and call psci_cpu_suspend(RTC_DDR).
		 * Returns here after full resume and TURN_ON for all devices.
		 */
		arch_pm_s2ram_suspend(am62l_system_off);

#ifdef CONFIG_SMP
#if IS_ENABLED(CONFIG_SOC_AM62L_S2RAM_CPU1_SUSPEND)
		/*
		 * Clear the request flag, then release CPU1 from TF-A's EL3
		 * WFI.  TF-A's pwr_domain_suspend_finish for CPU1 waits for
		 * any NS G1 interrupt; SGI3 wakes it.  CPU1 ERets to
		 * am62l_cpu1_resume, restores context, and returns to the
		 * IPI handler which exits cleanly (req == 0).
		 */
		atomic_set(&s2ram_cpu1_req, 0);
		gic_raise_sgi(SGI_S2RAM_IPI, CPU1_MPIDR, 1U << 1);
#else
		/*
		 * Restart CPU1 via the Zephyr SMP resume API.  CPU1 was
		 * fully powered off by PSCI CPU_OFF; pm_cpu_on (inside
		 * k_smp_cpu_resume) starts it fresh at __start.
		 * arch_secondary_cpu_init() reinits the GIC and arch timer,
		 * then the scheduler re-acquires CPU1 automatically.
		 */
		atomic_set(&s2ram_cpu1_req, 0);
		k_smp_cpu_resume(1, NULL, NULL, true, true);
#endif
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
