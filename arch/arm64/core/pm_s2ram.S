/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/toolchain.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/arch/common/pm_s2ram.h>
#include <zephyr/offsets.h>

_ASM_FILE_PROLOGUE

GTEXT(pm_s2ram_mark_set)
GTEXT(pm_s2ram_mark_check_and_clear)
GDATA(_cpu_context)

/*
 * int z_arm64_pm_s2ram_suspend(pm_s2ram_system_off_fn_t system_off)
 *
 * Saves callee-saved GPRs (x19-x30) on the stack and the EL1/EL0 system
 * registers that firmware resets across a suspend to _cpu_context, sets the
 * suspend marker, then calls system_off.
 *
 * On failure (system_off returns): clears the marker and returns the error.
 * On success: system_off does not return; resume continues in
 * arch_pm_s2ram_resume, which makes this function return 0.
 */
GTEXT(z_arm64_pm_s2ram_suspend)
SECTION_FUNC(text, z_arm64_pm_s2ram_suspend)
	stp	x19, x20, [sp, #-16]!
	stp	x21, x22, [sp, #-16]!
	stp	x23, x24, [sp, #-16]!
	stp	x25, x26, [sp, #-16]!
	stp	x27, x28, [sp, #-16]!
	stp	x29, x30, [sp, #-16]!	/* FP + LR */

	/* Preserve system_off fn pointer across bl calls */
	mov	x19, x0

	/*
	 * Save the EL1/EL0 system registers that firmware leaves at their
	 * reset values across a suspend and that Zephyr relies on.
	 *
	 * Not saved on purpose:
	 *  - FP/SIMD registers V0-V31, FPCR, FPSR: the caller flushes the
	 *    live FPU context into the owning thread before suspending
	 *    (arch_pm_s2ram_suspend), so the lazy-FPU trap reloads it later.
	 *  - GIC CPU interface and EL2 registers: rebuilt from constants on
	 *    resume (Zephyr never changes them after boot).
	 *  - Generic timer: stopped/re-armed by the arm arch timer driver
	 *    through a PM notifier (drivers/timer/arm_arch_timer.c).
	 */
	adrp	x0, _cpu_context
	add	x0, x0, :lo12:_cpu_context

	/* SP: the resume path needs a valid stack before anything else. */
	mov	x1, sp
	str	x1, [x0, #___cpu_context_t_sp_OFFSET]

	/* VBAR_EL1: exception vector base; needed for any fault/IRQ. */
	mrs	x1, vbar_el1
	str	x1, [x0, #___cpu_context_t_vbar_el1_OFFSET]

	/* MAIR_EL1, TCR_EL1, TTBR0_EL1: memory attributes and page tables. */
	mrs	x1, mair_el1
	str	x1, [x0, #___cpu_context_t_mair_el1_OFFSET]
	mrs	x1, tcr_el1
	str	x1, [x0, #___cpu_context_t_tcr_el1_OFFSET]
	mrs	x1, ttbr0_el1
	str	x1, [x0, #___cpu_context_t_ttbr0_el1_OFFSET]

	/* SCTLR_EL1: MMU/caches enable; written last to turn the MMU on. */
	mrs	x1, sctlr_el1
	str	x1, [x0, #___cpu_context_t_sctlr_el1_OFFSET]

	/* CPACR_EL1: FP/SIMD access control (lazy FPU state of the CPU). */
	mrs	x1, cpacr_el1
	str	x1, [x0, #___cpu_context_t_cpacr_el1_OFFSET]

	/* TPIDRRO_EL0: current CPU struct pointer, read on exception entry. */
	mrs	x1, tpidrro_el0
	str	x1, [x0, #___cpu_context_t_tpidrro_el0_OFFSET]

	/* TPIDR_EL0: per-thread pointer, reloaded at every context switch. */
	mrs	x1, tpidr_el0
	str	x1, [x0, #___cpu_context_t_tpidr_el0_OFFSET]

	/* SP_EL0: thread stack pointer while running at EL1h. */
	mrs	x1, sp_el0
	str	x1, [x0, #___cpu_context_t_sp_el0_OFFSET]

	dsb	sy

	bl	pm_s2ram_mark_set
	blr	x19			/* call system_off, no return on success */

	/* Failure path: system_off returned with an error */
	mov	x19, x0			/* preserve error code */
	bl	pm_s2ram_mark_check_and_clear
	mov	x0, x19
	ldp	x29, x30, [sp], #16
	ldp	x27, x28, [sp], #16
	ldp	x25, x26, [sp], #16
	ldp	x23, x24, [sp], #16
	ldp	x21, x22, [sp], #16
	ldp	x19, x20, [sp], #16
	ret

/*
 * arch_pm_s2ram_resume
 *
 * Resume entry point passed to the firmware suspend call (PSCI
 * SYSTEM_SUSPEND). Firmware enters it with the MMU off and all EL1/EL2
 * system registers at their reset values. If it enters at EL2h, as in
 * reset.S this code rebuilds the EL2 state and drops to EL1; if it enters at
 * EL1 the EL2 steps are skipped.
 *
 * Restore order:
 *   1. VBAR_EL1 and SP from _cpu_context
 *   2. MAIR_EL1, TCR_EL1, TTBR0_EL1 (page-table configuration)
 *   3. TLB invalidation
 *   4. SCTLR_EL1 (enables the MMU)
 *   5. EL2 context, mirroring z_arm64_el2_init(), then the GIC CPU
 *      interface system registers
 *   6. check and clear the suspend marker
 *   7. pop callee-saved GPRs, restore the remaining EL1/EL0 registers
 *   8. ERET to EL1h (or ret if entered at EL1)
 *
 * If the suspend marker is absent this entry point was reached unexpectedly;
 * spin forever rather than returning to an unknown call site.
 */
GTEXT(arch_pm_s2ram_resume)
SECTION_FUNC(text, arch_pm_s2ram_resume)
	/* Step 1: VBAR_EL1 and SP */
	adrp	x0, _cpu_context
	add	x0, x0, :lo12:_cpu_context
	ldr	x1, [x0, #___cpu_context_t_vbar_el1_OFFSET]
	msr	vbar_el1, x1
	isb
	ldr	x1, [x0, #___cpu_context_t_sp_OFFSET]
	mov	sp, x1

	/* Step 2: page-table configuration */
	ldr	x1, [x0, #___cpu_context_t_mair_el1_OFFSET]
	msr	mair_el1, x1
	ldr	x1, [x0, #___cpu_context_t_tcr_el1_OFFSET]
	msr	tcr_el1, x1
	ldr	x1, [x0, #___cpu_context_t_ttbr0_el1_OFFSET]
	msr	ttbr0_el1, x1
	isb

	/* Step 3: drop any stale TLB entries */
	tlbi	vmalle1is
	dsb	sy
	isb

	/* Step 4: enable the MMU (VA == PA, so execution continues) */
	ldr	x1, [x0, #___cpu_context_t_sctlr_el1_OFFSET]
	msr	sctlr_el1, x1
	isb

	/*
	 * Step 5: EL2 context, mirroring z_arm64_el2_init(). Write known
	 * values instead of read-modify-write, the residual state is unknown.
	 * Skipped when firmware resumes directly at EL1.
	 */
	mrs	x1, CurrentEL
	cmp	x1, #(2 << 2)
	bne	.L_skip_el2

	/* SCTLR_EL2: same value as z_arm64_el2_init() (needs a literal pool) */
	ldr	x1, =(SCTLR_EL2_RES1 | SCTLR_I_BIT | SCTLR_SA_BIT)
	msr	sctlr_el2, x1
	isb

#ifdef CONFIG_GIC_V3
	/* ICC_SRE_EL2: needed before any ICC_SRE_EL1/ICC_IGRPEN1_EL1 access */
	mrs	x1, s3_4_c12_c9_5	/* ICC_SRE_EL2 */
	orr	x1, x1, #(ICC_SRE_ELx_EN_BIT | ICC_SRE_ELx_DFB_BIT | \
			  ICC_SRE_ELx_DIB_BIT | ICC_SRE_ELx_SRE_BIT)
	msr	s3_4_c12_c9_5, x1	/* ICC_SRE_EL2 */
	isb
#endif /* CONFIG_GIC_V3 */

	/* HCR_EL2: EL1 is AArch64, no trapping or routing */
	mov	x1, #HCR_RW_BIT
	msr	hcr_el2, x1
	isb

	/* CPTR_EL2: RES1 with TFP=0, TCPAC=0 -- FP/SIMD not trapped from EL1 */
	mov	x1, #0x32ff		/* CPTR_EL2_RES1 with TFP=0, TCPAC=0 */
	msr	cptr_el2, x1

	/* VBAR_EL2: Zephyr's vector table (VA==PA, same table serves EL1+EL2) */
	ldr	x1, [x0, #___cpu_context_t_vbar_el1_OFFSET]	/* Zephyr vector table base */
	msr	vbar_el2, x1

	/* VTCR_EL2: no stage-2 translation used */
	msr	vtcr_el2, xzr

	/* CNTVOFF_EL2: virtual counter offset = 0 */
	msr	cntvoff_el2, xzr

	/* CNTHCTL_EL2: enable EL1/EL0 physical counter + timer access */
	mov	x1, #(CNTHCTL_EL2_EL1PCTEN | CNTHCTL_EL2_EL1PCEN)
	msr	cnthctl_el2, x1

	/* CNTHP_CTL_EL2: disable hypervisor physical timer */
	msr	cnthp_ctl_el2, xzr

	/* HSTR_EL2: disable AArch32 coprocessor trapping */
	msr	hstr_el2, xzr

	/* MDCR_EL2: clear debug/PMU trap-to-EL2 bits */
	msr	mdcr_el2, xzr

.L_skip_el2:

	/*
	 * GIC CPU interface system registers: firmware restores the GIC MMIO
	 * but not these, and the GICv3 driver has no resume hook. Set them to
	 * the constants used at cold boot.
	 */
#ifdef CONFIG_GIC_V3
	mov	x1, #(ICC_SRE_ELx_SRE_BIT | ICC_SRE_ELx_DFB_BIT | ICC_SRE_ELx_DIB_BIT)
	msr	s3_0_c12_c12_5, x1	/* ICC_SRE_EL1 */
	isb
	mov	x1, #0xFF		/* ICC_PMR_EL1: all priorities pass */
	msr	s3_0_c4_c6_0, x1	/* ICC_PMR_EL1 */
	mov	x1, #1
	msr	s3_0_c12_c12_7, x1	/* ICC_IGRPEN1_EL1: enable group 1 IRQs */
	isb
#endif /* CONFIG_GIC_V3 */

	bl	pm_s2ram_mark_check_and_clear
	cbz	x0, .L_no_magic

	/* Pop callee-saved GPRs x19-x30; caller-saved ones are dead here */
	ldp	x29, x30, [sp], #16
	ldp	x27, x28, [sp], #16
	ldp	x25, x26, [sp], #16
	ldp	x23, x24, [sp], #16
	ldp	x21, x22, [sp], #16
	ldp	x19, x20, [sp], #16

	/*
	 * Step 7: restore the remaining EL1/EL0 registers. x0 was clobbered by
	 * pm_s2ram_mark_check_and_clear, so reload the context address.
	 *
	 * TPIDRRO_EL0 goes first: exception entry dereferences it.
	 */
	adrp	x1, _cpu_context
	add	x1, x1, :lo12:_cpu_context

	ldr	x2, [x1, #___cpu_context_t_tpidrro_el0_OFFSET]
	msr	tpidrro_el0, x2

	/*
	 * CPACR_EL1: force FPEN=0b11 so FP use right after ERET does not trap.
	 * The FP registers are not restored here, the caller flushes FPU
	 * ownership before suspending (arch_pm_s2ram_suspend).
	 */
	ldr	x2, [x1, #___cpu_context_t_cpacr_el1_OFFSET]
	orr	x2, x2, #(3 << 20)	/* force FPEN=0b11: EL1/EL0 FP not trapped */
	msr	cpacr_el1, x2

	ldr	x2, [x1, #___cpu_context_t_tpidr_el0_OFFSET]
	msr	tpidr_el0, x2		/* EL0 thread pointer */

	ldr	x2, [x1, #___cpu_context_t_sp_el0_OFFSET]
	msr	sp_el0, x2		/* EL0 stack pointer */

	isb

	/*
	 * Step 8: if we entered at EL1, a plain ret returns to the caller.
	 */
	mrs	x2, CurrentEL
	cmp	x2, #(2 << 2)
	bne	.L_ret_el1

	/*
	 * Otherwise ERET to EL1h: a plain ret would stay at EL2. ELR_EL2 is the saved return address (x30). DAIF stays masked;
	 * pm_state_exit_post_ops() unmasks it via irq_unlock(0).
	 */
	mov	x2, sp
	msr	sp_el1, x2		/* EL1h SP = current SP after pops */
	msr	elr_el2, x30		/* EL2 return address = original LR */
	mov	x2, #(SPSR_DAIF_MASK | SPSR_MODE_EL1H)
	msr	spsr_el2, x2
	mov	x0, #0			/* return 0 = successful resume */
	eret

.L_ret_el1:
	mov	x0, #0			/* return 0 = successful resume */
	ret

.L_no_magic:
	/* Marker absent -- unexpected entry, spin to avoid executing garbage */
	b	.L_no_magic
