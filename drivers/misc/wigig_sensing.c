// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2019, The Linux foundation. All rights reserved.
 * Copyright (C) 2020 XiaoMi, Inc.
 */

#include <linux/cdev.h>
#include <linux/circ_buf.h>
#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/ioctl.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/poll.h>
#include <linux/spi/spi.h>
#include <linux/time.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <uapi/misc/wigig_sensing_uapi.h>
#include "wigig_sensing.h"

#define DRV_NAME "wigig_sensing"
#define CLEAR_LOW_23_BITS ~(BIT(24) - 1)
#define NUM_RETRIES  5
#define FW_TIMEOUT_MSECS (1000)

#define circ_cnt(circ, size) \
	((CIRC_CNT((circ)->head, (circ)->tail, size)) & ~3)
#define circ_cnt_to_end(circ, size) \
	((CIRC_CNT_TO_END((circ)->head, (circ)->tail, size)) & ~3)
#define circ_space(circ, size) \
	((CIRC_SPACE((circ)->head, (circ)->tail, size)) & ~3)
#define circ_space_to_end(circ, size) \
	((CIRC_SPACE_TO_END((circ)->head, (circ)->tail, size)) & ~3)

struct wigig_sensing_platform_data {
	struct gpio_desc *dri_gpio;
};

struct wigig_sensing_ctx *ctx;

static int spis_reset(struct spi_device *spi)
{
	struct wigig_sensing_ctx *ctx = spi_get_drvdata(spi);
	u8 cmd[] = { 0xDA, 0xBA, 0x00, 0x0 };
	int rc;

	memcpy(ctx->cmd_buf, cmd, sizeof(cmd));
	rc = spi_write(spi, ctx->cmd_buf, sizeof(cmd));

	return rc;
}

static int spis_clock_request(struct spi_device *spi)
{
	struct wigig_sensing_ctx *ctx = spi_get_drvdata(spi);
	u8 cmd[] = { 0xDA, 0xBA, 0x80, 0x0 };
	int rc;

	memcpy(ctx->cmd_buf, cmd, sizeof(cmd));
	rc = spi_write(spi, ctx->cmd_buf, sizeof(cmd));

	return rc;
}

static int spis_nop(struct spi_device *spi)
{
	struct wigig_sensing_ctx *ctx = spi_get_drvdata(spi);
	u8 cmd[] = { 0x0 };
	int rc;

	memcpy(ctx->cmd_buf, cmd, sizeof(cmd));
	rc = spi_write(spi, ctx->cmd_buf, sizeof(cmd));

	return rc;
}

static int spis_write_enable(struct spi_device *spi)
{
	struct wigig_sensing_ctx *ctx = spi_get_drvdata(spi);
	u8 cmd[] = { 0x6 };
	int rc;

	memcpy(ctx->cmd_buf, cmd, sizeof(cmd));
	rc = spi_write(spi, ctx->cmd_buf, sizeof(cmd));

	return rc;
}

static int spis_extended_reset(struct spi_device *spi)
{
	struct wigig_sensing_ctx *ctx = spi_get_drvdata(spi);
	u8 cmd[SPIS_EXTENDED_RESET_COMMAND_LEN] = { 0xDA, 0xBA };
	int rc;

	memcpy(ctx->cmd_buf, cmd, sizeof(cmd));
	rc = spi_write(spi, ctx->cmd_buf, sizeof(cmd));

	return rc;
}

static int spis_read_internal_reg(struct spi_device *spi, u8 address, u32 *val)
{
	struct wigig_sensing_ctx *ctx = spi_get_drvdata(spi);
	u8 cmd[] = { 0x81, address, 0, 0 };
	struct spi_transfer tx_xfer = {
		.tx_buf = ctx->cmd_buf,
		.rx_buf	= NULL,
		.len = 4,
		.bits_per_word = 8,
	};
	struct spi_transfer rx_xfer = {
		.tx_buf = NULL,
		.rx_buf	= ctx->cmd_reply_buf,
		.len = 4,
		.bits_per_word = 8,
	};
	int rc;
	struct spi_message msg;

	memcpy(ctx->cmd_buf, cmd, sizeof(cmd));

	spi_message_init(&msg);
	spi_message_add_tail(&tx_xfer, &msg);
	spi_message_add_tail(&rx_xfer, &msg);
	rc = spi_sync(ctx->spi_dev, &msg);

	*val = be32_to_cpu(*(u32 *)(ctx->cmd_reply_buf));

	return rc;
}

static int spis_read_mem(struct spi_device *spi, u32 address, u32 *val)
{
	struct wigig_sensing_ctx *ctx = spi_get_drvdata(spi);
	u8 cmd[] = {
		0x83,
		(address >> 16) & 0xFF,
		(address >>  8) & 0xFF,
		(address >>  0) & 0xFF,
		0, 0, 0, 0
	};
	struct spi_transfer tx_xfer = {
		.tx_buf = ctx->cmd_buf,
		.rx_buf	= NULL,
		.len = 8,
		.bits_per_word = 8,
	};
	struct spi_transfer rx_xfer = {
		.tx_buf = NULL,
		.rx_buf	= ctx->cmd_reply_buf,
		.len = 4,
		.bits_per_word = 8,
	};
	int rc;
	struct spi_message msg;

	memcpy(ctx->cmd_buf, cmd, sizeof(cmd));

	spi_message_init(&msg);
	spi_message_add_tail(&tx_xfer, &msg);
	spi_message_add_tail(&rx_xfer, &msg);
	rc = spi_sync(ctx->spi_dev, &msg);

	*val = be32_to_cpu(*(u32 *)(ctx->cmd_reply_buf));

	return rc;
}

static int spis_write_internal_reg(struct spi_device *spi, u8 address, u32 val)
{
	struct wigig_sensing_ctx *ctx = spi_get_drvdata(spi);
	u8 cmd[] = { 0x82, address, 0, 0, 0, 0 };
	int rc = spis_write_enable(ctx->spi_dev);

	cmd[2] = (val >> 24) & 0xFF;
	cmd[3] = (val >> 16) & 0xFF;
	cmd[4] = (val >>  8) & 0xFF;
	cmd[5] = (val >>  0) & 0xFF;

	memcpy(ctx->cmd_buf, cmd, sizeof(cmd));
	rc |= spi_write(spi, ctx->cmd_buf, sizeof(cmd));

	return rc;
}

