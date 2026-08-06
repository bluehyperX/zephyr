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
