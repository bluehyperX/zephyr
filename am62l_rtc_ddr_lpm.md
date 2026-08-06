# AM62L RTC+DDR Low Power Mode — Zephyr Implementation Plan

## Background: How RTC+DDR LPM works on TI K3

On AM62x/AM62L in Linux, RTC+DDR low power mode is a `SYSTEM_SUSPEND` flow:

1. Linux programs an RTC alarm (OFF→ON register) as the wakeup source.
2. Linux calls `PSCI_SYSTEM_SUSPEND` (SMC into EL3).
3. TF-A at EL3 signals the Device Manager (DM) firmware (running on R5F/M4) to put DDR into self-refresh.
4. TF-A suspends the A53 cluster.
5. On RTC alarm, DM brings DDR out of self-refresh, TF-A resumes A53.

Zephyr on A53 takes the same path — the DDR self-refresh is owned by TF-A/DM, not the OS. Zephyr just needs the plumbing to reach that `PSCI_SYSTEM_SUSPEND` call with proper setup.

---

## Gap Analysis

There is zero existing PM infrastructure for K3 in Zephyr. Six distinct layers need to be built.

---

## 1. arm64 arch S2RAM primitives — `arch/arm64/core/`

**Status: Missing entirely.** Every other arch that supports `PM_STATE_SUSPEND_TO_RAM` has a
`pm_s2ram.S` + `pm_s2ram.c`. ARM64 has neither, and `ARCH_HAS_SUSPEND_TO_RAM` is not selected
in `arch/arm64/Kconfig`.

**Work:**
- Add `arch/arm64/core/pm_s2ram.S`: save callee-saved registers + SP/LR to a per-CPU context
  buffer in retained memory, call the `system_off_fn` hook, define the resume trampoline
  (`arch_pm_s2ram_resume`).
- Add `arch_pm_s2ram_suspend(system_off_fn)` in a companion `.c` file.
- Select `ARCH_HAS_SUSPEND_TO_RAM` in `arch/arm64/Kconfig` inside a new `config PM_S2RAM` block.

Reference implementations: `arch/arm/core/cortex_m/pm_s2ram.S`,
`soc/nordic/nrf54h/soc_pm_s2ram.c`.

---

## 2. PSCI `SYSTEM_SUSPEND` in the PSCI driver — `drivers/pm_cpu_ops/pm_cpu_ops_psci.c`

**Status: Missing.** The driver only implements `CPU_SUSPEND`, `CPU_OFF`, `CPU_ON`,
`AFFINITY_INFO`, and `MIGRATE`. `PSCI_SYSTEM_SUSPEND` (function ID `0x8400000E`) is absent from
`include/zephyr/drivers/pm_cpu_ops/psci.h` and the driver.

**Work:**
- Add `PSCI_1_x_FN_SYSTEM_SUSPEND` to `psci.h`.
- Expose a `psci_system_suspend(unsigned long entry_point)` function from the driver.

This is the single SMC call that drops into TF-A and triggers the full DDR retention sequence.

---

## 3. AM62L SoC PM hooks — `soc/ti/k3/am62lx/`

**Status: Missing entirely.** No `power.c` exists; `HAS_PM` is not selected; `CONFIG_PM` cannot
be enabled.

**Work: create `soc/ti/k3/am62lx/power.c`:**
- Implement `pm_state_set(enum pm_state state, uint8_t substate_id)`:
  - For `PM_STATE_SUSPEND_TO_RAM`: call `arch_pm_s2ram_suspend(am62l_system_off)` where
    `am62l_system_off` calls `psci_system_suspend()`.
  - For `PM_STATE_RUNTIME_IDLE` / `PM_STATE_SUSPEND_TO_IDLE`: `wfi()` is sufficient.
- Implement `pm_state_exit_post_ops()` (re-enable interrupts, restore any clocks gated before
  suspend).
- Add `select HAS_PM` in `soc/ti/k3/am62lx/Kconfig`.

The RTC alarm must be armed before calling `psci_system_suspend()` — otherwise the system won't
wake up.

---

## 4. AM62L wakeup domain RTC node — `dts/vendor/ti/am62l-wakeup.dtsi`

