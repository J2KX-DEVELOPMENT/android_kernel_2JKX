/* dd_lcd.c
 *
 * Copyright (c) 2018 Samsung Electronics
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

/* temporary solution: Do not use these sysfs as official purpose */
/* these function are not official one. only purpose is for temporary test */

#if defined(CONFIG_DEBUG_FS) && !defined(CONFIG_SAMSUNG_PRODUCT_SHIP) && defined(CONFIG_SEC_GPIO_DVS)
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/lcd.h>
#include <linux/platform_device.h>
#include <linux/module.h>

#include <video/mipi_display.h>

#include "../dsim.h"
#include "../decon_notify.h"

#include "dd.h"

#define dbg_info(fmt, ...)	pr_no_info(pr_fmt("%s: %3d: %s: " fmt), "lcd panel", __LINE__, __func__, ##__VA_ARGS__)
#define dbg_warn(fmt, ...)	pr_warn(pr_fmt("%s: %3d: %s: " fmt), "lcd panel", __LINE__, __func__, ##__VA_ARGS__)

#define DEVICE_NAME		"panel"

struct rw_info {
	u8 type;
	u8 cmd;
	u8 len;
	u8 pos;
	u8 *buf;
};

struct cmdlist_info {
	struct rw_info rw;
	struct list_head node;
};

struct d_info {
	struct device	*dev;

	struct rw_info	rx;
	struct rw_info	tx;
	struct list_head	unlock_list;
	unsigned int *tx_dump;

	struct notifier_block	fb_notifier;
	unsigned int enable;

	unsigned char		dump_info[2];
	unsigned int		data_type;

	struct kobject		*dsi_access;
	struct kobj_attribute	dsi_access_r;
	struct kobj_attribute	dsi_access_w;
};

static unsigned int tx_dump;

static int get_dsi_data_type_tx(u8 len)
{
	if (len == 1)
		return MIPI_DSI_DCS_SHORT_WRITE;
	else if (len == 2)
		return MIPI_DSI_DCS_SHORT_WRITE_PARAM;
	else
		return MIPI_DSI_DCS_LONG_WRITE;
}

static int dsi_data_type_is_tx_short(u8 type)
{
	switch (type) {
	case MIPI_DSI_GENERIC_SHORT_WRITE_0_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_1_PARAM:
	case MIPI_DSI_GENERIC_SHORT_WRITE_2_PARAM:
	case MIPI_DSI_DCS_SHORT_WRITE:
	case MIPI_DSI_DCS_SHORT_WRITE_PARAM:
		return 1;
	}

	return 0;
}

static int dsi_data_type_is_tx_long(u8 type)
{
	switch (type) {
	case MIPI_DSI_GENERIC_LONG_WRITE:
	case MIPI_DSI_DCS_LONG_WRITE:
		return 1;
	}

	return 0;
}

static int dsi_data_type_is_rx(u8 type)
{
	switch (type) {
	case MIPI_DSI_GENERIC_READ_REQUEST_0_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_1_PARAM:
	case MIPI_DSI_GENERIC_READ_REQUEST_2_PARAM:
	case MIPI_DSI_DCS_READ:
		return 1;
	}

	return 0;
}

void dsim_write_data_dump(struct dsim_device *dsim, u32 id, unsigned long d0, u32 d1)
{
	if (likely(!tx_dump))
		return;

	if (dsi_data_type_is_tx_long(id))
		dbg_info("%02x: %*ph\n", id, d1, (u8 *)d0);
	else
		dbg_info("%02x: %02lx %2x\n", id, d0, d1);
}

static int tx(struct rw_info *rw, u8 *cmds)
{
	int ret = 0;
	struct dsim_device *dsim = get_dsim_drvdata(0);

	if (dsi_data_type_is_tx_long(rw->type))
		ret = dsim_write_data(dsim, rw->type, (unsigned long)cmds, rw->len);
	else
		ret = dsim_write_data(dsim, rw->type, cmds[0], (rw->len == 2) ? cmds[1] : 0);

	if (ret < 0)
		dbg_info("fail. ret: %d, type: %02x, cmd: %02x, len: %d, pos: %d\n", ret, rw->type, rw->cmd, rw->len, rw->pos);

	return ret;
}

