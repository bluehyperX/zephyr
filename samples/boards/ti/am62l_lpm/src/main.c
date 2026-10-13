/*
 * SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * AM62L S2RAM (suspend-to-RAM) demo.
 *
 * Each cycle:
 *   1. Arm the wakeup RTC alarm.
 *   2. CPU0 creates a highest-priority, CPU1-pinned control thread that
 *      calls pm_state_set(S2RAM) directly to power CPU1 off via PSCI CPU_OFF.
 *   3. CPU0 forces PM_STATE_SUSPEND_TO_RAM for itself. Its idle thread
 *      consumes the forced state and calls pm_state_set(), which first
 *      polls psci_affinity_info() until CPU1 is OFF (the cycle is skipped
 *      on timeout), then calls arch_pm_s2ram_suspend().
 *   4. TF-A gates clocks, puts DDR into self-refresh, powers down both A53s.
 *   5. RTC alarm fires. TF-A resumes CPU0, which restores its context.
 *      Devices are not suspended/resumed by the PM core in this flow: CPU1
 *      is powered off directly, so _cpus_active never drops to zero.
 *   6. pm_state_set() on CPU0 calls k_smp_cpu_start() to restart CPU1.
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

LOG_MODULE_REGISTER(am62l_lpm, LOG_LEVEL_INF);

#define WAKE_DELAY_S    10U

static const struct device *const rtc_dev = DEVICE_DT_GET(DT_NODELABEL(wkup_rtc0));

/* Forced S2RAM state used by both CPUs. */
static const struct pm_state_info s2ram_state = {
	.state = PM_STATE_SUSPEND_TO_RAM,
};

/* Lives in DDR; survives self-refresh across suspend cycles. */
static uint32_t cycles;

/* CPU1 control thread stack and struct (reused each cycle). */
K_THREAD_STACK_DEFINE(cpu1_pm_ctrl_stack, 4096);
static struct k_thread cpu1_pm_ctrl_thread;

/* CPU1 control thread entry: calls pm_state_set(S2RAM) directly. */
static void cpu1_pm_ctrl(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	pm_state_set(PM_STATE_SUSPEND_TO_RAM, 0);
	/* pm_cpu_off() never returns on success. */
}

/* CPU1 demo task: prints "I am alive" every 1 s (k_sleep), forever.
 * Pinned to CPU1 via SYS_INIT(APPLICATION) so it's ready before CPU1 boots.
 */
K_THREAD_STACK_DEFINE(cpu1_task_stack, 4096);
static struct k_thread cpu1_task_thread;

static void cpu1_task(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	uint32_t n = 0;

	while (1) {
		printk("CPU1: I am alive (%u)\n", n++);
		k_sleep(K_SECONDS(1));
	}
}

/* Create and pin CPU1 demo task at APPLICATION init level
 * (before z_smp_init() boots CPU1).
 */
static int cpu1_threads_init(void)
{
	/* cpu1_task at normal priority */
	k_thread_create(&cpu1_task_thread, cpu1_task_stack,
			K_THREAD_STACK_SIZEOF(cpu1_task_stack),
			cpu1_task, NULL, NULL, NULL,
			K_PRIO_PREEMPT(0), 0, K_FOREVER);
	k_thread_cpu_mask_clear(&cpu1_task_thread);
	k_thread_cpu_mask_enable(&cpu1_task_thread, 1);
	k_thread_start(&cpu1_task_thread);

	return 0;
}

SYS_INIT(cpu1_threads_init, APPLICATION, 0);

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

int main(void)
{
	LOG_INF("AM62L S2RAM demo - RTC wakeup");
	LOG_INF("DDR-retained cycle counter at boot: %u", cycles);

	if (!device_is_ready(rtc_dev)) {
		LOG_ERR("RTC not ready");
		return 0;
	}

	/*
	 * Suspend-to-RAM is the only power state in the devicetree. Lock it so
	 * the idle threads never pick it on their own; pm_state_force() ignores
	 * the lock.
	 */
	pm_policy_state_lock_get(PM_STATE_SUSPEND_TO_RAM, PM_ALL_SUBSTATES);

	for (;;) {
		cycles++;
		LOG_INF("--- Cycle %u ---", cycles);

		if (arm_rtc_alarm() < 0) {
			LOG_ERR("Failed to arm RTC alarm - aborting");
			break;
		}

		LOG_INF("RTC alarm set for %u s, entering S2RAM...", WAKE_DELAY_S);

		/* Create the CPU1 control thread. It preempts cpu1_task and
		 * powers CPU1 off via pm_state_set() -> pm_cpu_off().
		 */
		k_thread_create(&cpu1_pm_ctrl_thread, cpu1_pm_ctrl_stack,
				K_THREAD_STACK_SIZEOF(cpu1_pm_ctrl_stack),
				cpu1_pm_ctrl, NULL, NULL, NULL,
				K_HIGHEST_APPLICATION_THREAD_PRIO, 0, K_FOREVER);
		k_thread_cpu_mask_clear(&cpu1_pm_ctrl_thread);
		k_thread_cpu_mask_enable(&cpu1_pm_ctrl_thread, 1);
		k_thread_start(&cpu1_pm_ctrl_thread);

		/* Force S2RAM on CPU0; its idle thread consumes the state. */
		pm_state_force(0U, &s2ram_state);

		LOG_INF("Going into S2RAM...");
		/*
		 * Yield: idle thread consumes the forced state and calls
		 * pm_state_set(S2RAM). Returns here after full resume.
		 * The 1 s sleep gives CPU0 time to enter idle and consume
		 * the forced state before the cycle loop repeats.
		 */
		k_sleep(K_SECONDS(1));
		LOG_INF("Resumed from S2RAM (DDR-retained counter: %u)", cycles);
	}

	return 0;
}
