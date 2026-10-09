/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AM62L SoC PM backend.
 *
 * TF-A handles DDR self-refresh and LPM HAL entirely (via TIFS).  Zephyr
 * saves CPU context and drives PSCI CPU_SUSPEND.
 *
 * S2RAM with SMP (semaphore + control thread + affinity poll):
 *   1. CPU0 calls pm_state_force(1, &s2ram_state) then k_sem_give(&cpu1_pm_sem).
 *   2. CPU1's cpu1_pm_ctrl (highest-priority, CPU1-pinned thread) unblocks
 *      and calls pm_state_set(PM_STATE_SUSPEND_TO_RAM, 0) directly ->
 *      pm_cpu_off().  TF-A sets aff_info_state = AFF_STATE_OFF on CPU_OFF,
 *      so psci_affinity_info() works correctly on resume.
 *   3. CPU0 calls am62l_wait_cpu1_off() (see below) to confirm CPU1 is
 *      really off, then pm_state_force(0, &s2ram_state) and its own
 *      pm_system_suspend() -> arch_pm_s2ram_suspend() -> psci_system_suspend().
 *   4. On resume CPU0 calls k_smp_cpu_start(1, cpu1_pm_ctrl_restart, NULL)
 *      which recreates cpu1_pm_ctrl fresh on every resume cycle.
 *
 * Why CPU0 polls CPU1's affinity state (am62l_wait_cpu1_off()):
 *   TF-A's own am62l_pwr_domain_suspend() (plat/ti/k3low/common/am62l_psci.c)
 *   already polls CPU1's PSC MDSTAT register before CPU0's PSCI_SYSTEM_SUSPEND
 *   proceeds, bounded at 10ms - but on timeout it just logs an error and
 *   returns without ever calling the real suspend sequence, while the generic
 *   PSCI framework (pwr_domain_suspend() is void) still reports SUCCESS back
 *   to Zephyr.  A failed/skipped suspend would otherwise go undetected.
 *   am62l_wait_cpu1_off() is a Zephyr-side, bounded busy-wait poll of
 *   psci_affinity_info() mirroring Linux's cpu_psci_cpu_kill()
 *   (arch/arm64/kernel/psci.c): same PSCI call, same AFFINITY_LEVEL_OFF
 *   check, bounded poll (busy-wait, see below).  Unlike Linux (where this runs
 *   *after* the primary in-kernel handoff and a timeout is just a warning
 *   since the CPU is already presumed dead), here it runs *before* the risky
 *   PSCI_SYSTEM_SUSPEND hand-off, so a timeout aborts/retries instead.
 *   It busy-waits rather than sleeps: on hardware, sleeping in this
 *   window (k_msleep) left the A53 unable to power down in TIFS (~50 mW
 *   instead of ~15 mW).  Call it from thread context before forcing CPU0's
 *   state, not from inside pm_state_set().
 *
 * Why CPU1 is restarted with k_smp_cpu_start (not k_smp_cpu_resume):
 *   This applies to CPU1 only, which is powered off with PSCI CPU_OFF.
 *   CPU_OFF hardware-resets its GIC redistributor registers, leaving
 *   GICR_IGROUPR0 = 0 (Group 0 / FIQ).  TF-A's pwr_domain_on_finish
 *   restores IGROUPR to 0xFFFF (Group 1) but not GICR_IGRPMODR0, so
 *   SGI3 would stay Group 1 Secure (FIQ) and never be taken with
 *   PSTATE.F=1.  k_smp_cpu_resume skips Zephyr's secondary GIC init;
 *   k_smp_cpu_start runs arm_gic_secondary_init(), which writes
 *   IGRPMODR=0xFFFF and makes SGI3 Group 1 NS (IRQ) again.
 *   CPU0 does not need this: TF-A saves and restores its whole GIC
 *   (including IGROUPR/IGRPMODR) around PSCI_SYSTEM_SUSPEND.
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
#include <zephyr/sys/sys_io.h>
#include "power.h"

#if defined(CONFIG_SMP) && defined(CONFIG_FPU_SHARING)
/* arch-internal; no public equivalent */
extern void arch_flush_local_fpu(void);
#else
#define arch_flush_local_fpu() do { } while (0)
#endif

/* Forced S2RAM state used by both CPUs */
const struct pm_state_info s2ram_state = {
	.state = PM_STATE_SUSPEND_TO_RAM,
};

#ifdef CONFIG_SMP
/* MPIDR affinity value for CPU1 on this 2-core, single-cluster SoC. */
#define CPU1_MPIDR			0x1ULL
/* PSCI AFFINITY_INFO return value meaning "off" (PSCI spec, not in-tree elsewhere). */
#define PSCI_AFFINITY_LEVEL_OFF		1
#define CPU1_OFF_POLL_TIMEOUT_MS	100
#define CPU1_OFF_POLL_INTERVAL_MS	1

