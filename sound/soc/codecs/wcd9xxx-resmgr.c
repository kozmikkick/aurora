/* Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
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
#include <linux/module.h>
#include <linux/init.h>
#include <linux/firmware.h>
#include <linux/slab.h>
#include <linux/platform_device.h>
#include <linux/device.h>
#include <linux/printk.h>
#include <linux/ratelimit.h>
#include <linux/debugfs.h>
#include <linux/mfd/wcd9xxx/core.h>
#include <linux/mfd/wcd9xxx/wcd9xxx_registers.h>
#include <linux/mfd/wcd9xxx/wcd9320_registers.h>
#include <linux/mfd/wcd9xxx/pdata.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include <linux/bitops.h>
#include <linux/delay.h>
#include <linux/pm_runtime.h>
#include <linux/kernel.h>
#include <linux/gpio.h>
#include "wcd9xxx-resmgr.h"

static char wcd9xxx_event_string[][64] = {
	"WCD9XXX_EVENT_INVALID",

	"WCD9XXX_EVENT_PRE_RCO_ON",
	"WCD9XXX_EVENT_POST_RCO_ON",
	"WCD9XXX_EVENT_PRE_RCO_OFF",
	"WCD9XXX_EVENT_POST_RCO_OFF",

	"WCD9XXX_EVENT_PRE_MCLK_ON",
	"WCD9XXX_EVENT_POST_MCLK_ON",
	"WCD9XXX_EVENT_PRE_MCLK_OFF",
	"WCD9XXX_EVENT_POST_MCLK_OFF",

	"WCD9XXX_EVENT_PRE_BG_OFF",
	"WCD9XXX_EVENT_POST_BG_OFF",
	"WCD9XXX_EVENT_PRE_BG_AUDIO_ON",
	"WCD9XXX_EVENT_POST_BG_AUDIO_ON",
	"WCD9XXX_EVENT_PRE_BG_MBHC_ON",
	"WCD9XXX_EVENT_POST_BG_MBHC_ON",

	"WCD9XXX_EVENT_PRE_MICBIAS_1_OFF",
	"WCD9XXX_EVENT_POST_MICBIAS_1_OFF",
	"WCD9XXX_EVENT_PRE_MICBIAS_2_OFF",
	"WCD9XXX_EVENT_POST_MICBIAS_2_OFF",
	"WCD9XXX_EVENT_PRE_MICBIAS_3_OFF",
	"WCD9XXX_EVENT_POST_MICBIAS_3_OFF",
	"WCD9XXX_EVENT_PRE_MICBIAS_4_OFF",
	"WCD9XXX_EVENT_POST_MICBIAS_4_OFF",
	"WCD9XXX_EVENT_PRE_MICBIAS_1_ON",
	"WCD9XXX_EVENT_POST_MICBIAS_1_ON",
	"WCD9XXX_EVENT_PRE_MICBIAS_2_ON",
	"WCD9XXX_EVENT_POST_MICBIAS_2_ON",
	"WCD9XXX_EVENT_PRE_MICBIAS_3_ON",
	"WCD9XXX_EVENT_POST_MICBIAS_3_ON",
	"WCD9XXX_EVENT_PRE_MICBIAS_4_ON",
	"WCD9XXX_EVENT_POST_MICBIAS_4_ON",

	"WCD9XXX_EVENT_PRE_CFILT_1_OFF",
	"WCD9XXX_EVENT_POST_CFILT_1_OFF",
	"WCD9XXX_EVENT_PRE_CFILT_2_OFF",
	"WCD9XXX_EVENT_POST_CFILT_2_OFF",
	"WCD9XXX_EVENT_PRE_CFILT_3_OFF",
	"WCD9XXX_EVENT_POST_CFILT_3_OFF",
	"WCD9XXX_EVENT_PRE_CFILT_1_ON",
	"WCD9XXX_EVENT_POST_CFILT_1_ON",
	"WCD9XXX_EVENT_PRE_CFILT_2_ON",
	"WCD9XXX_EVENT_POST_CFILT_2_ON",
	"WCD9XXX_EVENT_PRE_CFILT_3_ON",
	"WCD9XXX_EVENT_POST_CFILT_3_ON",

	"WCD9XXX_EVENT_PRE_HPHL_PA_ON",
	"WCD9XXX_EVENT_POST_HPHL_PA_OFF",
	"WCD9XXX_EVENT_PRE_HPHR_PA_ON",
	"WCD9XXX_EVENT_POST_HPHR_PA_OFF",

	"WCD9XXX_EVENT_POST_RESUME",

	"WCD9XXX_EVENT_LAST",
};

struct wcd9xxx_resmgr_cond_entry {
	unsigned short reg;
	int shift;
	bool invert;
	enum wcd9xxx_resmgr_cond cond;
	struct list_head list;
};

static enum wcd9xxx_clock_type wcd9xxx_save_clock(struct wcd9xxx_resmgr
						  *resmgr);
static void wcd9xxx_restore_clock(struct wcd9xxx_resmgr *resmgr,
				  enum wcd9xxx_clock_type type);

const char *wcd9xxx_get_event_string(enum wcd9xxx_notify_event type)
{
	return wcd9xxx_event_string[type];
}

void wcd9xxx_resmgr_notifier_call(struct wcd9xxx_resmgr *resmgr,
				  const enum wcd9xxx_notify_event e)
{
	pr_debug("%s: notifier call event %d\n", __func__, e);
	blocking_notifier_call_chain(&resmgr->notifier, e, resmgr);
}

static void wcd9xxx_disable_bg(struct wcd9xxx_resmgr *resmgr)
{
	/* Notify bg mode change */
	wcd9xxx_resmgr_notifier_call(resmgr, WCD9XXX_EVENT_PRE_BG_OFF);
	/* Disable bg */
	snd_soc_update_bits(resmgr->codec, WCD9XXX_A_BIAS_CENTRAL_BG_CTL,
			    0x03, 0x00);
	usleep_range(100, 100);
	/* Notify bg mode change */
	wcd9xxx_resmgr_notifier_call(resmgr, WCD9XXX_EVENT_POST_BG_OFF);
}