static int rx(struct rw_info *rw, u8 *buf)
{
	int ret = 0, type;
	struct dsim_device *dsim = get_dsim_drvdata(0);

	if (rw->pos) {
		u8 posbuf[2] = {0xB0, };

		struct rw_info pos = {
			.type = MIPI_DSI_DCS_SHORT_WRITE_PARAM,
			.cmd = 0xB0,
			.len = 2,
			.buf = posbuf,
		};

		posbuf[1] = rw->pos;
		ret = tx(&pos, pos.buf);
		if (ret < 0)
			return ret;
	}

	type = (dsi_data_type_is_tx_short(rw->type) || dsi_data_type_is_tx_long(rw->type)) ? MIPI_DSI_DCS_READ : rw->type;

	ret = dsim_read_data(dsim, type, rw->cmd, rw->len, buf);
	dbg_info("%02x, %d, %d\n", rw->cmd, rw->len, ret);
	if (ret != rw->len) {
		dbg_info("fail. ret: %d, type: %02x, cmd: %02x, len: %d, pos: %d\n", ret, rw->type, rw->cmd, rw->len, rw->pos);
		ret = -EINVAL;
	}

	return ret;
}

static void tx_unlock(struct d_info *d)
{
	int ret = 0;
	struct cmdlist_info *cmdlist = NULL;
	struct rw_info *unlock = NULL;

	list_for_each_entry(cmdlist, &d->unlock_list, node) {
		unlock = &cmdlist->rw;
		ret = tx(unlock, unlock->buf);
		if (ret < 0)
			return;
	}
}

static void clean_unlock(struct d_info *d)
{
	struct cmdlist_info *cmdlist = NULL;
	struct cmdlist_info *cmdlist_tmp = NULL;
	struct rw_info *unlock = NULL;

	list_for_each_entry_safe(cmdlist, cmdlist_tmp, &d->unlock_list, node) {
		unlock = &cmdlist->rw;
		list_del_init(&cmdlist->node);
		dbg_info("%2d, %*ph\n", unlock->len, unlock->len, unlock->buf);
		kfree(unlock->buf);
		kfree(unlock);
	}
}

static int unlock_show(struct seq_file *m, void *unused)
{
	struct d_info *d = m->private;
	struct cmdlist_info *cmdlist = NULL;
	struct rw_info *rw = NULL;
	int ret = 0;

	u8 type, cmd, len, pos;
	u8 *buf;

	list_for_each_entry(cmdlist, &d->unlock_list, node) {
		rw = &cmdlist->rw;

		type = rw->type;
		cmd = rw->cmd;
		len = rw->len;
		pos = rw->pos;
		buf = rw->buf;

		if (!cmd || !len || cmd > U8_MAX || len > U8_MAX || pos > U8_MAX || !buf) {
			ret = -EINVAL;
			goto exit;
		}

		seq_printf(m, "type: %02x, cmd: %02x, len: %02x, pos: %02x, buf: %*ph\n", type, cmd, len, pos, len, buf);

		dbg_info("type: %02x, cmd: %02x, len: %02x, pos: %02x, buf: %*ph\n", type, cmd, len, pos, len, buf);
	}

exit:
	return 0;
}

static int unlock_open(struct inode *inode, struct file *f)
{
	return single_open(f, unlock_show, inode->i_private);
}

static int make_tx(struct d_info *d, struct rw_info *rw, unsigned char *ibuf)
{
	unsigned char obuf[MAX_INPUT] = {0, };
	unsigned char data;
	char *pbuf, *token;

	int ret = 0, end = 0;
	unsigned int is_datatype = 0, type = 0, cmd = 0, len = 0, pos = 0;

	pbuf = strim(ibuf);
	if (strchr(pbuf, ':')) {
		if (strchr(pbuf, ':') >= strchr(pbuf, ' ')) {
			dbg_info("if you want custom data type, colon(:) should come first\n");
			goto exit;
		}
		dbg_info("input has custom data type\n");
		is_datatype = 1;
	}

	if (is_datatype) {
		token = strsep(&pbuf, " ,:");
		ret = token ? kstrtou8(token, 16, &data) : -EINVAL;
		if (ret < 0) {
			if (dsi_data_type_is_tx_short(data) || dsi_data_type_is_tx_long(data)) {
				type = data;
				dbg_info("datatype is %02x\n", type);
			} else {
				dbg_info("datatype is invalid\n");
				goto exit;
			}
		}
	}

	while ((token = strsep(&pbuf, " ,"))) {
		ret = kstrtou8(token, 16, &data);
		if (ret < 0 || end == ARRAY_SIZE(obuf))
			break;

		obuf[end] = data;
		end++;
	}

	if (ret < 0 || !end || end == ARRAY_SIZE(obuf)) {
		dbg_info("invalid input: ret: %2d, end: %2d, buf: %s\n", ret, end, ibuf);
		goto exit;
	} else
		dbg_info("%2d, %*ph\n", end, end, obuf);

	type = type ? type : get_dsi_data_type_tx(end);
	len = end;
	cmd = obuf[0];

	dbg_info("type: %02x cmd: %02x len: %u pos: %u\n", type, cmd, len, pos);

	if (!cmd || !len || cmd > U8_MAX || len > U8_MAX || pos > U8_MAX) {
		ret = -EINVAL;
		goto exit;
	}

	rw->type = type;
	rw->cmd = cmd;
	rw->len = len;
	rw->pos = pos;
	rw->buf = kmemdup(obuf, rw->len * sizeof(u8), GFP_KERNEL);

exit:
	return ret;
}