/* See "Why CPU0 polls CPU1's affinity state" above. */
bool am62l_wait_cpu1_off(void)
{
	int64_t deadline = k_uptime_get() + CPU1_OFF_POLL_TIMEOUT_MS;

	do {
		if (psci_affinity_info(CPU1_MPIDR, 0) == PSCI_AFFINITY_LEVEL_OFF) {
			return true;
		}
		/* Busy-wait, not k_msleep(): sleeping lets CPU0 take timer/scheduler
		 * IPI activity that leaves the A53 unable to power down in TIFS
		 * (suspend stalls at ~50 mW instead of ~15 mW).
		 */
		k_busy_wait(CPU1_OFF_POLL_INTERVAL_MS * USEC_PER_MSEC);
	} while (k_uptime_get() < deadline);

	return false;
}
#endif /* CONFIG_SMP */

#ifdef CONFIG_SMP
/* CPU1 control thread: woken by CPU0 via semaphore to initiate its own shutdown */
K_THREAD_STACK_DEFINE(cpu1_pm_ctrl_stack, 4096);
struct k_thread cpu1_pm_ctrl_thread;
K_SEM_DEFINE(cpu1_pm_sem, 0, 1);

void cpu1_pm_ctrl(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (1) {
		k_sem_take(&cpu1_pm_sem, K_FOREVER);
		/* state passed explicitly below; no need to force a slot nothing reads. */
		pm_state_set(PM_STATE_SUSPEND_TO_RAM, 0);
		/* pm_cpu_off() failed (PSCI error): loop back and wait for retry. */
	}
}

/*
 * (Re)creates cpu1_pm_ctrl. Called via k_smp_cpu_start's fn on every CPU1
 * resume, and directly by the sample's init code for the first boot - same
 * k_thread_create() sequence either way, so it is not static.
 */
void cpu1_pm_ctrl_restart(void *arg)
{
	ARG_UNUSED(arg);
	k_thread_create(&cpu1_pm_ctrl_thread, cpu1_pm_ctrl_stack,
			K_THREAD_STACK_SIZEOF(cpu1_pm_ctrl_stack),
			cpu1_pm_ctrl, NULL, NULL, NULL,
			K_HIGHEST_APPLICATION_THREAD_PRIO, 0, K_FOREVER);
	k_thread_cpu_mask_clear(&cpu1_pm_ctrl_thread);
	k_thread_cpu_mask_enable(&cpu1_pm_ctrl_thread, 1);
	k_thread_start(&cpu1_pm_ctrl_thread);
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
		if (arch_curr_cpu()->id != 0U) {
			/* CPU1: entered only via cpu1_pm_ctrl's direct pm_state_set()
			 * call. Power this CPU off directly; never returns on success.
			 */
			/*
			 * Release any live FPU context on this CPU first.  With
			 * FPU_SHARING a thread (e.g. main) may own CPU1's FPU;
			 * if CPU1 goes off still owning it, CPU0 would spin
			 * forever in flush_owned_fpu() waiting for an IPI ack.
			 *
			 * Use the per-CPU arch_irq_lock(), not irq_lock(): on SMP
			 * irq_lock() takes the system-wide global lock, and
			 * pm_cpu_off() never returns, so CPU1 would die holding it
			 * and CPU0's next irq_lock() would spin forever.
			 */
			unsigned int key = arch_irq_lock();

			arch_flush_local_fpu();
			(void)pm_cpu_off();
			arch_irq_unlock(key);
			break;
		}

		/*
		 * CPU0: by the time we reach this point the caller (app
		 * code, a sleepable context) has already confirmed via
		 * am62l_wait_cpu1_off() that CPU1 reached PSCI AFF_STATE_OFF.
		 * That poll can't live here: pm_state_set() runs under
		 * k_sched_lock(), where only busy-waiting is legal, and a
		 * failed/timed-out wait needs to abort before any device
		 * suspend or PSCI_SYSTEM_SUSPEND call happens, not from
		 * inside this void, no-return-value function.
		 */
#endif
		/*
		 * Save CPU0 context and call PSCI_SYSTEM_SUSPEND.
		 * Returns here after full resume and TURN_ON for all devices.
		 */
		/*
		 * The FP/SIMD register file loses power with the A53 and is
		 * not saved by arch_pm_s2ram_suspend().  Spill any live FPU
		 * context into its thread now (IRQs are already masked here)
		 * so the lazy-FPU trap reloads it after resume.
		 */
		arch_flush_local_fpu();
		/*
		 * A non-zero return means CPU0 never actually suspended (e.g.
		 * psci_system_suspend() rejected it) and is still operational.
		 * The ARM arch timer is stopped/re-armed by its driver via a
		 * PM notifier (drivers/timer/arm_arch_timer.c).
		 */
		(void)arch_pm_s2ram_suspend(am62l_system_off);
#ifdef CONFIG_SMP
		/*
		 * CPU1 was powered off via PSCI CPU_OFF before suspend, so it
		 * needs a full restart (see "Why CPU1 is restarted with
		 * k_smp_cpu_start" at the top of this file).  Passing
		 * cpu1_pm_ctrl_restart recreates the control thread on every
		 * resume cycle.
		 */
		k_smp_cpu_start(1, cpu1_pm_ctrl_restart, NULL);
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