/*
 * BG enablement should always enable in slow mode.
 * The fast mode doesn't need to be enabled as fast mode BG is to be driven
 * by MBHC override.
 */
static void wcd9xxx_enable_bg(struct wcd9xxx_resmgr *resmgr)
{
	struct snd_soc_codec *codec = resmgr->codec;

	/* Enable BG in slow mode and precharge */
	snd_soc_update_bits(codec, WCD9XXX_A_BIAS_CENTRAL_BG_CTL, 0x80, 0x80);
	snd_soc_update_bits(codec, WCD9XXX_A_BIAS_CENTRAL_BG_CTL, 0x04, 0x04);
	snd_soc_update_bits(codec, WCD9XXX_A_BIAS_CENTRAL_BG_CTL, 0x01, 0x01);
	usleep_range(1000, 1000);
	snd_soc_update_bits(codec, WCD9XXX_A_BIAS_CENTRAL_BG_CTL, 0x80, 0x00);
}

static void wcd9xxx_enable_bg_audio(struct wcd9xxx_resmgr *resmgr)
{
	/* Notify bandgap mode change */
	wcd9xxx_resmgr_notifier_call(resmgr, WCD9XXX_EVENT_PRE_BG_AUDIO_ON);
	wcd9xxx_enable_bg(resmgr);
	/* Notify bandgap mode change */
	wcd9xxx_resmgr_notifier_call(resmgr, WCD9XXX_EVENT_POST_BG_AUDIO_ON);
}

static void wcd9xxx_enable_bg_mbhc(struct wcd9xxx_resmgr *resmgr)
{
	struct snd_soc_codec *codec = resmgr->codec;

	/* Notify bandgap mode change */
	wcd9xxx_resmgr_notifier_call(resmgr, WCD9XXX_EVENT_PRE_BG_MBHC_ON);

	/*
	 * mclk should be off or clk buff source souldn't be VBG
	 * Let's turn off mclk always
	 */
	WARN_ON(snd_soc_read(codec, WCD9XXX_A_CLK_BUFF_EN2) & (1 << 2));
	wcd9xxx_enable_bg(resmgr);
	/* Notify bandgap mode change */
	wcd9xxx_resmgr_notifier_call(resmgr, WCD9XXX_EVENT_POST_BG_MBHC_ON);
}

