# AM62L RTC PM Implementation - Executive Summary

## Bottom Line

**Zephyr's AM62L RTC driver is missing ALL power management hooks that Linux has.**

This makes S2RAM with RTC wakeup potentially unsafe because:
1. No suspend hook to configure poweroff state
2. No resume hook to clean up after wakeup
3. RTC may be left in unstable poweroff-armed state

## The Problem in 3 Lines

| | Linux | Zephyr |
|---|---|---|
| **Suspend hook** | ✅ Configures SW_OFF, PWR_OFF, wakeup GPIOs | ❌ **MISSING** |
| **Resume hook** | ✅ Clears SW_OFF, PWR_OFF, reloads 32k status | ❌ **MISSING** |
| **Impact** | S2RAM works reliably | RTC in undefined state after resume |

## What Linux Does That Zephyr Doesn't

### On Suspend (ti_k3_rtc_suspend → k3rtc_analog_suspend)
```c
1. Set PWR_OFF bit      → Signal PMIC for power transition
2. Set WKUP_POL         → Configure GPIO active-high/low
3. Enable GPIO IRQs     → Enable wakeup on external pins
4. Set WKUP_EN          → Enable wakeup functionality
5. Set SW_OFF bit       → Arm software poweroff
6. enable_irq_wake()    → Mark IRQ as wakeup source
```

### On Resume (ti_k3_rtc_resume → k3rtc_analog_resume)
```c
1. Clear SW_OFF         → De-arm poweroff (CRITICAL!)
2. Clear WRT_ERR        → Clear write error flag
3. Disable all IRQs     → Clear GPIO wakeup IRQs
4. Clear WKUP_EN        → Disable wakeup functionality
5. Read & clear status  → Determine what caused wake
6. Clear PWR_OFF        → Clear PMIC signal
7. Set RELOAD_FROM_BBD  → Sync status from 32k domain
8. disable_irq_wake()   → Unmark IRQ as wakeup source
```

**Zephyr does NONE of this.**

## Why This Matters

### Scenario: Add Suspend Hook Without Resume

```
User adds PM suspend hook to set SW_OFF/PWR_OFF:
  ✅ S2RAM entry works
  ✅ RTC alarm fires
  ✅ System wakes up
  
But resume hook is missing:
  🚨 SW_OFF = 1 (still set!)
  🚨 PWR_OFF = 1 (still set!)
  🚨 System may re-enter poweroff immediately
  🚨 RTC writes may fail (WRT_ERR not cleared)
  🚨 Status from 32k domain not reloaded (stale state)
```

**Takeaway:** You MUST implement BOTH suspend AND resume, or NEITHER.

## Current Zephyr State

The Zephyr counter driver has:
- ✅ Basic RTC counter read/write
- ✅ Dual alarm support (2 channels)
- ✅ IRQ handling for alarms
- ✅ Fence/sync for register writes
- ❌ **NO PM HOOKS AT ALL**
- ❌ No GPIO wakeup pins
- ❌ No NVMEM scratch registers
- ❌ No clock calibration

## Code Location Comparison

| Component | Linux | Zephyr |
|-----------|-------|--------|
| **Driver file** | `drivers/rtc/rtc-ti-k3.c` (1532 lines) | `drivers/counter/counter_k3_rtc.c` (528 lines) |
| **Driver type** | RTC subsystem | Counter subsystem |
| **PM hooks** | Lines 1467-1515 | ❌ None |
| **Analog suspend** | Lines 312-364 | ❌ None |
| **Analog resume** | Lines 366-458 | ❌ None |
| **SOC PM code** | `arch/arm64/boot/dts/.../am62l.dtsi` | `soc/ti/k3/am62lx/power.c` |
| **SOC handles RTC?** | No - driver does it | No - **nobody does it!** |

## What Needs to Be Added to Zephyr

### Minimal (Critical for S2RAM safety)

```c
PM_DEVICE_ACTION_SUSPEND:
  1. Set PWR_OFF = 1
  2. Set SW_OFF = 1
  3. Fence

PM_DEVICE_ACTION_RESUME:
  1. If (GENRAL_CTL & PWR_OFF):  // RTC retained power
       Clear SW_OFF
       Clear WRT_ERR
       Disable all IRQs
       Clear WKUP_EN
       Clear PWR_OFF
       Set RELOAD_FROM_BBD
       Fence
  2. Else:  // RTC lost context
       Full re-init
```

### Additional (for feature parity)

- GPIO wakeup chip (4 pins)
- IRQ domain for nested GPIO IRQs
- NVMEM provider for scratch registers
- Clock calibration API
- Erratum i2327 check
- Lock/unlock around sensitive ops

## Files to Review

1. **Linux RTC driver:**
   `/home/a0510137local/ti/ti-linux-kernel/drivers/rtc/rtc-ti-k3.c`
   - Focus on: `ti_k3_rtc_suspend()`, `ti_k3_rtc_resume()`
   - Focus on: `k3rtc_analog_suspend()`, `k3rtc_analog_resume()`

2. **Zephyr counter driver:**
   `/home/a0510137local/ti/zephyrproject/zephyr/drivers/counter/counter_k3_rtc.c`
   - Missing: All PM device hooks
   - Has: Basic counter + alarm functionality

3. **Zephyr SOC power code:**
   `/home/a0510137local/ti/zephyrproject/zephyr/soc/ti/k3/am62lx/power.c`
   - Handles: SMP CPU coordination only
   - Does NOT handle: RTC PM configuration

## Action Items

1. **Add PM_DEVICE_DEFINE() to counter_k3_rtc.c**
   - Implement `k3_rtc_pm_action()` callback
   - Handle SUSPEND and RESUME actions

2. **Test thoroughly**
   - Verify SW_OFF/PWR_OFF set on suspend
   - Verify SW_OFF/PWR_OFF cleared on resume
   - Verify RELOAD_FROM_BBD executed
   - Verify WRT_ERR cleared

3. **Optional enhancements**
   - GPIO wakeup pins (separate driver or integrate?)
   - NVMEM provider
   - Calibration

## References

- TI AM62L TRM Section on RTC
- Linux driver commit history
- Zephyr PM device documentation
- PSCI spec for system suspend

---

**Created:** 2026-08-07
**Author:** Analysis based on Linux rtc-ti-k3.c and Zephyr counter_k3_rtc.c comparison
