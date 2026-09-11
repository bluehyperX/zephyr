# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Repository Overview

This is the Zephyr RTOS repository, used here primarily for TI AM62L SoC low-power mode (LPM / S2RAM) development. The active branch (`am62l_lpm2`) adds Suspend-to-RAM support for the AM62L EVM board, exercising the PSCI System Suspend path via TF-A.

Key files under active development:
- `samples/boards/ti/am62l_lpm/src/main.c` — S2RAM demo: RTC-woken suspend/resume with a DDR-retained cycle counter proving RAM retention across self-refresh
- `soc/ti/k3/am62lx/power.c` — AM62L PM backend: CPU1 off via SGI IPI + PSCI `CPU_OFF`, CPU0 PSCI `psci_system_suspend()`, SMP resume via `k_smp_cpu_start`

This zephyr repo is one of several sibling checkouts under `~/ti/`; `~/ti/trusted-firmware-a` (TF-A) and `~/ti/ti-linux-kernel` are relevant when tracing the S2RAM path below the Zephyr boundary.

## Essential Commands

### Build Commands
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

# Build for native simulation (useful for testing)
west build -b native_sim -d build_native samples/boards/ti/am62l_lpm
```

The binary to flash is `build_lpm/zephyr/zephyr.bin`.

### Testing
Zephyr uses Twister for its test suite:

```bash
# Run tests for a specific test directory
./scripts/twister -T tests/kernel/pm/ -p native_sim

# Run a single test case
./scripts/twister -T tests/kernel/pm/ -p native_sim -k <test_name>

# Run tests relevant to the AM62L board
./scripts/twister -b am62l_evm/am62l3/a53 -T tests/

# Run with verbose output
./scripts/twister -v -b am62l_evm/am62l3/a53 -T tests/

# Run only failing tests from previous run
./scripts/twister --only-failed
```

### Linting and Compliance

```bash
# Checkpatch on modified files (Linux kernel style)
./scripts/checkpatch.pl --no-tree -f <file>

# Run on a git range
./scripts/checkpatch.pl --no-tree --git HEAD~1..HEAD

# Compliance check (used by CI)
./scripts/ci/check_compliance.py -m Checkpatch -c HEAD~1..

# Run full compliance suite
./scripts/ci/check_compliance.py

# Check for spelling errors
./scripts/spelling.py
```

### Configuration
```bash
# Update kernel configuration
west build -b am62l_evm/am62l3/a53 -d build_lpm samples/boards/ti/am62l_lpm --menuconfig

# Save current configuration
west build -d build_lpm
cp build_lpm/zephyr/.config boards/ti/am62l_evm/am62l3/a53_defconfig