static int spis_write_mem(struct spi_device *spi, u32 address, u32 val)
{
	struct wigig_sensing_ctx *ctx = spi_get_drvdata(spi);
	u8 cmd[] = {
		0x02,
		(address >> 16) & 0xFF,
		(address >>  8) & 0xFF,
		(address >>  0) & 0xFF,
		(val >> 24) & 0xFF,
		(val >> 16) & 0xFF,
		(val >>  8) & 0xFF,
		(val >>  0) & 0xFF
	};
	int rc = spis_write_enable(ctx->spi_dev);

	memcpy(ctx->cmd_buf, cmd, sizeof(cmd));
	rc |= spi_write(spi, ctx->cmd_buf, sizeof(cmd));

	return rc;
}

static int spis_write_reg(struct spi_device *spi, u32 addr, u32 val)
{
	int rc;

	if (addr < 256)
		rc = spis_write_internal_reg(spi, addr, val);
	else
		rc = spis_write_mem(spi, addr, val);

	pr_debug("write reg 0x%X val 0x%X\n", addr, val);

	return rc;
}

static int spis_block_read_mem(struct spi_device *spi,
			       u32 address,
			       u8 *data,
			       u32 length,
			       u32 last_read_length)
{
	struct wigig_sensing_ctx *ctx = spi_get_drvdata(spi);
	int rc;
	int overhead = OPCODE_WIDTH + ADDR_WIDTH + DUMMY_BYTES_WIDTH;
	int sz = overhead + length;
	struct spi_transfer xfer = {
		.tx_buf = ctx->tx_buf,
		.rx_buf = ctx->rx_buf,
		.len = sz,
		.bits_per_word = 32,
	};
	u32 frame = 0xB << 24;

	frame |= address & 0xFFFFFF;
	frame = cpu_to_le32(frame);

	/* Read length must be in 32 bit units */
	if (length & 3) {
		pr_err("Read length must be a multiple of 32 bits\n");
		return -EINVAL;
	}
	if (length > SPI_MAX_TRANSACTION_SIZE) {
		pr_err("Read length too large\n");
		return -EINVAL;
	}

	/* Write transfer length to SPI core */
	if (length != last_read_length) {
		rc = spis_write_reg(ctx->spi_dev,
				    SPIS_TRNS_LEN_REG_ADDR,
				    length << 16);
		if (rc) {
			pr_err("Failed setting SPIS_TRNS_LEN_REG_ADDR\n");
			return rc;
		}
		ctx->last_read_length = length;
	}

	memcpy(ctx->tx_buf, &frame, sizeof(frame));

	 /* Execute transaction */
	rc = spi_sync_transfer(spi, &xfer, 1);
	if (rc) {
		pr_err("SPI transaction failed, rc = %d\n", rc);
		return rc;
	}

	memcpy(data, ctx->rx_buf + overhead, length);

	pr_debug("Sent block_read_mem command, addr=0x%X, length=%u\n",
		 address, length);

	return rc;
}

static int spis_read_reg(struct spi_device *spi, u32 addr, u32 *val)
{
	int rc;

	if (addr < 256)
		rc = spis_read_internal_reg(spi, addr, val);
	else
		rc = spis_read_mem(spi, addr, val);

	pr_debug("read reg 0x%X, val = 0x%X\n", addr, *val);

	return rc;
}

#define RGF_SPI_FIFO_CONTROL_ADDR (0x8800A0)
#define RGF_SPI_FIFO_WR_PTR_ADDR (0x88009C)
#define RGF_SPI_FIFO_RD_PTR_ADDR (0x880098)
#define RGF_SPI_FIFO_BASE_ADDR_ADDR (0x880094)
#define RGF_SPI_CONTROL_ADDR (0x880090)
#define RGF_SPI_CONFIG_ADDR (0x88008C)
static int cache_fifo_regs(struct wigig_sensing_ctx *ctx)
{
	int rc;
	struct spi_device *spi = ctx->spi_dev;
	struct spi_fifo *f = &ctx->spi_fifo;

	rc  = spis_read_reg(spi, RGF_SPI_CONFIG_ADDR, &f->config.v);
	rc |= spis_read_reg(spi, RGF_SPI_CONTROL_ADDR, &f->control.v);
	rc |= spis_read_reg(spi, RGF_SPI_FIFO_BASE_ADDR_ADDR, &f->base_addr);
	rc |= spis_read_reg(spi, RGF_SPI_FIFO_RD_PTR_ADDR, &f->rd_ptr);
	rc |= spis_read_reg(spi, RGF_SPI_FIFO_WR_PTR_ADDR, &f->wr_ptr);
	if (rc) {
		pr_err("%s failed, rc=%u\n", __func__, rc);
		return -EFAULT;
	}

	/* Update read pointer to be the FIFO base address */
	f->rd_ptr = ctx->spi_fifo.base_addr;

	pr_debug("SPI FIFO base address  = 0x%X\n", f->base_addr);
	pr_debug("SPI FIFO size          = 0x%X\n", f->config.b.size);
	pr_debug("SPI FIFO enable        = %u\n", f->config.b.enable);
	pr_debug("SPI FIFO read pointer  = 0x%X\n", f->rd_ptr);
	pr_debug("SPI FIFO write pointer = 0x%X\n", f->wr_ptr);

	return 0;
}

static int spis_init(struct wigig_sensing_ctx *ctx)
{
	int rc;
	u32 spis_sanity_reg = 0;
	u32 jtag_id = 0;
	struct spi_device *spi = ctx->spi_dev;

	/* Initialize SPI */
	spi->max_speed_hz = 32e6;
	spi->bits_per_word = 8;
	spi->mode = SPI_MODE_0;
	rc = spi_setup(spi);
	if (rc) {
		pr_err("spi_setup() failed (%d)\n", rc);
		return rc;
	}

	rc  = spis_reset(spi);
	rc |= spis_nop(spi);
	rc |= spis_clock_request(spi);
	rc |= spis_nop(spi);
	rc |= spis_read_internal_reg(spi, SPIS_SANITY_REG_ADDR,
				     &spis_sanity_reg);
	pr_debug("sanity = 0x%x\n", spis_sanity_reg);
	/* use 4 dummy bytes to make block read/writes 32-bit aligned */
	rc |= spis_write_reg(spi, SPIS_CFG_REG_ADDR, SPIS_CONFIG_REG_OPT_VAL);
	rc |= spis_read_mem(spi, JTAG_ID_REG_ADDR, &jtag_id);
	pr_debug("jtag_id = 0x%x\n", jtag_id);
	if (rc || spis_sanity_reg != SPIS_SANITY_REG_VAL ||
	    jtag_id != JTAG_ID) {
		pr_err("SPI init failed, sanity=0x%X, jtag_id=0x%X\n",
		       spis_sanity_reg, jtag_id);
		return -EFAULT;
	}

	/* Copy FIFO register values from the HW */
	rc = cache_fifo_regs(ctx);
	if (rc)
		pr_err("cache_fifo_regs() failed\n");

	return rc;
}

