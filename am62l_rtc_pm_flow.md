# AM62L RTC PM Flow Comparison

## Linux S2RAM Flow with RTC

```
┌─────────────────────────────────────────────────────────────────────┐
│                         LINUX S2RAM FLOW                              │
└─────────────────────────────────────────────────────────────────────┘

SUSPEND PHASE:
┌──────────────┐
│   Userspace  │
│  echo mem >  │
│ /sys/power/  │
│    state     │
└──────┬───────┘
       │
       v
┌──────────────────────────────────────────────────────────────────────┐
│ PM Core: Freeze processes, suspend devices in order                   │
└────────┬───────────────────────────────────────────────────────────┬─┘
         │                                                            │
         v                                                            v
┌─────────────────┐                                         ┌─────────────────┐
│ ti_k3_rtc_      │ ◄──────── CRITICAL ────────────────────►│  Zephyr RTC     │
│   suspend()     │                                         │  (counter_k3)   │
│                 │                                         │                 │
│ 1. Unlock RTC   │                                         │  ❌ NO PM HOOK  │
│ 2. Set PWR_OFF  │                                         │                 │
│ 3. Set WKUP_POL │                                         │  ❌ No suspend  │
│ 4. Enable GPIO  │                                         │     handler     │
│    wakeup IRQs  │                                         │                 │
│ 5. Set WKUP_EN  │                                         │  ❌ No poweroff │
│ 6. Set SW_OFF   │                                         │     config      │
│ 7. Lock RTC     │                                         │                 │
│ 8. enable_irq_  │                                         │  ❌ No IRQ wake │
│    wake()       │                                         │                 │
└─────────────────┘                                         └─────────────────┘
         │
         v
┌──────────────────────────────────────────────────────────────────────┐
│ PSCI CPU_SUSPEND → TF-A → TIFS → DDR self-refresh + A53 power-down    │
└──────────────────────────────────────────────────────────────────────┘
         │
         │ [RTC ALARM FIRES]
         │ TF-A detects wakeup event
         v
┌──────────────────────────────────────────────────────────────────────┐
│ TF-A restores EL1 context → ERET to arch_pm_s2ram_resume             │
└────────┬─────────────────────────────────────────────────────────────┘
         │
         v
RESUME PHASE:
┌─────────────────┐                                         ┌─────────────────┐
│ ti_k3_rtc_      │ ◄──────── CRITICAL ────────────────────►│  Zephyr RTC     │
│   resume()      │                                         │  (counter_k3)   │
│                 │                                         │                 │
│ 1. Check if RTC │                                         │  ❌ NO PM HOOK  │
│    retained     │                                         │                 │
│    power (PWR_  │                                         │  ❌ No resume   │
│    OFF bit)     │                                         │     handler     │
│                 │                                         │                 │
│ 2. Clear SW_OFF │◄────── WITHOUT THIS, RTC STAYS ───────►│  ❌ SW_OFF may  │
│    (forced      │        IN POWEROFF-ARMED STATE!        │     still be 1! │
│    write)       │        System may re-enter suspend     │                 │
│                 │                                         │                 │
│ 3. Clear write  │                                         │  ❌ No WRT_ERR  │
│    error (WRT_  │                                         │     clear       │
│    ERR)         │                                         │                 │
│                 │                                         │                 │
│ 4. Disable all  │                                         │  ❌ Wakeup IRQs │
│    wakeup IRQs  │                                         │     still on    │
│                 │                                         │                 │
│ 5. Clear WKUP_  │                                         │  ❌ WKUP_EN may │
│    EN           │                                         │     still be 1  │
│                 │                                         │                 │
│ 6. Read & clear │                                         │  ❌ IRQ status  │
│    IRQ status   │                                         │     not cleared │
│                 │                                         │                 │
│ 7. Clear PWR_   │                                         │  ❌ PWR_OFF may │
│    OFF          │                                         │     still be 1  │
│                 │                                         │                 │
│ 8. Reload from  │◄────── CRITICAL FOR STATUS ───────────►│  ❌ Status may  │
│    32k domain   │        CONSISTENCY!                    │     be stale    │
│    (RELOAD_FROM_│                                         │                 │
│    BBD)         │                                         │                 │
│                 │                                         │                 │
│ 9. disable_irq_ │                                         │  ❌ No IRQ wake │
│    wake()       │                                         │     disable     │
└─────────────────┘                                         └─────────────────┘
         │
         v
┌──────────────────────────────────────────────────────────────────────┐
│ PM Core: Resume devices, thaw processes                               │
└──────────────────────────────────────────────────────────────────────┘
```