**Status: Missing.** `am62l-wakeup.dtsi` has wkup GPIO, two DM timers, and wkup I2C — but no
RTC. AM62x has `main_rtc0: rtc@2b1f0000` in `k3-am62-main.dtsi` using the `ti,k3-rtc-counter`
compatible already implemented in `drivers/counter/counter_k3_rtc.c`.

**Work:**
- Confirm from the AM62L TRM whether the RTC block exists at `0x2b1f0000` (wakeup domain).
- Add the RTC counter node to `am62l-wakeup.dtsi`:

```dts
wkup_rtc0: rtc@2b1f0000 {
    compatible = "ti,k3-rtc-counter";
    reg = <0x2b1f0000 0x100>;
    clock-frequency = <32768>;
    status = "disabled";
    counter_rtc0: counter_rtc {
        compatible = "zephyr,rtc-counter";
        alarms-count = <2>;
    };
};
```

- Enable it in `am62l_evm_am62l3_a53.dts`.

The existing `counter_k3_rtc.c` and `dts/bindings/counter/ti,k3-rtc-counter.yaml` already support
this — no new driver code is needed if the hardware block matches AM62x.

---

## 5. Board DTS power states — `dts/arm64/ti/am62l3_a53.dtsi`

**Status: Missing.** No `cpu-power-states` block exists anywhere for AM62L or AM62x.

**Work:** Add a `power-states` node and reference it from the A53 CPU node:

```dts
/ {
    cpus {
        cpu@0 {
            /* existing properties */
            cpu-idle-states = <&CPU_SLEEP>;
        };
    };

    idle-states {
        entry-method = "psci";
        CPU_SLEEP: cpu-sleep {
            compatible = "zephyr,power-state";
            power-state-name = "suspend-to-ram";
            min-residency-us = <500000>;
            exit-latency-us = <5000>;
        };
    };
};
```

---

## 6. Board defconfig — `am62l_evm_am62l3_a53_defconfig`

**Status: Needs additions.** Currently enables `CONFIG_PM_CPU_OPS`, `CONFIG_PM_DEVICE`,
`CONFIG_POWER_DOMAIN` but not `CONFIG_PM` or `CONFIG_PM_S2RAM`.

Once `HAS_PM` is selected by the SoC Kconfig, add:

```
CONFIG_PM=y
CONFIG_PM_S2RAM=y
```

---

## Work Outside Zephyr

**TF-A / ATF for AM62L** must implement `PSCI_SYSTEM_SUSPEND` such that it:
1. Signals the DM (Device Manager) to put DDR into self-refresh.
2. Powers down the A53 cluster.
3. On wakeup interrupt (RTC or GPIO), signals DM to bring DDR out of self-refresh and resumes
   the A53.

If TF-A already implements this for AM62L (as it does for AM62x in the upstream
`trusted-firmware-a` repo), Zephyr just needs the PSCI call wired. If not, TF-A work is a
prerequisite.

---

## Recommended Implementation Order

| Step | Work | Files |
|------|------|-------|
| 1 | PSCI `SYSTEM_SUSPEND` function | `drivers/pm_cpu_ops/pm_cpu_ops_psci.c`, `include/zephyr/drivers/pm_cpu_ops/psci.h` |
| 2 | arm64 `pm_s2ram.S` + `pm_s2ram.c` | `arch/arm64/core/` |
| 3 | AM62L RTC DTS node | `dts/vendor/ti/am62l-wakeup.dtsi`, `boards/ti/am62l_evm/*.dts` |
| 4 | AM62L SoC `power.c` | `soc/ti/k3/am62lx/power.c`, `soc/ti/k3/am62lx/Kconfig` |
| 5 | Board DTS power states + defconfig | `dts/arm64/ti/am62l3_a53.dtsi`, `boards/ti/am62l_evm/` |
| 6 | Integration test on hardware | `tests/` or sample app on `am62l_evm` |

The closest upstream references are the AM62x M4 path
(`boards/ti/sk_am62/sk_am62_am6254_m4.dts`) for the RTC DTS pattern and
`arch/arm/core/cortex_m/pm_s2ram.S` for the S2RAM assembly template.
