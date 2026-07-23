/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AMD Platform Security Processor (PSP) SPDM backend.
 *
 * Registers a per-PSP-device SPDM node with the vendor-independent SPDM core
 * (drivers/char/spdm.c) and services SPDM message round-trips via the PSP
 * extended mailbox.
 *
 * Copyright (C) 2026 Advanced Micro Devices, Inc.
 */

#ifndef __SPDM_DEV_H__
#define __SPDM_DEV_H__

#include <linux/device.h>
#include <linux/types.h>

#include "psp-dev.h"

/* TEE extended sub-command id for SPDM message passthrough (firmware ABI). */
#define TEE_SUB_CMD_SPDM_MESSAGE	0x6

/* Per-session working page layout: upper 2K request, lower 2K response. */
#define SPDM_REQ_OFFSET		0
#define SPDM_RESP_OFFSET	2048
#define SPDM_REGION_SIZE	2048

/**
 * struct tee_ext_spdm_message - SPDM message descriptor (firmware ABI)
 *
 * Mirrors TEE_EXT_SPDM_MESSAGE in the firmware psp_tee_if.h. Field order is
 * ABI: response_size (out) precedes buffer_size (in, response capacity); there
 * is no transport_type field. Request/response payloads are referenced by
 * physical address.
 */
struct tee_ext_spdm_message {
	u32 request_addr_lo;
	u32 request_addr_hi;
	u32 request_size;
	u32 response_addr_lo;
	u32 response_addr_hi;
	u32 response_size;	/* [out] actual response length */
	u32 buffer_size;	/* [in]  response region capacity */
} __packed;

/**
 * struct spdm_command - extended command buffer handed to the PSP mailbox
 * @hdr: standard extended-command header (sub_cmd_id, payload_size, status)
 * @msg: the SPDM message descriptor
 */
struct spdm_command {
	struct psp_ext_req_buffer_hdr hdr;
	struct tee_ext_spdm_message msg;
} __packed;

/**
 * struct spdm_dev - per-PSP-device SPDM backend state
 * @dev:  the PSP device
 * @psp:  owning psp_device (for the mailbox)
 * @sdev: handle returned by spdm_register()
 */
struct spdm_dev {
	struct device *dev;
	struct psp_device *psp;
	struct spdm_device *sdev;
};

/**
 * struct spdm_session - per-open session state (via spdm_set_filedata)
 * @page:          working page: SPDM_REQ_OFFSET request | SPDM_RESP_OFFSET response
 * @response_size: bytes of response available for the next read()
 */
struct spdm_session {
	void *page;
	u32 response_size;
};

int spdm_dev_init(struct psp_device *psp);
void spdm_dev_destroy(struct psp_device *psp);

#endif /* __SPDM_DEV_H__ */
