# AM62L RTC+DDR LPM: Implementation Plan

## Context

AM62L (Cortex-A53, arm64) needs RTC+DDR low power mode in Zephyr — system-level
suspend where DDR enters self-refresh, A53 powers down, and an RTC alarm wakes it.

**Key AM62L facts (confirmed):**
- No DM firmware. All RM/PM/LPM HAL and DDR self-refresh is handled by TF-A directly
  (via `plat/ti/k3low/common/am62l_psci.c` → `ti_sci_enter_sleep()` → TIFS).
- TF-A passes `core_resume_addr` to TIFS. On RTC wakeup, TIFS wakes TF-A at its own
  secure entry (`jump_to_atf_func`), TF-A restores GIC/IO-isolation/clocks, then
  drops to non-secure EL1 at the address Zephyr passed as `entry_point`.
- TF-A's standard `psci_warmboot_entrypoint()` path restores non-secure EL1 system
  registers (SCTLR_EL1, CPACR_EL1, TCR_EL1 etc.) via `cm_el1_sysregs_context_restore()`
  before branching to the EL1 entry_point.
- `CONFIG_ARM_MMU` is **not set** for AM62L in Zephyr → no MMU re-enable needed in
  the resume trampoline. Physical address == virtual address trivially.
- PSCI_SYSTEM_SUSPEND is implemented in TF-A for AM62L. ✓

**Firmware call chain:**
```
Zephyr pm_state_set(S2RAM)
  → arch_pm_s2ram_suspend(am62l_system_off)
    → saves x19–x30 on stack, SP + VBAR_EL1 to _cpu_context
    → pm_s2ram_mark_set()
    → am62l_system_off() → psci_system_suspend(virt_addr(arch_pm_s2ram_resume))
      → SMC → TF-A EL3 → ti_sci_enter_sleep(am62l_sec_entrypoint)
        → TIFS: DDR self-refresh, power down A53

[RTC alarm fires]
TIFS → TF-A am62l_sec_entrypoint (EL3)
  → TF-A restores GIC, IO isolation, EL1 system registers
  → ERET to EL1 at arch_pm_s2ram_resume

arch_pm_s2ram_resume:
  → pm_s2ram_mark_check_and_clear()  (magic = 0xDABBAD00 in __noinit DDR)
  → restore VBAR_EL1 from _cpu_context
  → restore SP from _cpu_context
  → restore x19–x30 from stack (DDR preserved in self-refresh)
  → return x0=0 to arch_pm_s2ram_suspend call site
```

---

## What Zephyr already provides (do NOT duplicate)

| What | Where |
|------|-------|
| PM subsystem state machine, device suspend/resume orchestration | `subsys/pm/pm.c` |
| `arch_pm_s2ram_suspend(system_off_fn)` interface + `pm_s2ram_mark_*` API | `include/zephyr/arch/common/pm_s2ram.h` |
| Default marking implementation pattern (magic 0xDABBAD00 in `__noinit`) | `arch/arm/core/cortex_m/pm_s2ram.c` (template) |
| `PM_S2RAM` Kconfig auto-enabled when `ARCH_HAS_SUSPEND_TO_RAM` + DT power-state present | `subsys/pm/Kconfig` |
| PSCI driver framework, `invoke_psci_fn`, `psci_cpu_suspend()` | `drivers/pm_cpu_ops/pm_cpu_ops_psci.c` |
| K3 RTC counter driver with 2-channel alarm API | `drivers/counter/counter_k3_rtc.c` |
| arm64 callee-saved register layout + offset symbols | `include/zephyr/arch/arm64/thread.h`, `arch/arm64/core/offsets/offsets.c` |

---

## Component 1: PSCI `SYSTEM_SUSPEND` function

**Files to modify:**

