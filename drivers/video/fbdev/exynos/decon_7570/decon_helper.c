/*
 * Copyright (c) 2015 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * Helper file for Samsung EXYNOS DECON driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/clk.h>
#include <linux/err.h>
#include <linux/pm_runtime.h>

#include "decon.h"
#include "dsim.h"
#include "decon_helper.h"
#include <video/mipi_display.h>

inline int decon_is_no_bootloader_fb(struct decon_device *decon)
{
#ifdef CONFIG_DECON_USE_BOOTLOADER_FB
	return !decon->bl_fb_info.phy_addr;
#else
	return 1;
#endif
}

inline int decon_clk_set_parent(struct device *dev, struct clk *p, struct clk *c)
{
	clk_set_parent(c, p);
	return 0;
}

int decon_clk_set_rate(struct device *dev, struct clk *clk,
		const char *conid, unsigned long rate)
{
	if (IS_ERR_OR_NULL(clk)) {
		if (IS_ERR_OR_NULL(conid)) {
			decon_err("%s: couldn't set clock(%ld)\n", __func__, rate);
			return -ENODEV;
		}
		clk = clk_get(dev, conid);
		clk_set_rate(clk, rate);
		clk_put(clk);
	} else {
		clk_set_rate(clk, rate);
	}

	return 0;
}

void decon_to_psr_info(struct decon_device *decon, struct decon_psr_info *psr)
{
	psr->psr_mode = decon->pdata->psr_mode;
	psr->trig_mode = decon->pdata->trig_mode;
	psr->out_type = decon->out_type;
}

void decon_to_init_param(struct decon_device *decon, struct decon_init_param *p)
{
	struct decon_lcd *lcd_info = decon->lcd_info;
	struct v4l2_mbus_framefmt mbus_fmt;

	mbus_fmt.width = 0;
	mbus_fmt.height = 0;
	mbus_fmt.code = 0;
	mbus_fmt.field = 0;
	mbus_fmt.colorspace = 0;

	p->lcd_info = lcd_info;
	p->psr.psr_mode = decon->pdata->psr_mode;
	p->psr.trig_mode = decon->pdata->trig_mode;
	p->psr.out_type = decon->out_type;
	p->nr_windows = decon->pdata->max_win;
	p->decon_clk = &decon->pdata->decon_clk;
}

/**
* ----- APIs for DISPLAY_SUBSYSTEM_EVENT_LOG -----
*/
/* ===== STATIC APIs ===== */

#ifdef CONFIG_DECON_EVENT_LOG
/* logging a event related with DECON */
void disp_log_win_config(char *ts, struct seq_file *s,
				struct decon_win_config *config)
{
	int win;
	struct decon_win_config *cfg;
	char *state[] = { "D", "C", "B", "U" };
	char *blending[] = { "N", "P", "C" };
	char *idma[] = { "G0", "G1", "V0", "V1", "VR0", "VR1", "G2" };

	for (win = 0; win < MAX_DECON_WIN; win++) {
		cfg = &config[win];

		if (cfg->state == DECON_WIN_STATE_DISABLED)
			continue;

		if (cfg->state == DECON_WIN_STATE_COLOR) {
/*			seq_printf(s, "%s\t[%d] %s, C(%d), D(%d,%d,%d,%d) "
				"P(%d)\n",
				ts, win, state[cfg->state], cfg->color,
				cfg->dst.x, cfg->dst.y,
				cfg->dst.x + cfg->dst.w,
				cfg->dst.y + cfg->dst.h,
				cfg->protection);*/
		} else {
/*			seq_printf(s, "%s\t[%d] %s,(%d,%d,%d), F(%d) P(%d)"
				" A(%d), %s, %s, f(%d) (%d,%d,%d,%d,%d,%d) ->"
				" (%d,%d,%d,%d,%d,%d)\n",
				ts, win, state[cfg->state], cfg->fd_idma[0],
				cfg->fd_idma[1], cfg->fd_idma[2],
				cfg->fence_fd, cfg->protection,
				cfg->plane_alpha, blending[cfg->blending],
				idma[cfg->idma_type], cfg->format,
				cfg->src.x, cfg->src.y,
				cfg->src.x + cfg->src.w,
				cfg->src.y + cfg->src.h,
				cfg->src.f_w, cfg->src.f_h,
				cfg->dst.x, cfg->dst.y,
				cfg->dst.x + cfg->dst.w,
				cfg->dst.y + cfg->dst.h,
				cfg->dst.f_w, cfg->dst.f_h);*/
		}
	}
}

void disp_log_update_info(char *ts, struct seq_file *s,
				struct decon_update_reg_data *reg)
{
	int win;

