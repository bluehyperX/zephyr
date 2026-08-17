# AM62L RTC Driver Comparison: Linux vs Zephyr

## Executive Summary

**Zephyr** implements the AM62L RTC as a **counter driver** (`drivers/counter/counter_k3_rtc.c`) with ~528 lines, providing basic alarm functionality.

**Linux** implements it as an **RTC driver** (`drivers/rtc/rtc-ti-k3.c`) with ~1532 lines, adding GPIO wakeup, power management, NVMEM, calibration, and comprehensive S2RAM support.

### Key Architectural Difference

- **Zephyr**: Counter API (tick-based, alarm callbacks)
- **Linux**: RTC API (time/date, alarm with IRQ framework, calendar semantics)

---

## PM Actions Comparison

### 1. **Suspend Hook** (`PM_DEVICE_ACTION_SUSPEND` vs `ti_k3_rtc_suspend`)

| Feature | Linux (ti_k3_rtc_suspend) | Zephyr | Status |
|---------|---------------------------|--------|--------|
| **Implementation** | ✅ Lines 1467-1484 | ❌ **MISSING** | **GAP** |
| Call to analog_suspend | ✅ Yes (for AM62L) | ❌ No | **GAP** |
| Configure SW_OFF | ✅ Yes | ❌ No | **GAP** |
| GPIO wakeup setup | ✅ Sets polarity + enable mask | ❌ No GPIO support | **GAP** |
| Enable IRQ wake | ✅ `enable_irq_wake(priv->irq)` | ❌ No | **GAP** |

**Linux behavior:**
```c
static int ti_k3_rtc_suspend(struct device *dev)
{
    if (priv->has_analog_block) {
        ret = k3rtc_analog_suspend(dev, priv);  // Configure poweroff
        if (ret) return ret;
    }
    
    if (device_may_wakeup(dev))
        return enable_irq_wake(priv->irq);
    return 0;
}
```

**Zephyr:** No PM hooks at all. Counter driver has no `pm_action_cb` defined.

---

### 2. **Resume Hook** (`PM_DEVICE_ACTION_RESUME` vs `ti_k3_rtc_resume`)

| Feature | Linux (ti_k3_rtc_resume) | Zephyr | Status |
|---------|--------------------------|--------|--------|
| **Implementation** | ✅ Lines 1486-1515 | ❌ **MISSING** | **GAP** |
| Check if RTC retained power | ✅ `k3rtc_is_rtc_already_configured()` | ❌ No | **GAP** |
| Cleanup poweroff config | ✅ `k3rtc_analog_resume()` | ❌ No | **GAP** |
| Clear SW_OFF bit | ✅ Forced write to GENERAL_CTL | ❌ No | **GAP** |
| Clear wakeup enables | ✅ Disables GPIO wakeup IRQs | ❌ No | **GAP** |
| Reload status from 32k domain | ✅ `RELOAD_FROM_BBD` | ❌ No | **GAP** |
| Reinit on context loss | ✅ Reconfigure if locked | ❌ No | **GAP** |
| Disable IRQ wake | ✅ `disable_irq_wake(priv->irq)` | ❌ No | **GAP** |

**Linux behavior:**
```c
static int ti_k3_rtc_resume(struct device *dev)
{
    if (priv->has_analog_block) {
        if (k3rtc_is_rtc_already_configured(priv)) {
            ret = k3rtc_analog_resume(dev, priv);  // Clear SW_OFF, wakeup state
        }
    } else if (k3rtc_check_unlocked(priv)) {
        ret = k3rtc_configure(dev);  // Re-init on context loss
    }
    
    if (device_may_wakeup(dev))
        disable_irq_wake(priv->irq);
    return 0;
}
```

**Zephyr:** No PM hooks. On resume from S2RAM, RTC is accessed directly without cleanup.

---

### 3. **Analog Suspend** (`k3rtc_analog_suspend`)

**Linux implementation** (lines 312-364):

```c
static int k3rtc_analog_suspend(struct device *dev, struct ti_k3_rtc *priv)
{
    guard(mutex)(&priv->mutex_lock);
    
    ret = k3rtc_unlock_rtc(dev, priv);
    
    /* Enable PMIC signal transition */
    k3rtc_field_write(priv, K3RTC_GEN_PWR_OFF, 1);
    ret = k3rtc_fence(priv);
    
    /* Configure wakeup polarity based on GPIO active-high/low */
    k3rtc_field_write(priv, K3RTC_GEN_WKUP_POL, priv->wakeup_polarity);
    
    /* Enable wakeup interrupts for GPIO pins set via irq_set_wake() */
    k3rtc_field_write(priv, K3RTC_IRQ_ENABLE_IO_WKUP, priv->wakeup_enabled);
    
    ret = k3rtc_lock_rtc(dev, priv);
    ret = k3rtc_unlock_rtc(dev, priv);
    
    /* Enable wakeup on configured GPIOs */
    k3rtc_field_write(priv, K3RTC_GEN_WKUP_EN, priv->wakeup_enabled);
    ret = k3rtc_fence(priv);
    
    /* Enable software poweroff */
    k3rtc_field_write(priv, K3RTC_GEN_SW_OFF, 1);
    ret = k3rtc_fence(priv);
    
    return 0;
}
```

