# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Build System

Zephyr uses `west` (a meta-tool) as its primary build frontend. All builds are
out-of-tree — `zephyr/` is **not** the application root; applications live
alongside it in the workspace.

```bash
# Build a sample for a specific board (pristine build)
west build -p always -b <BOARD> samples/hello_world

# Incremental rebuild (reuses existing build directory)
west build

# Build in an explicit directory
west build -b <BOARD> -d build/my_build samples/hello_world

# Flash to hardware
west flash

# Open an interactive debugger
west debug
```

CMake can be used directly, but `west build` is preferred. `ZEPHYR_BASE` must
point to this repository when using CMake directly.

## Testing

Zephyr uses **Twister** as its test runner and **Ztest** as its test framework.

```bash
# Run all tests under a directory on native_sim (host)
./scripts/twister -b -p native_sim -T tests/kernel/

# Run a single test scenario by name
./scripts/twister --scenario tests/foo/bar/your.test.scenario.name

# List all tests in a directory
./scripts/twister --list-tests -T tests/kernel

# Run tests via west wrapper
west twister -T tests/ztest/base
```

Two host-based test targets exist:
- **`native_sim`**: builds the full Zephyr OS for the host; use for integration
  tests. Tests do **not** set `type: unit`.
- **`unit_testing`**: links only the module under test + Ztest stubs; set
  `type: unit` in `tests.yaml`. Faster but no kernel context.

## Style and Compliance

Run the compliance script on your commit range before opening a PR:

```bash
./scripts/ci/check_compliance.py -c <commit-range>
```

For style-only checks on individual files:

```bash
# C style
git diff --cached | ./scripts/checkpatch.pl -

# Quick reformat of new C files (then verify against guidelines manually)
clang-format -i my_source_file.c

# Kconfig style (recursive)
./scripts/kconfig/kconfig_style.py drivers/sensor/
```

## Code Style Rules

