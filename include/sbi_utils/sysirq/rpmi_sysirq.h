/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#ifndef __RPMI_SYSIRQ_H__
#define __RPMI_SYSIRQ_H__

#include <sbi/sbi_types.h>

struct sbi_domain;

/* Callbacks operate on the calling domain and hart; ops must remain valid. */
struct rpmi_sysirq_ops {
	int (*claim)(u32 channel_id, u32 *irqs, u32 max_count, u32 *count);
	int (*complete)(u32 channel_id, u32 irq);
	void (*drained)(void);
};

int rpmi_sysirq_register_channel(struct sbi_domain *dom, u32 channel_id,
				u32 num_irqs, const struct rpmi_sysirq_ops *ops);

bool rpmi_sysirq_available(void);

/* Complete this domain/hart's batches, then ask backends to consider return. */
int rpmi_sysirq_complete_thishart(u32 *completed);

#endif
