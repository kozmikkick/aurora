/*
 * Copyright (c) 2013, The Linux Foundation. All rights reserved.
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

#define pr_fmt(fmt) "%s: " fmt, __func__

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <linux/errno.h>
#include <linux/cpufreq.h>
#include <linux/clk.h>
#include <linux/platform_device.h>
#include <linux/iopoll.h>

#include <mach/board.h>
#include <mach/msm_iomap.h>
#include <mach/msm_bus.h>
#include <mach/msm_bus_board.h>
#include <mach/rpm-regulator.h>
#include <mach/clk-provider.h>
#include <mach/rpm-regulator-smd.h>

#include "acpuclock.h"
#include "acpuclock-cortex.h"

#define POLL_INTERVAL_US		1
#define APCS_RCG_UPDATE_TIMEOUT_US	20

static struct acpuclk_drv_data *priv;
static uint32_t bus_perf_client;

/* Update the bus bandwidth request. */
static void set_bus_bw(unsigned int bw)
{
	int ret;

	if (bw >= priv->bus_scale->num_usecases) {
		pr_err("invalid bandwidth request (%d)\n", bw);
		return;
	}

	/* Update bandwidth if request has changed. This may sleep. */
	ret = msm_bus_scale_client_update_request(bus_perf_client, bw);
	if (ret)
		pr_err("bandwidth request failed (%d)\n", ret);

	return;
}

/* Apply any voltage increases. */
static int increase_vdd(unsigned int vdd_cpu, unsigned int vdd_mem)
{
	int rc = 0;

	/* Increase vdd_mem before vdd_cpu. vdd_mem should be >= vdd_cpu. */
	rc = regulator_set_voltage(priv->vdd_mem, vdd_mem, priv->vdd_max_mem);
	if (rc) {
		pr_err("vdd_mem increase failed (%d)\n", rc);
		return rc;
	}

	rc = regulator_set_voltage(priv->vdd_cpu, vdd_cpu, priv->vdd_max_cpu);
	if (rc)
		pr_err("vdd_cpu increase failed (%d)\n", rc);

	return rc;
}

/* Apply any per-cpu voltage decreases. */
static void decrease_vdd(unsigned int vdd_cpu, unsigned int vdd_mem)
{
	int ret;

	/* Update CPU voltage. */
	ret = regulator_set_voltage(priv->vdd_cpu, vdd_cpu, priv->vdd_max_cpu);
	if (ret) {
		pr_err("vdd_cpu decrease failed (%d)\n", ret);
		return;
	}

	/* Decrease vdd_mem after vdd_cpu. vdd_mem should be >= vdd_cpu. */
	ret = regulator_set_voltage(priv->vdd_mem, vdd_mem, priv->vdd_max_mem);
	if (ret)
		pr_err("vdd_mem decrease failed (%d)\n", ret);
}

static void select_clk_source_div(struct acpuclk_drv_data *drv_data,
	struct clkctl_acpu_speed *s)
{
	u32 regval, rc, src_div;
	void __iomem *apcs_rcg_config = drv_data->apcs_rcg_config;
	void __iomem *apcs_rcg_cmd = drv_data->apcs_rcg_cmd;
	struct acpuclk_reg_data *r = &drv_data->reg_data;

	src_div = s->src_div ? ((2 * s->src_div) - 1) : s->src_div;

	regval = readl_relaxed(apcs_rcg_config);
	regval &= ~r->cfg_src_mask;
	regval |= s->src_sel << r->cfg_src_shift;
	regval &= ~r->cfg_div_mask;
	regval |= src_div << r->cfg_div_shift;
	writel_relaxed(regval, apcs_rcg_config);

	/* Update the configuration */
	regval = readl_relaxed(apcs_rcg_cmd);
	regval |= r->update_mask;
	writel_relaxed(regval, apcs_rcg_cmd);

	/* Wait for the update to take effect */
	rc = readl_poll_timeout_noirq(apcs_rcg_cmd, regval,
		   !(regval & r->poll_mask),
		   POLL_INTERVAL_US,
		   APCS_RCG_UPDATE_TIMEOUT_US);
	if (rc)
		pr_warn("acpu rcg didn't update its configuration\n");
}

