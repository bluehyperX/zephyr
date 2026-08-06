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
- Use `/* */` for comments; `//` is **not** allowed.
- Use `/** */` for Doxygen API documentation.
- No binary literals (`0b...`), no non-ASCII symbols.
- Use fixed-width types (`uint8_t`, `int32_t`) instead of bare `int`/`char`.
- Capitalize correctly in comments: `UART` not `uart`, `CMake` not `cmake`.
- No dynamic memory allocation (`malloc`, `calloc`) — MISRA-C requirement.
- All public APIs must be prefixed by subsystem: `k_` (kernel), `sys_`, `net_`,
  `bt_`, `i2c_`, etc.
- Style is enforced on **new or modified code only** — do not reformat unrelated code.

### CMake Style

- 2-space indentation (no tabs); ≤ 100 columns.
- Lowercase commands: `add_library`, not `ADD_LIBRARY`.
- No space between command and parenthesis: `if(...)` not `if (...)`.
- UPPERCASE for cache/shared variables; lowercase/snake_case for locals.

### Kconfig Style

- Tab indentation; `help` text: one tab + two extra spaces.
- One blank line between symbol declarations.
- Comments: `# Comment` (space after `#`).

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
- Title: one line, ≤ 72 characters, followed by a blank line.
- Body **must not be empty** (CI will fail).
- `Signed-off-by` is required for DCO compliance (`git commit -s`). **AI agents must not add `Signed-off-by`** — only the human submitter may sign.
- Do **not** add `Co-authored-by`.
- When AI tools are used, add `Assisted-by: Claude:<model-version>`.

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

The `am62l_lpm` branch implements S2RAM (suspend-to-RAM / RTC+DDR low power mode)
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

The feature is spread across six layers that must all be consistent:

| File | Role |
|------|------|
| `arch/arm64/core/pm_s2ram.S` | Save/restore callee-saved GPRs + SP/VBAR_EL1; `arch_pm_s2ram_resume` is the EL1 re-entry point passed to TF-A |
| `arch/arm64/core/pm_s2ram.c` | `_cpu_context` struct, suspend marker (magic 0xDABBAD00 in `__noinit` DDR) |
| `soc/ti/k3/am62lx/power.c` | `pm_state_set()` / `pm_state_exit_post_ops()`; SMP CPU1 OFF handshake before suspend |
| `drivers/pm_cpu_ops/pm_cpu_ops_psci.c` | `psci_cpu_suspend()` — the SMC that drops into TF-A |
| `dts/arm64/ti/am62l3_a53.dtsi` | `idle-states` + `cpu-idle-states` that wire `suspend-to-ram` into the PM policy |
| `dts/vendor/ti/am62l-wakeup.dtsi` | `wkup_rtc0` node (address `0x2b1f0000`, compat `ti,am62l-rtc`) |
| `samples/boards/ti/am62l_lpm/` | End-to-end demo: arm RTC alarm → force S2RAM → verify I2C EEPROM after resume |

### Suspend call chain (CPU0)

```
pm_state_set(S2RAM)
  → [SMP] signal CPU1 via SGI3 → CPU1 calls psci_cpu_suspend or pm_cpu_off
  → arch_pm_s2ram_suspend(am62l_system_off)   [pm_s2ram.S]
    → save x19–x30 on stack, SP + VBAR_EL1 to _cpu_context
    → pm_s2ram_mark_set()  (write 0xDABBAD00 to __noinit DDR)
    → am62l_system_off()
      → psci_cpu_suspend(LPM_PSTATE_DEEPSLEEP, arch_pm_s2ram_resume)
        → SMC → TF-A EL3 → TIFS: DDR self-refresh, A53 power-down

[RTC alarm fires]
TF-A EL3 → restore EL1 sysregs → ERET to arch_pm_s2ram_resume
  → restore VBAR_EL1 + SP from _cpu_context (before any C call)
  → pm_s2ram_mark_check_and_clear()
  → pop x19–x30 from DDR-retained stack
  → return x0=0 to arch_pm_s2ram_suspend call site
pm_state_exit_post_ops()  → irq_unlock(0)
  → [SMP] wake CPU1 via SGI3
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
