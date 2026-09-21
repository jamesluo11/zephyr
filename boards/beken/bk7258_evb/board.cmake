# Copyright (c) 2026 Beken Corporation
# SPDX-License-Identifier: Apache-2.0
#
# Flash and debug runner configuration for the BK7258 EVB.
#
# No runner is declared on purpose. Programming this board normally goes
# through the BootROM UART download protocol, which needs no debug probe but
# does need the vendor's loader tool on the host, driven over a USB-UART
# bridge. There is no west runner for that, and the image also has to be
# post-processed by the vendor packaging tool before it can be programmed.
#
# The board is fully usable for building; see the board documentation page for
# the flashing procedure.
#
# If an SWD probe is attached later (the SoC supports SWD), OpenOCD can be
# enabled here:
#
# board_runner_args(openocd "--cmd-pre-init=set _CHIPNAME bk7258")
# board_runner_args(openocd "--cmd-pre-init=source [find target/bk7258.cfg]")
# include(${ZEPHYR_BASE}/boards/common/openocd.board.cmake)