**Zephyr:** ❌ **COMPLETELY MISSING**

---

### 4. **Analog Resume** (`k3rtc_analog_resume`)

**Linux implementation** (lines 366-458):

```c
static int k3rtc_analog_resume(struct device *dev, struct ti_k3_rtc *priv)
{
    /* Explicitly clear SW_OFF bit - forced write required */
    regmap_write_bits(priv->regmap, REG_K3RTC_GENERAL_CTL, K3RTC_SW_OFF_VAL, 0);
    ret = k3rtc_fence(priv);
    
    ret = k3rtc_lock_rtc(dev, priv);
    ret = k3rtc_unlock_rtc(dev, priv);
    
    /* Clear write error condition */
    k3rtc_field_write(priv, K3RTC_WRT_ERR, 0x1);
    
    /* Disable all wakeup-related interrupts */
    k3rtc_field_write(priv, K3RTC_IRQ_ENABLE_CLR_ALL, 0x1F);
    
    ret = k3rtc_lock_rtc(dev, priv);
    ret = k3rtc_fence(priv);
    ret = k3rtc_unlock_rtc(dev, priv);
    
    /* Clear wakeup enable bits */
    k3rtc_field_write(priv, K3RTC_GEN_WKUP_EN, 0);
    
    ret = k3rtc_lock_rtc(dev, priv);
    ret = k3rtc_unlock_rtc(dev, priv);
    
    /* Read interrupt source to determine wake reason */
    intr_src = k3rtc_field_read(priv, K3RTC_IRQ_STATUS_RAW_ALL);
    k3rtc_field_write(priv, K3RTC_IRQ_STATUS_ALL, intr_src);
    
    /* Clear the power off enable that was set during suspend */
    k3rtc_field_write(priv, K3RTC_GEN_PWR_OFF, 0);
    
    ret = k3rtc_lock_rtc(dev, priv);
    ret = k3rtc_unlock_rtc(dev, priv);
    
    /* Force 32k status reload to reflect correct state */
    k3rtc_field_write(priv, K3RTC_RELOAD_FROM_BBD, 0x1);
    ret = k3rtc_fence(priv);
    
    ret = k3rtc_lock_rtc(dev, priv);
    return ret;
}
```

**Zephyr:** ❌ **COMPLETELY MISSING**

---

### 5. **Turn Off / Turn On** (Zephyr PM device actions)

**Zephyr** has no PM device support in the counter driver:
- No `PM_DEVICE_DEFINE()` macro
- No `pm_action_cb` callback
- No `PM_DEVICE_ACTION_TURN_OFF` handler
- No `PM_DEVICE_ACTION_TURN_ON` handler

**Required for Zephyr PM integration:**
```c
static int k3_rtc_pm_action(const struct device *dev, enum pm_device_action action)
{
    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        /* Save alarm state, disable IRQs */
        return 0;
    case PM_DEVICE_ACTION_RESUME:
        /* Check if RTC retained power, cleanup poweroff config */
        return k3rtc_analog_resume_zephyr(dev);
    case PM_DEVICE_ACTION_TURN_OFF:
        /* Optional: disable clocks if manageable */
        return 0;
    case PM_DEVICE_ACTION_TURN_ON:
        /* Re-enable clocks, reconfigure if context lost */
        return 0;
    default:
        return -ENOTSUP;
    }
}

PM_DEVICE_DEFINE(k3_rtc, k3_rtc_pm_action);
```

---

## Register Access Comparison

### Locking/Unlocking

| Feature | Linux | Zephyr |
|---------|-------|--------|
| Unlock sequence | ✅ Full lock/unlock with fence | ✅ Basic unlock |
| Lock RTC | ✅ `k3rtc_lock_rtc()` | ❌ No lock support |
| Fence (sync wait) | ✅ `k3rtc_fence()` with regmap poll | ✅ `k3_rtc_fence()` with WAIT_FOR |
| Mutex protection | ✅ `mutex_lock` for multi-access | ❌ No mutex (counter is simpler) |
| Has analog block aware | ✅ Conditional lock/unlock | ❌ Not aware of analog IP variant |

