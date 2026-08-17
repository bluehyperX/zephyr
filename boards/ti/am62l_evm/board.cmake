# SPDX-License-Identifier: Apache-2.0
# Copyright The Zephyr Project Contributors

board_runner_args(openocd
  "--config" "${BOARD_DIR}/support/openocd.cfg"
  "--target-handle" "am62l.cpu.a53.0"
)

include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
