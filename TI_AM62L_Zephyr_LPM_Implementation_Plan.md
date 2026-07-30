# TI AM62L Low Power Mode (LPM) Implementation Plan for Zephyr

## Goal

Implement TI AM62L low power modes in TI's Zephyr fork with an
architecture that is suitable for upstreaming into Zephyr mainline.

------------------------------------------------------------------------

# Design Principles

-   Follow Zephyr's existing Power Management (PM) architecture.
-   Do **not** port Linux's PM framework.
-   Reuse only the hardware-specific logic from Linux.
-   Keep policy independent from the SoC implementation.
-   Isolate all TI-specific code under the SoC layer.

Architecture:

``` text
Application
      │
      ▼
PM Policy (Residency / Application)
      │
      ▼
Generic Zephyr PM Core
      │
      ▼
TI AM62L PM Backend
      ├── TI SCI
      ├── DDR
      ├── Power Domains
      ├── Wake Sources
      └── CPU Suspend
```

------------------------------------------------------------------------

# Mapping TI Low Power Modes

  TI Mode            Zephyr PM State            Substate
  ------------------ -------------------------- ---------------------
  Standby / s2idle   PM_STATE_SUSPEND_TO_IDLE   TI_AM62L_STANDBY
  RTC Only + DDR     PM_STATE_SUSPEND_TO_RAM    TI_AM62L_RTC_DDR
  Deep Sleep         PM_STATE_SUSPEND_TO_RAM    TI_AM62L_DEEP_SLEEP
  RTC Only           PM_STATE_SOFT_OFF          0

Example:

``` c
enum ti_am62l_pm_substate {
    TI_AM62L_STANDBY = 0,
    TI_AM62L_RTC_DDR,
    TI_AM62L_DEEP_SLEEP,
};
```

------------------------------------------------------------------------

# Use of substate_id

`substate_id` distinguishes multiple hardware implementations of the
same generic Zephyr power state.

Example:

``` c
PM_STATE_SUSPEND_TO_RAM
    substate = TI_AM62L_RTC_DDR

PM_STATE_SUSPEND_TO_RAM
    substate = TI_AM62L_DEEP_SLEEP
```

The Zephyr PM core treats `substate_id` as opaque; only the TI backend
interprets it.

------------------------------------------------------------------------

# Proposed SoC Directory Layout

``` text
soc/ti/am62l/
    pm.c
    rtc_ddr.c
    deep_sleep.c
    ddr.c
    tisci.c
    wakeup.c
```

`pm.c` should only dispatch to the appropriate backend.

------------------------------------------------------------------------

# Suspend Flow

``` text
PM Policy
    ↓
PM_STATE_SUSPEND_TO_RAM
    ↓
pm_state_set()
    ↓
ti_pm_enter()
    ↓
Select substate
    ├── RTC+DDR
    └── Deep Sleep
```

Backend sequence:

``` text
ti_sci_prepare()
↓
DDR self-refresh
↓
Power-domain transition
↓
CPU suspend (WFI)
↓
Wakeup
↓
Restore domains
↓
Exit DDR self-refresh
↓
Resume kernel
```

------------------------------------------------------------------------

# Code Reuse from Linux

Reuse:

-   TI SCI protocol
-   DDR self-refresh logic
-   Power-domain sequencing
-   Wake source configuration
-   Register save/restore where hardware-specific

Do not reuse:

-   Linux PM core
-   platform_suspend_ops
-   genpd
-   cpuidle
-   runtime PM framework
-   scheduler/freezer infrastructure

------------------------------------------------------------------------

# PM Policies

The backend should remain independent of the policy.

Recommended order:

1.  Force PM_STATE_SUSPEND_TO_RAM for validation.
2.  Enable Residency-based Policy.
3.  Support Application-based Policy without changing backend logic.

------------------------------------------------------------------------

# Upstream Strategy

## Patch 1

-   AM62L PM backend infrastructure
-   TI SCI interface

## Patch 2

-   RTC Only + DDR
-   PM_STATE_SUSPEND_TO_RAM

## Patch 3

-   Device PM integration

## Patch 4

-   RTC Only
-   PM_STATE_SOFT_OFF

## Patch 5

-   Deep Sleep (second substate)

## Patch 6

-   Residency policy tuning and automatic selection

------------------------------------------------------------------------

# Open Questions

-   Confirm whether Deep Sleep differs only in TI SCI sequencing or also
    in CPU/context restore.
-   Define residency and exit latency values for each substate.
-   Validate wake sources (RTC, GPIO, etc.).
-   Verify TF-A/SPL resume responsibilities versus Zephyr
    responsibilities.

------------------------------------------------------------------------

# Long-Term Improvement

Consider describing substates with metadata (minimum residency, exit
latency, power savings) so the generic residency policy can choose among
them automatically without TI-specific logic.
