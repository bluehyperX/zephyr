// SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
// SPDX-License-Identifier: Apache-2.0

#include <zephyr/toolchain.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/arch/common/pm_s2ram.h>

_ASM_FILE_PROLOGUE

GTEXT(pm_s2ram_mark_set)
GTEXT(pm_s2ram_mark_check_and_clear)
GDATA(_cpu_context)

/*
 * int arch_pm_s2ram_suspend(pm_s2ram_system_off_fn_t system_off)
 *
 * Saves callee-saved GPRs (x19-x30) on the stack, saves SP, VBAR_EL1,
 * MMU registers, CPACR_EL1 to _cpu_context, writes the suspend marker,
 * then calls system_off.
 *
 * On failure (system_off returns): clears the marker and returns the error.
 * On success: system_off never returns; resume continues in arch_pm_s2ram_resume.
 */
GTEXT(arch_pm_s2ram_suspend)
SECTION_FUNC(text, arch_pm_s2ram_suspend)
	stp	x19, x20, [sp, #-16]!
	stp	x21, x22, [sp, #-16]!
	stp	x23, x24, [sp, #-16]!
	stp	x25, x26, [sp, #-16]!
	stp	x27, x28, [sp, #-16]!
	stp	x29, x30, [sp, #-16]!	/* FP + LR */

	/* Preserve system_off fn pointer across bl calls */
	mov	x19, x0

	/* Save MMU context and SP/VBAR_EL1/CPACR_EL1 to _cpu_context */
	adrp	x0, _cpu_context
	add	x0, x0, :lo12:_cpu_context
	mov	x1, sp
	str	x1, [x0, #0]		/* _cpu_context.sp */
	mrs	x1, vbar_el1
	str	x1, [x0, #8]		/* _cpu_context.vbar_el1 */
	mrs	x1, mair_el1
	str	x1, [x0, #16]		/* _cpu_context.mair_el1 */
	mrs	x1, tcr_el1
	str	x1, [x0, #24]		/* _cpu_context.tcr_el1 */
	mrs	x1, ttbr0_el1
	str	x1, [x0, #32]		/* _cpu_context.ttbr0_el1 */
	mrs	x1, sctlr_el1
	str	x1, [x0, #40]		/* _cpu_context.sctlr_el1 */
	mrs	x1, cpacr_el1
	str	x1, [x0, #48]		/* _cpu_context.cpacr_el1 */

	dsb	sy

	bl	pm_s2ram_mark_set
	blr	x19			/* call system_off — never returns on success */

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
 * Resume entry point passed to TF-A via PSCI SYSTEM_SUSPEND.
 *
 * TF-A always ERETts to EL2h (SPSR_EL3.M[4:0]=9) — both at cold boot and
 * at S2RAM resume.  This function therefore runs at EL2.  At cold boot
 * Zephyr's reset.S immediately drops from EL2 to EL1 via z_arm64_el2_init +
 * ERET.  On S2RAM resume that drop must be replicated here.
 *
 * TF-A's PSCI_SYSTEM_SUSPEND path does not call
 * cm_el1_sysregs_context_restore() because am62l_lpm_state is INVALID
 * on the Zephyr call path (Zephyr does not go through the Linux S2Idle
 * flow that sets that variable).  It likewise does not restore EL2 sysregs.
 * The CPU arrives here with:
 *   SCTLR_EL1.M=0 (MMU off), MAIR_EL1=0, TCR_EL1=0, TTBR0_EL1=0
 *   HCR_EL2=0x2 (RW=0 — EL1 AArch32; ERET to AArch64 EL1h is CU)
 *   CPTR_EL2=0x33FF (TFP=1 — FP/SIMD from EL1 traps to EL2)
 *   VBAR_EL2=garbage (TF-A did not restore its EL2 context)
 *   CPACR_EL1=0 (FP/SIMD disabled at EL1)
 *
 * Restore order:
 *   1. VBAR_EL1 + SP from _cpu_context (no stack or translation yet)
 *   2. MAIR_EL1, TCR_EL1, TTBR0_EL1 — page-table config
 *   3. TLB invalidation
 *   4. SCTLR_EL1 — re-enables MMU (VA==PA for Zephyr, so no pipeline gap)
 *   5. HCR_EL2 (RW=1) + CPTR_EL2 + VBAR_EL2 — fix EL2 state TF-A reset
 *   6. pm_s2ram_mark_check_and_clear
 *   7. Pop callee-saved GPRs, restore CPACR_EL1
 *   8. ERET to EL1h to drop back from EL2
 *
 * If the suspend marker is absent this entry point was reached unexpectedly;
 * spin forever rather than returning to an unknown call site.
 */
GTEXT(arch_pm_s2ram_resume)
SECTION_FUNC(text, arch_pm_s2ram_resume)
	/* Step 1: restore VBAR_EL1 and SP — no stack or MMU needed yet */
	adrp	x0, _cpu_context
	add	x0, x0, :lo12:_cpu_context
	ldr	x1, [x0, #8]
	msr	vbar_el1, x1
	isb
	ldr	x1, [x0, #0]
	mov	sp, x1

	/* Step 2: restore page-table configuration registers */
	ldr	x1, [x0, #16]
	msr	mair_el1, x1
	ldr	x1, [x0, #24]
	msr	tcr_el1, x1
	ldr	x1, [x0, #32]
	msr	ttbr0_el1, x1
	isb

	/* Step 3: invalidate TLBs — TF-A may have left stale EL1 entries */
	tlbi	vmalle1is
	dsb	sy
	isb

	/* Step 4: re-enable MMU; VA==PA so the pipeline continues cleanly */
	ldr	x1, [x0, #40]
	msr	sctlr_el1, x1
	isb

	/*
	 * Step 5: fix EL2 registers that TF-A reset because lpm_state=INVALID
	 * prevents its own EL2 context restore on the Zephyr PSCI path.
	 *
	 * HCR_EL2: TF-A resets to 0x2 (RW=0 — EL1 executes AArch32).
	 * Without RW=1 an ERET to AArch64 EL1h is CONSTRAINED UNPREDICTABLE
	 * on Cortex-A53; the core takes a synchronous exception back to EL2
	 * and may reset CPTR_EL2 to the hardware reset value (0x33FF).
	 * Mirror what z_arm64_el2_init does: clear IMO/AMO/TGE, set RW=1.
	 *
	 * CPTR_EL2: TF-A resets to 0x33FF (TFP=1).  Boot value is 0x32FF
	 * (CPTR_EL2_RES1 with TFP=0 so FP/SIMD from EL1 does NOT trap to EL2).
	 *
	 * VBAR_EL2: TF-A resets to an invalid address.  Point to Zephyr's
	 * vector table — VA==PA identity map, same table serves both EL levels.
	 */
	mrs	x1, hcr_el2
	bic	x1, x1, #(1 << 4)	/* clear HCR_IMO_BIT */
	bic	x1, x1, #(1 << 5)	/* clear HCR_AMO_BIT */
	bic	x1, x1, #(1 << 27)	/* clear HCR_TGE_BIT */
	orr	x1, x1, #(1 << 31)	/* set  HCR_RW_BIT  — EL1 is AArch64 */
	msr	hcr_el2, x1
	isb
	mov	x1, #0x32ff		/* CPTR_EL2_RES1 with TFP=0, TCPAC=0 */
	msr	cptr_el2, x1
	ldr	x1, [x0, #8]		/* vbar_el1 = Zephyr vector table base */
	msr	vbar_el2, x1
	isb

	bl	pm_s2ram_mark_check_and_clear
	cbz	x0, .L_no_magic

	/* Pop callee-saved GPRs (DDR retained in self-refresh) */
	ldp	x29, x30, [sp], #16
	ldp	x27, x28, [sp], #16
	ldp	x25, x26, [sp], #16
	ldp	x23, x24, [sp], #16
	ldp	x21, x22, [sp], #16
	ldp	x19, x20, [sp], #16

	/*
	 * Step 6: restore CPACR_EL1 (TF-A resets to 0, disabling FP/SIMD at
	 * EL1).  Re-load _cpu_context address since x0 was clobbered by
	 * pm_s2ram_mark_check_and_clear above.
	 */
	adrp	x1, _cpu_context
	add	x1, x1, :lo12:_cpu_context
	ldr	x2, [x1, #48]
	msr	cpacr_el1, x2
	isb

	/*
	 * Step 7: ERET to EL1h.
	 * We are executing at EL2h (TF-A ERETted here with SPSR_EL3=EL2h).
	 * A plain ret would stay at EL2.  Use ERET with SPSR_EL2=EL1h and
	 * ELR_EL2=x30 (return address saved on DDR-retained stack) to switch
	 * back to EL1.  All DAIF bits masked; pm_state_exit_post_ops unmasks
	 * them via irq_unlock(0).
	 */
	mov	x1, sp
	msr	sp_el1, x1		/* EL1h SP = current SP after pops */
	msr	elr_el2, x30		/* EL2 return address = original LR */
	mov	x1, #(SPSR_DAIF_MASK | SPSR_MODE_EL1H)
	msr	spsr_el2, x1
	mov	x0, #0			/* return 0 = successful resume */
	eret

.L_no_magic:
	/* Marker absent — unexpected entry, spin to avoid executing garbage */
	b	.L_no_magic