static enum wigig_sensing_stm_e convert_mode_to_state(
	enum wigig_sensing_mode mode)
{
	switch (mode) {
	case WIGIG_SENSING_MODE_SEARCH:
		return WIGIG_SENSING_STATE_SEARCH;
	case WIGIG_SENSING_MODE_FACIAL_RECOGNITION:
		return WIGIG_SENSING_STATE_FACIAL;
	case WIGIG_SENSING_MODE_GESTURE_DETECTION:
		return WIGIG_SENSING_STATE_GESTURE;
	case WIGIG_SENSING_MODE_STOP:
		return WIGIG_SENSING_STATE_READY_STOPPED;
	case WIGIG_SENSING_MODE_CUSTOM:
		return WIGIG_SENSING_STATE_CUSTOM;
	default:
		return WIGIG_SENSING_STATE_MIN;
	}
}

static int wigig_sensing_send_change_mode_command(
	struct wigig_sensing_ctx *ctx,
	enum wigig_sensing_mode mode,
	u32 channel)
{
	int rc = 0;
	union user_rgf_spi_mbox_inb inb_reg = { { 0 } };

	pr_debug("mode=%d, channel=%d\n", mode, channel);
	inb_reg.b.mode = mode;
	inb_reg.b.channel_request = channel;

	ctx->inb_cmd = inb_reg;

	mutex_lock(&ctx->spi_lock);
	rc = spis_extended_reset(ctx->spi_dev);
	pr_debug("Sent extended reset command, rc = %d\n", rc);

	mutex_unlock(&ctx->spi_lock);
	if (rc) {
		pr_err("failed to send extended reset\n");
		return -EFAULT;
	}
	ctx->stm.waiting_for_deep_sleep_exit = true;

	return 0;
}

static int wigig_sensing_change_state(struct wigig_sensing_ctx *ctx,
				      struct wigig_sensing_stm *state,
				      enum wigig_sensing_stm_e new_state)
{
	enum wigig_sensing_stm_e curr_state;
	bool transition_allowed = false;

	if (!state) {
		pr_err("state is NULL\n");
		return -EINVAL;
	}
	if (new_state <= WIGIG_SENSING_STATE_MIN ||
	    new_state >= WIGIG_SENSING_STATE_MAX) {
		pr_err("new_state is invalid\n");
		return -EINVAL;
	}

	curr_state = state->state;
	if (new_state == curr_state) {
		pr_debug("Already in the requested state, bailing out\n");
		return 0;
	}

	if ((new_state == WIGIG_SENSING_STATE_SYS_ASSERT &&
	     !state->fw_is_ready) ||
	    (new_state == WIGIG_SENSING_STATE_SPI_READY)) {
		transition_allowed = true;
	} else {
		switch (curr_state) {
		case WIGIG_SENSING_STATE_INITIALIZED:
			if (new_state == WIGIG_SENSING_STATE_SPI_READY &&
			    state->fw_is_ready)
				transition_allowed = true;
			break;
		case WIGIG_SENSING_STATE_SPI_READY:
			if (new_state == WIGIG_SENSING_STATE_READY_STOPPED &&
			    state->enabled)
				transition_allowed = true;
			break;
		case WIGIG_SENSING_STATE_READY_STOPPED:
			if (new_state == WIGIG_SENSING_STATE_SEARCH        ||
			    new_state == WIGIG_SENSING_STATE_FACIAL        ||
			    new_state == WIGIG_SENSING_STATE_GESTURE       ||
			    new_state == WIGIG_SENSING_STATE_CUSTOM        ||
			    new_state == WIGIG_SENSING_STATE_GET_PARAMS)
				transition_allowed = true;
			break;
		case WIGIG_SENSING_STATE_SEARCH:
			if (new_state == WIGIG_SENSING_STATE_READY_STOPPED ||
			    new_state == WIGIG_SENSING_STATE_SEARCH ||
			    new_state == WIGIG_SENSING_STATE_FACIAL ||
			    new_state == WIGIG_SENSING_STATE_GESTURE)
				transition_allowed = true;
			break;
		case WIGIG_SENSING_STATE_FACIAL:
			if (new_state == WIGIG_SENSING_STATE_READY_STOPPED ||
			    new_state == WIGIG_SENSING_STATE_SEARCH)
				transition_allowed = true;
			break;
		case WIGIG_SENSING_STATE_GESTURE:
			if (new_state == WIGIG_SENSING_STATE_READY_STOPPED ||
			    new_state == WIGIG_SENSING_STATE_SEARCH)
				transition_allowed = true;
			break;
		case WIGIG_SENSING_STATE_CUSTOM:
			if (new_state == WIGIG_SENSING_STATE_READY_STOPPED)
				transition_allowed = true;
			break;
		case WIGIG_SENSING_STATE_GET_PARAMS:
			if (new_state == WIGIG_SENSING_STATE_READY_STOPPED)
				transition_allowed = true;
			break;
		case WIGIG_SENSING_STATE_SYS_ASSERT:
			if (new_state == WIGIG_SENSING_STATE_READY_STOPPED &&
			    state->fw_is_ready)
				transition_allowed = true;
			break;
		default:
			pr_err("new_state is invalid\n");
			return -EINVAL;
		}
	}

	if (transition_allowed) {
		pr_info("state transition (%d) --> (%d)\n", curr_state,
			new_state);
		state->state = new_state;
	} else {
		pr_info("state transition rejected (%d) xx> (%d)\n",
			curr_state, new_state);
	}

	return 0;
}

static int wigig_sensing_ioc_set_auto_recovery(struct wigig_sensing_ctx *ctx)
{
	pr_info("Handling WIGIG_SENSING_IOCTL_SET_AUTO_RECOVERY\n");
	pr_info("NOT SUPPORTED\n");

	ctx->stm.auto_recovery = true;

	return 0;
}

static int wigig_sensing_ioc_get_mode(struct wigig_sensing_ctx *ctx)
{
	return ctx->stm.mode;
}

