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
 * Saves callee-saved GPRs (x19-x30) on the stack, saves SP and VBAR_EL1 to
 * _cpu_context, writes the suspend marker, then calls system_off.
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

	/* Save SP and VBAR_EL1 to _cpu_context */
	adrp	x0, _cpu_context
	add	x0, x0, :lo12:_cpu_context
	mov	x1, sp
	str	x1, [x0, #0]		/* _cpu_context.sp */
	mrs	x1, vbar_el1
	str	x1, [x0, #8]		/* _cpu_context.vbar_el1 */

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
 * TF-A has already restored EL1 system registers (SCTLR_EL1, TCR_EL1,
 * CPACR_EL1, MAIR_EL1) before branching here.  TF-A does NOT restore
 * SP_EL1, so we must reload it from _cpu_context before any C function call.
 *
 * Order: restore VBAR_EL1 + SP from _cpu_context first (no C stack needed),
 * then call pm_s2ram_mark_check_and_clear() with a valid SP, then pop
 * callee-saved GPRs from the DDR-retained stack and return x0=0.
 *
 * If the suspend marker is absent this entry point was reached unexpectedly;
 * spin forever rather than returning to an unknown call site.
 */
GTEXT(arch_pm_s2ram_resume)
SECTION_FUNC(text, arch_pm_s2ram_resume)
	/* Restore VBAR_EL1 and SP before any C call — TF-A does not save SP_EL1 */
	adrp	x0, _cpu_context
	add	x0, x0, :lo12:_cpu_context
	ldr	x1, [x0, #8]
	msr	vbar_el1, x1
	isb
	ldr	x1, [x0, #0]
	mov	sp, x1

	bl	pm_s2ram_mark_check_and_clear
	cbz	x0, .L_no_magic

	/* Pop callee-saved GPRs (DDR retained in self-refresh) */
	ldp	x29, x30, [sp], #16
	ldp	x27, x28, [sp], #16
	ldp	x25, x26, [sp], #16
	ldp	x23, x24, [sp], #16
	ldp	x21, x22, [sp], #16
	ldp	x19, x20, [sp], #16

	mov	x0, #0			/* return 0 = successful resume */
	ret

.L_no_magic:
	/* Marker absent — unexpected entry, spin to avoid executing garbage */
	b	.L_no_magic