**Similarity:** Both check SYNCPEND and wait for register sync.

**Difference:** Linux locks/unlocks around sensitive operations; Zephyr doesn't lock.

---

## IRQ Handling Comparison

### Interrupt Service Routine

| Feature | Linux (ti_k3_rtc_interrupt) | Zephyr (k3_rtc_isr) |
|---------|------------------------------|---------------------|
| **Lines of code** | ~157 lines | ~40 lines |
| Pre-wait for 32k sync | ✅ `usleep_range(sync_timeout_us)` | ❌ No |
| Resume from LPM detection | ✅ Checks `k3rtc_is_rtc_already_configured()` | ❌ No |
| Clear SW_OFF on resume | ✅ Explicit clear + fence | ❌ No |
| Clear wakeup enables | ✅ Yes | ❌ No |
| GPIO IRQ demux | ✅ `handle_nested_irq()` for GPIO 0-3 | ❌ No GPIO support |
| Status clear + reload | ✅ Write status, reload from BBD | ✅ Write status only |
| Dual alarm support | ✅ Alarm1 + Alarm2 | ✅ Alarm1 + Alarm2 |
| Callback invocation | ✅ `rtc_update_irq()` | ✅ `alarm_cfg[].callback()` |

**Linux IRQ flow:**
1. Sleep for 32k sync
2. Check if resuming from LPM (SW_OFF set)
3. If LPM resume: clear SW_OFF, clear write error, disable wakeup, clear power-off enable
4. Unlock RTC
5. Read IRQ status (all bits 0-5)
6. Clear IRQ status
7. Fence + reload from battery-backed domain
8. Lock RTC
9. Poll status clear
10. Demux GPIO IRQs (bits 2-5) to nested handlers
11. If timer/alarm (bits 0-1): notify RTC core

**Zephyr IRQ flow:**
1. Read IRQ status (bits 0-1 only)
2. Clear IRQ status
3. If alarm1: invoke callback
4. If alarm2: invoke callback

**Critical Missing:** Zephyr doesn't handle LPM resume cleanup in IRQ handler.

---

## Feature Comparison Matrix

| Feature | Linux | Zephyr | Notes |
|---------|-------|--------|-------|
| **Core RTC** | | | |
| Read time/counter | ✅ rtc_time | ✅ counter ticks | Different APIs |
| Set time/counter | ✅ rtc_time | ✅ counter ticks | Different APIs |
| Alarm support | ✅ RTC alarm | ✅ Counter alarm | Both have 2 channels |
| 48-bit range | ✅ Yes | ✅ Yes (if CONFIG_COUNTER_64BITS_TICKS) | Same hardware |
| **PM Hooks** | | | |
| Suspend | ✅ `ti_k3_rtc_suspend()` | ❌ **MISSING** | **Critical gap** |
| Resume | ✅ `ti_k3_rtc_resume()` | ❌ **MISSING** | **Critical gap** |
| Analog suspend | ✅ `k3rtc_analog_suspend()` | ❌ **MISSING** | **Critical gap** |
| Analog resume | ✅ `k3rtc_analog_resume()` | ❌ **MISSING** | **Critical gap** |
| Power-off prepare | ✅ `sys_off_handler` | ❌ No | Linux-specific |
| Wakeup source | ✅ `device_may_wakeup()` | ❌ No | **Gap** |
| IRQ wake enable | ✅ `enable_irq_wake()` | ❌ No | **Gap** |
| **GPIO Wakeup** | | | |
| GPIO chip | ✅ 0-3 ext pins | ❌ **MISSING** | **Major gap** |
| IRQ domain | ✅ Nested IRQs | ❌ No | **Gap** |
| Polarity capture | ✅ Active-high/low | ❌ No | **Gap** |
| Wake enable tracking | ✅ Bitmask per GPIO | ❌ No | **Gap** |
| gpio-keys integration | ✅ Works | ❌ N/A | **Gap** |
| **Advanced Features** | | | |
| NVMEM (scratch regs) | ✅ 8x32-bit | ❌ **MISSING** | **Gap** |
| Clock calibration | ✅ ppb offset | ❌ **MISSING** | **Gap** |
| Analog IP config | ✅ LFXOSC trim | ❌ No | **Gap** |
| SOC variant aware | ✅ AM62 vs AM62L | ❌ Generic | Partial gap |
| Errata handling | ✅ i2327 quirk | ❌ No | **Gap** |
| **Robustness** | | | |
| Lock/unlock RTC | ✅ Full support | ⚠️ Unlock only | **Gap** |
| Mutex protection | ✅ Yes | ❌ No | Acceptable for counter |
| Context retention check | ✅ Check PWR_OFF bit | ❌ No | **Critical gap** |
| Write error recovery | ✅ Clear WRT_ERR | ❌ No | **Gap** |
| 32k domain reload | ✅ RELOAD_FROM_BBD | ❌ No | **Critical gap** |