static int wigig_sensing_ioc_change_mode(struct wigig_sensing_ctx *ctx,
					 struct wigig_sensing_change_mode req)
{
	struct wigig_sensing_stm sim_state;
	enum wigig_sensing_stm_e new_state;
	int rc;
	u32 ch;

	pr_info("mode = %d, channel = %d\n", req.mode, req.channel);
	if (!ctx)
		return -EINVAL;

	/* Simulate a state change */
	new_state = convert_mode_to_state(req.mode);
	sim_state = ctx->stm;
	rc = wigig_sensing_change_state(ctx, &sim_state, new_state);
	if (rc || sim_state.state != new_state) {
		pr_err("State change not allowed\n");
		rc = -EFAULT;
		goto End;
	}

	/* Send command to FW */
	ctx->stm.change_mode_in_progress = true;
	ch = req.has_channel ? req.channel : 0;
	ctx->stm.burst_size_ready = false;
	/* Change mode command must not be called during DRI processing */
	mutex_lock(&ctx->dri_lock);
	rc = wigig_sensing_send_change_mode_command(ctx, req.mode, ch);
	mutex_unlock(&ctx->dri_lock);
	if (rc) {
		pr_err("wigig_sensing_send_change_mode_command() failed, err %d\n",
		       rc);
		goto End;
	}

	/* Put the calling process to sleep until we get a response from FW */
	rc = wait_event_interruptible_timeout(
		ctx->cmd_wait_q,
		ctx->stm.burst_size_ready,
		msecs_to_jiffies(FW_TIMEOUT_MSECS));
	if (rc < 0) {
		/* Interrupted by a signal */
		pr_err("wait_event_interruptible_timeout() interrupted by a signal (%d)\n",
		       rc);
		return rc;
	}
	if (rc == 0) {
		/* Timeout, FW did not respond in time */
		pr_err("wait_event_interruptible_timeout() timed out\n");
		return -ETIME;
	}

	/* Change internal state */
	rc = wigig_sensing_change_state(ctx, &ctx->stm, new_state);
	if (rc || ctx->stm.state != new_state) {
		pr_err("wigig_sensing_change_state() failed\n");
		rc = -EFAULT;
		goto End;
	}

	ctx->dropped_bursts = 0;
	ctx->stm.channel_request = ch;
	ctx->stm.mode = req.mode;
	ctx->stm.change_mode_in_progress = false;

End:
	return ctx->stm.burst_size;
}

static int wigig_sensing_ioc_clear_data(struct wigig_sensing_ctx *ctx)
{
	struct cir_data *d = &ctx->cir_data;

	if (mutex_lock_interruptible(&d->lock))
		return -ERESTARTSYS;
	d->b.tail = d->b.head = 0;

	/* Make sure that the write above is visible to other threads */
	wmb();
	mutex_unlock(&d->lock);

	return 0;
}

static int wigig_sensing_ioc_get_num_dropped_bursts(
	struct wigig_sensing_ctx *ctx)
{
	return ctx->dropped_bursts;
}

static int wigig_sensing_ioc_get_event(struct wigig_sensing_ctx *ctx)
{
	return 0;
}

static int wigig_sensing_open(struct inode *inode, struct file *filp)
{
	/* forbid opening more then one instance at a time */
	if (mutex_lock_interruptible(&ctx->file_lock))
		return -ERESTARTSYS;

	if (ctx->opened) {
		mutex_unlock(&ctx->file_lock);
		return -EBUSY;
	}

	filp->private_data = ctx;
	ctx->opened = true;
	mutex_unlock(&ctx->file_lock);

	return 0;
}

static unsigned int wigig_sensing_poll(struct file *filp, poll_table *wait)
{
	struct wigig_sensing_ctx *ctx = filp->private_data;
	unsigned int mask = 0;

	if (!ctx->opened)
		return -ENODEV;

	poll_wait(filp, &ctx->data_wait_q, wait);

	if (circ_cnt(&ctx->cir_data.b, ctx->cir_data.size_bytes))
		mask |= (POLLIN | POLLRDNORM);

	if (ctx->event_pending)
		mask |= (POLLPRI);

	return mask;
}

static ssize_t wigig_sensing_read(struct file *filp, char __user *buf,
				  size_t count, loff_t *f_pos)
{
	int rc = 0;
	struct wigig_sensing_ctx *ctx = filp->private_data;
	u32 copy_size, size_to_end;
	int tail;
	struct cir_data *d = &ctx->cir_data;

	/* Driver not ready to send data */
	if ((!ctx) ||
	    (!ctx->spi_dev) ||
	    (!d->b.buf))
		return -ENODEV;

	/* No data in the buffer */
	while (circ_cnt(&d->b, d->size_bytes) == 0) {
		if (filp->f_flags & O_NONBLOCK)
			return -EAGAIN;

		if (wait_event_interruptible(ctx->data_wait_q,
			circ_cnt(&d->b, d->size_bytes) != 0))
			return -ERESTARTSYS;
	}

	if (mutex_lock_interruptible(&d->lock))
		return -ERESTARTSYS;

	copy_size = min_t(u32, circ_cnt(&d->b, d->size_bytes), count);
	size_to_end = circ_cnt_to_end(&d->b, d->size_bytes);
	tail = d->b.tail;
	pr_debug("copy_size=%u, size_to_end=%u, head=%u, tail=%u\n",
		 copy_size, size_to_end, d->b.head, tail);
	if (copy_size <= size_to_end) {
		rc = copy_to_user(buf, &d->b.buf[tail], copy_size);
		if (rc) {
			pr_err("copy_to_user() failed.\n");
			rc = -EFAULT;
			goto bail_out;
		}
	} else {
		rc = copy_to_user(buf, &d->b.buf[tail], size_to_end);
		if (rc) {
			pr_err("copy_to_user() failed.\n");
			rc = -EFAULT;
			goto bail_out;
		}

		rc = copy_to_user(buf + size_to_end, &d->b.buf[0],
				  copy_size - size_to_end);
		if (rc) {
			pr_err("copy_to_user() failed.\n");
			rc = -EFAULT;
			goto bail_out;
		}
	}

	/* Increment tail pointer */
	d->b.tail = (d->b.tail + copy_size) & (d->size_bytes - 1);

bail_out:
	mutex_unlock(&d->lock);
	return (rc == 0) ? copy_size : rc;
}