struct cmdlist_info *add_unlock(struct d_info *d, unsigned char *ibuf)
{
	struct cmdlist_info *cmdlist = NULL;
	struct rw_info *rw = NULL;
	int ret = 0;

	cmdlist = kzalloc(sizeof(struct cmdlist_info), GFP_KERNEL);
	rw = &cmdlist->rw;

	ret = make_tx(d, rw, ibuf);
	if (ret < 0 || !rw || !rw->buf) {
		/* if (rw->buf) */
		kfree(rw->buf);
		/* if (cmdlist) */
		kfree(cmdlist);
		cmdlist = NULL;
		goto exit;
	}

	list_add(&cmdlist->node, &d->unlock_list);

exit:
	return cmdlist;
}

static ssize_t unlock_store(struct file *f, const char __user *user_buf,
					size_t count, loff_t *ppos)
{
	struct d_info *d = ((struct seq_file *)f->private_data)->private;
	struct cmdlist_info *cmdlist = NULL;
	struct rw_info *rw = NULL;
	unsigned char ibuf[MAX_INPUT] = {0, };
	int ret = 0;

	if (!d->enable) {
		dbg_info("enable is %s\n", d->enable ? "on" : "off");
		goto exit;
	}

	if (count > sizeof(ibuf))
		goto exit;

	if (!strncmp(user_buf, "0", count)) {
		dbg_info("input is 0(zero). reset unlock parameter to default(nothing)\n");
		clean_unlock(d);
		goto exit;
	}

	ret = simple_write_to_buffer(ibuf, sizeof(ibuf) - 1, ppos, user_buf, count);
	if (ret < 0)
		goto exit;

	ibuf[ret] = '\0';

	cmdlist = add_unlock(d, ibuf);
	if (!cmdlist)
		goto exit;
	rw = &cmdlist->rw;
	ret = tx(rw, rw->buf);
	if (ret < 0)
		goto exit;

exit:
	return count;
}

static const struct file_operations unlock_fops = {
	.open		= unlock_open,
	.write		= unlock_store,
	.read		= seq_read,
	.llseek		= no_llseek,
	.release	= single_release,
};

static int tx_show(struct seq_file *m, void *unused)
{
	struct d_info *d = m->private;
	struct rw_info *rw = &d->tx;

	u8 type, cmd, len, pos;
	int ret = 0, i;

	unsigned char rbuf[U8_MAX] = {0, };

	type = rw->type;
	cmd = rw->cmd;
	len = rw->len;
	pos = rw->pos;

	if (!d->enable) {
		dbg_info("enable is %s\n", d->enable ? "on" : "off");
		goto exit;
	}

	if (!cmd || !len || cmd > U8_MAX || len > U8_MAX || pos > U8_MAX) {
		ret = -EINVAL;
		goto exit;
	}

	ret = rx(rw, rbuf);
	if (ret < 0)
		goto exit;

	seq_printf(m, "type: %02x, cmd: %02x, len: %02x, pos: %02x\n", type, cmd, len, pos);
	seq_printf(m, "+ [%02x]\n", cmd);
	for (i = 0; i < len; i++)
		seq_printf(m, "%2d: %02x\n", i + pos + 1, rbuf[i]);
	seq_printf(m, "- [%02x]\n", cmd);

	dbg_info("type: %02x, cmd: %02x, len: %02x, pos: %02x\n", type, cmd, len, pos);
	dbg_info("+ [%02x]\n", cmd);
	for (i = 0; i < len; i++)
		dbg_info("%2d: %02x\n", i + pos + 1, rbuf[i]);
	dbg_info("- [%02x]\n", cmd);

exit:
	return 0;
}

