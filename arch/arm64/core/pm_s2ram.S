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
 * MMU registers, CPACR_EL1, and all other EL1/EL0 sysregs that TF-A
 * resets on the Zephyr PSCI path to _cpu_context, writes the suspend
 * marker, then calls system_off.
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

	/* Save all sysregs to _cpu_context */
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
	mrs	x1, tpidrro_el0
	str	x1, [x0, #56]		/* _cpu_context.tpidrro_el0 */
	mrs	x1, mdscr_el1
	str	x1, [x0, #64]		/* _cpu_context.mdscr_el1 */
	mrs	x1, tpidr_el0
	str	x1, [x0, #72]		/* _cpu_context.tpidr_el0 */
	mrs	x1, contextidr_el1
	str	x1, [x0, #80]		/* _cpu_context.contextidr_el1 */
	mrs	x1, sp_el0
	str	x1, [x0, #88]		/* _cpu_context.sp_el0 */
	mrs	x1, osdlr_el1
	str	x1, [x0, #96]		/* _cpu_context.osdlr_el1 */

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
 * cm_el1_sysregs_context_restore() or cm_el2_sysregs_context_restore()
 * because am62l_lpm_state is INVALID on the Zephyr call path (Zephyr does
 * not go through the Linux S2Idle flow that sets that variable).  The CPU
 * arrives here with all EL1/EL2 sysregs at hardware reset values:
 *   SCTLR_EL1.M=0 (MMU off), MAIR_EL1=0, TCR_EL1=0, TTBR0_EL1=0
 *   tpidrro_el0=0, tpidr_el0=0, cpacr_el1=0, mdscr_el1=0, ...
 *   HCR_EL2=0x2 (RW=0 — EL1 AArch32), CPTR_EL2=0x33FF (TFP=1)
 *
 * Restore order:
 *   1. VBAR_EL1 + SP from _cpu_context (no stack or translation yet)
 *   2. MAIR_EL1, TCR_EL1, TTBR0_EL1 — page-table config
 *   3. TLB invalidation
 *   4. SCTLR_EL1 — re-enables MMU (VA==PA for Zephyr, so no pipeline gap)
 *   5. Full EL2 context restore — mirrors z_arm64_el2_init:
 *        SCTLR_EL2, ICC_SRE_EL2, HCR_EL2, CPTR_EL2, VBAR_EL2, VTCR_EL2,
 *        CNTVOFF_EL2, CNTHCTL_EL2, CNTHP_CTL_EL2, HSTR_EL2, MDCR_EL2;
 *        then GIC CPU interface sysregs ICC_SRE_EL1/PMR/IGRPEN1
 *   6. pm_s2ram_mark_check_and_clear
 *   7. Pop callee-saved GPRs, restore EL1 sysregs from _cpu_context
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
	 * Step 5: full EL2 context restore — mirrors z_arm64_el2_init().
	 * TF-A skips all EL2 sysreg restore when am62l_lpm_state=INVALID.
	 * Write exact known-good values; do NOT read-modify-write on TF-A's
	 * unknown residual state.
	 */

	/* SCTLR_EL2: write exact value that z_arm64_el2_init sets.
	 * SCTLR_EL2_RES1(0x30C50830) | SCTLR_I_BIT(BIT12) | SCTLR_SA_BIT(BIT3)
	 * = 0x30C51838.  Not encodable as ARM64 logical immediate → literal pool. */
	ldr	x1, =(SCTLR_EL2_RES1 | SCTLR_I_BIT | SCTLR_SA_BIT)
	msr	sctlr_el2, x1
	isb

	/* ICC_SRE_EL2: enable GIC system-register interface from EL1.
	 * Must be set before any ICC_SRE_EL1 or ICC_IGRPEN1_EL1 access. */
	mrs	x1, s3_4_c12_c9_5	/* ICC_SRE_EL2 */
	orr	x1, x1, #(ICC_SRE_ELx_EN_BIT | ICC_SRE_ELx_DFB_BIT | \
			  ICC_SRE_ELx_DIB_BIT | ICC_SRE_ELx_SRE_BIT)
	msr	s3_4_c12_c9_5, x1	/* ICC_SRE_EL2 */
	isb

	/* HCR_EL2: write known-good value directly.
	 * RW=1 (AArch64 at EL1); all trap/routing bits off for bare-metal EL1. */
	mov	x1, #HCR_RW_BIT
	msr	hcr_el2, x1
	isb

	/* CPTR_EL2: RES1 with TFP=0, TCPAC=0 — FP/SIMD not trapped from EL1 */
	mov	x1, #0x32ff		/* CPTR_EL2_RES1 with TFP=0, TCPAC=0 */
	msr	cptr_el2, x1

	/* VBAR_EL2: Zephyr's vector table (VA==PA, same table serves EL1+EL2) */
	ldr	x1, [x0, #8]		/* vbar_el1 = Zephyr vector table base */
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

	/*
	 * GIC CPU interface sysregs — TF-A restores Distributor/Redistributor
	 * MMIO but NOT the EL1 CPU interface sysregs.  Zephyr's gicv3_cpuif_init()
	 * is not called on resume (GICv3 driver has no PM TURN_ON hook).
	 * Restore to the constants Zephyr sets at cold boot.
	 *
	 * ICC_SRE_EL1  = s3_0_c12_c12_5
	 * ICC_PMR_EL1  = s3_0_c4_c6_0
	 * ICC_IGRPEN1_EL1 = s3_0_c12_c12_7
	 */
	mov	x1, #(ICC_SRE_ELx_SRE_BIT | ICC_SRE_ELx_DFB_BIT | ICC_SRE_ELx_DIB_BIT)
	msr	s3_0_c12_c12_5, x1	/* ICC_SRE_EL1 */
	isb
	mov	x1, #0xFF		/* ICC_PMR_EL1: all priorities pass */
	msr	s3_0_c4_c6_0, x1	/* ICC_PMR_EL1 */
	mov	x1, #1
	msr	s3_0_c12_c12_7, x1	/* ICC_IGRPEN1_EL1: enable group 1 IRQs */
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
	 * Step 6: restore EL1 sysregs from _cpu_context.
	 * Re-load _cpu_context address since x0 was clobbered by
	 * pm_s2ram_mark_check_and_clear above.
	 *
	 * tpidrro_el0 FIRST — the exception entry macro reads it immediately
	 * to locate the CPU struct.  Any exception after ERET will dereference
	 * it; a stale 0 causes a NULL pointer data abort.
	 */
	adrp	x1, _cpu_context
	add	x1, x1, :lo12:_cpu_context

	ldr	x2, [x1, #56]
	msr	tpidrro_el0, x2		/* CPU struct pointer — must be first */

	/* CPACR_EL1: force FPEN=0b11 so the first FP instruction after ERET
	 * does not trap even if the saved value had FPEN=0 (lazy FP disabled).
	 * The lazy-FP path will re-disable FPEN on the next context switch. */
	ldr	x2, [x1, #48]
	orr	x2, x2, #(3 << 20)	/* force FPEN=0b11: EL1/EL0 FP not trapped */
	msr	cpacr_el1, x2

	ldr	x2, [x1, #64]
	msr	mdscr_el1, x2		/* debug state control */

	ldr	x2, [x1, #72]
	msr	tpidr_el0, x2		/* EL0 thread pointer */

	ldr	x2, [x1, #80]
	msr	contextidr_el1, x2	/* ASID context */

	ldr	x2, [x1, #88]
	msr	sp_el0, x2		/* EL0 stack pointer */

	ldr	x2, [x1, #96]
	msr	osdlr_el1, x2		/* OS Double Lock */
	isb

	/*
	 * Step 7: ERET to EL1h.
	 * We are executing at EL2h (TF-A ERETted here with SPSR_EL3=EL2h).
	 * A plain ret would stay at EL2.  Use ERET with SPSR_EL2=EL1h and
	 * ELR_EL2=x30 (return address saved on DDR-retained stack) to switch
	 * back to EL1.  All DAIF bits masked; pm_state_exit_post_ops unmasks
	 * them via irq_unlock(0).
	 */
	mov	x2, sp
	msr	sp_el1, x2		/* EL1h SP = current SP after pops */
	msr	elr_el2, x30		/* EL2 return address = original LR */
	mov	x2, #(SPSR_DAIF_MASK | SPSR_MODE_EL1H)
	msr	spsr_el2, x2
	mov	x0, #0			/* return 0 = successful resume */
	eret

.L_no_magic:
	/* Marker absent — unexpected entry, spin to avoid executing garbage */
	b	.L_no_magic