static int wigig_sensing_release(struct inode *inode, struct file *filp)
{
	struct wigig_sensing_ctx *ctx = filp->private_data;

	if (!ctx || !ctx->spi_dev)
		return -ENODEV;

	mutex_lock(&ctx->file_lock);

	if (!ctx->opened) {
		mutex_unlock(&ctx->file_lock);
		return -ENODEV;
	}

	filp->private_data = NULL;
	ctx->opened = false;

	mutex_unlock(&ctx->file_lock);

	return 0;
}

static long wigig_sensing_ioctl(struct file *file, unsigned int cmd,
				unsigned long arg)
{
	int rc;
	struct wigig_sensing_ctx *ctx = file->private_data;

	if (!ctx || !ctx->spi_dev)
		return -ENODEV;

	if (mutex_lock_interruptible(&ctx->ioctl_lock))
		return -ERESTARTSYS;

	if (!ctx->opened) {
		mutex_unlock(&ctx->ioctl_lock);
		return -ENODEV;
	}

	/* Check type and command number */
	if (_IOC_TYPE(cmd) != WIGIG_SENSING_IOC_MAGIC) {
		mutex_unlock(&ctx->ioctl_lock);
		return -ENOTTY;
	}

	switch (_IOC_NR(cmd)) {
	case WIGIG_SENSING_IOCTL_SET_AUTO_RECOVERY:
		pr_info("Received WIGIG_SENSING_IOCTL_SET_AUTO_RECOVERY command\n");
		rc = wigig_sensing_ioc_set_auto_recovery(ctx);
		break;
	case WIGIG_SENSING_IOCTL_GET_MODE:
		pr_info("Received WIGIG_SENSING_IOCTL_GET_MODE command\n");
		rc = wigig_sensing_ioc_get_mode(ctx);
		break;
	case WIGIG_SENSING_IOCTL_CHANGE_MODE:
	{
		struct wigig_sensing_change_mode req;

		pr_info("Received WIGIG_SENSING_IOCTL_CHANGE_MODE command\n");

		if (copy_from_user(&req, (void *)arg, sizeof(req)))
			return -EFAULT;

		rc = wigig_sensing_ioc_change_mode(ctx, req);
		break;
	}
	case WIGIG_SENSING_IOCTL_CLEAR_DATA:
		pr_info("Received WIGIG_SENSING_IOCTL_CLEAR_DATA command\n");
		rc = wigig_sensing_ioc_clear_data(ctx);
		break;
	case WIGIG_SENSING_IOCTL_GET_NUM_DROPPED_BURSTS:
		pr_info("Received WIGIG_SENSING_IOCTL_GET_NUM_DROPPED_BURSTS command\n");
		rc = wigig_sensing_ioc_get_num_dropped_bursts(ctx);
		break;
	case WIGIG_SENSING_IOCTL_GET_EVENT:
		pr_info("Received WIGIG_SENSING_IOCTL_GET_EVENT command\n");
		rc = wigig_sensing_ioc_get_event(ctx);
		break;
	default:
		rc = -EINVAL;
		break;
	}

	mutex_unlock(&ctx->ioctl_lock);

	return rc;
}

static const struct file_operations wigig_sensing_fops = {
	.owner          = THIS_MODULE,
	.llseek         = no_llseek,
	.open           = wigig_sensing_open,
	.poll           = wigig_sensing_poll,
	.read           = wigig_sensing_read,
	.release        = wigig_sensing_release,
	.unlocked_ioctl = wigig_sensing_ioctl,
};

static int wigig_sensing_alloc_buffer(struct wigig_sensing_ctx *ctx,
				      u32 buffer_size_bytes)
{
	/* Allocate CIR data_buffer */
	ctx->cir_data.b.buf = vmalloc(buffer_size_bytes);
	if (!ctx->cir_data.b.buf)
		return -ENOMEM;

	ctx->cir_data.size_bytes = buffer_size_bytes;
	mutex_init(&ctx->cir_data.lock);

	return 0;
}

static int wigig_sensing_deassert_dri(
	struct wigig_sensing_ctx *ctx,
	union user_rgf_spi_mbox_inb additional_cmd)
{
	union user_rgf_spi_mbox_inb inb_reg;
	int rc;

	inb_reg = additional_cmd;
	inb_reg.b.deassert_dri = 1;
	mutex_lock(&ctx->spi_lock);
	rc = spis_write_reg(ctx->spi_dev, RGF_USER_SPI_SPI_MBOX_INB, inb_reg.v);
	mutex_unlock(&ctx->spi_lock);
	if (rc)
		pr_err("Fail to write RGF_USER_SPI_SPI_MBOX_INB, err %d\n",
		       rc);

	return rc;
}

#define MAX_SPI_READ_CHUNKS (10)
/*
 * Calculate SPI transaction size so that the size is smaller than the maximum
 * size and the size is equal per iteration. The equal sizes causes less SPI
 * transactions since the length register does not need reprogramming.
 */
static u32 calc_spi_transaction_size(
	u32 fill_level, u32 max_spi_transaction_size)
{
	int i;
	u32 res;

	if (fill_level <= max_spi_transaction_size)
		return fill_level;

	for (i = 2; i < MAX_SPI_READ_CHUNKS; i++) {
		res = fill_level / i;
		if (res <= max_spi_transaction_size && fill_level % i == 0)
			return res;
	}

	return max_spi_transaction_size;
}