---

## Zephyr S2RAM Flow (Current Implementation)

```
┌─────────────────────────────────────────────────────────────────────┐
│                        ZEPHYR S2RAM FLOW                              │
└─────────────────────────────────────────────────────────────────────┘

SUSPEND PHASE:
┌──────────────┐
│ Application  │
│ pm_state_    │
│  force()     │
└──────┬───────┘
       │
       v
┌──────────────────────────────────────────────────────────────────────┐
│ PM Subsystem: Suspend all PM_DEVICE_SYSTEM_MANAGED devices           │
└────────┬──────────────────────────────────────────────────────────┬──┘
         │                                                           │
         v                                                           v
┌─────────────────┐                                        ┌─────────────────┐
│  Other drivers  │                                        │   RTC counter   │
│  get SUSPEND    │                                        │   driver        │
│  action         │                                        │                 │
│                 │                                        │  ⚠️  SKIPPED!   │
│  (UARTs, I2C,   │                                        │                 │
│   GPIO, etc)    │                                        │  Counter driver │
│                 │                                        │  has no PM      │
└─────────────────┘                                        │  hooks defined  │
         │                                                 │                 │
         │                                                 │  RTC state not  │
         │                                                 │  saved, poweroff│
         │                                                 │  not configured │
         v                                                 └─────────────────┘
┌──────────────────────────────────────────────────────────────────────┐
│ soc/ti/k3/am62lx/power.c: pm_state_set(PM_STATE_SUSPEND_TO_RAM)      │
│                                                                        │
│  1. Signal CPU1 to PSCI CPU_OFF (if SMP)                              │
│  2. Poll until CPU1 is AFF_STATE_OFF                                  │
│  3. arch_pm_s2ram_suspend() → psci_system_suspend()                   │
│                                                                        │
│  ⚠️  No RTC configuration here — expects RTC driver to do it!         │
└──────────────────────────────────────────────────────────────────────┘
         │
         v
┌──────────────────────────────────────────────────────────────────────┐
│ PSCI CPU_SUSPEND → TF-A → TIFS → DDR self-refresh + A53 power-down    │
└──────────────────────────────────────────────────────────────────────┘
         │
         │ [RTC ALARM FIRES]
         │ TF-A detects wakeup event
         v
┌──────────────────────────────────────────────────────────────────────┐
│ TF-A restores EL1 context → ERET to arch_pm_s2ram_resume             │
└────────┬─────────────────────────────────────────────────────────────┘
         │
         v
RESUME PHASE:
┌──────────────────────────────────────────────────────────────────────┐
│ soc/ti/k3/am62lx/power.c: arch_pm_s2ram_suspend() returns            │
│                                                                        │
│  1. k_smp_cpu_resume(1) to restart CPU1 (if SMP)                      │
│  2. Returns to PM subsystem                                           │
│                                                                        │
│  ⚠️  No RTC cleanup here — expects RTC driver to do it!               │
└────────┬─────────────────────────────────────────────────────────────┘
         │
         v
┌──────────────────────────────────────────────────────────────────────┐
│ PM Subsystem: Resume all PM_DEVICE_SYSTEM_MANAGED devices            │
└────────┬──────────────────────────────────────────────────────────┬──┘
         │                                                           │
         v                                                           v
┌─────────────────┐                                        ┌─────────────────┐
│  Other drivers  │                                        │   RTC counter   │
│  get RESUME     │                                        │   driver        │
│  action         │                                        │                 │
│                 │                                        │  ⚠️  SKIPPED!   │
│  (UARTs, I2C,   │                                        │                 │
│   GPIO, etc)    │                                        │  Counter driver │
│                 │                                        │  has no PM      │
└─────────────────┘                                        │  hooks defined  │
         │                                                 │                 │
         │                                                 │  🚨 DANGER!     │
         │                                                 │                 │
         │                                                 │  SW_OFF=1       │
         │                                                 │  PWR_OFF=1      │
         │                                                 │  WKUP_EN=?      │
         │                                                 │                 │
         │                                                 │  RTC may be in  │
         │                                                 │  poweroff-armed │
         │                                                 │  state!         │
         v                                                 └─────────────────┘
┌──────────────────────────────────────────────────────────────────────┐
│ Application continues — but RTC is in UNDEFINED STATE!                │
│                                                                        │
│ Possible outcomes:                                                     │
│  - System may re-enter poweroff mode unexpectedly                     │
│  - RTC alarms may not work correctly                                  │
│  - RTC status may be stale from before suspend                        │
│  - Write errors may prevent RTC updates                               │
└──────────────────────────────────────────────────────────────────────┘
```