---

## Missing in Zephyr - Detailed Analysis

### 🔴 **Critical (Blocks S2RAM wake)**

1. **No PM hooks at all**
   - Missing `PM_DEVICE_ACTION_SUSPEND` handler
   - Missing `PM_DEVICE_ACTION_RESUME` handler
   - RTC never told about suspend/resume events
   
2. **No analog suspend/resume**
   - `k3rtc_analog_suspend()` configures:
     - SW_OFF bit (required for PMIC power-down)
     - GPIO wakeup polarity
     - GPIO wakeup enable mask
     - Wakeup enable in GENERAL_CTL
   - `k3rtc_analog_resume()` clears:
     - SW_OFF bit (prevents stuck poweroff state)
     - Wakeup IRQ enables
     - Write error condition
     - Reloads status from battery-backed 32k domain
   
   **Impact:** After S2RAM wake, RTC can be in poweroff-armed state with SW_OFF=1, causing system to re-enter poweroff mode immediately.

3. **No context retention detection**
   - Linux checks `K3RTC_GEN_PWR_OFF` bit to detect if RTC retained power
   - If retained: cleanup poweroff config
   - If lost context: full re-initialization
   - Zephyr blindly assumes RTC is in clean state

4. **No 32k domain status reload**
   - After resume, Linux writes `RELOAD_FROM_BBD = 1` to force status sync from battery-backed 32k domain
   - Zephyr doesn't reload, status may be stale

### 🟡 **Important (Limits functionality)**

5. **No GPIO wakeup support**
   - Linux exposes 4 GPIOs via gpiochip for external wakeup pins
   - gpio-keys can use these for button wakeup
   - Tracks polarity (active-high/low) and enable mask
   - Zephyr: no GPIO chip, no external wakeup pins

6. **No NVMEM scratch registers**
   - 8x32-bit scratch registers survive power-down
   - Useful for storing boot reason, flags, etc.
   - Zephyr: not exposed

7. **No clock calibration**
   - Linux: `read_offset`/`set_offset` for ppb compensation
   - Useful for long-running timekeeping accuracy
   - Zephyr: no calibration API

8. **No lock support**
   - Linux locks RTC after writes to protect from spurious access
   - Zephyr only unlocks, never locks
   - Less critical for counter use case

### 🟢 **Nice to have**

9. **No erratum i2327 handling**
   - AM62X SR1.0 requires bootloader to unlock RTC within 1 second
   - Linux checks and refuses to operate if locked
   - Zephyr: no check

10. **No write error recovery**
    - Linux clears `K3RTC_WRT_ERR` bit on resume
    - Zephyr: if write error happens, stuck

---

## Recommendations for Zephyr

### **Phase 1: Critical S2RAM fixes (must have)**

```c
/* Add PM device support to counter_k3_rtc.c */

static int k3_rtc_pm_action(const struct device *dev, enum pm_device_action action)
{
    k3_rtc_regs_t *rtc_regs = DEV_REGS(dev);
    struct k3_rtc_counter_data *data = DEV_DATA(dev);
    int ret;
    
    switch (action) {
    case PM_DEVICE_ACTION_SUSPEND:
        /* Enable PMIC power-off signal */
        rtc_regs->GENRAL_CTL |= BIT(16);  /* K3RTC_GEN_PWR_OFF */
        ret = k3_rtc_fence(dev);
        if (ret) return ret;
        
        /* Enable software poweroff */
        rtc_regs->GENRAL_CTL |= BIT(17);  /* K3RTC_GEN_SW_OFF */
        ret = k3_rtc_fence(dev);
        return ret;
        
    case PM_DEVICE_ACTION_RESUME:
        /* Check if RTC retained power (PWR_OFF bit set) */
        if (rtc_regs->GENRAL_CTL & BIT(16)) {
            /* Explicitly clear SW_OFF - forced write required */
            rtc_regs->GENRAL_CTL &= ~BIT(17);
            ret = k3_rtc_fence(dev);
            if (ret) return ret;
            
            /* Clear write error condition */
            rtc_regs->SYNCPEND = BIT(3);  /* WRT_ERR */
            
            /* Disable all wakeup IRQs */
            rtc_regs->IRQENABLE_CLR_SYS = 0x1F;
            
            /* Clear wakeup enables */
            rtc_regs->GENRAL_CTL &= ~(0xF);  /* WKUP_EN[3:0] */
            
            /* Clear power-off enable */
            rtc_regs->GENRAL_CTL &= ~BIT(16);
            ret = k3_rtc_fence(dev);
            if (ret) return ret;
            
            /* Reload status from battery-backed domain */
            rtc_regs->SYNCPEND = BIT(31);  /* RELOAD_FROM_BBD */
            ret = k3_rtc_fence(dev);
            return ret;
        }
        /* Else: RTC lost context, fall through to TURN_ON for reinit */
        __fallthrough;
        
    case PM_DEVICE_ACTION_TURN_ON:
        /* Re-initialize if context lost */
        return k3_counter_init(dev);
        
    case PM_DEVICE_ACTION_TURN_OFF:
        return 0;
        
    default:
        return -ENOTSUP;
    }
}

PM_DEVICE_DT_INST_DEFINE(0, k3_rtc_pm_action);

/* Update DEVICE_DT_INST_DEFINE to use PM_DEVICE_DT_INST_GET(0) */
```

