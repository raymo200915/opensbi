/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * "virtual wired IRQ" support for couriering claimed wired interrupts
 * to S-Mode domains.
 *
 * Step 2.1 implementation:
 * - per-domain pending hwirq queue
 * - courier handler: enqueue + raise SSIP on current hart
 * - optional ecall interface for bring-up (see sbi_ecall_virq.c)
 *
 * Copyright (c) 2026 RISCstar Solutions.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#ifndef __SBI_VIRQ_H__
#define __SBI_VIRQ_H__

#include <sbi/sbi_domain.h>
#include <sbi/sbi_types.h>

//#define SBI_VIRQ_SSE_EVENT_ID   0

/* Enqueue a claimed wired hwirq for a domain. Returns SBI_OK or error. */
int sbi_virq_enqueue(struct sbi_domain *dom, u32 hwirq);
/*
 * Complete a previously couried hwirq for the current domain.
 *
 * This will unmask the wired hwirq at the active INTC provider, allowing
 * further interrupts once S-mode has cleared the device interrupt source.
 */
void sbi_virq_complete_thishart(u32 hwirq);

/* Pop next pending hwirq for current domain on this hart. Returns 0 if none. */
u32 sbi_virq_pop_thishart(void);

/*
 * Courier handler for wired INTC dispatch.
 *
 * Intended usage:
 *   sbi_intc_set_handler(hwirq, sbi_virq_courier_handler, dom);
 *
 * It will enqueue (hwirq) for the provided domain and raise SSIP on the
 * current hart to notify S-mode.
 */
int sbi_virq_courier_handler(u32 hwirq, void *priv);

/* Convenience bind helper: register courier handler for a given hwirq. */
int sbi_virq_bind_hwirq_to_domain(u32 hwirq, struct sbi_domain *dom);

/* Initialize per-domain virq state (alloc + lock init). */
int sbi_virq_domain_init(struct sbi_domain *dom);

/* Free per-domain virq state. Optional for now. */
void sbi_virq_domain_exit(struct sbi_domain *dom);

#endif