static int tx_open(struct inode *inode, struct file *f)
{
	return single_open(f, tx_show, inode->i_private);
}

static ssize_t tx_store(struct file *f, const char __user *user_buf,
					size_t count, loff_t *ppos)
{
	struct d_info *d = ((struct seq_file *)f->private_data)->private;
	struct rw_info *rw = &d->tx;
	unsigned char ibuf[MAX_INPUT] = {0, };
	int ret = 0;

	if (!d->enable) {
		dbg_info("enable is %s\n", d->enable ? "on" : "off");
		goto exit;
	}

	if (count > sizeof(ibuf))
		goto exit;

	ret = simple_write_to_buffer(ibuf, sizeof(ibuf) - 1, ppos, user_buf, count);
	if (ret < 0)
		goto exit;

	ibuf[ret] = '\0';

	tx_unlock(d);

	ret = make_tx(d, rw, ibuf);
	if (ret < 0 || !rw || !rw->buf)
		goto exit;

	ret = tx(rw, rw->buf);
	kfree(rw->buf);
	rw->buf = NULL;
	if (ret < 0)
		goto exit;

exit:
	return count;
}

static const struct file_operations tx_fops = {
	.open		= tx_open,
	.write		= tx_store,
	.read		= seq_read,
	.llseek		= no_llseek,
	.release	= single_release,
};

static int rx_show(struct seq_file *m, void *unused)
{
	struct d_info *d = m->private;
	struct rw_info *rw = &d->rx;

	u8 type, cmd, len, pos;
	int ret = 0, i;

	unsigned char rbuf[U8_MAX] = {0, };

	if (!d->enable) {
		dbg_info("enable is %s\n", d->enable ? "on" : "off");
		goto exit;
	}

	type = rw->type;
	cmd = rw->cmd;
	len = rw->len;
	pos = rw->pos;

	if (!cmd || !len || cmd > U8_MAX || len > U8_MAX || pos > U8_MAX) {
		ret = -EINVAL;
		goto exit;
	}

	tx_unlock(d);
	ret = rx(rw, rbuf);
	if (ret < 0)
		goto exit;

	seq_printf(m, "+ [%02x]\n", cmd);
	for (i = 0; i < len; i++)
		seq_printf(m, "%2d: %02x\n", i + pos + 1, rbuf[i]);
	seq_printf(m, "- [%02x]\n", cmd);

	dbg_info("+ [%02x]\n", cmd);
	for (i = 0; i < len; i++)
		dbg_info("%2d: %02x\n", i + pos + 1, rbuf[i]);
	dbg_info("- [%02x]\n", cmd);

exit:
	return 0;
}

static int rx_open(struct inode *inode, struct file *f)
{
	return single_open(f, rx_show, inode->i_private);
}

static ssize_t rx_store(struct file *f, const char __user *user_buf,
					size_t count, loff_t *ppos)
{
	struct d_info *d = ((struct seq_file *)f->private_data)->private;
	struct rw_info *rw = &d->rx;

	unsigned char ibuf[MAX_INPUT] = {0, };
	unsigned char rbuf[U8_MAX] = {0, };
	char *pbuf;

	int ret = 0, i;
	unsigned int is_datatype = 0, type = 0, cmd = 0, len = 0, pos = 0;

	if (!d->enable) {
		dbg_info("enable is %s\n", d->enable ? "on" : "off");
		goto exit;
	}

	if (count > sizeof(ibuf))
		goto exit;

	ret = simple_write_to_buffer(ibuf, sizeof(ibuf) - 1, ppos, user_buf, count);
	if (ret < 0)
		goto exit;

	ibuf[ret] = '\0';

	pbuf = strim(ibuf);
	if (strchr(pbuf, ':')) {
		if (strchr(pbuf, ':') >= strchr(pbuf, ' ')) {
			dbg_info("if you want custom data type, colon(:) should come first\n");
			goto exit;
		}
		dbg_info("input has custom data type\n");
		is_datatype = 1;
	}

	if (is_datatype)
		ret = sscanf(pbuf, "%8x: %8x %8d %8d", &type, &cmd, &len, &pos);
	else {
		ret = sscanf(pbuf, "%8x %8d %8d", &cmd, &len, &pos);
		type = MIPI_DSI_DCS_READ;
	}

	if (ret < 0 || !cmd)
		goto exit;

	type = dsi_data_type_is_rx(type) ? type : MIPI_DSI_DCS_READ;

	dbg_info("ret: %d, type: %02x, cmd: %02x, len: %u, pos: %u\n", ret, type, cmd, len, pos);

	if (!cmd || !len || cmd > U8_MAX || len > U8_MAX || pos > U8_MAX) {
		ret = -EINVAL;
		goto exit;
	}

	rw->type = type;
	rw->cmd = cmd;
	rw->len = len;
	rw->pos = pos;

	tx_unlock(d);
	ret = rx(rw, rbuf);
	if (ret < 0)
		goto exit;

	dbg_info("+ [%02x]\n", cmd);
	for (i = 0; i < len; i++)
		dbg_info("%2d: %02x\n", i + pos + 1, rbuf[i]);
	dbg_info("- [%02x]\n", cmd);

exit:
	return count;
}