static void wcd9xxx_disable_clock_block(struct wcd9xxx_resmgr *resmgr)
{
	struct snd_soc_codec *codec = resmgr->codec;

	pr_debug("%s: enter\n", __func__);
	WCD9XXX_BCL_ASSERT_LOCKED(resmgr);

	/* Notify */
	if (resmgr->clk_type == WCD9XXX_CLK_RCO)
		wcd9xxx_resmgr_notifier_call(resmgr, WCD9XXX_EVENT_PRE_RCO_OFF);
	else
		wcd9xxx_resmgr_notifier_call(resmgr,
					     WCD9XXX_EVENT_PRE_MCLK_OFF);
	/* Disable clock */
	snd_soc_update_bits(codec, WCD9XXX_A_CLK_BUFF_EN2, 0x04, 0x00);
	usleep_range(50, 50);
	snd_soc_update_bits(codec, WCD9XXX_A_CLK_BUFF_EN2, 0x02, 0x02);
	snd_soc_update_bits(codec, WCD9XXX_A_CLK_BUFF_EN1, 0x05, 0x00);
	usleep_range(50, 50);
	/* Notify */
	if (resmgr->clk_type == WCD9XXX_CLK_RCO)
		wcd9xxx_resmgr_notifier_call(resmgr,
					     WCD9XXX_EVENT_POST_RCO_OFF);
	else
		wcd9xxx_resmgr_notifier_call(resmgr,
					     WCD9XXX_EVENT_POST_MCLK_OFF);
	pr_debug("%s: leave\n", __func__);
}

void wcd9xxx_resmgr_post_ssr(struct wcd9xxx_resmgr *resmgr)
{
	int old_bg_audio_users, old_bg_mbhc_users;
	int old_clk_rco_users, old_clk_mclk_users;

	pr_debug("%s: enter\n", __func__);
	WCD9XXX_BCL_ASSERT_LOCKED(resmgr);

	old_bg_audio_users = resmgr->bg_audio_users;
	old_bg_mbhc_users = resmgr->bg_mbhc_users;
	old_clk_rco_users = resmgr->clk_rco_users;
	old_clk_mclk_users = resmgr->clk_mclk_users;
	resmgr->bg_audio_users = 0;
	resmgr->bg_mbhc_users = 0;
	resmgr->bandgap_type = WCD9XXX_BANDGAP_OFF;
	resmgr->clk_rco_users = 0;
	resmgr->clk_mclk_users = 0;
	resmgr->clk_type = WCD9XXX_CLK_OFF;

	if (old_bg_audio_users) {
		while (old_bg_audio_users--)
			wcd9xxx_resmgr_get_bandgap(resmgr,
						  WCD9XXX_BANDGAP_AUDIO_MODE);
	}

	if (old_bg_mbhc_users) {
		while (old_bg_mbhc_users--)
			wcd9xxx_resmgr_get_bandgap(resmgr,
						  WCD9XXX_BANDGAP_MBHC_MODE);
	}

	if (old_clk_mclk_users) {
		while (old_clk_mclk_users--)
			wcd9xxx_resmgr_get_clk_block(resmgr, WCD9XXX_CLK_MCLK);
	}

	if (old_clk_rco_users) {
		while (old_clk_rco_users--)
			wcd9xxx_resmgr_get_clk_block(resmgr, WCD9XXX_CLK_RCO);
	}
	pr_debug("%s: leave\n", __func__);
}

/*
 * wcd9xxx_resmgr_get_bandgap : Vote for bandgap ref
 * choice : WCD9XXX_BANDGAP_AUDIO_MODE, WCD9XXX_BANDGAP_MBHC_MODE
 */
void wcd9xxx_resmgr_get_bandgap(struct wcd9xxx_resmgr *resmgr,
				const enum wcd9xxx_bandgap_type choice)
{
	enum wcd9xxx_clock_type clock_save;

	pr_debug("%s: enter, wants %d\n", __func__, choice);

	WCD9XXX_BCL_ASSERT_LOCKED(resmgr);
	switch (choice) {
	case WCD9XXX_BANDGAP_AUDIO_MODE:
		resmgr->bg_audio_users++;
		if (resmgr->bg_audio_users == 1 && resmgr->bg_mbhc_users) {
			/*
			 * Current bg is MBHC mode, about to switch to
			 * audio mode.
			 */
			WARN_ON(resmgr->bandgap_type !=
				WCD9XXX_BANDGAP_MBHC_MODE);

			/* BG mode can be changed only with clock off */
			clock_save = wcd9xxx_save_clock(resmgr);
			/* Swtich BG mode */
			wcd9xxx_disable_bg(resmgr);
			wcd9xxx_enable_bg_audio(resmgr);
			/* restore clock */
			wcd9xxx_restore_clock(resmgr, clock_save);
		} else if (resmgr->bg_audio_users == 1) {
			/* currently off, just enable it */
			WARN_ON(resmgr->bandgap_type != WCD9XXX_BANDGAP_OFF);
			wcd9xxx_enable_bg_audio(resmgr);
		}
		resmgr->bandgap_type = WCD9XXX_BANDGAP_AUDIO_MODE;
		break;
	case WCD9XXX_BANDGAP_MBHC_MODE:
		resmgr->bg_mbhc_users++;
		if (resmgr->bandgap_type == WCD9XXX_BANDGAP_MBHC_MODE ||
		    resmgr->bandgap_type == WCD9XXX_BANDGAP_AUDIO_MODE)
			/* do nothing */
			break;

		/* bg mode can be changed only with clock off */
		clock_save = wcd9xxx_save_clock(resmgr);
		/* enable bg with MBHC mode */
		wcd9xxx_enable_bg_mbhc(resmgr);
		/* restore clock */
		wcd9xxx_restore_clock(resmgr, clock_save);
		/* save current mode */
		resmgr->bandgap_type = WCD9XXX_BANDGAP_MBHC_MODE;
		break;
	default:
		pr_err("%s: Error, Invalid bandgap settings\n", __func__);
		break;
	}

	pr_debug("%s: bg users audio %d, mbhc %d\n", __func__,
		 resmgr->bg_audio_users, resmgr->bg_mbhc_users);
}