`drivers/pm_cpu_ops/pm_cpu_ops_psci.h` — add to PSCI 1.0 block:
```c
#define PSCI_1_0_FN_SYSTEM_SUSPEND    PSCI_1_0_FN(14)
#define PSCI_1_0_FN64_SYSTEM_SUSPEND  PSCI_1_0_FN64(14)
```
(Linux uses `PSCI_1_0` for SYSTEM_SUSPEND, not 1_1)

`drivers/pm_cpu_ops/pm_cpu_ops_psci.c` — add after `psci_cpu_suspend()`:
```c
int psci_system_suspend(uintptr_t entry_point)
{
    return psci_to_dev_err(
        psci_data.invoke_psci_fn(
            PSCI_FN_NATIVE(1_0, SYSTEM_SUSPEND), entry_point, 0, 0));
}
```

`include/zephyr/drivers/pm_cpu_ops/psci.h` — add declaration:
```c
int psci_system_suspend(uintptr_t entry_point);
```

---

## Component 2: arm64 S2RAM arch layer (first in Zephyr for arm64)

**Files to create:**
- `arch/arm64/core/pm_s2ram.S`
- `arch/arm64/core/pm_s2ram.c`

**Files to modify:**
- `arch/Kconfig` — add `select ARCH_HAS_SUSPEND_TO_RAM` inside `config ARM64` block
- `arch/arm64/core/CMakeLists.txt` — add:
  ```cmake
  zephyr_library_sources_ifdef(CONFIG_PM_S2RAM pm_s2ram.S pm_s2ram.c)
  ```

### pm_s2ram.c

```c
/* CPU context: only what TF-A does NOT restore for us */
struct _cpu_context {
    uint64_t sp;
    uint64_t vbar_el1;
};
typedef struct _cpu_context _cpu_context_t;

static _cpu_context_t _cpu_context;

/* Magic word in __noinit DDR — survives DDR self-refresh */
static __noinit uint32_t marker;
#define S2RAM_MAGIC 0xDABBAD00U

void pm_s2ram_mark_set(void) { marker = S2RAM_MAGIC; }

bool pm_s2ram_mark_check_and_clear(void)
{
    if (marker == S2RAM_MAGIC) {
        marker = 0U;
        return true;
    }
    return false;
}
```

TF-A's `cm_el1_sysregs_context_restore()` restores SCTLR_EL1, TCR_EL1, CPACR_EL1,
MAIR_EL1, and the page-table registers before branching to EL1. We only need
to save `sp` (TF-A has no knowledge of our stack) and `vbar_el1` (defensive: same
value TF-A will restore, but protects against older TF-A versions).

### pm_s2ram.S — suspend path

```asm
/* arch_pm_s2ram_suspend(pm_s2ram_system_off_fn_t system_off) */
GTEXT(arch_pm_s2ram_suspend)
SECTION_FUNC(TEXT, arch_pm_s2ram_suspend)
    /* Save callee-saved GPRs on stack (16-byte aligned pairs) */
    stp  x19, x20, [sp, #-16]!
    stp  x21, x22, [sp, #-16]!
    stp  x23, x24, [sp, #-16]!
    stp  x25, x26, [sp, #-16]!
    stp  x27, x28, [sp, #-16]!
    stp  x29, x30, [sp, #-16]!   /* FP + LR */

    /* Save system_off function pointer across calls */
    mov  x19, x0

    /* Save SP and VBAR_EL1 to _cpu_context */
    adrp x0, _cpu_context
    add  x0, x0, :lo12:_cpu_context
    mov  x1, sp
    str  x1, [x0, #0]            /* _cpu_context.sp */
    mrs  x1, vbar_el1
    str  x1, [x0, #8]            /* _cpu_context.vbar_el1 */

    /* Write suspend marker */
    dsb  sy
    bl   pm_s2ram_mark_set

    /* Call system_off — never returns on success */
    blr  x19

    /* Abort path: clear marker, restore regs, return -EBUSY */
    mov  x19, x0                 /* preserve error code */
    bl   pm_s2ram_mark_check_and_clear
    mov  x0, x19
    ldp  x29, x30, [sp], #16
    ldp  x27, x28, [sp], #16
    ldp  x25, x26, [sp], #16
    ldp  x23, x24, [sp], #16
    ldp  x21, x22, [sp], #16
    ldp  x19, x20, [sp], #16
    ret
```

