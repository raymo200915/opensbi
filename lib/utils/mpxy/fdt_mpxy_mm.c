/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 Andes Technology Corporation. All rights reserved.
 */

#include <sbi/sbi_error.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_fifo.h>
#include <sbi/sbi_mpxy.h>
#include <libfdt.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi_utils/mpxy/fdt_mpxy.h>
#include <sbi_utils/mpxy/fdt_mpxy_rpmi_mbox.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_console.h>

/**
 * MPXY MM instance per MPXY channel.
 */
struct mpxy_mm {
	struct mpxy_rpmi_channel_attrs msgprot_attrs;
	struct sbi_mpxy_channel channel;

	/* Owner Hart ID of this channel */
	u32 hartid;
	/* Corresponding Reqfwd channel id */
	u32 reqfwd_channel_id;

	/* TEE domain */
	char mm_domain_name[64];
	struct sbi_domain *mm_domain;
};

/*
 * Setup target domain of this MM channel.
 * If the target domain has not been registered, the domain will be setup in
 * mm_domain_setup_deferred() when MM channel is used.
 */
static int mm_domain_setup(const void *fdt, int nodeoff,
			   const struct fdt_match *match,
			   struct mpxy_mm *mm)
{
	struct sbi_domain *dom = NULL;
	const u32 *prop_instance;
	int len, offset;

	prop_instance = fdt_getprop(fdt, nodeoff, "opensbi-domain-instance",
				    &len);
	if (!prop_instance || len < 4)
		return SBI_EINVAL;

	offset = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(*prop_instance));
	if (offset < 0)
		return SBI_EINVAL;

	strncpy(mm->mm_domain_name, fdt_get_name(fdt, offset, NULL),
		sizeof(mm->mm_domain_name));
	mm->mm_domain_name[sizeof(mm->mm_domain_name) - 1] = '\0';

	sbi_domain_for_each(dom) {
		if (!sbi_strcmp(dom->name, mm->mm_domain_name)) {
			mm->mm_domain = dom;
			break;
		}
	}

	return SBI_OK;
}

/* Setup target domain of this MM channel */
static int mm_domain_setup_deferred(struct mpxy_mm *mm)
{
	struct sbi_domain *dom = NULL;

	if (mm->mm_domain)
		 return SBI_OK;

	sbi_domain_for_each(dom) {
		if (!sbi_strcmp(dom->name, mm->mm_domain_name)) {
			mm->mm_domain = dom;
			return SBI_OK;
		}
	}

	return SBI_ENOENT;
}

/* Switch to target domain of this MM channel */
static int mm_domain_enter(struct mpxy_mm *mm)
{
	int rc;

	/* Try to setup MM domain if it has not been set */
	rc = mm_domain_setup_deferred(mm);
	if (rc)
		return rc;

	return sbi_domain_context_enter(mm->mm_domain);
}

/** Copy attributes word size */
static void mpxy_copy_attrs(u32 *outmem, u32 *inmem, u32 count)
{
	u32 idx;
	for (idx = 0; idx < count; idx++)
		outmem[idx] = cpu_to_le32(inmem[idx]);
}

static int mpxy_mm_read_attributes(struct sbi_mpxy_channel *channel,
				   u32 *outmem, u32 base_attr_id,
				   u32 attr_count)
{
	struct mpxy_mm *mm = container_of(channel, struct mpxy_mm, channel);
	u32 *attr_array = (u32 *)&mm->msgprot_attrs;
	u32 end_id = base_attr_id + attr_count - 1;

	if (end_id >= MPXY_MSGPROT_RPMI_ATTR_MAX_ID)
		return SBI_EBAD_RANGE;

	mpxy_copy_attrs(outmem, &attr_array[attr_id2index(base_attr_id)],
			attr_count);

	return SBI_OK;
}