static const struct file_operations rx_fops = {
	.open		= rx_open,
	.write		= rx_store,
	.read		= seq_read,
	.llseek		= no_llseek,
	.release	= single_release,
};

static int help_show(struct seq_file *m, void *unused)
{
	struct d_info *d = m->private;
	struct cmdlist_info *cmdlist = NULL;
	struct rw_info *rw = NULL;
	int ret = 0;

	u8 type, cmd, len, pos;
	u8 *buf;

	seq_puts(m, "\n");
	seq_puts(m, "------------------------------------------------------------\n");
	seq_puts(m, "* ATTENTION\n");
	seq_puts(m, "* These sysfs can NOT be mandatory official purpose\n");
	seq_puts(m, "* These sysfs has risky, harmful, unstable function\n");
	seq_puts(m, "* So we can not support these sysfs for official use\n");
	seq_puts(m, "* DO NOT request improvement related with these function\n");
	seq_puts(m, "* DO NOT request these function as the mandatory requirements\n");
	seq_puts(m, "* If you insist, we eliminate these function immediately\n");
	seq_puts(m, "------------------------------------------------------------\n");
	seq_puts(m, "\n");
	seq_puts(m, "---------- rx usage\n");
	seq_puts(m, "# echo cmd len > rx\n");
	seq_puts(m, "# echo (datatype:) cmd len (pos) > rx\n");
	seq_puts(m, "# cat rx\n");
	seq_puts(m, "1. datatype cmd are hexadecimal. len pos are decimal\n");
	seq_puts(m, "2. colon(:) is delimiter for custom datatype. optional\n");
	seq_puts(m, "3. pos is offset position with sending b0 command. optional\n");
	seq_puts(m, "ex) # echo 0xf0 2 > rx\n");
	seq_puts(m, "ex) # echo f0 2 > rx\n");
	seq_puts(m, "= set read 0xf0's 2 byte(1th, 2nd para, pos 0) with DCS_READ(0x06) datatype\n");
	seq_puts(m, "ex) # cat rx\n");
	seq_puts(m, "= get the result of previous 0xf0's 2 byte read data via sysfs\n");
	seq_puts(m, "ex) # echo 4: f0 2 1 > rx\n");
	seq_puts(m, "ex) # echo 0x4: 0xf0 2 1 > rx\n");
	seq_puts(m, "= read 0xf0's 2 byte(2nd, 3rd para, pos 1) with GENERIC_READ_REQUEST_0_PARAM(0x04) datatype\n");
	seq_puts(m, "\n");
	seq_puts(m, "---------- tx usage\n");
	seq_puts(m, "# echo cmd1 cmd2 cmd3 cmd 4 ... cmd N > tx\n");
	seq_puts(m, "# echo (datatype:) cmd1 cmd2 cmd3 cmd 4 ... cmd N > tx\n");
	seq_puts(m, "# cat tx\n");
	seq_puts(m, "1. colon(:) is delimiter for custom datatype. optional\n");
	seq_puts(m, "2. maximum command sequence(N) is 255. but do not try\n");
	seq_puts(m, "ex) # echo 29 > tx\n");
	seq_puts(m, "ex) # echo 0x29 > tx\n");
	seq_puts(m, "= send 0x29 with DCS_SHORT_WRITE(0x05) datatype\n");
	seq_puts(m, "ex) # cat tx\n");
	seq_puts(m, "= read 0x29 with DCS_READ(0x06) datatype\n");
	seq_puts(m, "ex) # echo 29 00 > tx\n");
	seq_puts(m, "ex) # echo 0x29 0x00 > tx\n");
	seq_puts(m, "= send 0x29 0x00 with DCS_SHORT_WRITE_PARAM(0x15) datatype\n");
	seq_puts(m, "ex) # echo 29 00 00 > tx\n");
	seq_puts(m, "ex) # echo 0x29 0x00 0x00 > tx\n");
	seq_puts(m, "= send 0x29 0x00 0x00 with DCS_LONG_WRITE(0x39) datatype\n");
	seq_puts(m, "ex) # echo 29: 29 00 00 > tx\n");
	seq_puts(m, "ex) # echo 0x29: 0x29 0x00 0x00 > tx\n");
	seq_puts(m, "= send 0x29 0x00 0x00 with GENERIC_LONG_WRITE(0x29) datatype\n");
	seq_puts(m, "\n");
	seq_puts(m, "---------- unlock usage\n");
	seq_puts(m, "# echo cmd1 cmd2 cmd3 cmd 4 ... cmd N > unlock\n");
	seq_puts(m, "# echo (datatype:) cmd1 cmd2 cmd3 cmd 4 ... cmd N > unlock\n");
	seq_puts(m, "# cat unlock\n");
	seq_puts(m, "# echo 0 > unlock\n");
	seq_puts(m, "1. you can program commands to send every before rx/tx\n");
	seq_puts(m, "2. colon(:) is delimiter for custom datatype. optional\n");
	seq_puts(m, "3. maximum command sequence(N) is 255. but do not try\n");
	seq_puts(m, "4. 'echo 0' is for reset unlock parameter to default(nothing)\n");
	seq_puts(m, "5. 'cat unlcok' is for check current unlock status\n");
	seq_puts(m, "6. current unlock commands are below\n");
	list_for_each_entry(cmdlist, &d->unlock_list, node) {
		rw = &cmdlist->rw;

		type = rw->type;
		cmd = rw->cmd;
		len = rw->len;
		pos = rw->pos;
		buf = rw->buf;

		if (!cmd || !len || cmd > U8_MAX || len > U8_MAX || pos > U8_MAX || !buf) {
			ret = -EINVAL;
			goto exit;
		}

		seq_printf(m, "type: %02x, cmd: %02x, len: %02x, pos: %02x, buf: %*ph\n", type, cmd, len, pos, len, buf);
	}
	seq_puts(m, "\n");
	seq_puts(m, "---------- tx_dump usage\n");
	seq_puts(m, "# echo 1 > tx_dump\n");
	seq_puts(m, "= turn on dsi tx dump\n");
	seq_puts(m, "# echo 0 > tx_dump\n");
	seq_puts(m, "= turn off dsi tx dump\n");
	seq_puts(m, "# cat tx_dump\n");
	seq_puts(m, "= check current tx_dump status\n");
	seq_puts(m, "\n");
	seq_puts(m, "---------- usage summary\n");
	seq_puts(m, "# echo 29 > tx\n");
	seq_puts(m, "# cat tx\n");
	seq_puts(m, "# echo a1 4 > rx\n");
	seq_puts(m, "# cat rx\n");
	seq_puts(m, "# echo f0 5a 5a > unlock\n");
	seq_puts(m, "# echo 0 > unlock\n");
	seq_puts(m, "# echo 1 > tx_dump\n");
	seq_puts(m, "\n");

exit:
	return 0;
}