/*
 * wcd9xxx_resmgr_put_bandgap : Unvote bandgap ref that has been voted
 * choice : WCD9XXX_BANDGAP_AUDIO_MODE, WCD9XXX_BANDGAP_MBHC_MODE
 */
void wcd9xxx_resmgr_put_bandgap(struct wcd9xxx_resmgr *resmgr,
				enum wcd9xxx_bandgap_type choice)
{
	enum wcd9xxx_clock_type clock_save;

	pr_debug("%s: enter choice %d\n", __func__, choice);

	WCD9XXX_BCL_ASSERT_LOCKED(resmgr);
	switch (choice) {
	case WCD9XXX_BANDGAP_AUDIO_MODE:
		if (--resmgr->bg_audio_users == 0) {
			if (resmgr->bg_mbhc_users) {
				/* bg mode can be changed only with clock off */
				clock_save = wcd9xxx_save_clock(resmgr);
				/* switch to MBHC mode */
				wcd9xxx_enable_bg_mbhc(resmgr);
				/* restore clock */
				wcd9xxx_restore_clock(resmgr, clock_save);
				resmgr->bandgap_type =
				    WCD9XXX_BANDGAP_MBHC_MODE;
			} else {
				/* turn off */
				wcd9xxx_disable_bg(resmgr);
				resmgr->bandgap_type = WCD9XXX_BANDGAP_OFF;
			}
		}
		break;
	case WCD9XXX_BANDGAP_MBHC_MODE:
		WARN(resmgr->bandgap_type == WCD9XXX_BANDGAP_OFF,
		     "Unexpected bandgap type %d\n", resmgr->bandgap_type);
		if (--resmgr->bg_mbhc_users == 0 &&
		    resmgr->bandgap_type == WCD9XXX_BANDGAP_MBHC_MODE) {
			wcd9xxx_disable_bg(resmgr);
			resmgr->bandgap_type = WCD9XXX_BANDGAP_OFF;
		}
		break;
	default:
		pr_err("%s: Error, Invalid bandgap settings\n", __func__);
		break;
	}

	pr_debug("%s: bg users audio %d, mbhc %d\n", __func__,
		 resmgr->bg_audio_users, resmgr->bg_mbhc_users);
}

void wcd9xxx_resmgr_enable_rx_bias(struct wcd9xxx_resmgr *resmgr, u32 enable)
{
	struct snd_soc_codec *codec = resmgr->codec;

	if (enable) {
		resmgr->rx_bias_count++;
		if (resmgr->rx_bias_count == 1)
			snd_soc_update_bits(codec, WCD9XXX_A_RX_COM_BIAS,
					    0x80, 0x80);
	} else {
		resmgr->rx_bias_count--;
		if (!resmgr->rx_bias_count)
			snd_soc_update_bits(codec, WCD9XXX_A_RX_COM_BIAS,
					    0x80, 0x00);
	}
}

