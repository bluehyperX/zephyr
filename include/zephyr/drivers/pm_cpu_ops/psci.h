/*
 * Copyright 2020 Carlo Caione <ccaione@baylibre.com>
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_PM_CPU_OPS_PSCI_H_
#define ZEPHYR_INCLUDE_DRIVERS_PM_CPU_OPS_PSCI_H_

#include <zephyr/types.h>
#include <zephyr/arch/arm64/arm-smccc.h>
#include <stddef.h>
#include <zephyr/device.h>

#ifdef __cplusplus
extern "C" {
#endif

/* PSCI version decoding (independent of PSCI version) */
#define PSCI_VERSION_MAJOR_SHIFT		16
#define PSCI_VERSION_MINOR_MASK			\
		((1U << PSCI_VERSION_MAJOR_SHIFT) - 1)
#define PSCI_VERSION_MAJOR_MASK			~PSCI_VERSION_MINOR_MASK

#define PSCI_VERSION_MAJOR(ver)			\
		(((ver) & PSCI_VERSION_MAJOR_MASK) >> PSCI_VERSION_MAJOR_SHIFT)
#define PSCI_VERSION_MINOR(ver)			\
		((ver) & PSCI_VERSION_MINOR_MASK)

uint32_t psci_version(void);

/**
 * @brief Function to call PSCI CPU_SUSPEND
 *
 * This function is API for CPU_SUSPEND PSCI SMC call.
 *
 * @param state CPU Power state
 * @param entry_point The address entry when returning from suspend
 *
 * @retval 0 if enter suspend successfully, others for failure
 */

int psci_cpu_suspend(uint32_t state, uintptr_t entry_point);

/**
 * @brief Function to call PSCI SYSTEM_SUSPEND
 *
 * Suspends the entire system. On wakeup, execution resumes at entry_point.
 *
 * @param entry_point Physical address of the resume entry point
 *
 * @retval 0 on successful suspend/resume, negative errno on failure
 */
int psci_system_suspend(uintptr_t entry_point);

/**
 * @brief Query the power state of a CPU via PSCI AFFINITY_INFO
 *
 * @param target_affinity MPIDR of the target CPU
 * @param lowest_affinity_level Lowest affinity level to query (0 = core)
 *
 * @retval 0 CPU is ON
 * @retval 1 CPU is OFF
 * @retval 2 CPU is ON_PENDING
 * @retval Negative PSCI error code on error (not converted to errno)
 */
int psci_affinity_info(uintptr_t target_affinity, uint32_t lowest_affinity_level);

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_PM_CPU_OPS_PSCI_H_ */