static int set_speed_atomic(struct clkctl_acpu_speed *tgt_s)
{
	struct clkctl_acpu_speed *strt_s = priv->current_speed;
	struct clk *strt = priv->src_clocks[strt_s->src].clk;
	struct clk *tgt = priv->src_clocks[tgt_s->src].clk;
	int rc = 0;

	WARN(strt_s->src == ACPUPLL && tgt_s->src == ACPUPLL,
		"can't reprogram ACPUPLL during atomic context\n");
	rc = clk_enable(tgt);
	if (rc)
		return rc;

	select_clk_source_div(priv, tgt_s);
	clk_disable(strt);

	return rc;
}

static int set_speed(struct clkctl_acpu_speed *tgt_s)
{
	int rc = 0;
	unsigned int tgt_freq_hz = tgt_s->khz * 1000;
	struct clkctl_acpu_speed *strt_s = priv->current_speed;
	struct clkctl_acpu_speed *cxo_s = &priv->freq_tbl[0];
	struct clk *strt = priv->src_clocks[strt_s->src].clk;
	struct clk *tgt = priv->src_clocks[tgt_s->src].clk;

	if (strt_s->src == ACPUPLL && tgt_s->src == ACPUPLL) {
		/* Switch to another always on src */
		select_clk_source_div(priv, cxo_s);

		/* Re-program acpu pll */
		clk_disable_unprepare(tgt);

		rc = clk_set_rate(tgt, tgt_freq_hz);
		if (rc)
			pr_err("Failed to set ACPU PLL to %u\n", tgt_freq_hz);

		BUG_ON(clk_prepare_enable(tgt));

		/* Switch back to acpu pll */
		select_clk_source_div(priv, tgt_s);

	} else if (strt_s->src != ACPUPLL && tgt_s->src == ACPUPLL) {
		rc = clk_set_rate(tgt, tgt_freq_hz);
		if (rc) {
			pr_err("Failed to set ACPU PLL to %u\n", tgt_freq_hz);
			return rc;
		}

		rc = clk_prepare_enable(tgt);

		if (rc) {
			pr_err("ACPU PLL enable failed\n");
			return rc;
		}

		select_clk_source_div(priv, tgt_s);

		clk_disable_unprepare(strt);

	} else {
		rc = clk_prepare_enable(tgt);

		if (rc) {
			pr_err("%s enable failed\n",
				priv->src_clocks[tgt_s->src].name);
			return rc;
		}

		select_clk_source_div(priv, tgt_s);

		clk_disable_unprepare(strt);

	}

	return rc;
}

static int acpuclk_cortex_set_rate(int cpu, unsigned long rate,
				 enum setrate_reason reason)
{
	struct clkctl_acpu_speed *tgt_s, *strt_s;
	int rc = 0;

	if (reason == SETRATE_CPUFREQ)
		mutex_lock(&priv->lock);

	strt_s = priv->current_speed;

	/* Return early if rate didn't change */
	if (rate == strt_s->khz)
		goto out;

	/* Find target frequency */
	for (tgt_s = priv->freq_tbl; tgt_s->khz != 0; tgt_s++)
		if (tgt_s->khz == rate)
			break;
	if (tgt_s->khz == 0) {
		rc = -EINVAL;
		goto out;
	}

	/* Increase VDD levels if needed */
	if ((reason == SETRATE_CPUFREQ || reason == SETRATE_INIT)
			&& (tgt_s->khz > strt_s->khz)) {
		rc = increase_vdd(tgt_s->vdd_cpu, tgt_s->vdd_mem);
		if (rc)
			goto out;
	}

	pr_debug("Switching from CPU rate %u KHz -> %u KHz\n",
		strt_s->khz, tgt_s->khz);

	/* Switch CPU speed. Flag indicates atomic context */
	if (reason == SETRATE_CPUFREQ || reason == SETRATE_INIT)
		rc = set_speed(tgt_s);
	else
		rc = set_speed_atomic(tgt_s);

	if (rc)
		goto out;