# List all available boards
west boards | grep am62l
```

## Code Structure

### Key Directories
- `arch/arm64/` - ARM64 architecture-specific code including `arch_pm_s2ram_suspend/resume`
- `boards/ti/am62l_evm/` - AM62L EVM board definitions (DTS, defconfig, pinctrl)
- `drivers/` - Device drivers (I2C, counter/RTC, EEPROM, etc.)
- `dts/arm64/ti/` - AM62L devicetree source
- `include/zephyr/pm/` - Power management framework public API headers
- `kernel/` - Core kernel (scheduler, SMP, idle thread)
- `samples/boards/ti/am62l_lpm/` - Active LPM sample application
- `soc/ti/k3/am62lx/` - AM62L SoC code (power.c, Kconfig)
- `subsys/pm/` - Zephyr power management subsystem implementation
- `scripts/` - Build, test, and validation tools (twister, checkpatch.pl, etc.)

### Power Management Flow (S2RAM)
The Suspend-to-RAM implementation varies by architecture:

#### ARM64 (TI AM62L) - Multicore Approach (SMP)
The Suspend-to-RAM implementation spans three layers:

1. **Zephyr Layer** (`soc/ti/k3/am62lx/power.c`):
   - `pm_state_set()` coordinates S2RAM entry
   - Only CPU0 drives S2RAM; CPU1 redirected to `k_cpu_idle()`
   - CPU0 signals CPU1 via SGI3; CPU1's ISR calls PSCI `CPU_OFF`
   - CPU0 busy-polls `psci_affinity_info()` until CPU1 reports off
   - CPU0 saves ARM virtual timer registers, calls `arch_pm_s2ram_suspend()`
   - On resume: restores CPU context, timer registers, then `k_smp_cpu_start(1, ...)`

2. **TF-A Layer** (in `~/ti/trusted-firmware-a`):
   - Receives PSCI System Suspend
   - Gates clocks, puts DDR into self-refresh, powers down A53 cores
   - On RTC wakeup, powers up A53 and returns to Zephyr resume path

3. **Zephyr PM Device Framework**:
   - With `CONFIG_PM_DEVICE_SYSTEM_MANAGED=y`
   - Calls `PM_DEVICE_ACTION_TURN_OFF` on all devices before suspend
   - Calls `PM_DEVICE_ACTION_TURN_ON` after resume

#### Cortex-M Approaches
Cortex-M implementations vary based on whether they are single-core or multicore systems:

##### Single-Core Cortex-M Approach
Most Cortex-M MCUs are single-core and handle S2RAM on that single core:

1. **NXP MCXN/LPC Series** (`soc/nxp/mcx/mcxn/s2ram.c`):
   - Saves NVIC registers (ISER, IPR) and SCB context to retained RAM
   - Uses VBAT domain to retain SRAM arrays during Deep Power Down
   - Saves/restores System Control Space (SCS) state including NVIC and SCB
   - Uses CMC (Clock Management Control) to enter Deep Power Down mode
   - Restores clock tree and SystemInit() on resume

2. **Nordic nRF54H Series** (`soc/nordic/nrf54h/soc_pm_s2ram.c`):
   - Saves NVIC context (ISER, ISPR, IPR), SCB context, and optionally MPU/FPU contexts
   - Uses SYSTEM_OFF mode with GPREGRET registers to indicate valid resume state
   - Powers down cache and FPU during suspend, powers up on resume
   - Uses reset reason checking to distinguish between cold boot and S2RAM resume

3. **Common Single-Core Cortex-M Pattern**:
   - CPU context saved to retained RAM before power-down
   - Minimal register set saved (typically NVIC, SCB, optionally MPU/FPU)
   - System-off mechanism varies by vendor (WFI/WFE with specific PMU configuration)
   - On resume, context is restored and execution continues from suspend point

##### Multicore Cortex-M (AMP) Approach
Some NXP and Nordic devices feature multicore Cortex-M systems, which are typically AMP (Asymmetric Multiprocessing) rather than SMP:

1. **NXP i.MX RT11xx Family** (Cortex-M7 + Cortex-M4):
   - Heterogeneous multicore with different Cortex-M variants
   - Each core managed independently via the General Power Controller (GPC)
   - Power states coordinated per-core; S2RAM entry requires both cores to cooperate
   - Uses CPU-specific power mode configurations and wakeup interrupts
   - Example: `soc/nxp/imxrt/imxrt11xx/power.c` shows separate GPC controls for M7 (CPU0) and M4 (CPU1) cores

2. **NXP i.MX 94 Family** (Dual Cortex-M7):
   - Homogeneous multicore Cortex-M7 system
   - Uses SCMI (System Control Management Interface) for inter-core communication
   - Each CPU can be independently suspended/resumed via SCMI commands
   - S2RAM involves coordinating sleep states across both cores via SCMI
   - Example: `soc/nxp/imx/imx9/imx943/pm_mcore.c` shows SCMI-based CPU power control

3. **Nordic NRF54H Series** (Dual Cortex-M33):
   - Homogeneous multicore with CPUapp (application) and CPURad (radio) cores
   - Different power state capabilities per core (CPUapp supports S2RAM, CPURad does not)
   - Power domains managed via LRCCONF (Low Retention Control Configuration)
   - Each core can retain different memory blocks during low power modes
   - Example: `soc/nordic/nrf54h/soc_power.c` shows domain-specific power control

4. **Common Multicore Cortex-M (AMP) Pattern**:
   - Each core manages its own power state independently
   - Power coordination typically via mailbox, interrupts, or management controller (SCMI/GPC)
   - S2RAM requires all participating cores to enter compatible low-power states
   - Context saving is per-core; each core saves its own NVIC/SCB/context
   - Memory retention may be core-specific or shared depending on architecture
   - On resume, each core restores its own context and resumes execution

### Critical Kconfig Symbols
- `CONFIG_PM`, `CONFIG_PM_S2RAM`, `CONFIG_PM_DEVICE`
- `CONFIG_PM_DEVICE_SYSTEM_MANAGED`
- `CONFIG_SMP` (relevant for multicore systems like AM62L; some AMP systems may use it for synchronization)
- `CONFIG_POWER_DOMAIN_ARM_SCMI_LPM_MANAGED=y` (in board defconfig for AM62L)
- `CONFIG_LOG_MODE_IMMEDIATE=y` (required in sample's prj.conf for AM62L)
- `CONFIG_HAS_PM_S2RAM_CUSTOM_MARKING` (enables custom resume detection mechanisms)
- `CONFIG_ARM_MPU` and `CONFIG_FPU` (affect context save/restore size on Cortex-M)
- Multicore-specific options:
  - `CONFIG_SCMI` (System Control Management Interface for inter-core communication)
  - `CONFIG_IPC` (Inter-Processor Communication for core coordination)
  - `CONFIG_MBOOT` (Mailbox mechanisms for AMP systems)

## Code Style Rules

Source: `.github/copilot-instructions.md`. Apply only to new or modified code.

### C Language
- Tabs (8-char) for indentation; line length ≤ 100 columns
- `snake_case` for all identifiers
- Always brace all control structures, even single-line bodies
- `/* */` comments only — `//` is not allowed (SPDX headers excepted)
- `/** */` for Doxygen API documentation
- No ASCII decoration or rulers in comments
- Fixed-width types (`uint32_t`, `int32_t`) instead of bare `int`/`char`
- No `malloc`/`calloc` — static allocation only (MISRA)
- No commented-out code sections (MISRA)
- Check return values of all functions that return error information (MISRA)
- Prefer `inline` or `static` functions over function-like macros (MISRA)
- Apply `u` or `U` suffix to all unsigned integer constants
- No binary literals (`0b...`); no octal constants
- All header files must have include guards
- Public API identifiers must be prefixed by subsystem: `k_` (kernel), `sys_`, `net_`, `bt_`, `i2c_`, etc.
- SPDX header required on every new file:
  ```c
  // SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
  // SPDX-License-Identifier: Apache-2.0
  ```