### pm_s2ram.S — resume trampoline

```asm
/* Called by TF-A at EL1, no-MMU (MMU not used anyway on AM62L).
 * DDR is out of self-refresh when we reach here.
 * Returns 0 to arch_pm_s2ram_suspend call site. */
GTEXT(arch_pm_s2ram_resume)
SECTION_FUNC(TEXT, arch_pm_s2ram_resume)
    /* Check and clear marker — if not set this is a cold boot, return != 0 */
    bl   pm_s2ram_mark_check_and_clear
    cbz  x0, 1f                  /* not set → cold boot, return nonzero value */

    /* Restore system registers */
    adrp x0, _cpu_context
    add  x0, x0, :lo12:_cpu_context
    ldr  x1, [x0, #8]
    msr  vbar_el1, x1            /* restore vector table */
    isb
    ldr  x1, [x0, #0]
    mov  sp, x1                  /* restore stack pointer */

    /* Restore callee-saved GPRs from stack (DDR preserved) */
    ldp  x29, x30, [sp], #16
    ldp  x27, x28, [sp], #16
    ldp  x25, x26, [sp], #16
    ldp  x23, x24, [sp], #16
    ldp  x21, x22, [sp], #16
    ldp  x19, x20, [sp], #16

    /* Return 0 = successful resume */
    mov  x0, #0
    ret

1:  /* Cold boot: return non-zero so caller proceeds normally */
    mov  x0, #1
    ret
```

---

## Component 3: AM62L SoC PM layer

**File to create:** `soc/ti/k3/am62lx/power.c`

```c
/* No DM: TF-A handles DDR self-refresh and LPM HAL entirely.
 * Zephyr only needs to save CPU context and call PSCI SYSTEM_SUSPEND. */

#include <zephyr/pm/pm.h>
#include <zephyr/arch/common/pm_s2ram.h>
#include <zephyr/drivers/pm_cpu_ops/psci.h>

static int am62l_system_off(void)
{
    /* Without MMU: virtual address == physical address */
    uintptr_t resume_pa = (uintptr_t)arch_pm_s2ram_resume;
    return psci_system_suspend(resume_pa);
}

void pm_state_set(enum pm_state state, uint8_t substate_id)
{
    ARG_UNUSED(substate_id);
    switch (state) {
    case PM_STATE_SUSPEND_TO_RAM:
        arch_pm_s2ram_suspend(am62l_system_off);
        break;
    case PM_STATE_SUSPEND_TO_IDLE:
    case PM_STATE_RUNTIME_IDLE:
        k_cpu_idle();
        break;
    default:
        break;
    }
}

void pm_state_exit_post_ops(enum pm_state state, uint8_t substate_id)
{
    ARG_UNUSED(state);
    ARG_UNUSED(substate_id);
    irq_unlock(0);
}
```

**File to modify:** `soc/ti/k3/am62lx/Kconfig` — add inside existing `config SOC_AM62L3`:
```kconfig
select HAS_PM
```

---

## Component 4: AM62L RTC DTS node + driver binding

**Finding:** Both Linux and Zephyr target the same silicon at `0x2b1f0000`.
Linux uses `ti,am62l-rtc`; Zephyr's counter driver uses `ti,k3-rtc-counter`.
The AM62L variant adds an analog block (`has_analog_block = true` in Linux) used for
calibration — not needed for basic alarm/wakeup counter use.

**Change:** Add `ti,am62l-rtc` as a second compatible to the existing Zephyr driver:

