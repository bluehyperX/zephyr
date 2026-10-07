# AGENTS.md

Quick orientation for agents working in this checkout. Read `CLAUDE.md`
(repo root) first — it has the full build/test/style/commit workflow for
the TI AM62L LPM work. This file only adds what `CLAUDE.md` and
`.github/copilot-instructions.md` don't already cover.

## Workspace structure (easy to get wrong)

- This `zephyr/` directory is **not** the west workspace root. The root is
  the parent `~/ti/zephyrproject/` — it holds `.west/`, `modules/`,
  `bootloader/`, `tools/`, and the Python venv. `west` commands work from
  either directory, but always use the workspace venv:
  `~/ti/zephyrproject/.venv` (Python 3.12, west 1.5.0). A bare `west` on
  `$PATH` may resolve via a pyenv shim instead — if a `west` command
  behaves unexpectedly, check `which west` and prefer activating the venv.
- Sibling checkouts `~/ti/trusted-firmware-a` and `~/ti/ti-linux-kernel`
  are separate git repos, relevant when tracing the S2RAM path below the
  Zephyr boundary (TF-A does the actual PSCI suspend/resume sequence;
  Zephyr only saves context and hands off).
- Multiple stale build dirs already exist at repo root (`build/`,
  `build_lpm/`, `build_lpm2/`), all for the same
  `am62l_evm/am62l3/a53` + `samples/boards/ti/am62l_lpm` app from earlier
  sessions. Reuse `build_lpm` per `CLAUDE.md`'s documented commands, or
  pick a new `-d` name — don't assume these dirs are disposable scratch.

## Git state

- `CLAUDE.md` says the active branch is `am62l_lpm2` — verify with
  `git branch --show-current` before relying on that; the branch name
  drifts across sessions (observed on `test`, then `test2`).
- `git log --oneline` on this branch is a sequence of local WIP commits
  (`WIP`, `Both CPU Deepsleep working with RTC`, …) — this is scratch
  history, not upstream-clean. Don't assume recent commits are
  individually meaningful; check the diff, not just the message.

## Compliance / CI entrypoints not in CLAUDE.md

- `./scripts/ci/check_compliance.py -l` lists which individual checks
  exist; CI runs `./scripts/ci/check_compliance.py --annotate -c
  origin/<base>..` against the PR diff, not the whole tree.
- DCO (`Signed-off-by`) is enforced by CI, not just convention — commits
  without it fail. Only the human adds it (`git commit -s`); agents must
  not add `Signed-off-by` or `Co-authored-by`.
- `west boards | grep am62l` confirms `am62l_evm` is a valid board target
  if you need to double check before a build.

## Scope note

This is the full upstream `zephyrproject-rtos/zephyr` monorepo (`arch/`,
`drivers/`, `subsys/`, `tests/`, etc. — see `.github/copilot-instructions.md`
for the full source-tree table and official contribution rules:
licensing/SPDX, MISRA subset, Kconfig/CMake style, commit format). Active
work in this checkout touches only a small AM62L-specific slice; don't
assume changes elsewhere in the tree are in scope unless asked.