int wcd9xxx_resmgr_enable_config_mode(struct snd_soc_codec *codec, int enable)
{
	pr_debug("%s: enable = %d\n", __func__, enable);
	if (enable) {
		snd_soc_update_bits(codec, WCD9XXX_A_RC_OSC_FREQ, 0x10, 0);
		/* bandgap mode to fast */
		snd_soc_write(codec, WCD9XXX_A_BIAS_OSC_BG_CTL, 0x17);
		usleep_range(5, 5);
		snd_soc_update_bits(codec, WCD9XXX_A_RC_OSC_FREQ, 0x80, 0x80);
		snd_soc_update_bits(codec, WCD9XXX_A_RC_OSC_TEST, 0x80, 0x80);
		usleep_range(10, 10);
		snd_soc_update_bits(codec, WCD9XXX_A_RC_OSC_TEST, 0x80, 0);
		usleep_range(10000, 10000);
		snd_soc_update_bits(codec, WCD9XXX_A_CLK_BUFF_EN1, 0x08, 0x08);
	} else {
		snd_soc_update_bits(codec, WCD9XXX_A_BIAS_OSC_BG_CTL, 0x1, 0);
		snd_soc_update_bits(codec, WCD9XXX_A_RC_OSC_FREQ, 0x80, 0);
	}

	return 0;
}

static void wcd9xxx_enable_clock_block(struct wcd9xxx_resmgr *resmgr,
				       int config_mode)
{
	struct snd_soc_codec *codec = resmgr->codec;

	pr_debug("%s: config_mode = %d\n", __func__, config_mode);
	/* transit to RCO requires mclk off */
	WARN_ON(snd_soc_read(codec, WCD9XXX_A_CLK_BUFF_EN2) & (1 << 2));
	if (config_mode) {
		/* Notify */
		wcd9xxx_resmgr_notifier_call(resmgr, WCD9XXX_EVENT_PRE_RCO_ON);
		/* enable RCO and switch to it */
		wcd9xxx_resmgr_enable_config_mode(codec, 1);
		snd_soc_write(codec, WCD9XXX_A_CLK_BUFF_EN2, 0x02);
		usleep_range(1000, 1000);
	} else {
		/* Notify */
		wcd9xxx_resmgr_notifier_call(resmgr, WCD9XXX_EVENT_PRE_MCLK_ON);
		/* switch to MCLK */
		snd_soc_update_bits(codec, WCD9XXX_A_CLK_BUFF_EN1, 0x08, 0x00);
		/* if RCO is enabled, switch from it */
		if (snd_soc_read(codec, WCD9XXX_A_RC_OSC_FREQ) & 0x80) {
			snd_soc_write(codec, WCD9XXX_A_CLK_BUFF_EN2, 0x02);
			wcd9xxx_resmgr_enable_config_mode(codec, 0);
		}
		/* clk source to ext clk and clk buff ref to VBG */
		snd_soc_update_bits(codec, WCD9XXX_A_CLK_BUFF_EN1, 0x0C, 0x04);
	}

	snd_soc_update_bits(codec, WCD9XXX_A_CLK_BUFF_EN1, 0x01, 0x01);
	/* sleep time required by codec hardware to enable clock buffer */
	usleep_range(1000, 1200);

	snd_soc_update_bits(codec, WCD9XXX_A_CLK_BUFF_EN2, 0x02, 0x00);

	/* on MCLK */
	snd_soc_update_bits(codec, WCD9XXX_A_CLK_BUFF_EN2, 0x04, 0x04);
	snd_soc_update_bits(codec, WCD9XXX_A_CDC_CLK_MCLK_CTL, 0x01, 0x01);
	usleep_range(50, 50);

	/* Notify */
	if (config_mode)
		wcd9xxx_resmgr_notifier_call(resmgr,
					     WCD9XXX_EVENT_POST_RCO_ON);
	else
		wcd9xxx_resmgr_notifier_call(resmgr,
					     WCD9XXX_EVENT_POST_MCLK_ON);
}

/*
 * disable clock and return previous clock state
 */
static enum wcd9xxx_clock_type wcd9xxx_save_clock(struct wcd9xxx_resmgr *resmgr)
{
	WCD9XXX_BCL_ASSERT_LOCKED(resmgr);
	if (resmgr->clk_type != WCD9XXX_CLK_OFF)
		wcd9xxx_disable_clock_block(resmgr);
	return resmgr->clk_type != WCD9XXX_CLK_OFF;
}

static void wcd9xxx_restore_clock(struct wcd9xxx_resmgr *resmgr,
				  enum wcd9xxx_clock_type type)
{
	if (type != WCD9XXX_CLK_OFF)
		wcd9xxx_enable_clock_block(resmgr, type == WCD9XXX_CLK_RCO);
}

