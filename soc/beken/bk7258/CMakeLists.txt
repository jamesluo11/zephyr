# Copyright (c) 2026 Beken Corporation
# SPDX-License-Identifier: Apache-2.0
#
# BK7258 SoC sources
#
# soc.h has to register its own include path here: do not drop the
# zephyr_include_directories(.) line below.
#
# Zephyr has no mechanism that puts an SoC directory on the include path
# automatically:
#   - cmake/modules/soc.cmake:30-35 only registers the soc.yml found in
#     SOC_<name>_DIRECTORIES as a configure dependency; it sets no include
#     directory
#   - grepping the whole tree for SOC_DIR / SOC_FULL_DIR only hits one
#     cmake_path(IS_PREFIX ...) test in soc/CMakeLists.txt:6, unrelated to
#     includes
# The upstream convention is "whoever ships soc.h calls
# zephyr_include_directories(.) themselves":
#   - nordic: soc/nordic/nrf52/CMakeLists.txt
#   - st:     soc/st/stm32/<series>/CMakeLists.txt
# There are 308 such calls in the tree: a hard requirement, not an optimisation.
#
# Without it the very first build fails with
# `fatal error: soc.h: No such file or directory`, and not just for this
# directory: zephyr/modules/cmsis_6/cmsis_core_m.h in the arch layer also does
# #include <soc.h>, so even the Cortex-M arch cannot get through and the build
# never reaches the application. Three files include <soc.h> in this port:
# soc.c, drivers/serial/uart_bk7258.c and samples/hello_world/src/main.c.
#
# (An earlier comment claimed the search path came from Zephyr's SOC_DIR
# mechanism and no zephyr_include_directories() was needed. Checking the whole
# Zephyr 4.4.2 tree on 2026-09-17 showed that to be wrong; corrected here.)

zephyr_include_directories(.)
zephyr_sources(soc.c)
zephyr_sources_ifdef(CONFIG_SOC_BK7258_IPC_HEARTBEAT soc_ipc.c)

# ---------------------------------------------------------------------------
# Linker script
#
# The first real build failed with:
# `CMake Error ... Could not find linker script: ''`
#
# Zephyr's `choice LINKER_SCRIPT` (Kconfig.zephyr:191) defaults to
# LD_LINKER_TEMPLATE (because LD_LINKER_SCRIPT_SUPPORTED defaults to y). In that
# mode zephyr/CMakeLists.txt:1203-1210 looks for, in order,
#     CONFIG_HAVE_CUSTOM_LINKER_SCRIPT -> BOARD_LINKER_SCRIPT
#     -> SOC_LINKER_SCRIPT
# and if none of the three is set it fails with
# "Could not find linker script: ''". The SoC has to name its linker script;
# Zephyr does not guess.
#
# No custom script is needed here: the devicetree already declares zephyr,flash
# and zephyr,sram (flash0 = the XIP window at 0x02150000, sram0 = 0x28010000),
# so the generic Cortex-M template lays the image out from those regions:
#
#     ${ZEPHYR_BASE}/include/zephyr/arch/arm/cortex_m/scripts/linker.ld
#
# That is exactly what upstream does: every STM32 series (stm32u5x, stm32h7x,
# stm32g0x, ...) has this same line in its series CMakeLists.txt.
# ---------------------------------------------------------------------------
set(SOC_LINKER_SCRIPT ${ZEPHYR_BASE}/include/zephyr/arch/arm/cortex_m/scripts/linker.ld CACHE INTERNAL "")