`drivers/counter/counter_k3_rtc.c` — add a second compat alias at the bottom:
```c
/* After existing DT_INST_FOREACH_STATUS_OKAY(K3_RTC_INIT) */
#undef DT_DRV_COMPAT
#define DT_DRV_COMPAT ti_am62l_rtc
DT_INST_FOREACH_STATUS_OKAY(K3_RTC_INIT)
```

`dts/bindings/counter/ti,am62l-rtc.yaml` — new 3-line binding that inherits existing:
```yaml
description: TI AM62L RTC counter
compatible: "ti,am62l-rtc"
include: ti,k3-rtc-counter.yaml
```

`dts/vendor/ti/am62l-wakeup.dtsi` — add node:
```dts
wkup_rtc0: rtc@2b1f0000 {
    compatible = "ti,am62l-rtc";
    reg = <0x00 0x2b1f0000 0x00 0x100>;
    interrupts = <GIC_SPI 28 IRQ_TYPE_LEVEL_HIGH>;
    clocks = <&scmi_clk 271>, <&scmi_clk 268>;
    clock-names = "vbus", "osc32k";
    power-domains = <&scmi_pds 59>;
    status = "disabled";
};
```

`boards/ti/am62l_evm/am62l_evm_am62l3_a53.dts`:
```dts
&wkup_rtc0 { status = "okay"; };
```

---

## Component 5: Board DTS power states

**File to modify:** `dts/arm64/ti/am62l3_a53.dtsi`

```dts
/ {
    idle-states {
        entry-method = "psci";

        SOC_SLEEP_RTC_DDR: rtc-ddr {
            compatible = "zephyr,power-state";
            power-state-name = "suspend-to-ram";
            min-residency-us = <1000000>;
            exit-latency-us = <600000>;
        };
    };

    cpus {
        cpu@0 {
            /* existing properties */
            cpu-idle-states = <&SOC_SLEEP_RTC_DDR>;
        };
        cpu@1 {
            /* existing properties */
            cpu-idle-states = <&SOC_SLEEP_RTC_DDR>;
        };
    };
};
```

---

## Component 6: Defconfig

**File to modify:** `boards/ti/am62l_evm/am62l_evm_am62l3_a53_defconfig`

```kconfig
CONFIG_PM=y
# CONFIG_PM_S2RAM is auto-selected by Kconfig when ARCH_HAS_SUSPEND_TO_RAM
# is set and a "suspend-to-ram" power-state node exists in DT.
```

---

## Implementation order

| # | Work | Files | Risk |
|---|------|-------|------|
| 1 | PSCI SYSTEM_SUSPEND | `pm_cpu_ops_psci.h/.c`, `psci.h` | Low |
| 2 | arm64 pm_s2ram.c (struct + marking) | `arch/arm64/core/pm_s2ram.c` | Low |
| 3 | arm64 pm_s2ram.S (suspend path) | `arch/arm64/core/pm_s2ram.S` | Medium |
| 4 | arm64 pm_s2ram.S (resume trampoline) | `arch/arm64/core/pm_s2ram.S` | Medium |
| 5 | Kconfig + CMakeLists wiring | `arch/Kconfig`, `arch/arm64/core/CMakeLists.txt` | Low |
| 6 | AM62L power.c + Kconfig | `soc/ti/k3/am62lx/power.c`, `Kconfig` | Low |
| 7 | RTC binding alias + DTS node | `ti,am62l-rtc.yaml`, `am62l-wakeup.dtsi`, board DTS | Low |
| 8 | Board power-states + defconfig | `am62l3_a53.dtsi`, `defconfig` | Low |

---

## Remaining open question

**Stack direction alignment in resume trampoline:** The suspend path pushes pairs
with `[sp, #-16]!` (descending). The resume path pops with `[sp], #16` (ascending).
Verify at implementation time that the `stp` push order and `ldp` pop order match
(x29/x30 pushed last, popped first).
