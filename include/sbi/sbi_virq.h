/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#ifndef __SBI_VIRQ_H__
#define __SBI_VIRQ_H__

#include <sbi/sbi_domain.h>
#include <sbi/sbi_types.h>

/*
 * Keep the queue small for bring-up. If it overflows, we drop and warn.
 */
#define SBI_VIRQ_QSIZE  32

/* per-(domain,hart) IRQ state */
struct sbi_domain_virq_state {
	spinlock_t lock;
	u32 head;
	u32 tail;
	/* pending IRQ queue */
	u32 q[SBI_VIRQ_QSIZE];
};

/* Per-domain VIRQ context */
struct sbi_domain_virq_priv {
	/* harts number of the domain */
	u32 nharts;
	/* IRQ states of all harts of the domain */
	struct sbi_domain_virq_state st[];
};

struct sbi_virq_courier_priv {
	struct sbi_domain *dom;
	u32 virq;
};

/* Enqueue an interrupt (as seen by the generic irqchip layer) for a domain. */
int sbi_virq_enqueue(struct sbi_domain *dom, u32 irq);

/*
 * Complete a previously couriered irq for the current domain.
 *
 * This will unmask the interrupt line at the active irqchip provider, allowing
 * further interrupts once S-mode has cleared the device interrupt source.
 */
void sbi_virq_complete_thishart(u32 irq);

/* Pop next pending irq for current domain on this hart. Returns 0 if none. */
u32 sbi_virq_pop_thishart(void);

/*
 * Courier handler for wired irqchip dispatch.
 *
 * Intended usage:
 *   sbi_irqchip_set_handler(hwirq, sbi_virq_courier_handler, dom);
 *
 * It will enqueue (irq) for the provided domain and inject SSE on the
 * current hart to notify S-mode.
 */
int sbi_virq_courier_handler(void *priv);

/*
 * Bind helper function: bind a given irq and register the courier handler
 * for a domain.
 */
int sbi_virq_bind_irq_to_domain(u32 irq, struct sbi_domain *dom);

/* Initialize per-domain virq state (alloc + lock init). */
int sbi_virq_domain_init(struct sbi_domain *dom);


/*
 * Optional: map a virtual IRQ number (virq) to a hardware wired IRQ (hwirq).
 *
 * If no explicit mapping exists, 'virq==hwirq' is assumed.
 *
 * This allows upper layers (e.g. VIRQ courier/emulation) to use stable virq
 * identifiers without exposing the wired controller's hwirq numbering.
 */
int sbi_virq_map_irq(u32 virq, u32 hwirq);
int sbi_virq_unmap_irq(u32 virq);
u32 sbi_virq_irq_to_hwirq(u32 virq);
u32 sbi_virq_hwirq_to_irq(u32 hwirq);

#endif