static int help_open(struct inode *inode, struct file *f)
{
	return single_open(f, help_show, inode->i_private);
}

static const struct file_operations help_fops = {
	.open		= help_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= seq_release,
};

static int compare_with_name(struct device *dev, void *data)
{
	const char *keyword = data;

	return dev_name(dev) ? !!strstr(dev_name(dev), keyword) : 0;
}

struct device *find_platform_device_by_keyword(const char *keyword)
{
	return bus_find_device(&platform_bus_type, NULL, (void *)keyword, compare_with_name);
}

struct device *find_lcd_device(void)
{
	struct device *parent = NULL;
	struct device *dev = NULL;

	parent = find_platform_device_by_keyword("dsim");

	if (!parent) {
		dbg_warn("bus_find_device fail\n");
		return parent;
	}

	dev = device_find_child(parent, DEVICE_NAME, compare_with_name);

	if (parent)
		put_device(parent);

	if (dev)
		put_device(dev);

	return dev ? dev : parent;
}

static int fb_notifier_callback(struct notifier_block *self,
			unsigned long event, void *data)
{
	struct fb_event *evdata = data;
	struct d_info *d = NULL;
	int fb_blank;

	switch (event) {
	case FB_EVENT_BLANK:
	case FB_EARLY_EVENT_BLANK:
		break;
	default:
		return NOTIFY_DONE;
	}

	d = container_of(self, struct d_info, fb_notifier);

	fb_blank = *(int *)evdata->data;

	if (evdata->info->node)
		return NOTIFY_DONE;

	if (event == FB_EVENT_BLANK && fb_blank == FB_BLANK_UNBLANK)
		d->enable = 1;
	else if (fb_blank == FB_BLANK_POWERDOWN)
		d->enable = 0;

	return NOTIFY_DONE;
}