---

## Key Observation

**Linux:** RTC driver takes full responsibility for suspend/resume PM configuration.

**Zephyr:** 
- SOC `power.c` only handles CPU SMP coordination
- RTC counter driver has **NO PM HOOKS** 
- Result: RTC left in unknown/dangerous state after S2RAM

**The fix must be in the RTC driver itself, not in SOC power.c!**

---

## Detailed Register State Comparison

### Before Suspend

| Register | Linux State | Zephyr State | Notes |
|----------|-------------|--------------|-------|
| GENRAL_CTL[16] (PWR_OFF) | 0 | 0 | Both clean |
| GENRAL_CTL[17] (SW_OFF) | 0 | 0 | Both clean |
| GENRAL_CTL[3:0] (WKUP_EN) | 0 | 0 | Both clean |
| GENRAL_CTL[7:4] (WKUP_POL) | Configured from GPIO | 0 | Linux tracks polarity |
| IRQENABLE_SET_SYS | Alarm only | Alarm only | Similar |

### After ti_k3_rtc_suspend() (Linux) / No Hook (Zephyr)

| Register | Linux State | Zephyr State | Notes |
|----------|-------------|--------------|-------|
| GENRAL_CTL[16] (PWR_OFF) | **1** ← Set | 0 ← Unchanged | ⚠️ Not configured for suspend! |
| GENRAL_CTL[17] (SW_OFF) | **1** ← Set | 0 ← Unchanged | ⚠️ Not configured for suspend! |
| GENRAL_CTL[3:0] (WKUP_EN) | **GPIO bitmask** ← Set | 0 ← Unchanged | ⚠️ No GPIO wakeup! |
| GENRAL_CTL[7:4] (WKUP_POL) | **GPIO polarity** ← Set | 0 ← Unchanged | N/A (no GPIO) |
| IRQENABLE_SET_SYS[4:2] | **GPIO IRQs** ← Enabled | 0 ← Unchanged | ⚠️ No GPIO IRQ wakeup! |
| IRQENABLE_SET_SYS[1] | **Alarm IRQ** ← Enabled | Enabled | ✅ Similar |

**Critical:** Without setting SW_OFF and PWR_OFF, the PMIC may not enter the correct power state!

### After RTC Alarm Wakeup (Hardware)

| Register | Linux State | Zephyr State | Notes |
|----------|-------------|--------------|-------|
| GENRAL_CTL[16] (PWR_OFF) | **1** ← Still set | 0 ← Never set | Must be cleared on resume! |
| GENRAL_CTL[17] (SW_OFF) | **1** ← Still set | 0 ← Never set | 🚨 DANGER if not cleared! |
| IRQSTATUS_SYS[1] | **1** ← Alarm fired | 1 ← Alarm fired | ✅ Similar |

### After ti_k3_rtc_resume() (Linux) / No Hook (Zephyr)

| Register | Linux State | Zephyr State | Notes |
|----------|-------------|--------------|-------|
| GENRAL_CTL[16] (PWR_OFF) | **0** ← Cleared | 0 ← Was never set | Safe in Zephyr (coincidence) |
| GENRAL_CTL[17] (SW_OFF) | **0** ← Cleared | 0 ← Was never set | Safe in Zephyr (coincidence) |
| GENRAL_CTL[3:0] (WKUP_EN) | **0** ← Cleared | 0 ← Unchanged | OK |
| IRQENABLE_SET_SYS | **Alarm only** ← GPIO cleared | Alarm only | ✅ Similar |
| IRQSTATUS_SYS | **0** ← All cleared | Cleared in IRQ handler | Different timing |
| SYNCPEND[31] (RELOAD_FROM_BBD) | **1 then 0** ← Reloaded | 0 ← Never reloaded | ⚠️ Stale status! |
| SYNCPEND[3] (WRT_ERR) | **Cleared** | Uncleared | ⚠️ May block writes! |

