// SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
// SPDX-License-Identifier: Apache-2.0

#include <zephyr/arch/cpu.h>
#include <zephyr/linker/sections.h>
#include <zephyr/arch/common/pm_s2ram.h>

#define MAGIC (0xDABBAD00U)

__noinit _cpu_context_t _cpu_context;

#ifndef CONFIG_HAS_PM_S2RAM_CUSTOM_MARKING
static __noinit uint32_t marker;

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
