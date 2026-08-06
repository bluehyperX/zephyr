// SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
// SPDX-License-Identifier: Apache-2.0

/*
 * AM62L SMP S2RAM helpers for the secondary CPU (CPU1).
 *
 * CPU1 must enter PSCI CPU_SUSPEND with LPM_PSTATE_SEC before CPU0 can
 * issue its own CPU_SUSPEND with the deep-sleep power state.  TF-A holds
 * CPU1 in an EL3 WFI after system resume until the kernel sends SGI1.
 *
 * am62l_cpu1_s2ram_suspend():
 *   Saves CPU1 callee-saved GPRs on its stack, SP and VBAR_EL1 to
 *   _cpu1_context, then calls psci_cpu_suspend(LPM_PSTATE_SEC, am62l_cpu1_resume).
 *   On return (resume): restores context and returns to caller.
 *
 * am62l_cpu1_resume():
 *   TF-A resume entry point for CPU1.  Restores VBAR_EL1 and SP from
 *   _cpu1_context, pops callee-saved GPRs, returns 0 to the
 *   am62l_cpu1_s2ram_suspend call site.
 */

#include <zephyr/toolchain.h>
#include <zephyr/arch/cpu.h>

_ASM_FILE_PROLOGUE

/*
 * LPM power state for the secondary CPU suspend.
 * TF-A am62l_validate_power_state recognises 0x00012233 as the
 * secondary-cpu-off state and gates only this core's power domain.
 */
#define LPM_PSTATE_SEC	0x00012233

GDATA(_cpu1_context)

/*
 * int am62l_cpu1_s2ram_suspend(void)
 *
 * Called on CPU1 in response to SGI from CPU0.
 * Saves context, calls psci_cpu_suspend, returns on resume.
 */
GTEXT(am62l_cpu1_s2ram_suspend)
SECTION_FUNC(text, am62l_cpu1_s2ram_suspend)
	stp	x19, x20, [sp, #-16]!
	stp	x21, x22, [sp, #-16]!
	stp	x23, x24, [sp, #-16]!
	stp	x25, x26, [sp, #-16]!
	stp	x27, x28, [sp, #-16]!
	stp	x29, x30, [sp, #-16]!	/* FP + LR */

	/* Save SP and VBAR_EL1 to _cpu1_context */
	adrp	x0, _cpu1_context
	add	x0, x0, :lo12:_cpu1_context
	mov	x1, sp
	str	x1, [x0, #0]		/* _cpu1_context.sp */
	mrs	x1, vbar_el1
	str	x1, [x0, #8]		/* _cpu1_context.vbar_el1 */
	dsb	sy

	/* psci_cpu_suspend(LPM_PSTATE_SEC, am62l_cpu1_resume) */
	mov	w0, #(LPM_PSTATE_SEC & 0xffff)
	movk	w0, #((LPM_PSTATE_SEC >> 16) & 0xffff), lsl #16
	adrp	x1, am62l_cpu1_resume
	add	x1, x1, :lo12:am62l_cpu1_resume
	bl	psci_cpu_suspend		/* returns only on error or resume */

	/* Resume / error path: restore and return */
	ldp	x29, x30, [sp], #16
	ldp	x27, x28, [sp], #16
	ldp	x25, x26, [sp], #16
	ldp	x23, x24, [sp], #16
	ldp	x21, x22, [sp], #16
	ldp	x19, x20, [sp], #16
	ret

/*
 * am62l_cpu1_resume
 *
 * TF-A resumes CPU1 here at EL1 after system wakeup.  TF-A has already
 * restored EL1 system registers.  We restore VBAR_EL1 and SP from
 * _cpu1_context, pop callee-saved GPRs, and return 0 to the
 * psci_cpu_suspend call site inside am62l_cpu1_s2ram_suspend.
 */
GTEXT(am62l_cpu1_resume)
SECTION_FUNC(text, am62l_cpu1_resume)
	adrp	x0, _cpu1_context
	add	x0, x0, :lo12:_cpu1_context
	ldr	x1, [x0, #8]
	msr	vbar_el1, x1
	isb
	ldr	x1, [x0, #0]
	mov	sp, x1

	ldp	x29, x30, [sp], #16
	ldp	x27, x28, [sp], #16
	ldp	x25, x26, [sp], #16
	ldp	x23, x24, [sp], #16
	ldp	x21, x22, [sp], #16
	ldp	x19, x20, [sp], #16

	mov	x0, #0
	ret
