# Copyright (c) 2024 Nordic Semiconductor ASA
# SPDX-License-Identifier: Apache-2.0

add_subdirectory(./common)

add_subdirectory_ifdef(CONFIG_SOC_SERIES_AM62LX_A53 am62lx)