static ssize_t read_show(struct kobject *kobj,
	struct kobj_attribute *attr, char *buf)
{
	struct d_info *d = container_of(attr, struct d_info, dsi_access_r);
	struct dsim_device *dsim = get_dsim_drvdata(0);

	char *pos = buf;
	u8 reg, len, param = 0;
	int i;
	u8 *dump = NULL;
	unsigned int data_type;

	if (!d->enable) {
		dbg_info("enable is %s\n", d->enable ? "on" : "off");
		goto exit;
	}

	reg = d->dump_info[0];
	len = d->dump_info[1];
	data_type = d->data_type;

	if (!reg || !len || reg > U8_MAX || len > 255 || param > U8_MAX)
		goto exit;

	dump = kcalloc(len, sizeof(u8), GFP_KERNEL);

	dsim_read_data(dsim, data_type, reg, len, dump);

	for (i = 0; i < len; i++)
		pos += sprintf(pos, "%02x ", dump[i]);
	pos += sprintf(pos, "\n");

	dbg_info("+ [%02x]\n", reg);
	for (i = 0; i < len; i++)
		dbg_info("%2d: %02x\n", i + 1, dump[i]);
	dbg_info("- [%02x]\n", reg);

	kfree(dump);
exit:
	return pos - buf;
}

static ssize_t read_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t size)
{
	struct d_info *d = container_of(attr, struct d_info, dsi_access_r);
	unsigned int reg, len, param;
	unsigned int data_type, return_packet_type;
	int ret;

	ret = sscanf(buf, "%8x %8x %8x %8x %8x", &data_type, &reg, &param, &return_packet_type, &len);
	if (ret != 5)
		return -EINVAL;

	dbg_info("%x %x %x %x %x", data_type, reg, param, return_packet_type, len);

	if (!reg || !len || reg > U8_MAX || len > 255 || param > U8_MAX)
		return -EINVAL;

	d->data_type = data_type;
	d->dump_info[0] = reg;
	d->dump_info[1] = len;

	return size;
}

static ssize_t write_store(struct kobject *kobj,
	struct kobj_attribute *attr, const char *buf, size_t size)
{
	struct d_info *d = container_of(attr, struct d_info, dsi_access_w);
	struct dsim_device *dsim = get_dsim_drvdata(0);

	int ret, i, val, len = 0;
	unsigned char seqbuf[255] = {0, };
	unsigned char *printbuf = NULL;
	char *pos, *token;

	if (!d->enable) {
		dbg_info("enable is %s\n", d->enable ? "on" : "off");
		goto exit;
	}

	pos = (char *)buf;
	while ((token = strsep(&pos, " ")) != NULL) {
		ret = kstrtouint(token, 16, &val);
		if (!ret) {
			seqbuf[len] = val;
			len++;
		}
		if (len == ARRAY_SIZE(seqbuf))
			break;
	}

	pos = printbuf = kcalloc(size, sizeof(u8), GFP_KERNEL);
	for (i = 0; i < len; i++)
		pos += sprintf(pos, "%02x ", seqbuf[i]);
	pos += sprintf(pos, "\n");

	len--;
	if (len < 1) {
		dbg_info("invalid input, %s\n", printbuf);
		goto exit;
	} else
		dbg_info("%d, %s\n", len, printbuf);

	{
		if ((seqbuf[0] == 0x29) || (seqbuf[0] == 0x39))
			ret = dsim_write_data(dsim, (unsigned int)seqbuf[0], (unsigned long)&seqbuf[1], len);
		else if (len == 1)
			ret = dsim_write_data(dsim, (unsigned int)seqbuf[0], seqbuf[1], len);
		else if (len == 2)
			ret = dsim_write_data(dsim, (unsigned int)seqbuf[0], seqbuf[1], seqbuf[2]);
		else
			ret = dsim_write_data(dsim, (unsigned int)seqbuf[0], (unsigned long)&seqbuf[1], len);
	}

exit:
	kfree(printbuf);

