/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/err.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/memory.h>
#include <asm/hardware/gic.h>
#include <asm/mach/map.h>
#include <asm/mach/arch.h>
#include <mach/board.h>
#include <mach/gpiomux.h>
#include <mach/msm_iomap.h>
#include <mach/msm_memtypes.h>
#include <mach/msm_smd.h>
#include <mach/restart.h>
#include <mach/socinfo.h>
#include <mach/clk-provider.h>
#include "board-dt.h"
#include "clock.h"
#include "devices.h"
#include "platsmp.h"

static struct memtype_reserve apq8084_reserve_table[] __initdata = {
	[MEMTYPE_SMI] = {
	},
	[MEMTYPE_EBI0] = {
		.flags  =       MEMTYPE_FLAGS_1M_ALIGN,
	},
	[MEMTYPE_EBI1] = {
		.flags  =       MEMTYPE_FLAGS_1M_ALIGN,
	},
};

static int apq8084_paddr_to_memtype(phys_addr_t paddr)
{
	return MEMTYPE_EBI1;
}

static struct reserve_info apq8084_reserve_info __initdata = {
	.memtype_reserve_table = apq8084_reserve_table,
	.paddr_to_memtype = apq8084_paddr_to_memtype,
};

void __init apq8084_reserve(void)
{
	reserve_info = &apq8084_reserve_info;
	of_scan_flat_dt(dt_scan_for_memory_reserve, apq8084_reserve_table);
	msm_reserve();
}

static void __init apq8084_early_memory(void)
{
	reserve_info = &apq8084_reserve_info;
	of_scan_flat_dt(dt_scan_for_memory_hole, apq8084_reserve_table);
}

static struct clk_lookup msm_clocks_dummy[] = {
	CLK_DUMMY("core_clk",   BLSP1_UART_CLK, "f991f000.serial", OFF),
	CLK_DUMMY("iface_clk",  BLSP1_UART_CLK, "f991f000.serial", OFF),
};

static struct clock_init_data msm_dummy_clock_init_data __initdata = {
	.table = msm_clocks_dummy,
	.size = ARRAY_SIZE(msm_clocks_dummy),
};

/*
 * Used to satisfy dependencies for devices that need to be
 * run early or in a particular order. Most likely your device doesn't fall
 * into this category, and thus the driver should not be added here. The
 * EPROBE_DEFER can satisfy most dependency problems.
 */
void __init apq8084_add_drivers(void)
{
	msm_smd_init();
	msm_clock_init(&msm_dummy_clock_init_data);
}

static void __init apq8084_map_io(void)
{
	msm_map_8084_io();
}

void __init apq8084_init(void)
{
	if (socinfo_init() < 0)
		pr_err("%s: socinfo_init() failed\n", __func__);

	apq8084_init_gpiomux();
	of_platform_populate(NULL, of_default_bus_match_table, NULL, NULL);
	apq8084_add_drivers();
}

void __init apq8084_init_very_early(void)
{
	apq8084_early_memory();
}

static const char *apq8084_dt_match[] __initconst = {
	"qcom,apq8084",
	NULL
};

DT_MACHINE_START(APQ8084_DT, "Qualcomm APQ 8084 (Flattened Device Tree)")
	.map_io = apq8084_map_io,
	.init_irq = msm_dt_init_irq,
	.init_machine = apq8084_init,
	.handle_irq = gic_handle_irq,
	.timer = &msm_dt_timer,
	.dt_compat = apq8084_dt_match,
	.reserve = apq8084_reserve,
	.init_very_early = apq8084_init_very_early,
	.restart = msm_restart,
	.smp = &msm8974_smp_ops,
MACHINE_END