### Kconfig
- Tabs for indentation; `help` text: one tab + two extra spaces; line length ≤ 100 columns
- One blank line between symbol declarations; comments: `# Comment` (space after `#`)
- Use `menuconfig` (not `config`) for symbols with sub-options
- Encapsulate sub-symbols in an `if` block prefixed with enabling symbol's name
- Symbol naming: drivers → `{DRIVER_TYPE}_{DRIVER_NAME}`; SoCs → `SOC_FAMILY_...` / `SOC_SERIES_...`; samples → `SAMPLE_...`

### CMake
- 2-space indent; line length ≤ 100 characters
- Lowercase commands: `add_library`, not `ADD_LIBRARY`; no space before opening parenthesis
- One source file per line in `target_sources()` and similar calls
- `UPPERCASE` for cache variables; `lowercase/snake_case` for local variables
- Always quote strings and path variables; do not quote boolean values (`ON`, `OFF`)

## Commit Message Format

```
area: summary of change (≤75 chars total including area)

Body explaining what, why, assumptions, and how tested.
Lines ≤ 75 chars.

Assisted-by: Claude:claude-sonnet-4-6
Signed-off-by: Your Name <email>
```

- **AI agents must not add `Signed-off-by`** — only the human submitter signs off
- Do **not** add `Co-authored-by`
- Body must not be empty (CI enforces this)
- Use `git log <file>` to find the correct `area:` prefix for a file

## Development Workflow

1. Create feature branch: `git checkout -b feature/am62l-lpm-improvement`
2. Make changes following the above style guidelines
3. Build and test: `west build -b am62l_evm/am62l3/a53 -d build_lpm samples/boards/ti/am62l_lpm`
4. Validate with checkpatch: `./scripts/checkpatch.pl --no-tree -f <modified_file>`
5. Commit with proper format (AI adds Assisted-by, human adds Signed-off-by)
6. Create pull request targeting `main` branch