**Key Insight:** 
- In current Zephyr, SW_OFF/PWR_OFF are **never set**, so they're **never dangling** after resume
- BUT: This means S2RAM may not work correctly at all (PMIC not configured)
- If you **add** suspend hook to set SW_OFF/PWR_OFF, you **MUST** add resume hook to clear them!

---

## What Happens if We Add Suspend Hook Without Resume Hook?

### Scenario: Add PM suspend to set SW_OFF, but forget resume cleanup

```
SUSPEND:
  RTC suspend hook sets:
    - PWR_OFF = 1  ✅ Good
    - SW_OFF = 1   ✅ Good
    - WKUP_EN = 0x1 (RTC alarm)  ✅ Good

HARDWARE SUSPEND:
  TF-A → TIFS → DDR self-refresh → A53 power-down  ✅ Good

HARDWARE RESUME (RTC alarm fires):
  TF-A → EL1 restore → arch_pm_s2ram_resume  ✅ Good

RESUME WITHOUT HOOK:
  PM subsystem calls RESUME on all devices
  RTC driver: NO HOOK → Nothing happens
  
  Register state:
    - PWR_OFF = 1  🚨 STILL SET!
    - SW_OFF = 1   🚨 STILL SET!
    - WKUP_EN = 0x1  ⚠️ Still set
    
APPLICATION CONTINUES:
  Next time system tries to do anything with power:
    SW_OFF=1 may trigger immediate poweroff
    System enters an unstable state
```

**Conclusion:** Must add **BOTH** suspend and resume hooks, or **NEITHER**!

---

## Minimal Viable Fix for Zephyr

```c
/* In drivers/counter/counter_k3_rtc.c */

#ifdef CONFIG_PM_DEVICE
static int k3_rtc_pm_action(const struct device *dev, enum pm_device_action action)
{
    k3_rtc_regs_t *rtc_regs = DEV_REGS(dev);
    int ret;

    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        /* Enable PMIC power-off signal */
        rtc_regs->GENRAL_CTL |= BIT(16);  /* PWR_OFF */
        ret = k3_rtc_fence(dev);
        if (ret) return ret;
        
        /* Enable software poweroff */
        rtc_regs->GENRAL_CTL |= BIT(17);  /* SW_OFF */
        return k3_rtc_fence(dev);

    case PM_DEVICE_ACTION_RESUME:
        /* Clear SW_OFF if RTC retained power */
        if (rtc_regs->GENRAL_CTL & BIT(16)) {
            rtc_regs->GENRAL_CTL &= ~BIT(17);  /* Clear SW_OFF */
            ret = k3_rtc_fence(dev);
            if (ret) return ret;
            
            rtc_regs->SYNCPEND = BIT(3);  /* Clear WRT_ERR */
            rtc_regs->IRQENABLE_CLR_SYS = 0x3F;  /* Clear all IRQ enables */
            rtc_regs->GENRAL_CTL &= ~(0xF);  /* Clear WKUP_EN */
            rtc_regs->GENRAL_CTL &= ~BIT(16);  /* Clear PWR_OFF */
            ret = k3_rtc_fence(dev);
            if (ret) return ret;
            
            rtc_regs->SYNCPEND = BIT(31);  /* RELOAD_FROM_BBD */
            return k3_rtc_fence(dev);
        }
        return 0;

    default:
        return -ENOTSUP;
    }
}

PM_DEVICE_DT_INST_DEFINE(0, k3_rtc_pm_action);

/* Update DEVICE_DT_INST_DEFINE macro to include PM_DEVICE_DT_INST_GET(0) */
#endif /* CONFIG_PM_DEVICE */
```

This is the **minimum** to make S2RAM safe with RTC wakeup!
