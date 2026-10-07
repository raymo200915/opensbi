// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2026 RISCstar Solutions.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#include <sbi/sbi_byteorder.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_mpxy.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_string.h>
#include <sbi_utils/mailbox/rpmi_msgprot.h>
#include <sbi_utils/sysirq/rpmi_sysirq.h>

#define SYSIRQ_BATCH_MAX 63U
#define SYSIRQ_MSG_MAX_LEN ((SYSIRQ_BATCH_MAX + 2) * sizeof(u32))
#define SYSIRQ_ATTR_RESPONSE_LEN (8 * sizeof(u32))

struct rpmi_sysirq_target {
	u32 count;
	u32 claimed[SYSIRQ_BATCH_MAX];
};

struct rpmi_sysirq_channel {
	struct sbi_dlist node;
	struct sbi_mpxy_channel channel;
	const struct rpmi_sysirq_ops *ops;
	u32 num_irqs;
	struct rpmi_sysirq_target targets[];
};

static SBI_LIST_HEAD(sysirq_channels);

static int sysirq_complete_batch(struct rpmi_sysirq_channel *sc,
				 struct rpmi_sysirq_target *target,
				 u32 *completed)
{
	u32 irq;
	int rc;

	while (target->count) {
		irq = target->claimed[target->count - 1];
		rc = sc->ops->complete(sc->channel.channel_id, irq);
		if (rc)
			return rc;
		target->count--;
		(*completed)++;
	}
	return SBI_OK;
}

bool rpmi_sysirq_available(void)
{
	return !sbi_list_empty(&sysirq_channels);
}

int rpmi_sysirq_complete_thishart(u32 *completed)
{
	struct sbi_domain *dom = sbi_domain_thishart_ptr();
	struct rpmi_sysirq_channel *sc;
	u32 index = 0, hidx;
	int rc;

	if (!dom || !completed)
		return SBI_EINVAL;
	*completed = 0;
	sbi_hartmask_for_each_hartindex(hidx, dom->possible_harts) {
		if (hidx == current_hartindex())
			break;
		index++;
	}
	if (index == sbi_hartmask_weight(dom->possible_harts))
		return SBI_EINVAL;

	sbi_list_for_each_entry(sc, &sysirq_channels, node) {
		if (sc->channel.owner_domain != dom)
			continue;
		rc = sysirq_complete_batch(sc, &sc->targets[index], completed);
		if (rc)
			return rc;
	}

	/* Only consider return once every owned channel completed successfully. */
	sbi_list_for_each_entry(sc, &sysirq_channels, node) {
		if (sc->channel.owner_domain == dom && sc->ops->drained)
			sc->ops->drained();
	}
	return SBI_OK;
}

static int sysirq_read_attributes(struct sbi_mpxy_channel *channel,
				  u32 *out, u32 base, u32 count)
{
	u32 attrs[] = { RPMI_SRVGRP_SYSTEM_IRQ, RPMI_VERSION(2, 0), 0, 0 };
	u32 index, i;

	if (base < SBI_MPXY_ATTR_MSGPROTO_ATTR_START)
		return SBI_EBAD_RANGE;
	index = base - SBI_MPXY_ATTR_MSGPROTO_ATTR_START;
	if (index >= array_size(attrs) || count > array_size(attrs) - index)
		return SBI_EBAD_RANGE;

	for (i = 0; i < count; i++)
		out[i] = cpu_to_le32(attrs[index + i]);

	return SBI_OK;
}

static u32 sysirq_target_hartindex(struct sbi_domain *dom, u32 target)
{
	u32 index = 0, hidx;

	sbi_hartmask_for_each_hartindex(hidx, dom->possible_harts) {
		if (index++ == target)
			return hidx;
	}

	return -1U;
}