void wcd9xxx_resmgr_get_clk_block(struct wcd9xxx_resmgr *resmgr,
				  enum wcd9xxx_clock_type type)
{
	pr_debug("%s: current %d, requested %d, rco_users %d, mclk_users %d\n",
		 __func__, resmgr->clk_type, type,
		 resmgr->clk_rco_users, resmgr->clk_mclk_users);
	WCD9XXX_BCL_ASSERT_LOCKED(resmgr);
	switch (type) {
	case WCD9XXX_CLK_RCO:
		if (++resmgr->clk_rco_users == 1 &&
		    resmgr->clk_type == WCD9XXX_CLK_OFF) {
			/* enable RCO and switch to it */
			wcd9xxx_enable_clock_block(resmgr, 1);
			resmgr->clk_type = WCD9XXX_CLK_RCO;
		}
		break;
	case WCD9XXX_CLK_MCLK:
		if (++resmgr->clk_mclk_users == 1 &&
		    resmgr->clk_type == WCD9XXX_CLK_OFF) {
			/* switch to MCLK */
			wcd9xxx_enable_clock_block(resmgr, 0);
			resmgr->clk_type = WCD9XXX_CLK_MCLK;
		} else if (resmgr->clk_mclk_users == 1 &&
			   resmgr->clk_type == WCD9XXX_CLK_RCO) {
			/* if RCO is enabled, switch from it */
			WARN_ON(!(snd_soc_read(resmgr->codec,
					       WCD9XXX_A_RC_OSC_FREQ) & 0x80));
			/* disable clock block */
			wcd9xxx_disable_clock_block(resmgr);
			/* switch to RCO */
			wcd9xxx_enable_clock_block(resmgr, 0);
			resmgr->clk_type = WCD9XXX_CLK_MCLK;
		}
		break;
	default:
		pr_err("%s: Error, Invalid clock get request %d\n", __func__,
		       type);
		break;
	}
	pr_debug("%s: leave\n", __func__);
}

void wcd9xxx_resmgr_put_clk_block(struct wcd9xxx_resmgr *resmgr,
				  enum wcd9xxx_clock_type type)
{
	pr_debug("%s: current %d, put %d\n", __func__, resmgr->clk_type, type);

	WCD9XXX_BCL_ASSERT_LOCKED(resmgr);
	switch (type) {
	case WCD9XXX_CLK_RCO:
		if (--resmgr->clk_rco_users == 0 &&
		    resmgr->clk_type == WCD9XXX_CLK_RCO) {
			wcd9xxx_disable_clock_block(resmgr);
			resmgr->clk_type = WCD9XXX_CLK_OFF;
		}
		break;
	case WCD9XXX_CLK_MCLK:
		if (--resmgr->clk_mclk_users == 0 &&
		    resmgr->clk_rco_users == 0) {
			wcd9xxx_disable_clock_block(resmgr);
			resmgr->clk_type = WCD9XXX_CLK_OFF;
		} else if (resmgr->clk_mclk_users == 0 &&
			   resmgr->clk_rco_users) {
			/* disable clock */
			wcd9xxx_disable_clock_block(resmgr);
			/* switch to RCO */
			wcd9xxx_enable_clock_block(resmgr, 1);
			resmgr->clk_type = WCD9XXX_CLK_RCO;
		}
		break;
	default:
		pr_err("%s: Error, Invalid clock get request %d\n", __func__,
		       type);
		break;
	}
	WARN_ON(resmgr->clk_rco_users < 0);
	WARN_ON(resmgr->clk_mclk_users < 0);

	pr_debug("%s: new rco_users %d, mclk_users %d\n", __func__,
		 resmgr->clk_rco_users, resmgr->clk_mclk_users);
}

