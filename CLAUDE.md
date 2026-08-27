# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository Overview

This is the Zephyr RTOS repository, used here primarily for TI AM62L SoC low-power mode (LPM / S2RAM) development. The active branch (`am62l_lpm`) adds Suspend-to-RAM support for the AM62L EVM board, exercising the PSCI CPU_SUSPEND path via TF-A.

Key files under active development:
- `samples/boards/ti/am62l_lpm/src/main.c` — S2RAM demo: RTC-woken suspend/resume with I2C EEPROM verification
- `soc/ti/k3/am62lx/power.c` — AM62L PM backend: CPU1 off via SGI IPI, CPU0 PSCI CPU_SUSPEND, SMP resume via `k_smp_cpu_start`

## Build Commands

The workspace root is `~/ti/zephyrproject/` (west workspace). All `west` commands must be run from the zephyr repo root or workspace root.

```bash
# Build the LPM sample (matches build_lpm/ directory)
west build -b am62l_evm/am62l3/a53 -d build_lpm samples/boards/ti/am62l_lpm

# Rebuild without re-running CMake
west build -d build_lpm

# Force CMake re-run
west build -d build_lpm -c

# Build with extra Kconfig options
west build -b am62l_evm/am62l3/a53 -d build_lpm samples/boards/ti/am62l_lpm -- -DCONFIG_FOO=y
```

The binary to flash is `build_lpm/zephyr/zephyr.bin`.

## Running Tests

Zephyr uses Twister for its test suite:

```bash
# Run tests for a specific test directory
./scripts/twister -T tests/kernel/pm/ -p native_sim

# Run a single test case
./scripts/twister -T tests/kernel/pm/ -p native_sim -k <test_name>

# Run tests relevant to the AM62L board
./scripts/twister -b am62l_evm/am62l3/a53 -T tests/
```

## Linting and Compliance

```bash
# Checkpatch on modified files (Linux kernel style)
./scripts/checkpatch.pl --no-tree -f <file>

# Run on a git range
./scripts/checkpatch.pl --no-tree --git HEAD~1..HEAD

# Compliance check (used by CI)
./scripts/ci/check_compliance.py -m Checkpatch -c HEAD~1..
```

## Code Style Rules (from `.github/copilot-instructions.md`)

- Tabs (8-char) for C; 2-space indent for CMake.
- Line length ≤ 100 columns.
- Always brace all control structures, even single-line.
- `/* */` comments only — `//` is not allowed in C.
- Fixed-width types (`uint32_t`, `int32_t`) instead of bare `int`/`char`.
- No `malloc`/`calloc` — static allocation only.
- SPDX header required on every new file:
  ```c
  // SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
  // SPDX-License-Identifier: Apache-2.0
  ```

## Commit Message Format

```
area: summary of change (≤75 chars total including area)

Body explaining what, why, assumptions, and how tested.
Lines ≤ 75 chars.

Assisted-by: Claude:claude-sonnet-4-6
Signed-off-by: Your Name <email>
```

- `AI agents must not add Signed-off-by` — only the human submitter signs off.
- Do **not** add `Co-authored-by`.
- Body must not be empty (CI enforces this).
- Use `git log <file>` to find the correct `area:` prefix for a file.

## Architecture: AM62L LPM / S2RAM Flow

The S2RAM path spans three layers:

1. **Zephyr (`soc/ti/k3/am62lx/power.c`)** — implements `pm_state_set()`:
   - On SMP: broadcasts SGI3 to CPU1 → CPU1 calls `PSCI_CPU_OFF`; CPU0 polls `psci_affinity_info()` until CPU1 is confirmed off.
   - CPU0 calls `arch_pm_s2ram_suspend()` → saves CPU context to DDR → issues `PSCI_CPU_SUSPEND` (deep sleep power state).
   - On resume: `arch_pm_s2ram_resume()` restores context; `k_smp_cpu_start(1, ...)` reinitialises and restarts CPU1.

2. **TF-A** (external, in `~/ti/trusted-firmware-a`) — receives `CPU_SUSPEND`, gates clocks, puts DDR into self-refresh, powers down A53 cores. On RTC wakeup, powers up A53 and returns to the Zephyr resume path.

3. **Zephyr PM device framework** — `CONFIG_PM_DEVICE_SYSTEM_MANAGED` calls `PM_DEVICE_ACTION_TURN_OFF` on all devices before suspend and `PM_DEVICE_ACTION_TURN_ON` after resume. The OMAP I2C driver's `TURN_ON` handler re-programs I2C hardware registers lost when the MAIN power domain is cut.

Key Kconfig symbols for this flow: `CONFIG_PM`, `CONFIG_PM_S2RAM`, `CONFIG_PM_DEVICE`, `CONFIG_PM_DEVICE_SYSTEM_MANAGED`, `CONFIG_SMP`.

## Source Tree Reference

| Path | Contents |
|---|---|
| `arch/arm64/` | ARM64 arch code including `arch_pm_s2ram_suspend/resume` |
| `boards/ti/am62l_evm/` | AM62L EVM board DTS, defconfig, pinctrl |
| `drivers/` | Device drivers (I2C, counter/RTC, EEPROM, …) |
| `dts/arm64/ti/` | AM62L devicetree source |
| `include/zephyr/pm/` | PM framework public API headers |
| `kernel/` | Core kernel (scheduler, SMP, idle thread) |
| `samples/boards/ti/am62l_lpm/` | Active LPM sample application |
| `soc/ti/k3/am62lx/` | AM62L SoC code (power.c, Kconfig) |
| `subsys/pm/` | Zephyr PM subsystem implementation |