Follow [Linux kernel coding style](https://kernel.org/doc/html/latest/process/coding-style.html) with these Zephyr-specific overrides:

- **Tabs** are 8 characters; **line length** ≤ 100 columns.
- Always add braces to `if`, `else`, `do`, `while`, `for`, and `switch` bodies — even single-line blocks.
- Use `/* */` for comments; `//` is **not** allowed (SPDX headers are the only exception).
- Use `/** */` for Doxygen API documentation.
- No binary literals (`0b...`), no non-ASCII symbols, no ASCII decoration in comments.
- Use fixed-width types (`uint8_t`, `int32_t`) instead of bare `int`/`char`.
- Capitalize correctly in comments: `UART` not `uart`, `CMake` not `cmake`.
- All public APIs must be prefixed by subsystem: `k_` (kernel), `sys_`, `net_`,
  `bt_`, `i2c_`, etc.
- Style is enforced on **new or modified code only** — do not reformat unrelated code.

### MISRA-C Rules (non-obvious subset)

- No dynamic memory allocation (`malloc`, `calloc`).
- No commented-out code sections.
- All header files must have include guards.
- No octal constants (`0755` is forbidden; use hex or decimal).
- Apply a `u` or `U` suffix to all unsigned integer constants; never use lowercase `l`.
- Check the return value of every function that returns error information.
- Prefer `static inline` functions over function-like macros.
- Identifiers must be unique across scopes; no shadowing of outer-scope identifiers.

### CMake Style

- 2-space indentation (no tabs); ≤ 100 columns.
- Lowercase commands: `add_library`, not `ADD_LIBRARY`.
- No space between command and parenthesis: `if(...)` not `if (...)`.
- UPPERCASE for cache/shared variables; lowercase/snake_case for locals.
- Quote strings and path variables; do not quote boolean values (`ON`, `OFF`).
- One source file argument per line in `target_sources()` and similar calls.

### Kconfig Style

- Tab indentation; `help` text: one tab + two extra spaces.
- One blank line between symbol declarations; one blank line before/after top-level `if`/`endif`.
- Comments: `# Comment` (space after `#`).
- Use `menuconfig` (not `config`) for symbols that have sub-options; encapsulate sub-symbols in an `if` block keyed to the enabling symbol and prefix them with the enabling symbol's name.

Symbol naming conventions:

| Subtree | Symbol format |
|---|---|
| Drivers | `{DRIVER_TYPE}_{DRIVER_NAME}` |
| Sensors | `SENSOR_{SENSOR_NAME}` |
| Samples | `SAMPLE_...` |
| Tests | `TEST_...` |
| Boards | `BOARD_...` |
| SoCs | `SOC_FAMILY_...`, `SOC_SERIES_...`, or `SOC_...` |

### Devicetree Style

- Tab indentation; tab = 8 characters.
- Dashes for node/property names; underscores for node labels.
- Single space on each side of `=` in property definitions.

## Commit Message Format

```
[area]: [summary of change]

[Commit message body — must be non-empty. Explain what, why, assumptions, and
how you know it works. Lines ≤ 75 chars.]

Assisted-by: Claude:claude-sonnet-4-6
Signed-off-by: Your Full Name <your.email@example.com>
```

Rules:
- Title: one line, ≤ 75 characters, followed by a blank line.
- Body **must not be empty** (CI will fail). Lines ≤ 75 characters.
- `Signed-off-by` is required for DCO compliance (`git commit -s`). **AI agents must not add `Signed-off-by`** — only the human submitter may sign.
- Do **not** add `Co-authored-by`.
- When AI tools are used, add `Assisted-by: Claude:<model-version>` (add tool names if advanced tools like coccinelle were used).
- To auto-close a GitHub issue on merge, add `Fixes #[issue number]` to the commit message body or PR description.

Common `[area]` prefixes (run `git log <file>` to follow existing conventions):
- `doc:` — documentation changes
- `drivers: <type>: <name>:` — driver changes
- `Bluetooth: <component>:` — Bluetooth changes
- `net: <component>:` — networking changes
- `kernel:` — kernel changes
- `dts:` — treewide devicetree changes
- `boards: <vendor>:` — board configuration changes

## Licensing

All new files must have SPDX headers at the very top:

```c
// SPDX-FileCopyrightText: Copyright The Zephyr Project Contributors
// SPDX-License-Identifier: Apache-2.0
```

Use the file's native comment syntax. "Copyright The Zephyr Project Contributors"
is the preferred copyright form.

## Repository Architecture

| Directory | Contents |
|---|---|
| `arch/` | Architecture-specific kernel code (arm, arm64, x86, riscv, xtensa, arc, mips, …) |
| `boards/` | Board definitions (DTS, Kconfig, cmake) |
| `drivers/` | Device drivers organized by bus/peripheral type |
| `dts/` | Devicetree source files and bindings (`bindings/` subdirectory) |
| `include/zephyr/` | Public API headers |
| `kernel/` | Architecture-independent kernel (scheduling, IPC, memory) |
| `lib/` | Libraries (minimal libc, posix, cpp, …) |
| `samples/` | Sample applications |
| `scripts/` | Build tooling, CI, twister, checkpatch, west commands |
| `soc/` | SoC-specific code and Kconfig |
| `subsys/` | Subsystems: networking, Bluetooth, USB, FS, logging, mgmt, … |
| `tests/` | Tests and benchmarks (ztest-based) |

### Build System Flow

1. An application's `CMakeLists.txt` calls `find_package(Zephyr)` to pull in Zephyr's build system.
2. Kconfig runs to produce `.config` from `prj.conf` + board + SoC Kconfig trees.
3. Devicetree compiler processes `.dts` + overlays into a generated header (`devicetree_generated.h`).
4. CMake compiles kernel + drivers + subsystems + application into a single ELF.
5. The `zephyr.elf`/`.bin`/`.hex` outputs live in the build directory.

Syscall stubs are auto-generated from headers annotated with `__syscall`. The
`arch/` and `soc/` layers provide the platform-specific glue (interrupt
controllers, MMU, linker scripts, context switching).

### Key Abstractions

- **Kconfig**: software feature selection (compile-time `CONFIG_*` symbols).
- **Devicetree**: hardware description (runtime `DT_*` macros, `DEVICE_DT_GET`).
- **Device driver model**: `DEVICE_DEFINE` / `DEVICE_DT_DEFINE` + API structs.
- **Ztest**: `ZTEST_SUITE` + `ZTEST(suite, test_name)` macros.

## TI AM62L Platform (branch: am62l_lpm)

The `am62l_lpm` branch implements S2RAM (suspend-to-RAM / deep-sleep LPM)
for the TI AM62L EVM (Cortex-A53, arm64). This is the first S2RAM implementation
for any arm64 board in upstream Zephyr.

### Board-specific build commands

```bash
# Build the LPM sample for AM62L
west build -p always -b am62l_evm/am62l3/a53 samples/boards/ti/am62l_lpm

# Build a simple hello-world to test the board toolchain
west build -p always -b am62l_evm/am62l3/a53 samples/hello_world

# Run compliance on the LPM feature commits
./scripts/ci/check_compliance.py -c cde0a961d82..HEAD
```

### Full firmware stack build (TF-A → U-Boot → Zephyr)

All three components must be built to produce a bootable SD card image.
The toolchain is at `/opt/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-linux-gnu/`.

#### TF-A

```bash
cd /home/a0510137local/ti/trusted-firmware-a

# Clean
make PLAT=k3low TARGET_BOARD=am62lx clean

# Build
make -j32 \
  CROSS_COMPILE=/opt/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu- \
  ARCH=aarch64 PLAT=k3low TARGET_BOARD=am62lx
```

Output: `build/k3low/am62lx/release/bl1.bin` and `bl31.bin`.

#### U-Boot

```bash
cd /home/a0510137local/ti/ti-u-boot

# Clean
rm -rf out/a53_am62l

# Configure
make ARCH=arm \
  CROSS_COMPILE=/opt/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu- \
  am62lx_evm_defconfig O=out/a53_am62l

# Workaround for pylibfdt _shipped bug
mkdir -p out/a53_am62l/scripts/dtc/pylibfdt
cp scripts/dtc/pylibfdt/libfdt.i_shipped out/a53_am62l/scripts/dtc/pylibfdt/libfdt.i
make ARCH=arm \
  CROSS_COMPILE=/opt/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu- \
  O=out/a53_am62l scripts/dtc/pylibfdt/

# Build
make -j32 \
  CROSS_COMPILE=/opt/arm-gnu-toolchain-15.2.rel1-x86_64-aarch64-none-linux-gnu/bin/aarch64-none-linux-gnu- \
  ARCH=arm \
  BL1=/home/a0510137local/ti/trusted-firmware-a/build/k3low/am62lx/release/bl1.bin \
  BL31=/home/a0510137local/ti/trusted-firmware-a/build/k3low/am62lx/release/bl31.bin \
  BINMAN_INDIRS=/home/a0510137local/ti/ti-linux-firmware \
  TEE=/home/a0510137local/Downloads/tee.bin \
  PYTHONPATH=/home/a0510137local/ti/ti-u-boot/out/a53_am62l/scripts/dtc/pylibfdt \
  O=out/a53_am62l
```

#### Deploy

```bash
# Copy U-Boot artifacts to SD card (mount at /media/a0510137local/boot/)
cp /home/a0510137local/ti/ti-u-boot/out/a53_am62l/tiboot3.bin \
   /home/a0510137local/ti/ti-u-boot/out/a53_am62l/tispl.bin \
   /home/a0510137local/ti/ti-u-boot/out/a53_am62l/u-boot.img \
   /media/a0510137local/boot/
sync

# Copy Zephyr binary to tftp server (U-Boot loads via network)
sudo cp /home/a0510137local/ti/zephyrproject/zephyr/build/zephyr/zephyr.bin /srv/tftp/zephyr.bin
```

### S2RAM architecture — key files and their roles

The feature is spread across several layers that must all be consistent:

| File | Role |
|------|------|
| `arch/arm64/core/pm_s2ram.S` | Save/restore callee-saved GPRs + SP/VBAR_EL1; `arch_pm_s2ram_resume` is the EL1 re-entry point passed to TF-A |
| `arch/arm64/core/pm_s2ram.c` | `_cpu_context` struct (`_cpu_context_t`), suspend marker (magic 0xDABBAD00 in `__noinit` DDR) |
| `include/zephyr/arch/arm64/cpu.h` | `_cpu_context_t` struct (sp + vbar_el1); `arch_pm_s2ram_resume` extern declaration |
| `soc/ti/k3/am62lx/power.c` | `pm_state_set()` / `pm_state_exit_post_ops()`; SMP CPU1 OFF handshake; `am62l_system_off()` calls `psci_system_suspend()` |
| `drivers/pm_cpu_ops/pm_cpu_ops_psci.c` | `psci_system_suspend(entry_point)` — PSCI 1.0 SYSTEM_SUSPEND SMC; also `psci_affinity_info()` for SMP polling |
| `drivers/counter/counter_k3_rtc.c` | K3 RTC counter driver — wakeup alarm, `GENERAL_CTL` unlock sequence, PM suspend/resume hooks |
| `drivers/i2c/i2c_omap.c` | OMAP I2C PM hooks — re-programs all hardware registers on `TURN_ON` (registers lost when MAIN domain is cut) |
| `drivers/serial/uart_ns16550.c` | NS16550 UART PM hooks — saves/restores UART registers across S2RAM |
| `drivers/power_domain/power_domain_arm_scmi.c` | `CONFIG_POWER_DOMAIN_ARM_SCMI_LPM_MANAGED` bypass — skips Zephyr SCMI power-off/on since TF-A/TIFS manages domains during LPM |
| `dts/arm64/ti/am62l3_a53.dtsi` | `idle-states` + `cpu-idle-states` that wire `suspend-to-ram` into the PM policy |
| `dts/vendor/ti/am62l-wakeup.dtsi` | `wkup_rtc0` node (address `0x2b1f0000`, compat `ti,am62l-rtc`) |
| `samples/boards/ti/am62l_lpm/` | End-to-end demo: arm RTC alarm → force S2RAM → verify I2C EEPROM after resume |

### Suspend call chain (CPU0)

```
pm_state_set(S2RAM)
  → [SMP] signal CPU1 via SGI3 → CPU1 calls pm_cpu_off() (PSCI CPU_OFF)
  → [SMP] CPU0 polls psci_affinity_info(CPU1_MPIDR) until AFF_STATE_OFF
  → arch_pm_s2ram_suspend(am62l_system_off)   [pm_s2ram.S]
    → save x19–x30 on stack, SP + VBAR_EL1 to _cpu_context
    → pm_s2ram_mark_set()  (write 0xDABBAD00 to __noinit DDR)
    → am62l_system_off()
      → psci_system_suspend(arch_pm_s2ram_resume)  ← SYSTEM_SUSPEND not CPU_SUSPEND
        → SMC → TF-A EL3 → TIFS: DDR self-refresh, A53 power-down

[RTC alarm fires]
TF-A EL3 → restore EL1 sysregs → ERET to arch_pm_s2ram_resume
  → restore VBAR_EL1 + SP from _cpu_context (before any C call)
  → pm_s2ram_mark_check_and_clear()
  → pop x19–x30 from DDR-retained stack
  → return x0=0 to arch_pm_s2ram_suspend call site
pm_state_exit_post_ops()  → irq_unlock(0)
  → [SMP] k_smp_cpu_resume(1) → PSCI CPU_ON cold-boots CPU1
```

### SMP behaviour

CPU1 shutdown/resume is controlled by `soc_per_core_init_hook()` (called by
`arch_secondary_cpu_init()` after GIC init) and `am62l_s2ram_cpu1_ipi()` (SGI3
handler):

- CPU1 runs `pm_cpu_off()` (PSCI CPU_OFF)
- CPU0 polls `psci_affinity_info()` until AFF_STATE_OFF
- On resume `k_smp_cpu_resume()` cold-boots CPU1 via PSCI CPU_ON

### PM-relevant Kconfig symbols

| Symbol | Where set | Meaning |
|--------|-----------|---------|
| `CONFIG_PM` | defconfig | Enable PM subsystem |
| `CONFIG_PM_S2RAM` | auto (Kconfig) | Auto-selected when `ARCH_HAS_SUSPEND_TO_RAM` + DT power-state present |
| `CONFIG_PM_DEVICE_SYSTEM_MANAGED` | defconfig | Kernel calls TURN_OFF/TURN_ON on all devices around suspend |
| `CONFIG_POWER_DOMAIN_ARM_SCMI_LPM_MANAGED` | defconfig | Skip Zephyr SCMI power-off calls — TF-A/TIFS manages domains during LPM |

### AM62L LPM modes

S2RAM uses **deep sleep** (`TI_K3_SLEEP_MODE_DEEP_SLEEP = 0x0`): A53 cluster
powered off, DDR retained in self-refresh, all standard wakeup sources active.

| TIFS mode value | Name | Description |
|---|---|---|
| `0x0` | `DEEP_SLEEP` | S2RAM — A53 off, DDR retained, all wakeup sources |
| `0x6` | `RTC_PLUS_DDR` | A53 off, DDR retained, RTC wakeup only |
| `0x7` | `RTC_ONLY` | Most aggressive — DDR not retained |
| `0xFF` | `INVALID` | Sentinel — never passed to TIFS |

**TF-A bug 1 — missing `am62l_lpm_state` assignment:** `am62l_get_sys_suspend_power_state` in
`plat/ti/k3low/common/am62l_psci.c` must set
`am62l_lpm_state = TI_K3_SLEEP_MODE_DEEP_SLEEP`. Without this assignment,
the `k3_gic_its_save()` call in `am62l_pwr_domain_suspend` is skipped
(it is gated on `am62l_lpm_state != INVALID`), leaving GIC ITS context
lost on resume and causing a corrupted PC on re-entry to Zephyr.

**TF-A bug 2 — bakery lock deadlock on warmboot:** The PSCI framework holds
power-domain bakery locks throughout `pwr_domain_suspend()` and releases
them at `psci_cpu_suspend_start:214` only if that callback returns. The
original AM62L code called `k3low_suspend_to_ram()` (the final WFI) at
the end of `pwr_domain_suspend()`, which never returned, so the lock was
still held in DDR-retained memory on warm-boot. `psci_warmboot_entrypoint`
then hit `assert(!is_lock_acquired(...))` in `bakery_get_ticket()`. Fix:
remove `k3low_suspend_to_ram()` from `pwr_domain_suspend()` (let it return
normally) and call it from `pwr_domain_pwr_down()` instead, which runs
after the PSCI lock release.
