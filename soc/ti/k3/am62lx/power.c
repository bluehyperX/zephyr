// SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
// SPDX-License-Identifier: Apache-2.0

/*
 * AM62L SoC PM backend.
 *
 * TF-A handles DDR self-refresh and LPM HAL entirely (via TIFS).  Zephyr
 * saves CPU context and drives PSCI CPU_SUSPEND.
 *
 * S2RAM with SMP (SGI + polled CPU_OFF):
 *   1. CPU0 sets s2ram_cpu1_req and raises SGI3 to CPU1.
 *   2. CPU1's SGI3 ISR sees s2ram_cpu1_req and calls pm_cpu_off().
 *      TF-A sets aff_info_state = AFF_STATE_OFF on CPU_OFF, so
 *      psci_affinity_info() works correctly here.
 *   3. CPU0 busy-polls psci_affinity_info() until CPU1 is off.
 *   4. CPU0 calls arch_pm_s2ram_suspend() -> psci_system_suspend().
 *   5. On resume CPU0 clears s2ram_cpu1_req and calls k_smp_cpu_start(1).
 *      k_smp_cpu_start runs full secondary init (GIC + timer), then the
 *      scheduler re-acquires CPU1.
 *
 * Why SGI + k_smp_cpu_start (not k_smp_cpu_resume):
 *   PSCI CPU_OFF hardware-resets GICR_IGROUPR0 to 0 (Group 0 / FIQ).
 *   TF-A's pwr_domain_on_finish restores IGROUPR to 0xFFFF (Group 1) but
 *   does NOT restore GICR_IGRPMODR0.  k_smp_cpu_resume skips Zephyr's
 *   secondary GIC init, so IGRPMODR remains 0 (Group 1 Secure / FIQ) and
 *   SGI3 is never taken (PSTATE.F=1 permanently).  k_smp_cpu_start runs
 *   arm_gic_secondary_init() which writes IGRPMODR=0xFFFF, making SGI3
 *   Group 1 NS (IRQ) and deliverable from cycle 2 onward.
 *
 * Why CPU0 guard in pm_state_set:
 *   Zephyr's PM framework calls pm_state_set on every idle CPU.  If CPU1's
 *   idle thread selects PM_STATE_SUSPEND_TO_RAM and enters this backend,
 *   it would issue PSCI for its own suspend, triggering TF-A's
 *   pwr_domain_suspend which calls k3_gic_cpuif_disable() -> ProcessorSleep=1.
 *   With ProcessorSleep=1 the redistributor stops forwarding interrupts, so
 *   CPU0's subsequent SGI3 is never presented to CPU1's CPU interface
 *   (GICR_ISPENDR0[3]=1 but ICC_HPPIR1_EL1=0x3FF).  The guard redirects
 *   secondary CPUs to WFI, keeping ProcessorSleep=0.
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
		printk("CPU1: SGI3 ISR: calling pm_cpu_off\n");
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
	if (arch_curr_cpu()->id == 0U) {
		return;
	}
	/*
	 * CPU1 only.  arm_gic_secondary_init() has already run, so the
	 * redistributor is configured and IGRPMODR=0xFFFF (Group 1 NS).
	 * Enable SGI3 so CPU0 can signal CPU1 to power itself off.
	 */
	printk("CPU1: soc_per_core_init_hook: enabling SGI3\n");
	irq_enable(SGI_S2RAM_IPI);
}
#endif /* CONFIG_SMP */

static int am62l_system_off(void)
{
	uintptr_t resume_pa = (uintptr_t)arch_pm_s2ram_resume;

	return psci_system_suspend(resume_pa);
}

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

void pm_state_set(enum pm_state state, uint8_t substate_id)
{
	ARG_UNUSED(substate_id);

	switch (state) {
	case PM_STATE_SUSPEND_TO_RAM:
#ifdef CONFIG_SMP
		/*
		 * Secondary CPUs must not independently drive S2RAM.  If the
		 * PM policy selects S2RAM for CPU1, redirect it to WFI.  CPU1
		 * will receive SGI3 from CPU0 (which does drive S2RAM) and
		 * power itself off via the ISR.  Falling through to WFI keeps
		 * ProcessorSleep=0 so the redistributor forwards the SGI.
		 */
		if (arch_curr_cpu()->id != 0U) {
			k_cpu_idle();
			break;
		}

		/*
		 * CPU0: signal CPU1 to power itself off via SGI3.
		 * The ISR (am62l_s2ram_cpu1_ipi) checks s2ram_cpu1_req and
		 * calls PSCI CPU_OFF.  TF-A updates aff_info_state = OFF so
		 * psci_affinity_info() detects completion.
		 */
		printk("CPU0: sending SGI3 to CPU1\n");
		atomic_set(&s2ram_cpu1_req, 1);
		gic_raise_sgi(SGI_S2RAM_IPI, CPU1_MPIDR, 1U << 1);

		printk("CPU0: polling affinity_info for CPU1\n");
		while (psci_affinity_info(CPU1_MPIDR, 0) == PSCI_AFF_STATE_ON) {
			k_busy_wait(100);
		}
		printk("CPU0: CPU1 is off\n");
#endif
		/*
		 * Save CPU0 context and call PSCI_SYSTEM_SUSPEND.
		 * Returns here after full resume and TURN_ON for all devices.
		 */
		s2ram_cntv_cval = read_sysreg(cntv_cval_el0);
		s2ram_cntv_ctl  = read_sysreg(cntv_ctl_el0);
		arch_pm_s2ram_suspend(am62l_system_off);
		printk("pm_state_set: resumed from S2RAM\n");
#ifdef CONFIG_SMP
		/*
		 * CPU1 was powered off via PSCI CPU_OFF before suspend.
		 * k_smp_cpu_start (not k_smp_cpu_resume) is required here:
		 * it runs arm_gic_secondary_init() which writes IGRPMODR=0xFFFF,
		 * restoring SGI3 to Group 1 NS after the power-domain reset
		 * that left IGROUPR=0 (Group 0 / FIQ) on the redistributor.
		 */
		atomic_set(&s2ram_cpu1_req, 0);
		printk("CPU0: starting CPU1 via k_smp_cpu_start\n");
		k_smp_cpu_start(1, NULL, NULL);
		printk("CPU0: k_smp_cpu_start done\n");
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

	if (state == PM_STATE_SUSPEND_TO_RAM) {
		/*
		 * Restore the ARM arch timer registers saved before suspend.
		 * CVAL is written before CTL so the compare value is in place
		 * before the timer is re-enabled.
		 *
		 * The restore must be a direct write, not read-modify-write:
		 * the power-domain reset left CNTV_CTL_EL0 = 0x2 (IMASK=1).
		 * Restoring the saved value (0x1: ENABLE=1, IMASK=0) clears
		 * IMASK.  An OR-based enable would keep IMASK=1 and suppress
		 * the interrupt even with ENABLE=1.
		 *
		 * The saved CVAL is always in the past after a multi-second
		 * suspend, so the interrupt fires immediately once IRQs are
		 * unmasked by irq_unlock() below.  The timer ISR announces all
		 * elapsed ticks via sys_clock_announce(), unblocking k_msleep.
		 */
		write_sysreg(s2ram_cntv_cval, cntv_cval_el0);
		write_sysreg(s2ram_cntv_ctl,  cntv_ctl_el0);
	}

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