static int mpxy_mm_send_message_withresp(struct sbi_mpxy_channel *channel,
					 u32 message_id,
					 void *tx, u32 tx_len,
					 void *rx, u32 rx_max_len,
					 unsigned long *ack_len)
{
	struct mpxy_mm *mm = container_of(channel, struct mpxy_mm, channel);
	struct sbi_mpxy_channel *recv_channel;
	struct rpmi_message_header header;
	u32 recv_channel_id = mm->reqfwd_channel_id;
	int rc;

	if (RPMI_MM_SRV_COMMUNICATE == message_id) {
		recv_channel = sbi_mpxy_find_channel(recv_channel_id);
		if (!recv_channel)
			return SBI_ENODEV;

		/* Prepare the header to be written into the slot */
		header.servicegroup_id = cpu_to_le16(RPMI_SRVGRP_MM);
		header.service_id = message_id;
		header.flags = RPMI_MSG_NORMAL_REQUEST;
		header.datalen = cpu_to_le16((u16)tx_len);
		header.token = cpu_to_le16(0);
		/* Forward message to MPXT ReqFwd channel */
		rc = mpxy_reqfwd_forward_message(recv_channel, &header,
						 tx, tx_len, rx, rx_max_len,
						 ack_len);
		if (rc)
			return rc;

		/* Enter MM domain */
		rc = mm_domain_enter(mm);
		if (rc)
			return rc;
	} else {
		return SBI_EFAIL;
	}

	return SBI_OK;
}

static int mpxy_mm_init(const void *fdt, int nodeoff,
			const struct fdt_match *match)
{
	struct mpxy_mm *mm;
	const fdt32_t *val;
	u32 channel_id, reqfwd_channel_id, hartid;
	int rc, len;

	/* Allocate context for Request Forward */
	mm = sbi_zalloc(sizeof(*mm));
	if (!mm)
		return SBI_ENOMEM;

	val = fdt_getprop(fdt, nodeoff, "riscv,sbi-mpxy-channel-id", &len);
	if (len > 0 && val)
		channel_id = fdt32_to_cpu(*val);
	else
		sbi_panic("Failed to get riscv,sbi-mpxy-channel-id");

	val = fdt_getprop(fdt, nodeoff, "test,owner-hartid", &len);
	if (len > 0 && val)
		hartid = fdt32_to_cpu(*val);
	else
		sbi_panic("Failed to get test,owner-hartid");

	val = fdt_getprop(fdt, nodeoff, "test,reqfwd-channel-id", &len);
	if (len > 0 && val)
		reqfwd_channel_id = fdt32_to_cpu(*val);
	else
		sbi_panic("Failed to get test,reqfwd-channel-id");

	mm->hartid = hartid;
	mm->reqfwd_channel_id = reqfwd_channel_id;
	mm->channel.channel_id = channel_id;
	mm->channel.read_attributes = mpxy_mm_read_attributes;
	mm->channel.send_message_with_response = mpxy_mm_send_message_withresp;
	mm->channel.attrs.msg_proto_id = SBI_MPXY_MSGPROTO_RPMI_ID;
	mm->channel.attrs.msg_data_maxlen = PAGE_SIZE;

	/* RPMI service group attributes */
	mm->msgprot_attrs.servicegrp_id = RPMI_SRVGRP_MM;
	mm->msgprot_attrs.servicegrp_ver = 1;

	rc = sbi_mpxy_register_channel(&mm->channel);
	if (rc)
		goto fail_free_mm;

	/* Try to setup MM domain */
	rc = mm_domain_setup(fdt, nodeoff, match, mm);
	if (rc)
		goto fail_free_mm;

	return SBI_SUCCESS;

fail_free_mm:
	sbi_free(mm);
	return rc;
}

static const struct fdt_match mpxy_mm_match[] = {
	{ .compatible = "riscv,sbi-mpxy-mm", .data = NULL },
	{},
};

const struct fdt_driver fdt_mpxy_mm = {
	.match_table = mpxy_mm_match,
	.init = mpxy_mm_init,
	.experimental = true,
};
