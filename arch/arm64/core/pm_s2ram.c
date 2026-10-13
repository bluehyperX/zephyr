/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <zephyr/devicetree.h>
#include <zephyr/arch/cpu.h>
#include <zephyr/linker/sections.h>
#include <zephyr/sys/util.h>
#include <zephyr/arch/common/pm_s2ram.h>
#include <kernel_arch_func.h>
#include <pm_s2ram_struct.h>

/*
 * Place the context and marker in the DT pm_s2ram region if defined,
 * otherwise in __noinit (which must then be retained across suspend).
 */
#if DT_NODE_EXISTS(DT_NODELABEL(pm_s2ram)) &&                                                      \
	DT_NODE_HAS_COMPAT(DT_NODELABEL(pm_s2ram), zephyr_memory_region)
#define PM_S2RAM_RETAINED                                                                          \
	__attribute__((section(DT_PROP(DT_NODELABEL(pm_s2ram), zephyr_memory_region))))
#else
#define PM_S2RAM_RETAINED __noinit
#endif

#define MAGIC (0xDABBAD00U)

PM_S2RAM_RETAINED _cpu_context_t _cpu_context;

#ifndef CONFIG_HAS_PM_S2RAM_CUSTOM_MARKING
static PM_S2RAM_RETAINED uint32_t marker;

void pm_s2ram_mark_set(void)
{
	marker = MAGIC;
}

bool pm_s2ram_mark_check_and_clear(void)
{
	if (marker == MAGIC) {
		marker = 0U;
		return true;
	}
	return false;
}
#endif /* CONFIG_HAS_PM_S2RAM_CUSTOM_MARKING */

extern int z_arm64_pm_s2ram_suspend(pm_s2ram_system_off_fn_t system_off);

int arch_pm_s2ram_suspend(pm_s2ram_system_off_fn_t system_off)
{
#ifdef CONFIG_FPU_SHARING
	/* The FP/SIMD registers are lost: hand them back to the owning thread. */
	arch_flush_local_fpu();
#endif
	return z_arm64_pm_s2ram_suspend(system_off);
}
