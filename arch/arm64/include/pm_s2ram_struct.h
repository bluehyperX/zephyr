/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_ARCH_ARM64_PM_S2RAM_STRUCT_H_
#define ZEPHYR_ARCH_ARM64_PM_S2RAM_STRUCT_H_

#ifdef CONFIG_PM_S2RAM

#include <stdint.h>

/*
 * CPU context saved across S2RAM. Firmware resets these registers on a system
 * suspend, so they are restored by pm_s2ram.S. GPRs x19-x30 are saved on the
 * stack. sctlr_el1 is restored last so the MMU is enabled only once
 * MAIR/TCR/TTBR0 are in place.
 */
struct __cpu_context {
	uint64_t sp;
	uint64_t vbar_el1;
	uint64_t mair_el1;
	uint64_t tcr_el1;
	uint64_t ttbr0_el1;
	uint64_t sctlr_el1;
	uint64_t cpacr_el1;
	uint64_t tpidrro_el0;
	uint64_t tpidr_el0;
	uint64_t sp_el0;
};

typedef struct __cpu_context _cpu_context_t;

#endif /* CONFIG_PM_S2RAM */

#endif /* ZEPHYR_ARCH_ARM64_PM_S2RAM_STRUCT_H_ */