	priv->current_speed = tgt_s;
	pr_debug("CPU speed change complete\n");

	/* Nothing else to do for SWFI or power-collapse. */
	if (reason == SETRATE_SWFI || reason == SETRATE_PC)
		goto out;

	/* Update bus bandwith request */
	set_bus_bw(tgt_s->bw_level);

	/* Drop VDD levels if we can. */
	if (tgt_s->khz < strt_s->khz)
		decrease_vdd(tgt_s->vdd_cpu, tgt_s->vdd_mem);

out:
	if (reason == SETRATE_CPUFREQ)
		mutex_unlock(&priv->lock);
	return rc;
}

static unsigned long acpuclk_cortex_get_rate(int cpu)
{
	return priv->current_speed->khz;
}

#ifdef CONFIG_CPU_FREQ_MSM
static struct cpufreq_frequency_table freq_table[30];

static void __init cpufreq_table_init(void)
{
	int i, freq_cnt = 0;

	/* Construct the freq_table tables from priv->freq_tbl. */
	for (i = 0; priv->freq_tbl[i].khz != 0
			&& freq_cnt < ARRAY_SIZE(freq_table); i++) {
		if (!priv->freq_tbl[i].use_for_scaling)
			continue;
		freq_table[freq_cnt].index = freq_cnt;
		freq_table[freq_cnt].frequency = priv->freq_tbl[i].khz;
		freq_cnt++;
	}
	/* freq_table not big enough to store all usable freqs. */
	BUG_ON(priv->freq_tbl[i].khz != 0);

	freq_table[freq_cnt].index = freq_cnt;
	freq_table[freq_cnt].frequency = CPUFREQ_TABLE_END;

	pr_info("CPU: %d scaling frequencies supported.\n", freq_cnt);

	/* Register table with CPUFreq. */
	for_each_possible_cpu(i)
		cpufreq_frequency_table_get_attr(freq_table, i);
}
#else
static void __init cpufreq_table_init(void) {}
#endif

static struct acpuclk_data acpuclk_cortex_data = {
	.set_rate = acpuclk_cortex_set_rate,
	.get_rate = acpuclk_cortex_get_rate,
};

int __init acpuclk_cortex_init(struct platform_device *pdev,
	struct acpuclk_drv_data *data)
{
	unsigned long max_cpu_khz = 0;
	int i, rc;

	priv = data;
	mutex_init(&priv->lock);

	acpuclk_cortex_data.power_collapse_khz = priv->wait_for_irq_khz;
	acpuclk_cortex_data.wait_for_irq_khz = priv->wait_for_irq_khz;

	bus_perf_client = msm_bus_scale_register_client(priv->bus_scale);
	if (!bus_perf_client) {
		pr_err("Unable to register bus client\n");
		BUG();
	}

	/* Improve boot time by ramping up CPU immediately */
	for (i = 0; priv->freq_tbl[i].khz != 0; i++)
		if (priv->freq_tbl[i].use_for_scaling)
			max_cpu_khz = priv->freq_tbl[i].khz;

	/* Initialize regulators */
	rc = increase_vdd(priv->vdd_max_cpu, priv->vdd_max_mem);
	if (rc)
		goto err_vdd;

	rc = regulator_enable(priv->vdd_mem);
	if (rc) {
		dev_err(&pdev->dev, "regulator_enable for mem failed\n");
		goto err_vdd;
	}

	rc = regulator_enable(priv->vdd_cpu);
	if (rc) {
		dev_err(&pdev->dev, "regulator_enable for cpu failed\n");
		goto err_vdd_cpu;
	}

	/*
	 * Select a state which is always a valid transition to align SW with
	 * the HW configuration set by the bootloaders.
	 */
	acpuclk_cortex_set_rate(0, acpuclk_cortex_data.power_collapse_khz,
		SETRATE_INIT);
	acpuclk_cortex_set_rate(0, max_cpu_khz, SETRATE_INIT);

	acpuclk_register(&acpuclk_cortex_data);
	cpufreq_table_init();

	return 0;

err_vdd_cpu:
	regulator_disable(priv->vdd_mem);
err_vdd:
	return rc;
}