static int wigig_sensing_handle_fifo_ready_dri(struct wigig_sensing_ctx *ctx)
{
	int rc = 0;
	u32 burst_size = 0;
	u32 spi_transaction_size;

	mutex_lock(&ctx->spi_lock);

	/* Read burst size over SPI */
	rc = spis_read_reg(ctx->spi_dev, RGF_USER_SPI_SPI_EXT_MBOX_OUTB,
			   &burst_size);
	if (rc) {
		pr_err("Fail to read RGF_USER_SPI_SPI_EXT_MBOX_OUTB, err %d\n",
		       rc);
		goto End;
	}

	if (!ctx->stm.enabled && burst_size != 0) {
		pr_info("Invalid burst size while disabled %d\n", burst_size);
		rc = -EFAULT;
		goto End;
	}

	ctx->stm.burst_size = burst_size;
	if (!ctx->stm.enabled ||
	    ctx->stm.state >= WIGIG_SENSING_STATE_SYS_ASSERT ||
	    ctx->stm.state < WIGIG_SENSING_STATE_SPI_READY) {
		pr_err("Received burst_size in an unexpected state\n");
		rc = -EFAULT;
		goto End;
	}

	/* Program burst size into the transfer length register */
	spi_transaction_size = calc_spi_transaction_size(burst_size,
					SPI_MAX_TRANSACTION_SIZE);
	rc = spis_write_reg(ctx->spi_dev, SPIS_TRNS_LEN_REG_ADDR,
			    spi_transaction_size << 16);
	if (rc) {
		pr_err("Failed setting SPIS_TRNS_LEN_REG_ADDR\n");
		goto End;
	}
	ctx->last_read_length = spi_transaction_size;

	ctx->stm.burst_size_ready = true;

	/*
	 * Allocate a temporary buffer to be used in case of cir_data buffer
	 * wrap around
	 */
	if (burst_size != 0) {
		ctx->temp_buffer = vmalloc(burst_size);
		if (!ctx->temp_buffer) {
			rc = -ENOMEM;
			goto End;
		}
	} else if (ctx->temp_buffer) {
		vfree(ctx->temp_buffer);
		ctx->temp_buffer = 0;
	}

	wake_up_interruptible(&ctx->cmd_wait_q);
End:
	mutex_unlock(&ctx->spi_lock);
	return rc;
}

static int wigig_sensing_chip_data_ready(struct wigig_sensing_ctx *ctx,
					 u16 fill_level, u32 burst_size)
{
	int rc = 0;
	enum wigig_sensing_stm_e stm_state = ctx->stm.state;
	struct spi_fifo *spi_fifo = &ctx->spi_fifo;
	struct cir_data *d = &ctx->cir_data;
	struct circ_buf local;
	u32 bytes_to_read;
	u32 idx = 0;
	u32 spi_transaction_size;
	u32 available_space_to_end;

	if (stm_state == WIGIG_SENSING_STATE_INITIALIZED ||
	    stm_state == WIGIG_SENSING_STATE_SPI_READY ||
	    stm_state == WIGIG_SENSING_STATE_READY_STOPPED ||
	    stm_state == WIGIG_SENSING_STATE_SYS_ASSERT) {
		pr_err("Received data ready interrupt in an unexpected stm_state, disregarding\n");
		return 0;
	}

	if (!ctx->cir_data.b.buf)
		return -EFAULT;

	/*
	 * Read data from FIFO over SPI
	 * Disregard the lowest 2 bits of fill_level as SPI can read in 32 bit
	 * units. Additionally, HW reports the fill_level as one more than
	 * there is actually (1 when the FIFO is empty)
	 */
	if (fill_level == 0 || fill_level == 1) {
		pr_err("Wrong fill_level received\n");
		return -EFAULT;
	}
	fill_level = (fill_level - 1) & ~0x3;

	if (fill_level > burst_size) {
		pr_err("FIFO fill level too large, fill_level = %u, burst_size = %u\n",
		       fill_level, burst_size);
		return -EFAULT;
	}

	/*
	 * In case there is not enough space in the buffer, discard an old
	 * burst
	 */
	if (circ_space(&d->b, d->size_bytes) < fill_level) {
		mutex_lock(&d->lock);
		if (circ_space(&d->b, d->size_bytes) < fill_level) {
			pr_debug("Buffer full, dropping burst\n");
			d->b.tail = (d->b.tail + burst_size) &
				(d->size_bytes - 1);
			ctx->dropped_bursts++;
		}
		mutex_unlock(&d->lock);
	}

	spi_transaction_size =
		calc_spi_transaction_size(fill_level, SPI_MAX_TRANSACTION_SIZE);
	local = d->b;
	mutex_lock(&ctx->spi_lock);
	while (fill_level > 0) {
		bytes_to_read = (fill_level < spi_transaction_size) ?
			fill_level : spi_transaction_size;
		available_space_to_end =
			circ_space_to_end(&local, d->size_bytes);
		pr_debug("fill_level=%u, bytes_to_read=%u, idx=%u, available_space_to_end = %u\n",
			 fill_level, bytes_to_read, idx,
			 available_space_to_end);
		/* Determine transaction type */
		if (available_space_to_end >= bytes_to_read) {
			rc = spis_block_read_mem(ctx->spi_dev,
						 spi_fifo->base_addr + idx,
						 &d->b.buf[local.head],
						 bytes_to_read,
						 ctx->last_read_length
			);
		} else {
			/*
			 * There is not enough place in the CIR buffer, copy to
			 * a temporay buffer and then split
			 */
			rc = spis_block_read_mem(ctx->spi_dev,
						 spi_fifo->base_addr + idx,
						 ctx->temp_buffer,
						 bytes_to_read,
						 ctx->last_read_length
			);
			memcpy(&d->b.buf[local.head], ctx->temp_buffer,
			       available_space_to_end);
			memcpy(&d->b.buf[0],
			       &ctx->temp_buffer[available_space_to_end],
			       bytes_to_read - available_space_to_end);
		}

		fill_level -= bytes_to_read;
		idx += bytes_to_read;
		local.head = (local.head + bytes_to_read) & (d->size_bytes - 1);
	}
	mutex_unlock(&ctx->spi_lock);

	/* Increment destination rd_ptr */
	mutex_lock(&d->lock);
	d->b.head = local.head;
	pr_debug("head=%u, tail=%u\n", d->b.head, d->b.tail);
	mutex_unlock(&d->lock);

	wake_up_interruptible(&ctx->data_wait_q);

	return 0;
}

static int wigig_sensing_spi_init(struct wigig_sensing_ctx *ctx)
{
	int rc;

	/* Allocate buffers for SPI transactions */
	ctx->cmd_buf = devm_kzalloc(ctx->dev, SPI_CMD_BUFFER_SIZE, GFP_KERNEL);
	if (!ctx->cmd_buf)
		return -ENOMEM;

	ctx->cmd_reply_buf = devm_kzalloc(ctx->dev, SPI_CMD_BUFFER_SIZE,
					  GFP_KERNEL);
	if (!ctx->cmd_reply_buf) {
		rc = -ENOMEM;
		goto cmd_reply_buf_alloc_failed;
	}

	ctx->rx_buf = devm_kzalloc(ctx->dev, SPI_BUFFER_SIZE, GFP_KERNEL);
	if (!ctx->rx_buf) {
		rc = -ENOMEM;
		goto rx_buf_alloc_failed;
	}

	ctx->tx_buf = devm_kzalloc(ctx->dev, SPI_BUFFER_SIZE, GFP_KERNEL);
	if (!ctx->tx_buf) {
		rc = -ENOMEM;
		goto tx_buf_alloc_failed;
	}

	/* Initialize SPI slave device */
	mutex_lock(&ctx->spi_lock);
	rc = spis_init(ctx);
	mutex_unlock(&ctx->spi_lock);
	if (rc) {
		rc = -EFAULT;
		goto spis_init_failed;
	}

	return 0;

spis_init_failed:
	devm_kfree(ctx->dev, ctx->tx_buf);
	ctx->tx_buf = NULL;
tx_buf_alloc_failed:
	devm_kfree(ctx->dev, ctx->rx_buf);
	ctx->rx_buf = NULL;
rx_buf_alloc_failed:
	devm_kfree(ctx->dev, ctx->cmd_reply_buf);
	ctx->cmd_reply_buf = NULL;
cmd_reply_buf_alloc_failed:
	devm_kfree(ctx->dev, ctx->cmd_buf);
	ctx->cmd_buf = NULL;

	return rc;
}