### **Phase 2: IRQ handler S2RAM awareness**

```c
static void k3_rtc_isr(const struct device *dev)
{
    k3_rtc_regs_t *rtc_regs = DEV_REGS(dev);
    struct k3_rtc_counter_data *data = DEV_DATA(dev);
    uint32_t status;
    
    /* Wait for 32k sync before reading status */
    k_usleep(data->sync_timeout_us);
    
    /* Check if resuming from S2RAM (SW_OFF was set) */
    if (rtc_regs->GENRAL_CTL & BIT(17)) {
        /* Clear SW_OFF */
        rtc_regs->GENRAL_CTL &= ~BIT(17);
        k3_rtc_fence(dev);
        
        /* Clear write error */
        rtc_regs->SYNCPEND = BIT(3);
        
        /* Disable wakeup IRQs */
        rtc_regs->IRQENABLE_CLR_SYS = 0x1F;
        
        /* Clear wakeup enables */
        rtc_regs->GENRAL_CTL &= ~(0xF);
        k3_rtc_fence(dev);
        
        /* Clear power-off enable */
        rtc_regs->GENRAL_CTL &= ~BIT(16);
        k3_rtc_fence(dev);
    }
    
    /* Rest of IRQ handling... */
    status = rtc_regs->IRQSTATUS_SYS;
    if (status == 0) return;
    
    rtc_regs->IRQSTATUS_SYS = status;
    
    /* Reload from battery-backed domain */
    rtc_regs->SYNCPEND = BIT(31);
    k3_rtc_fence(dev);
    
    /* Invoke callbacks... */
}
```

### **Phase 3: Lock support**

```c
static int k3_rtc_lock(const struct device *dev)
{
    k3_rtc_regs_t *rtc_regs = DEV_REGS(dev);
    struct k3_rtc_counter_data *data = DEV_DATA(dev);
    
    rtc_regs->KICK0 = 0x0;
    
    /* Poll UNLOCK bit until clear */
    int timeout = WAIT_FOR((rtc_regs->GENRAL_CTL & RTC_UNLOCK_MASK) == 0,
                           data->sync_timeout_us, k_busy_wait(1));
    return timeout;
}
```

### **Phase 4: Optional enhancements**

- NVMEM scratch register API
- Clock calibration via counter calibration API
- GPIO wakeup pins via Zephyr GPIO driver
- Erratum i2327 check

---

## Summary

### ✅ **Similarities**
- Both access same hardware registers
- Both support dual alarms (2 channels)
- Both fence/sync correctly
- Both handle 48-bit counter

### ❌ **Critical Differences**
1. **Linux has full PM suspend/resume hooks; Zephyr has NONE**
2. **Linux configures analog block for S2RAM; Zephyr doesn't**
3. **Linux detects context retention; Zephyr assumes clean state**
4. **Linux reloads status from 32k domain; Zephyr doesn't**
5. **Linux clears SW_OFF on resume; Zephyr leaves it armed (DANGEROUS)**

### 🚨 **Impact on S2RAM**
Without PM hooks, Zephyr's RTC is **NOT safe for S2RAM**:
- May stay in poweroff-armed state after resume
- System could re-enter suspend immediately
- No cleanup of wakeup configuration
- Stale interrupt status

**Current workaround in `soc/ti/k3/am62lx/power.c` probably includes some of this cleanup, but it should be in the driver.**