	for (win = 0; win < MAX_DECON_WIN; win++) {
		if (!(reg->wincon[win] & WINCON_ENWIN))
			continue;

/*		seq_printf(s, "%s\t[%d] U(%d): (%d,%d,%d,%d) -> "
			"(%d,%d,%d,%d)\n",
			ts, win, reg->need_update,
			reg->offset_x[win], reg->offset_y[win],
			reg->whole_w[win], reg->whole_h[win],
			(reg->vidosd_a[win] >> 13) & 0x1fff,
			(reg->vidosd_a[win]) & 0x1fff,
			(reg->vidosd_b[win] >> 13) & 0x1fff,
			(reg->vidosd_b[win]) & 0x1fff);*/
	}
}

void disp_ss_event_log_win_update(char *ts, struct seq_file *s,
					struct decon_update_reg_data *reg)
{
	disp_log_win_config(ts, s, reg->win_config);
	disp_log_update_info(ts, s, reg);
}

void disp_ss_event_log_win_config(char *ts, struct seq_file *s,
					struct decon_win_config_data *win_cfg)
{
	disp_log_win_config(ts, s, win_cfg->config);
}

static inline void disp_ss_event_log_decon
	(disp_ss_event_t type, struct v4l2_subdev *sd, ktime_t time)
{
	struct decon_device *decon = container_of(sd, struct decon_device, sd);
	int idx = atomic_inc_return(&decon->disp_ss_log_idx) % DISP_EVENT_LOG_MAX;
	struct disp_ss_log *log = &decon->disp_ss_log[idx];

	if (time.tv64)
		log->time = time;
	else
		log->time = ktime_get();
	log->type = type;

	switch (type) {
	case DISP_EVT_DECON_SUSPEND:
	case DISP_EVT_DECON_RESUME:
	case DISP_EVT_ENTER_LPD:
	case DISP_EVT_EXIT_LPD:
		log->data.pm.pm_status = pm_runtime_active(decon->dev);
		log->data.pm.elapsed = ktime_sub(ktime_get(), log->time);
		break;
	case DISP_EVT_TE_INTERRUPT:
	case DISP_EVT_UNDERRUN:
	case DISP_EVT_LINECNT_ZERO:
		break;
	default:
		/* Any remaining types will be log just time and type */
		break;
	}
}

/* logging a event related with DSIM */
static inline void disp_ss_event_log_dsim
	(disp_ss_event_t type, struct v4l2_subdev *sd, ktime_t time)
{
	struct dsim_device *dsim = container_of(sd, struct dsim_device, sd);
	struct decon_device *decon = get_decon_drvdata(dsim->id);
	int idx = atomic_inc_return(&decon->disp_ss_log_idx) % DISP_EVENT_LOG_MAX;
	struct disp_ss_log *log = &decon->disp_ss_log[idx];

	if (time.tv64)
		log->time = time;
	else
		log->time = ktime_get();
	log->type = type;

	switch (type) {
	case DISP_EVT_DSIM_SUSPEND:
	case DISP_EVT_DSIM_RESUME:
	case DISP_EVT_ENTER_ULPS:
	case DISP_EVT_EXIT_ULPS:
		log->data.pm.pm_status = pm_runtime_active(dsim->dev);
		log->data.pm.elapsed = ktime_sub(ktime_get(), log->time);
		break;
	default:
		/* Any remaining types will be log just time and type */
		break;
	}
}

/* If event are happend continuously, then ignore */
static bool disp_ss_event_ignore
	(disp_ss_event_t type, struct decon_device *decon)
{
	int latest = atomic_read(&decon->disp_ss_log_idx) % DISP_EVENT_LOG_MAX;
	struct disp_ss_log *log;
	int idx;

	/* Seek a oldest from current index */
	idx = (latest + DISP_EVENT_LOG_MAX - DECON_ENTER_LPD_CNT) % DISP_EVENT_LOG_MAX;
	do {
		if (++idx >= DISP_EVENT_LOG_MAX)
			idx = 0;

		log = &decon->disp_ss_log[idx];
		if (log->type != type)
			return false;
	} while (latest != idx);

	return true;
}

/* ===== EXTERN APIs ===== */
/* Common API to log a event related with DECON/DSIM */
/* display logged events related with DECON */
#if defined(CONFIG_DEBUG_LIST)
void DISP_SS_DUMP(u32 type)
{
	struct decon_device *decon = get_decon_drvdata(0);

	if (!decon || decon->disp_dump & BIT(type))
		return;

	switch (type) {
	case DISP_DUMP_DECON_UNDERRUN:
	case DISP_DUMP_LINECNT_ZERO:
	case DISP_DUMP_VSYNC_TIMEOUT:
	case DISP_DUMP_VSTATUS_TIMEOUT:
	case DISP_DUMP_COMMAND_WR_TIMEOUT:
	case DISP_DUMP_COMMAND_RD_ERROR:
		decon_dump(decon);
		BUG();
		break;
	}
}
#endif
#endif