static int sysirq_send_message(struct sbi_mpxy_channel *channel,
			       u32 msg_id, void *msg, u32 msg_len,
			       void *response, u32 response_max_len,
			       unsigned long *response_len)
{
	struct rpmi_sysirq_channel *sc =
		container_of(channel, struct rpmi_sysirq_channel, channel);
	struct sbi_domain *dom = channel->owner_domain;
	struct rpmi_sysirq_target *target;
	u32 *out = response;
	u32 index = 0, hartindex, count, max_count, i, completed = 0;
	s32 status = RPMI_SUCCESS;
	int rc;

	if (!response || !response_len || response_max_len < sizeof(u32))
		return SBI_EINVAL;
	*response_len = sizeof(u32);
	if (sbi_domain_thishart_ptr() != dom) {
		status = RPMI_ERR_DENIED;
		goto done;
	}

	switch (msg_id) {
	case RPMI_SYSIRQ_SRV_GET_ATTRIBUTES:
		if (msg_len) {
			status = RPMI_ERR_INVALID_PARAM;
			break;
		}
		if (response_max_len < SYSIRQ_ATTR_RESPONSE_LEN)
			return SBI_EINVAL;
		sbi_memset(out, 0, SYSIRQ_ATTR_RESPONSE_LEN);
		out[1] = cpu_to_le32(sbi_hartmask_weight(dom->possible_harts));
		out[2] = cpu_to_le32(sc->num_irqs);
		*response_len = SYSIRQ_ATTR_RESPONSE_LEN;
		break;
	case RPMI_SYSIRQ_SRV_GET_TARGET_ATTRIBUTES:
	case RPMI_SYSIRQ_SRV_GET_IRQ_ATTRIBUTES:
	case RPMI_SYSIRQ_SRV_CLAIM:
		if (!msg || msg_len != sizeof(u32)) {
			status = RPMI_ERR_INVALID_PARAM;
			break;
		}
		/* MPXY uses the same shared buffer for request and response. */
		index = le32_to_cpu(*(u32 *)msg);
		if (msg_id == RPMI_SYSIRQ_SRV_GET_IRQ_ATTRIBUTES) {
			if (index >= sc->num_irqs) {
				status = RPMI_ERR_INVALID_PARAM;
				break;
			}
			if (response_max_len < SYSIRQ_ATTR_RESPONSE_LEN)
				return SBI_EINVAL;
			sbi_memset(out, 0, SYSIRQ_ATTR_RESPONSE_LEN);
			sbi_snprintf((char *)&out[3], 20, "virq%u", index);
			*response_len = SYSIRQ_ATTR_RESPONSE_LEN;
			break;
		}
		hartindex = sysirq_target_hartindex(dom, index);
		if (!sbi_hartindex_valid(hartindex)) {
			status = RPMI_ERR_INVALID_PARAM;
			break;
		}
		if (msg_id == RPMI_SYSIRQ_SRV_GET_TARGET_ATTRIBUTES) {
			if (response_max_len < SYSIRQ_ATTR_RESPONSE_LEN)
				return SBI_EINVAL;
			sbi_memset(out, 0, SYSIRQ_ATTR_RESPONSE_LEN);
			out[2] = cpu_to_le32(sbi_hartindex_to_hartid(hartindex));
			sbi_snprintf((char *)&out[3], 20, "hart%u",
				     sbi_hartindex_to_hartid(hartindex));
			*response_len = SYSIRQ_ATTR_RESPONSE_LEN;
			break;
		}
		if (hartindex != current_hartindex()) {
			status = RPMI_ERR_DENIED;
			break;
		}
		if (response_max_len > channel->attrs.msg_data_maxlen)
			response_max_len = channel->attrs.msg_data_maxlen;
		if (response_max_len < 3 * sizeof(u32))
			return SBI_EINVAL;
		max_count = response_max_len / sizeof(u32) - 2;
		target = &sc->targets[index];
		if (max_count > array_size(target->claimed))
			max_count = array_size(target->claimed);
		/* A new CLAIM follows device handling of the previous batch. */
		rc = sysirq_complete_batch(sc, target, &completed);
		if (rc) {
			status = RPMI_ERR_FAILED;
			goto done;
		}
		rc = sc->ops->claim(channel->channel_id, target->claimed,
				    max_count, &count);
		if (rc) {
			status = RPMI_ERR_FAILED;
			break;
		}
		target->count = count;
		out[1] = cpu_to_le32(count);
		for (i = 0; i < count; i++)
			out[i + 2] = cpu_to_le32(target->claimed[i]);
		*response_len = (count + 2) * sizeof(u32);
		break;
	default:
		status = RPMI_ERR_NOTSUPP;
		break;
	}

done:
	out[0] = cpu_to_le32((u32)status);
	return SBI_OK;
}

int rpmi_sysirq_register_channel(struct sbi_domain *dom, u32 channel_id,
				u32 num_irqs, const struct rpmi_sysirq_ops *ops)
{
	struct rpmi_sysirq_channel *sc;
	int rc;

	if (!dom || !dom->possible_harts || !num_irqs || !ops ||
	    !ops->claim || !ops->complete)
		return SBI_EINVAL;

	sc = sbi_zalloc(sizeof(*sc) +
			sbi_hartmask_weight(dom->possible_harts) * sizeof(sc->targets[0]));
	if (!sc)
		return SBI_ENOMEM;
	sc->ops = ops;
	sc->num_irqs = num_irqs;
	sc->channel.channel_id = channel_id;
	sc->channel.owner_domain = dom;
	sc->channel.attrs.msg_proto_id = SBI_MPXY_MSGPROTO_RPMI_ID;
	sc->channel.attrs.msg_proto_version = RPMI_VERSION(2, 0);
	sc->channel.attrs.msg_data_maxlen = SYSIRQ_MSG_MAX_LEN;
	sc->channel.read_attributes = sysirq_read_attributes;
	sc->channel.send_message_with_response = sysirq_send_message;
	rc = sbi_mpxy_register_channel(&sc->channel);
	if (rc)
		sbi_free(sc);
	else
		sbi_list_add_tail(&sc->node, &sysirq_channels);

	return rc;
}
