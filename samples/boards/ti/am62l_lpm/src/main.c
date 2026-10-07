/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AM62L S2RAM (suspend-to-RAM) demo.
 *
 * Each cycle:
 *   1. Arm the wakeup RTC alarm.
 *   2. CPU0 signals CPU1 via semaphore (no pm_state_force() for CPU1: its
 *      control thread calls pm_state_set() directly, so a forced slot would
 *      go stale and make CPU1 power itself off again after resume).
 *   3. CPU1's cpu1_pm_ctrl (highest-priority, CPU1-pinned) wakes and calls
 *      pm_state_set(PM_STATE_SUSPEND_TO_RAM, 0) directly -> pm_cpu_off().
 *   4. CPU0 polls am62l_wait_cpu1_off() (bounded, sleepable) to confirm
 *      CPU1 reached PSCI AFF_STATE_OFF; skips this cycle on timeout.
 *   5. CPU0 forces PM_STATE_SUSPEND_TO_RAM for itself and calls its own
 *      pm_system_suspend() -> arch_pm_s2ram_suspend() -> PSCI SYSTEM_SUSPEND.
 *   6. TF-A gates clocks, puts DDR into self-refresh and powers down the A53.
 *   7. RTC alarm fires.  TF-A resumes the A53 at arch_pm_s2ram_resume.
 *   8. PM_DEVICE_SYSTEM_MANAGED issues TURN_ON on all devices.
 *   9. CPU0 calls k_smp_cpu_start(1, cpu1_pm_ctrl_restart, NULL) to recreate
 *      cpu1_pm_ctrl for the next cycle.
 *
 * The 'cycles' counter lives in DDR and proves RAM retention across
 * self-refresh (it increments each cycle and is never reset by firmware).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/pm/pm.h>
#include <zephyr/pm/policy.h>
#include <zephyr/drivers/counter.h>
#include <zephyr/sys_clock.h>
#include <zephyr/logging/log.h>
#include "power.h"

LOG_MODULE_REGISTER(am62l_lpm, LOG_LEVEL_INF);

#define WAKE_DELAY_S    10U

static const struct device *const rtc_dev  = DEVICE_DT_GET(DT_NODELABEL(wkup_rtc0));

/* Lives in DDR; survives self-refresh across suspend cycles. */
static uint32_t cycles;

static void rtc_alarm_cb(const struct device *dev, uint8_t chan_id,
			  uint32_t ticks, void *user_data)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(chan_id);
	ARG_UNUSED(ticks);
	ARG_UNUSED(user_data);
}

static int arm_rtc_alarm(void)
{
	struct counter_alarm_cfg alarm = {
		.callback = rtc_alarm_cb,
		.ticks    = counter_us_to_ticks(rtc_dev,
				(uint64_t)WAKE_DELAY_S * USEC_PER_SEC),
		.flags    = 0,
	};
	int ret;

	ret = counter_start(rtc_dev);
	if (ret < 0 && ret != -EALREADY) {
		LOG_ERR("counter_start: %d", ret);
		return ret;
	}

	ret = counter_set_channel_alarm(rtc_dev, 0, &alarm);
	if (ret < 0) {
		LOG_ERR("counter_set_channel_alarm: %d", ret);
	}

	return ret;
}

/* CPU1 demo task: prints "I am alive" every 500ms, forever.
 * Pinned to CPU1 via SYS_INIT(APPLICATION) so it's ready before CPU1 boots.
 */
K_THREAD_STACK_DEFINE(cpu1_task_stack, 4096);
static struct k_thread cpu1_task_thread;

static void cpu1_task(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	while (1) {
		printk("CPU1: I am alive\n");
		k_sleep(K_SECONDS(1));
	}
}

/* Create and pin both CPU1 threads at APPLICATION init level
 * (before z_smp_init() boots CPU1). Both are cooperative; cpu1_pm_ctrl's
 * higher priority means it is dispatched the next time cpu1_task yields
 * (its k_sleep() call) after the semaphore is given - cooperative threads
 * run to their own yield point regardless of what becomes ready meanwhile.
 */
static int cpu1_threads_init(void)
{
	/* cpu1_pm_ctrl: same creation power.c itself uses on every resume,
	 * so stack size / priority can't drift out of sync between the two
	 * call sites.
	 */
	cpu1_pm_ctrl_restart(NULL);

	/* cpu1_task at normal priority */
	k_thread_create(&cpu1_task_thread, cpu1_task_stack,
			K_THREAD_STACK_SIZEOF(cpu1_task_stack),
			cpu1_task, NULL, NULL, NULL,
			K_PRIO_COOP(7), 0, K_FOREVER);
	k_thread_cpu_mask_clear(&cpu1_task_thread);
	k_thread_cpu_mask_enable(&cpu1_task_thread, 1);
	k_thread_start(&cpu1_task_thread);

	return 0;
}

SYS_INIT(cpu1_threads_init, APPLICATION, 0);

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

	LOG_INF("AM62L S2RAM demo — RTC wakeup");
	LOG_INF("DDR-retained cycle counter at boot: %u", cycles);

	if (!device_is_ready(rtc_dev)) {
		LOG_ERR("RTC not ready");
		return -ENODEV;
	}

	for (;;) {
		cycles++;
		LOG_INF("--- Cycle %u ---", cycles);

		if (arm_rtc_alarm() < 0) {
			LOG_ERR("Failed to arm RTC alarm — aborting");
			break;
		}

		LOG_INF("RTC alarm set for %u s, entering S2RAM...", WAKE_DELAY_S);

		/* Step 1: Kick CPU1 first so its shutdown proceeds while
		 * CPU0 confirms it below.
		 */
		k_sem_give(&cpu1_pm_sem);

		/* Step 2: Confirm CPU1 actually reached PSCI AFF_STATE_OFF
		 * before risking PSCI_SYSTEM_SUSPEND - see power.c's
		 * am62l_wait_cpu1_off() comment for why this can't be
		 * skipped (TF-A's own internal poll can silently give up
		 * and still report success).
		 */
		if (!am62l_wait_cpu1_off()) {
			LOG_WRN("CPU1 did not shut down in time, skipping S2RAM this cycle");
			k_msleep((WAKE_DELAY_S * 3U) * MSEC_PER_SEC);
			continue;
		}

		/* Step 3: CPU0 enters S2RAM */
		pm_state_force(0U, &s2ram_state);

		LOG_INF("Going into S2RAM...");
		/*
		 * Yield: idle thread consumes the forced state and calls
		 * pm_state_set(S2RAM).  Returns here after full resume and
		 * PM_DEVICE_ACTION_TURN_ON has been issued to all devices.
		 */
		k_msleep((WAKE_DELAY_S * 3U) * MSEC_PER_SEC);
		LOG_INF("Resumed from S2RAM (DDR-retained counter: %u)", cycles);
	}

	return 0;
}
