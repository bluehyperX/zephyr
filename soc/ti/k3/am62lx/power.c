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
 *   am62l_wait_cpu1_off() is a Zephyr-side, sleepable, bounded poll of
 *   psci_affinity_info() mirroring Linux's cpu_psci_cpu_kill()
 *   (arch/arm64/kernel/psci.c): same PSCI call, same AFFINITY_LEVEL_OFF
 *   check, same style of bounded sleep-poll.  Unlike Linux (where this runs
 *   *after* the primary in-kernel handoff and a timeout is just a warning
 *   since the CPU is already presumed dead), here it runs *before* the risky
 *   PSCI_SYSTEM_SUSPEND hand-off, so a timeout aborts/retries instead.
 *   Must be called from a sleepable context (not from inside pm_state_set(),
 *   which runs under k_sched_lock() where only busy-waiting is legal).
 *
 * Why k_smp_cpu_start (not k_smp_cpu_resume):
 *   PSCI CPU_OFF hardware-resets GICR_IGROUPR0 to 0 (Group 0 / FIQ).
 *   TF-A's pwr_domain_on_finish restores IGROUPR to 0xFFFF (Group 1) but
 *   does NOT restore GICR_IGRPMODR0.  k_smp_cpu_resume skips Zephyr's
 *   secondary GIC init, so IGRPMODR remains 0 (Group 1 Secure / FIQ) and
 *   SGI3 is never taken (PSTATE.F=1 permanently).  k_smp_cpu_start runs
 *   arm_gic_secondary_init() which writes IGRPMODR=0xFFFF, making SGI3
 *   Group 1 NS (IRQ) and deliverable from cycle 2 onward.
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

/*
 * ARM arch timer save/restore across S2RAM.
 *
 * arch_pm_s2ram_suspend/resume saves MMU, VBAR, and callee-saved GPRs but
 * not the ARM arch timer registers.  The MAIN power domain cut resets
 * CNTV_CTL_EL0 to 0x2 (ENABLE=0, IMASK=1) and CNTV_CVAL_EL0 to an
 * unknown value, preventing tick interrupts after resume.
 *
 * Zephyr's arm64 timer driver uses the virtual timer (CNTV), not the
 * physical timer (CNTP).
 */
static uint64_t s2ram_cntv_cval;
static uint64_t s2ram_cntv_ctl;

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
		k_msleep(CPU1_OFF_POLL_INTERVAL_MS);
	} while (k_uptime_get() < deadline);

	return false;
}
#endif /* CONFIG_SMP */

/*
 * True only across the window where s2ram_cntv_{cval,ctl} hold a value
 * actually worth restoring, i.e. after CPU0 saved them and only until
 * arch_pm_s2ram_suspend() reports whether the suspend really happened.
 * If suspend fails, CPU0 never lost power and its live timer registers
 * were never touched, so pm_state_exit_post_ops() must skip the restore.
 */
static bool s2ram_timer_saved;

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
			(void)pm_cpu_off();
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
		s2ram_cntv_cval = read_sysreg(cntv_cval_el0);
		s2ram_cntv_ctl  = read_sysreg(cntv_ctl_el0);
		/*
		 * A non-zero return means CPU0 never actually suspended (e.g.
		 * psci_system_suspend() rejected it) and is still fully
		 * operational with its timer untouched - nothing to restore.
		 */
		printk("DBG: isr_el1=%#llx cntv_ctl=%#llx cval-cnt=%lld daif=%#llx\n", /* DBG */
		       (unsigned long long)read_sysreg(isr_el1), /* DBG */
		       (unsigned long long)s2ram_cntv_ctl, /* DBG */
		       (long long)(s2ram_cntv_cval - read_sysreg(cntvct_el0)), /* DBG */
		       (unsigned long long)read_sysreg(daif)); /* DBG */
		int sus_ret = arch_pm_s2ram_suspend(am62l_system_off); /* DBG */

		printk("DBG: arch_pm_s2ram_suspend returned %d\n", sus_ret); /* DBG */
		s2ram_timer_saved = (sus_ret == 0);
#ifdef CONFIG_SMP
		/*
		 * CPU1 was powered off via PSCI CPU_OFF before suspend.
		 * k_smp_cpu_start (not k_smp_cpu_resume) is required here:
		 * it runs arm_gic_secondary_init() which writes IGRPMODR=0xFFFF,
		 * restoring SGI3 to Group 1 NS after the power-domain reset
		 * that left IGROUPR=0 (Group 0 / FIQ) on the redistributor.
		 * Pass cpu1_pm_ctrl_restart to recreate the control thread
		 * on every resume cycle.
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
	ARG_UNUSED(substate_id);

	/*
	 * Only CPU0 actually drives S2RAM and saves/restores the timer
	 * (see pm_state_set).  The PM framework still calls this hook on
	 * CPU1 with state == PM_STATE_SUSPEND_TO_RAM (its pending exit
	 * state was latched before the guard redirected it to k_cpu_idle()),
	 * so without this CPU0 check CPU1 would overwrite its own live
	 * CNTV_CTL_EL0/CNTV_CVAL_EL0 with CPU0's stale (zero-initialized)
	 * saved values, disabling CPU1's tick from the first guard cycle on.
	 */
	if (state == PM_STATE_SUSPEND_TO_RAM && arch_curr_cpu()->id == 0U && s2ram_timer_saved) {
		/*
		 * Restore the ARM arch timer registers saved before suspend.
		 * CVAL is written before CTL so the compare value is in place
		 * before the timer is re-enabled.
		 *
		 * Force IMASK=0 (bit 1) regardless of the saved value.  In
		 * tickless mode the timer ISR sets IMASK=1 before announcing
		 * ticks; sys_clock_set_timeout() clears it when programming the
		 * next event.  If the save in pm_state_set() captured CNTV_CTL
		 * between ISR (IMASK=1) and sys_clock_set_timeout() (IMASK=0),
		 * the saved value has IMASK=1.  Restoring that value faithfully
		 * would leave the timer permanently masked after resume, causing
		 * k_msleep() and all timeout-dependent code to hang forever.
		 * Stripping IMASK here is always safe: CVAL is in the past after
		 * a multi-second suspend, so the ISR fires immediately once IRQs
		 * are unmasked by irq_unlock() below and sys_clock_announce()
		 * unblocks any pending timeouts.
		 */
		write_sysreg(s2ram_cntv_cval, cntv_cval_el0);
		write_sysreg(s2ram_cntv_ctl & ~2ULL, cntv_ctl_el0);
	}

	irq_unlock(0);
}