static irqreturn_t wigig_sensing_dri_isr_thread(int irq, void *cookie)
{
	struct wigig_sensing_ctx *ctx = cookie;
	union user_rgf_spi_status spi_status;
	int rc;
	u32 sanity_reg = 0;
	union user_rgf_spi_mbox_inb additional_inb_command;
	u8 num_retries = 0;

	mutex_lock(&ctx->dri_lock);

	pr_debug("Process DRI signal, ctx->stm.state == %d\n", ctx->stm.state);
	memset(&additional_inb_command, 0, sizeof(additional_inb_command));

	if (ctx->stm.waiting_for_deep_sleep_exit) {
		pr_debug("waiting_for_deep_sleep_exit is set, sending NOP\n");
		ctx->stm.waiting_for_deep_sleep_exit_first_pass = true;
		mutex_lock(&ctx->spi_lock);
		rc = spis_nop(ctx->spi_dev);
		/* use 4 dummy bytes to make block read/writes 32-bit aligned */
		rc |= spis_write_reg(ctx->spi_dev, SPIS_CFG_REG_ADDR,
				     SPIS_CONFIG_REG_OPT_VAL);
		mutex_unlock(&ctx->spi_lock);
	}

	while (num_retries < NUM_RETRIES) {
		if (ctx->stm.state == WIGIG_SENSING_STATE_INITIALIZED ||
		    ctx->stm.spi_malfunction) {
			pr_debug("Initializing SPI for the first time, or SPI malfunction\n");

			rc = wigig_sensing_spi_init(ctx);
			if (rc) {
				pr_err("wigig_sensing_spi_init() failed\n");
				goto bail_out;
			}

			ctx->stm.spi_malfunction = false;
			if (ctx->stm.state == WIGIG_SENSING_STATE_INITIALIZED)
				wigig_sensing_change_state(ctx, &ctx->stm,
					WIGIG_SENSING_STATE_SPI_READY);
		}

		pr_debug("Reading SANITY register\n");
		mutex_lock(&ctx->spi_lock);
		rc = spis_read_reg(ctx->spi_dev, SPIS_SANITY_REG_ADDR,
				   &sanity_reg);
		mutex_unlock(&ctx->spi_lock);
		if (rc || sanity_reg != SPIS_SANITY_REG_VAL) {
			pr_err("Fail to read SANITY, expected 0x%X found 0x%X err %d\n",
			       SPIS_SANITY_REG_VAL, sanity_reg, (int)rc);
			ctx->stm.spi_malfunction = true;
		} else {
			pr_debug("SANITY OK\n");
			ctx->stm.spi_malfunction = false;
			break;
		}

		num_retries++;
		if (num_retries == NUM_RETRIES) {
			pr_err("SPI bus malfunction, bailing out\n");
			goto bail_out;
		}
	}

	pr_debug("Reading RGF_USER_SPI_SPI_MBOX_FILL_STATUS register\n");
	mutex_lock(&ctx->spi_lock);
	rc = spis_read_reg(ctx->spi_dev, RGF_USER_SPI_SPI_MBOX_FILL_STATUS,
			   &spi_status.v);
	mutex_unlock(&ctx->spi_lock);

	if (rc) {
		pr_err("Fail to read RGF_USER_SPI_SPI_MBOX_FILL_STATUS, err %d\n",
		       rc);
		goto bail_out;
	}

	if (spi_status.b.int_fw_ready) {
		pr_debug("FW READY INTERRUPT\n");
		ctx->stm.fw_is_ready = true;
		ctx->stm.channel_request = 0;
		ctx->stm.burst_size = 0;
		ctx->stm.mode = WIGIG_SENSING_MODE_STOP;
		ctx->stm.enabled = true;

		wigig_sensing_change_state(ctx, &ctx->stm,
					   WIGIG_SENSING_STATE_READY_STOPPED);

		spi_status.v &= ~INT_FW_READY;
	}
	if (spi_status.b.int_data_ready) {
		pr_debug("DATA READY INTERRUPT\n");
		if (!ctx->stm.change_mode_in_progress)
			wigig_sensing_chip_data_ready(ctx,
						      spi_status.b.fill_level,
						      ctx->stm.burst_size);
		else
			pr_debug("Change mode in progress, aborting data processing\n");
		spi_status.v &= ~INT_DATA_READY;
	}
	if (spi_status.b.int_sysassert) {
		pr_debug("SYSASSERT INTERRUPT\n");
		ctx->stm.fw_is_ready = false;

		rc = wigig_sensing_change_state(ctx, &ctx->stm,
				WIGIG_SENSING_STATE_SYS_ASSERT);
		if (rc != 0 ||
		    ctx->stm.state != WIGIG_SENSING_STATE_SYS_ASSERT)
			pr_err("State change to WIGIG_SENSING_SYS_ASSERT failed\n");

		ctx->stm.spi_malfunction = true;
		spi_status.v &= ~INT_SYSASSERT;
	}
	if (spi_status.b.int_deep_sleep_exit ||
	    (ctx->stm.waiting_for_deep_sleep_exit &&
	     ctx->stm.waiting_for_deep_sleep_exit_first_pass)) {
		if (spi_status.b.int_deep_sleep_exit)
			pr_debug("DEEP SLEEP EXIT INTERRUPT\n");

		if (ctx->stm.waiting_for_deep_sleep_exit) {
			additional_inb_command = ctx->inb_cmd;
			memset(&ctx->inb_cmd, 0, sizeof(ctx->inb_cmd));
		} else {
			pr_err("Got deep sleep exit DRI when ctx->stm.waiting_for_deep_sleep_exit is false\n");
		}
		ctx->stm.waiting_for_deep_sleep_exit = false;
		ctx->stm.waiting_for_deep_sleep_exit_first_pass = false;
		spi_status.v &= ~INT_DEEP_SLEEP_EXIT;
	}
	if (spi_status.b.int_fifo_ready) {
		pr_debug("FIFO READY INTERRUPT\n");
		wigig_sensing_handle_fifo_ready_dri(ctx);

		spi_status.v &= ~INT_FIFO_READY;
	}

	/*
	 * Check if there are unexpected interrupts, lowest 23 bits needs to be
	 * cleared
	 */
	if ((spi_status.v & CLEAR_LOW_23_BITS) != 0)
		pr_err("Unexpected interrupt received, spi_status=0x%X\n",
		       spi_status.v & CLEAR_LOW_23_BITS);

	/* Notify FW we are done with interrupt handling */
	rc = wigig_sensing_deassert_dri(ctx, additional_inb_command);
	if (rc)
		pr_err("wigig_sensing_deassert_dri() failed, rc=%d\n",
		       rc);

bail_out:
	mutex_unlock(&ctx->dri_lock);
	return IRQ_HANDLED;
}