static void wcd9xxx_resmgr_update_cfilt_usage(struct wcd9xxx_resmgr *resmgr,
					      enum wcd9xxx_cfilt_sel cfilt_sel,
					      bool inc)
{
	u16 micb_cfilt_reg;
	enum wcd9xxx_notify_event e_pre_on, e_post_off;
	struct snd_soc_codec *codec = resmgr->codec;

	switch (cfilt_sel) {
	case WCD9XXX_CFILT1_SEL:
		micb_cfilt_reg = WCD9XXX_A_MICB_CFILT_1_CTL;
		e_pre_on = WCD9XXX_EVENT_PRE_CFILT_1_ON;
		e_post_off = WCD9XXX_EVENT_POST_CFILT_1_OFF;
		break;
	case WCD9XXX_CFILT2_SEL:
		micb_cfilt_reg = WCD9XXX_A_MICB_CFILT_2_CTL;
		e_pre_on = WCD9XXX_EVENT_PRE_CFILT_2_ON;
		e_post_off = WCD9XXX_EVENT_POST_CFILT_2_OFF;
		break;
	case WCD9XXX_CFILT3_SEL:
		micb_cfilt_reg = WCD9XXX_A_MICB_CFILT_3_CTL;
		e_pre_on = WCD9XXX_EVENT_PRE_CFILT_3_ON;
		e_post_off = WCD9XXX_EVENT_POST_CFILT_3_OFF;
		break;
	default:
		WARN(1, "Invalid CFILT selection %d\n", cfilt_sel);
		return; /* should not happen */
	}

	if (inc) {
		if ((resmgr->cfilt_users[cfilt_sel]++) == 0) {
			/* Notify */
			wcd9xxx_resmgr_notifier_call(resmgr, e_pre_on);
			/* Enable CFILT */
			snd_soc_update_bits(codec, micb_cfilt_reg, 0x80, 0x80);
		}
	} else {
		/*
		 * Check if count not zero, decrease
		 * then check if zero, go ahead disable cfilter
		 */
		WARN(resmgr->cfilt_users[cfilt_sel] == 0,
		     "Invalid CFILT use count 0\n");
		if ((--resmgr->cfilt_users[cfilt_sel]) == 0) {
			/* Disable CFILT */
			snd_soc_update_bits(codec, micb_cfilt_reg, 0x80, 0);
			/* Notify MBHC so MBHC can switch CFILT to fast mode */
			wcd9xxx_resmgr_notifier_call(resmgr, e_post_off);
		}
	}
}

void wcd9xxx_resmgr_cfilt_get(struct wcd9xxx_resmgr *resmgr,
			      enum wcd9xxx_cfilt_sel cfilt_sel)
{
	return wcd9xxx_resmgr_update_cfilt_usage(resmgr, cfilt_sel, true);
}

void wcd9xxx_resmgr_cfilt_put(struct wcd9xxx_resmgr *resmgr,
			      enum wcd9xxx_cfilt_sel cfilt_sel)
{
	return wcd9xxx_resmgr_update_cfilt_usage(resmgr, cfilt_sel, false);
}

int wcd9xxx_resmgr_get_k_val(struct wcd9xxx_resmgr *resmgr,
			     unsigned int cfilt_mv)
{
	int rc = -EINVAL;
	unsigned int ldoh_v = resmgr->pdata->micbias.ldoh_v;
	unsigned min_mv, max_mv;

	switch (ldoh_v) {
	case WCD9XXX_LDOH_1P95_V:
		min_mv = 160;
		max_mv = 1800;
		break;
	case WCD9XXX_LDOH_2P35_V:
		min_mv = 200;
		max_mv = 2200;
		break;
	case WCD9XXX_LDOH_2P75_V:
		min_mv = 240;
		max_mv = 2600;
		break;
	case WCD9XXX_LDOH_3P0_V:
		min_mv = 260;
		max_mv = 2875;
		break;
	default:
		goto done;
	}

	if (cfilt_mv < min_mv || cfilt_mv > max_mv)
		goto done;

	for (rc = 4; rc <= 44; rc++) {
		min_mv = max_mv * (rc) / 44;
		if (min_mv >= cfilt_mv) {
			rc -= 4;
			break;
		}
	}
done:
	return rc;
}

void wcd9xxx_resmgr_cond_trigger_cond(struct wcd9xxx_resmgr *resmgr,
				      enum wcd9xxx_resmgr_cond cond)
{
	struct list_head *l;
	struct wcd9xxx_resmgr_cond_entry *e;
	bool set;

	pr_debug("%s: enter\n", __func__);
	WCD9XXX_BCL_ASSERT_LOCKED(resmgr);
	set = !!test_bit(cond, &resmgr->cond_flags);
	list_for_each(l, &resmgr->update_bit_cond_h) {
		e = list_entry(l, struct wcd9xxx_resmgr_cond_entry, list);
		if (e->cond == cond)
			snd_soc_update_bits(resmgr->codec, e->reg,
					    1 << e->shift,
					    (set ? !e->invert : e->invert)
					    << e->shift);
	}
	pr_debug("%s: leave\n", __func__);
}

void wcd9xxx_resmgr_cond_update_cond(struct wcd9xxx_resmgr *resmgr,
				     enum wcd9xxx_resmgr_cond cond, bool set)
{
	WCD9XXX_BCL_ASSERT_LOCKED(resmgr);
	if ((set && !test_and_set_bit(cond, &resmgr->cond_flags)) ||
	    (!set && test_and_clear_bit(cond, &resmgr->cond_flags))) {
		pr_debug("%s: Resource %d condition changed to %s\n", __func__,
			 cond, set ? "set" : "clear");
		wcd9xxx_resmgr_cond_trigger_cond(resmgr, cond);
	}
}

