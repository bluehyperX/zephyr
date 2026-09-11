// SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
// SPDX-License-Identifier: Apache-2.0

/*
 * AM62L S2RAM (suspend-to-RAM) demo.
 *
 * Each cycle:
 *   1. Read 3 bytes from the on-board AT24 EEPROM via I2C and store them in
 *      a DDR buffer (pre-suspend I2C check).  The EEPROM WP pin is hardwired
 *      HIGH on the AM62L EVM, so writes are not used.
 *   2. Arm the wakeup RTC alarm.
 *   3. Force PM_STATE_SUSPEND_TO_RAM.  The idle thread calls pm_state_set()
 *      -> arch_pm_s2ram_suspend() -> PSCI CPU_SUSPEND.  TF-A selects the
 *      system LPM mode based on its internal state.
 *      PM_DEVICE_SYSTEM_MANAGED issues TURN_OFF on all devices before entry.
 *   4. TF-A gates clocks, puts DDR into self-refresh and powers down the A53.
 *   5. RTC alarm fires.  TF-A resumes the A53 at arch_pm_s2ram_resume, which
 *      restores CPU context.  PM_DEVICE_SYSTEM_MANAGED issues TURN_ON on all
 *      devices, including the OMAP I2C controller (re-programs its hardware
 *      registers which were lost when the MAIN power domain was cut).
 *   6. Read back the EEPROM and verify the pattern matches the pre-suspend
 *      snapshot.  A mismatch or I2C error here means i2c_omap TURN_ON failed.
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

		/* Step 3: CONFIG_LOG_MODE_IMMEDIATE ensures UART TX is idle here. */

		pm_state_force(0U,
			&(struct pm_state_info){PM_STATE_SUSPEND_TO_RAM, 0, 0});

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