	return size;
}

static void lcd_init_dsi_access(struct d_info *d)
{
	int ret = 0;

	if (!IS_ENABLED(CONFIG_SEC_INCELL))
		return;

	d->dsi_access = kobject_create_and_add("dsi_access", NULL);
	if (!d->dsi_access)
		return;

	sysfs_attr_init(&d->dsi_access_r.attr);
	d->dsi_access_r.attr.name = "read";
	d->dsi_access_r.attr.mode = 0644;
	d->dsi_access_r.store = read_store;
	d->dsi_access_r.show = read_show;
	ret = sysfs_create_file(d->dsi_access, &d->dsi_access_r.attr);
	if (ret < 0)
		dbg_info("failed to add dsi_access_r\n");

	sysfs_attr_init(&d->dsi_access_w.attr);
	d->dsi_access_w.attr.name = "write";
	d->dsi_access_w.attr.mode = 0220;
	d->dsi_access_w.store = write_store;
	ret = sysfs_create_file(d->dsi_access, &d->dsi_access_w.attr);
	if (ret < 0)
		dbg_info("failed to add dsi_access_w\n");
}

static int init_add_unlock(struct d_info *d, unsigned char *input)
{
	unsigned char *ibuf = NULL;

	ibuf = kstrdup(input, GFP_KERNEL);
	if (!ibuf)
		return -ENOMEM;

	add_unlock(d, ibuf);
	kfree(ibuf);

	return 0;
}

static int init_debugfs_lcd(void)
{
	int ret = 0;
	static struct dentry *debugfs_root;
	struct d_info *d = NULL;
	struct device *dev = find_lcd_device();

	if (!dev) {
		dbg_warn("failed to get device\n");
		ret = -ENODEV;
		goto exit;
	}

	dbg_info("+\n");

	d = kzalloc(sizeof(struct d_info), GFP_KERNEL);

	if (!debugfs_root) {
		debugfs_root = debugfs_create_dir("dd_lcd", NULL);
		debugfs_create_file("_help", S_IRUSR, debugfs_root, d, &help_fops);
	}

	d->dev = dev;
	d->tx_dump = &tx_dump;

	debugfs_create_u32("tx_dump", S_IRUSR | S_IWUSR, debugfs_root, d->tx_dump);
	debugfs_create_file("rx", S_IRUSR | S_IWUSR, debugfs_root, d, &rx_fops);
	debugfs_create_file("tx", S_IRUSR | S_IWUSR, debugfs_root, d, &tx_fops);
	debugfs_create_file("unlock", S_IRUSR | S_IWUSR, debugfs_root, d, &unlock_fops);

	INIT_LIST_HEAD(&d->unlock_list);
	init_add_unlock(d, "f0 5a 5a");
	init_add_unlock(d, "f1 5a 5a");
	init_add_unlock(d, "fc 5a 5a");

	lcd_init_dsi_access(d);

	d->fb_notifier.notifier_call = fb_notifier_callback;
	decon_register_notifier(&d->fb_notifier);
	d->enable = 1;

	dbg_info("-\n");

exit:
	return ret;
}

static struct notifier_block dd_lcd_fb_notifier;

static int fb_register_callback(struct notifier_block *self,
			unsigned long event, void *data)
{
	struct fb_event *evdata = data;

	switch (event) {
	case FB_EVENT_FB_REGISTERED:
		break;
	default:
		return NOTIFY_DONE;
	}

	if (evdata->info->node)
		return NOTIFY_DONE;

	init_debugfs_lcd();

	fb_unregister_client(&dd_lcd_fb_notifier);

	return NOTIFY_DONE;
}

static int __init dd_lcd_init(void)
{
	/* init_debugfs_lcd(); */

	dd_lcd_fb_notifier.notifier_call = fb_register_callback;
	fb_register_client(&dd_lcd_fb_notifier);

	return 0;
}

static void __exit dd_lcd_exit(void)
{
	fb_unregister_client(&dd_lcd_fb_notifier);
}

/* we use fb notification instead of late_initcall */
/* late_initcall(dd_lcd_init); */
module_init(dd_lcd_init);
module_exit(dd_lcd_exit);
#endif

