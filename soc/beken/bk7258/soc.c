/*
 * Copyright (c) 2026 Beken Corporation
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * BK7258 SoC-level initialisation.
 *
 * There is little to configure: there are no clock gating or power domain
 * registers to set up, interrupts go straight to the NVIC and are handled by
 * the generic Cortex-M code, and startup assembly, the vector table and the
 * system tick all come from Zephyr.
 */

#include <soc.h>

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_io.h>

#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(soc_bk7258, CONFIG_SOC_LOG_LEVEL);

static int soc_bk7258_early_init(void)
{
	uint32_t reg = sys_read32(SOC_SYS_CPU_CLK_DIV_MODE1_ADDR);

	/*
	 * The console port's clock gate is off when this image owns the chip:
	 * the firmware the device ships with turns it on. Enabling it here when
	 * it is already on changes nothing.
	 */
	if (!IS_ENABLED(CONFIG_SOC_BK7258_VENDOR_FIRMWARE)) {
		sys_set_bits(SOC_SYS_CPU_DEVICE_CLK_ENABLE_ADDR, SOC_SYS_UART0_CKEN_BIT);
	}

	/*
	 * Report that this image is up before anything else runs: the peer waits
	 * only 2 s for that indication after releasing it, so the call must not
	 * sit behind a later init step that could stall.
	 *
	 * The periodic heartbeat that follows comes from soc_ipc.c.
	 */
	if (IS_ENABLED(CONFIG_SOC_BK7258_IPC_HEARTBEAT)) {
		soc_bk7258_ipc_power_up();
	}

	/*
	 * Print only; no register is touched. If the frequency turns out to be
	 * something else, the DPLL and dividers are configured from here rather
	 * than poked in the log path, so that "does not run at all" and "clock
	 * configured wrong" stay distinguishable.
	 */
	LOG_INF("SYS_CPU_CLK_DIV_MODE1(%#x) = %#010x", SOC_SYS_CPU_CLK_DIV_MODE1_ADDR, reg);
	LOG_INF("  clkdiv_core=%u cksel_core=%u clkdiv_bus=%u", reg & 0xFU, (reg >> 4) & 0x3U,
		(reg >> 6) & 0x1U);
	LOG_INF("  clkdiv_uart0=%u clksel_uart0=%u (0=XTAL26M 1=APLL)",
		(reg >> SOC_SYS_CLKDIV_UART0_SHIFT) & 0x3U,
		/* BIT() is 1UL, so narrow explicitly: a bare %u trips -Wformat. */
		(unsigned int)((reg & SOC_SYS_CLKSEL_UART0_BIT) >> 10));
	LOG_INF("expected: clksel_uart0=0 (XTAL) and a CPU frequency that makes "
		"CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC=%u hold",
		CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC);

	return 0;
}

SYS_INIT(soc_bk7258_early_init, PRE_KERNEL_1, 0);
