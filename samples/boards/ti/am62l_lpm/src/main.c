// SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
// SPDX-License-Identifier: Apache-2.0

/*
 * AM62L S2RAM (suspend-to-RAM) demo — single CPU (CPU0), JTAG wakeup.
 *
 * Each cycle:
 *   1. Force PM_STATE_SUSPEND_TO_RAM.  The idle thread calls pm_state_set()
 *      -> arch_pm_s2ram_suspend() -> PSCI CPU_SUSPEND.  TF-A selects the
 *      system LPM mode based on its internal state.
 *      PM_DEVICE_SYSTEM_MANAGED issues TURN_OFF on all devices before entry.
 *   2. TF-A gates clocks, puts DDR into self-refresh and powers down the A53.
 *   3. JTAG hardware wakeup fires.  TF-A resumes the A53 at
 *      arch_pm_s2ram_resume, which restores CPU context.
 *      PM_DEVICE_SYSTEM_MANAGED issues TURN_ON on all devices.
 *
 * The 'cycles' counter lives in DDR and proves RAM retention across
 * self-refresh (it increments each cycle and is never reset by firmware).
 */

#include <zephyr/kernel.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/policy.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(am62l_lpm, LOG_LEVEL_INF);

/* Lives in DDR; survives self-refresh across suspend cycles. */
static uint32_t cycles;

int main(void)
{
	/*
	 * Lock all PM states before anything else.  The idle thread starts
	 * immediately on both cores; without these locks it can select S2RAM
	 * before the RTC alarm is armed, causing a premature suspend.
	 */
	pm_policy_state_lock_get(PM_STATE_RUNTIME_IDLE, PM_ALL_SUBSTATES);
	pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_IDLE, PM_ALL_SUBSTATES);
	pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);

	LOG_INF("AM62L S2RAM demo — JTAG wakeup, single CPU");
	LOG_INF("DDR-retained cycle counter at boot: %u", cycles);

	for (;;) {
		cycles++;
		LOG_INF("--- Cycle %u ---", cycles);

		pm_state_force(0U,
			&(struct pm_state_info){PM_STATE_SUSPEND_TO_RAM, 0, 0});

		/*
		 * Yield to the idle thread, which consumes the forced state and
		 * calls pm_state_set(S2RAM).  Returns here after JTAG wakeup,
		 * full resume, and PM_DEVICE_ACTION_TURN_ON on all devices.
		 */
		LOG_INF("Going into S2RAM...");
		k_msleep(5 * MSEC_PER_SEC);
		LOG_INF("Resumed from S2RAM (DDR-retained counter: %u)", cycles);
	}

	return 0;
}
