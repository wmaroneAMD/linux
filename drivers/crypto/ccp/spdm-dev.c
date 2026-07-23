// SPDX-License-Identifier: GPL-2.0-only
/*
 * AMD Platform Security Processor (PSP) SPDM backend.
 *
 * Registers a per-PSP-device SPDM node with the vendor-independent SPDM core
 * and relays SPDM messages to the responder via the PSP extended mailbox.
 * Each open() is a session with its own working page; write() issues the SPDM
 * request synchronously and read() returns the buffered response.
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */

#include <linux/cleanup.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/psp.h>
#include <linux/slab.h>
#include <linux/spdm.h>
#include <linux/uaccess.h>

#include "psp-dev.h"
#include "spdm-dev.h"

#define SPDM_CMD_TIMEOUT_MS	(2 * MSEC_PER_SEC)

static int ccp_spdm_open(struct spdm_device *sdev, struct file *file)
{
	struct spdm_session *sess __free(kfree) = NULL;

	/*
	 * The core hands out a fresh file per open() and clears the backend
	 * pointer on release(), so a live session here means the core handed us
	 * the same file twice. Refuse rather than leak the previous session.
	 */
	if (spdm_get_filedata(file))
		return -EINVAL;

	sess = kzalloc_obj(*sess);
	if (!sess)
		return -ENOMEM;

	sess->page = (void *)__get_free_page(GFP_KERNEL);
	if (!sess->page)
		return -ENOMEM;

	spdm_set_filedata(file, no_free_ptr(sess));
	return 0;
}

static int ccp_spdm_release(struct spdm_device *sdev, struct file *file)
{
	struct spdm_session *sess = spdm_get_filedata(file);

	if (sess) {
		free_page((unsigned long)sess->page);
		kfree(sess);
		spdm_set_filedata(file, NULL);
	}
	return 0;
}

static ssize_t ccp_spdm_write(struct spdm_device *sdev, struct file *file,
			      u8 session_id, const char __user *buf,
			      size_t count)
{
	struct spdm_command *cmd __free(kfree) = NULL;
	struct spdm_dev *spdm_dev = spdm_drvdata(sdev);
	struct spdm_session *sess = spdm_get_filedata(file);
	phys_addr_t page_pa;
	int ret;

	if (count == 0 || count > SPDM_REGION_SIZE)
		return -EINVAL;

	cmd = kzalloc_obj(*cmd);
	if (!cmd)
		return -ENOMEM;

	/* Fresh request each time; clear both regions of the working page. */
	memset(sess->page, 0, PAGE_SIZE);
	if (copy_from_user(sess->page + SPDM_REQ_OFFSET, buf, count))
		return -EFAULT;
	sess->response_size = 0;

	page_pa = __psp_pa(sess->page);

	cmd->hdr.sub_cmd_id  = TEE_SUB_CMD_SPDM_MESSAGE;
	cmd->hdr.payload_size = sizeof(*cmd);
	cmd->hdr.status      = 0;
	cmd->msg.request_addr_lo  = lower_32_bits(page_pa + SPDM_REQ_OFFSET);
	cmd->msg.request_addr_hi  = upper_32_bits(page_pa + SPDM_REQ_OFFSET);
	cmd->msg.request_size     = count;
	cmd->msg.response_addr_lo = lower_32_bits(page_pa + SPDM_RESP_OFFSET);
	cmd->msg.response_addr_hi = upper_32_bits(page_pa + SPDM_RESP_OFFSET);
	cmd->msg.response_size    = 0;
	cmd->msg.buffer_size      = SPDM_REGION_SIZE;

	/* Make the request visible to the PSP before issuing the command. */
	dma_wmb();

	ret = psp_extended_mailbox_cmd(spdm_dev->psp, SPDM_CMD_TIMEOUT_MS,
				       (struct psp_ext_request *)cmd);
	if (ret) {
		dev_dbg(spdm_dev->dev,
			"SPDM message failed: ret=%d status=0x%x\n",
			ret, cmd->hdr.status);
		return ret;
	}

	/* Ensure we observe the PSP-written response. */
	dma_rmb();

	sess->response_size = min_t(u32, cmd->msg.response_size, SPDM_REGION_SIZE);

	return count;
}

static ssize_t ccp_spdm_read(struct spdm_device *sdev, struct file *file,
			     u8 session_id, char __user *buf, size_t count)
{
	struct spdm_session *sess = spdm_get_filedata(file);
	size_t len;

	if (sess->response_size == 0)
		return 0;

	len = min_t(size_t, count, sess->response_size);
	if (copy_to_user(buf, sess->page + SPDM_RESP_OFFSET, len))
		return -EFAULT;

	/* Response is consumed once read. */
	sess->response_size = 0;
	return len;
}

static const struct spdm_ops ccp_spdm_ops = {
	.owner			= THIS_MODULE,
	.max_session_count	= 1,
	.open			= ccp_spdm_open,
	.release		= ccp_spdm_release,
	.read			= ccp_spdm_read,
	.write			= ccp_spdm_write,
};

int spdm_dev_init(struct psp_device *psp)
{
	struct device *dev = psp->dev;
	struct spdm_dev *spdm_dev;

	spdm_dev = devm_kzalloc(dev, sizeof(*spdm_dev), GFP_KERNEL);
	if (!spdm_dev)
		return -ENOMEM;

	spdm_dev->dev = dev;
	spdm_dev->psp = psp;

	spdm_dev->sdev = spdm_register(dev, &ccp_spdm_ops, spdm_dev);
	if (IS_ERR(spdm_dev->sdev)) {
		int ret = PTR_ERR(spdm_dev->sdev);

		dev_err(dev, "failed to register SPDM device (%d)\n", ret);
		return ret;
	}

	psp->spdm_data = spdm_dev;
	dev_notice(dev, "SPDM support is available\n");

	return 0;
}

void spdm_dev_destroy(struct psp_device *psp)
{
	struct spdm_dev *spdm_dev = psp->spdm_data;

	if (!spdm_dev)
		return;

	spdm_unregister(spdm_dev->sdev);
	psp->spdm_data = NULL;
}