int wcd9xxx_resmgr_add_cond_update_bits(struct wcd9xxx_resmgr *resmgr,
					enum wcd9xxx_resmgr_cond cond,
					unsigned short reg, int shift,
					bool invert)
{
	struct wcd9xxx_resmgr_cond_entry *entry;

	entry = kmalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->cond = cond;
	entry->reg = reg;
	entry->shift = shift;
	entry->invert = invert;

	WCD9XXX_BCL_LOCK(resmgr);
	list_add_tail(&entry->list, &resmgr->update_bit_cond_h);

	wcd9xxx_resmgr_cond_trigger_cond(resmgr, cond);
	WCD9XXX_BCL_UNLOCK(resmgr);

	return 0;
}

/*
 * wcd9xxx_resmgr_rm_cond_update_bits :
 * Clear bit and remove from the conditional bit update list
 */
int wcd9xxx_resmgr_rm_cond_update_bits(struct wcd9xxx_resmgr *resmgr,
				       enum wcd9xxx_resmgr_cond cond,
				       unsigned short reg, int shift,
				       bool invert)
{
	struct list_head *l, *next;
	struct wcd9xxx_resmgr_cond_entry *e = NULL;

	pr_debug("%s: enter\n", __func__);
	WCD9XXX_BCL_LOCK(resmgr);
	list_for_each_safe(l, next, &resmgr->update_bit_cond_h) {
		e = list_entry(l, struct wcd9xxx_resmgr_cond_entry, list);
		if (e->reg == reg && e->shift == shift && e->invert == invert) {
			snd_soc_update_bits(resmgr->codec, e->reg,
					    1 << e->shift,
					    e->invert << e->shift);
			list_del(&e->list);
			WCD9XXX_BCL_UNLOCK(resmgr);
			kfree(e);
			return 0;
		}
	}
	WCD9XXX_BCL_UNLOCK(resmgr);
	pr_err("%s: Cannot find update bit entry reg 0x%x, shift %d\n",
	       __func__, e ? e->reg : 0, e ? e->shift : 0);

	return -EINVAL;
}

int wcd9xxx_resmgr_register_notifier(struct wcd9xxx_resmgr *resmgr,
				     struct notifier_block *nblock)
{
	return blocking_notifier_chain_register(&resmgr->notifier, nblock);
}

int wcd9xxx_resmgr_unregister_notifier(struct wcd9xxx_resmgr *resmgr,
				       struct notifier_block *nblock)
{
	return blocking_notifier_chain_unregister(&resmgr->notifier, nblock);
}

int wcd9xxx_resmgr_init(struct wcd9xxx_resmgr *resmgr,
			struct snd_soc_codec *codec,
			struct wcd9xxx *wcd9xxx,
			struct wcd9xxx_pdata *pdata,
			struct wcd9xxx_reg_address *reg_addr)
{
	WARN(ARRAY_SIZE(wcd9xxx_event_string) != WCD9XXX_EVENT_LAST + 1,
	     "Event string table isn't up to date!, %d != %d\n",
	     ARRAY_SIZE(wcd9xxx_event_string), WCD9XXX_EVENT_LAST + 1);

	resmgr->bandgap_type = WCD9XXX_BANDGAP_OFF;
	resmgr->codec = codec;
	/* This gives access of core handle to lock/unlock suspend */
	resmgr->core = wcd9xxx;
	resmgr->pdata = pdata;
	resmgr->reg_addr = reg_addr;

	INIT_LIST_HEAD(&resmgr->update_bit_cond_h);

	BLOCKING_INIT_NOTIFIER_HEAD(&resmgr->notifier);

	mutex_init(&resmgr->codec_resource_lock);

	return 0;
}

void wcd9xxx_resmgr_deinit(struct wcd9xxx_resmgr *resmgr)
{
	mutex_destroy(&resmgr->codec_resource_lock);
}

void wcd9xxx_resmgr_bcl_lock(struct wcd9xxx_resmgr *resmgr)
{
	mutex_lock(&resmgr->codec_resource_lock);
}

void wcd9xxx_resmgr_bcl_unlock(struct wcd9xxx_resmgr *resmgr)
{
	mutex_unlock(&resmgr->codec_resource_lock);
}

MODULE_DESCRIPTION("wcd9xxx resmgr module");
MODULE_LICENSE("GPL v2");