static irqreturn_t wigig_sensing_dri_isr_hard(int irq, void *cookie)
{
	return IRQ_WAKE_THREAD;
}

static int wigig_sensing_probe(struct spi_device *spi)
{
	int rc = 0;
	struct wigig_sensing_platform_data pdata;

	/* Allocate driver context */
	ctx = devm_kzalloc(&spi->dev, sizeof(struct wigig_sensing_ctx),
			   GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	/* Initialize driver context */
	spi_set_drvdata(spi, ctx);
	ctx->spi_dev = spi;
	ctx->opened = false;
	mutex_init(&ctx->ioctl_lock);
	mutex_init(&ctx->file_lock);
	mutex_init(&ctx->spi_lock);
	mutex_init(&ctx->dri_lock);
	init_waitqueue_head(&ctx->cmd_wait_q);
	init_waitqueue_head(&ctx->data_wait_q);
	ctx->stm.state = WIGIG_SENSING_STATE_INITIALIZED;

	/* Allocate memory for the CIRs */
	/* Allocate a 2MB == 2^21 buffer for CIR data */
	rc = wigig_sensing_alloc_buffer(ctx, 1 << 21);
	if (rc) {
		pr_err("wigig_sensing_alloc_buffer() failed (%d)\n", rc);
		return rc;
	}

	/* Create device node */
	rc = alloc_chrdev_region(&ctx->wigig_sensing_dev, 0, 1, DRV_NAME);
	if (rc < 0) {
		pr_err("alloc_chrdev_region() failed\n");
		return rc;
	}

	ctx->class = class_create(THIS_MODULE, DRV_NAME);
	if (IS_ERR(ctx->class)) {
		rc = PTR_ERR(ctx->class);
		pr_err("Error creating ctx->class: %d\n", rc);
		goto fail_class_create;
	}

	ctx->dev = device_create(ctx->class, NULL, ctx->wigig_sensing_dev, ctx,
				 DRV_NAME);
	if (IS_ERR(ctx->dev)) {
		rc = PTR_ERR(ctx->dev);
		pr_err("device_create failed: %d\n", rc);
		goto fail_device_create;
	}

	cdev_init(&ctx->cdev, &wigig_sensing_fops);
	ctx->cdev.owner = THIS_MODULE;
	ctx->cdev.ops = &wigig_sensing_fops;
	rc = cdev_add(&ctx->cdev, ctx->wigig_sensing_dev, 1);
	if (rc) {
		pr_err("Error calling cdev_add: %d\n", rc);
		goto fail_cdev_add;
	}

	/* Setup DRI - interrupt */
	pdata.dri_gpio = devm_gpiod_get(&spi->dev, "dri", GPIOD_IN);
	if (IS_ERR(pdata.dri_gpio)) {
		pr_err("Could not find dri gpio\n");
		rc = -EFAULT;
		goto fail_gpiod_get;
	}
	ctx->dri_gpio = pdata.dri_gpio;
	ctx->dri_irq = gpiod_to_irq(ctx->dri_gpio);
	pr_debug("dri_irq = %d\n", ctx->dri_irq);
	rc = devm_request_threaded_irq(&spi->dev, ctx->dri_irq,
		wigig_sensing_dri_isr_hard, wigig_sensing_dri_isr_thread,
		IRQF_TRIGGER_RISING, "wigig_sensing_dri", ctx);
	if (rc) {
		pr_err("devm_request_threaded_irq() failed (%d)\n", rc);
		goto fail_gpiod_get;
	}

	return 0;

fail_gpiod_get:
	cdev_del(&ctx->cdev);
fail_cdev_add:
	device_destroy(ctx->class, ctx->wigig_sensing_dev);
fail_device_create:
	class_destroy(ctx->class);
fail_class_create:
	unregister_chrdev_region(ctx->wigig_sensing_dev, 1);

	return rc;
}

static int wigig_sensing_remove(struct spi_device *spi)
{
	struct wigig_sensing_ctx *ctx = spi_get_drvdata(spi);
	struct wigig_sensing_change_mode req = {
		.mode = WIGIG_SENSING_MODE_STOP,
		.has_channel = false,
		.channel = 0,
	};

	/* Make sure that FW is in STOP mode */
	wigig_sensing_ioc_change_mode(ctx, req);

	device_destroy(ctx->class, ctx->wigig_sensing_dev);
	unregister_chrdev_region(ctx->wigig_sensing_dev, 1);
	class_destroy(ctx->class);
	cdev_del(&ctx->cdev);
	vfree(ctx->cir_data.b.buf);
	spi_set_drvdata(ctx->spi_dev, NULL);

	return 0;
}

static const struct of_device_id wigig_sensing_match_table[] = {
	{ .compatible = DRV_NAME },
	{}
};

static struct spi_driver radar_spi_driver = {
	.driver = {
		.name = DRV_NAME,
		.of_match_table = of_match_ptr(wigig_sensing_match_table),
	},
	.probe = wigig_sensing_probe,
	.remove = wigig_sensing_remove,
};

module_spi_driver(radar_spi_driver);

MODULE_DESCRIPTION("Wigig sensing slave SPI Driver");
MODULE_LICENSE("GPL v2");

