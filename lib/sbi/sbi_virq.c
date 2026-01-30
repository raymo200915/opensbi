/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 RISCstar Solutions.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#include <sbi/sbi_console.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_irqchip.h>
#include <sbi/sbi_hartmask.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_sse.h>
#include <sbi/sbi_virq.h>
#include <sbi/riscv_asm.h>
#include <sbi/riscv_locks.h>

#define SBI_VIRQ_MAX_IRQS SBI_IRQCHIP_MAX_HWIRQS
/*
 * Mapping tables:
 *  - irq_to_hwirq[irq] == 0 means identity mapping (hwirq == irq)
 *  - hwirq_to_irq[hwirq] == 0 means identity mapping (irq == hwirq)
 */
static u32 irq_to_hwirq[SBI_VIRQ_MAX_IRQS];
static u32 hwirq_to_irq[SBI_IRQCHIP_MAX_HWIRQS];
static spinlock_t virq_lock = SPIN_LOCK_INITIALIZER;

/* VIRQ and domain binding table */
static struct sbi_virq_courier_priv g_virq_binding[SBI_VIRQ_MAX_IRQS];

static inline u32 __irq_to_hwirq(u32 irq)
{
	u32 hwirq;

	if (!irq || irq >= SBI_VIRQ_MAX_IRQS)
		return 0;

	hwirq = irq_to_hwirq[irq];
	return hwirq ? hwirq : irq;
}

static inline u32 __hwirq_to_irq(u32 hwirq)
{
	u32 irq;

	if (!hwirq || hwirq >= SBI_IRQCHIP_MAX_HWIRQS)
		return 0;

	irq = hwirq_to_irq[hwirq];
	return irq ? irq : hwirq;
}

#ifdef SINGLE_QUEUE_PER_DOMAIN

static inline struct sbi_domain_virq_state *domain_virq(struct sbi_domain *dom)
{
	return (struct sbi_domain_virq_state *)dom->virq_priv;
}

#else /* per-(domain,hart) */

static inline struct sbi_domain_virq_state *domain_virq_thishart(struct sbi_domain *dom)
{
	struct sbi_domain_virq_priv *p = (struct sbi_domain_virq_priv *)dom->virq_priv;
	unsigned long hartidx = sbi_hartid_to_hartindex(current_hartid());

	if (!p || hartidx >= p->nharts)
		return NULL;
	return &p->st[hartidx];
}

#endif

static inline bool q_full(struct sbi_domain_virq_state *st)
{
	return ((st->tail + 1) % SBI_VIRQ_QSIZE) == st->head;
}

static inline bool q_empty(struct sbi_domain_virq_state *st)
{
	return st->head == st->tail;
}

static void notify_smode_thishart(void)
{
	int ret;
	u64 event = SBI_SSE_EVENT_LOCAL_SOFTWARE;

	ret = sbi_sse_inject_event(event);
	sbi_printf("[VIRQ] Inject SSE event 0x%lx, ret:%d\n", event, ret);
}

static u32 sbi_virq_platform_hart_count(void)
{
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
	const struct sbi_platform *plat = sbi_platform_ptr(scratch);

	return sbi_platform_hart_count(plat);
}

int sbi_virq_enqueue(struct sbi_domain *dom, u32 irq)
{
	struct sbi_domain_virq_state *st;

	if (!dom || !irq)
		return SBI_EINVAL;

#ifdef SINGLE_QUEUE_PER_DOMAIN
	st = domain_virq(dom);
#else  /* per-(domain,hart) */
	st = domain_virq_thishart(dom);
#endif

	if (!st)
		return SBI_ENODEV;

	spin_lock(&st->lock);
	if (q_full(st)) {
		spin_unlock(&st->lock);
		sbi_printf("[VIRQ] drop IRQ %u (queue full)\n", irq);
		return SBI_ENOMEM;
	}
	st->q[st->tail] = irq;
	st->tail = (st->tail + 1) % SBI_VIRQ_QSIZE;
	spin_unlock(&st->lock);

	return SBI_OK;
}

u32 sbi_virq_pop_thishart(void)
{
	struct sbi_domain *dom = sbi_domain_thishart_ptr();
#ifdef SINGLE_QUEUE_PER_DOMAIN
	struct sbi_domain_virq_state *st = dom ? domain_virq(dom) : NULL;
#else  /* per-(domain,hart) */
	struct sbi_domain_virq_state *st = dom ? domain_virq_thishart(dom) : NULL;
#endif
	u32 irq = 0;

	if (!st)
		return 0;

	spin_lock(&st->lock);
	if (!q_empty(st)) {
		irq = st->q[st->head];
		st->head = (st->head + 1) % SBI_VIRQ_QSIZE;
		sbi_printf("[VIRQ] Get IRQ %d from queue\n", irq);
	} else {
		sbi_printf("[VIRQ] IRQ queue is empty\n");
	}
	spin_unlock(&st->lock);

	return irq;
}

void sbi_virq_complete_thishart(u32 irq)
{
	sbi_printf("[VIRQ] Complete IRQ %d\n", irq);
	/* Unmask now that S-mode has cleared the device */
	sbi_irqchip_unmask_irq(sbi_virq_irq_to_hwirq(irq));
}

int sbi_virq_courier_handler(void *priv)
{
	struct sbi_virq_courier_priv *courier;
	struct sbi_domain *dom;
	int rc;
	u32 hwirq;

	courier = (struct sbi_virq_courier_priv *)priv;
	dom = courier->dom;

	hwirq = sbi_virq_irq_to_hwirq(courier->virq);
	sbi_printf("[VIRQ] IRQ %d maps to hwirq %d\n", courier->virq, hwirq);
	/* Mask to avoid level-trigger storm before S-mode clears device */
	sbi_irqchip_mask_irq(hwirq);

	rc = sbi_virq_enqueue(dom, courier->virq);
	if (!rc) {
		notify_smode_thishart();
		sbi_printf("[VIRQ] SSE injected\n");
	} else {
		/* enqueue failed; re-enable to avoid deadlock */
		sbi_irqchip_unmask_irq(hwirq);
	}

	return rc;
}

int sbi_virq_bind_irq_to_domain(u32 irq, struct sbi_domain *dom)
{
	u32 hwirq = sbi_virq_irq_to_hwirq(irq);

	g_virq_binding[irq].virq = irq;
	g_virq_binding[irq].dom = dom;

	return sbi_irqchip_set_handler(hwirq, sbi_virq_courier_handler,
				       &g_virq_binding[irq]);
}

int sbi_virq_domain_init(struct sbi_domain *dom)
{
#ifdef SINGLE_QUEUE_PER_DOMAIN
	struct sbi_domain_virq_state *st;
#else  /* per-(domain,hart) */
	struct sbi_domain_virq_priv *p;
	u32 i, nharts;
#endif

	if (!dom)
		return SBI_EINVAL;

	if (dom->virq_priv)
		return SBI_OK;

	sbi_printf("[VIRQ] Init per-domain wired-IRQ courier state\n");
#ifdef SINGLE_QUEUE_PER_DOMAIN
	st = sbi_zalloc(sizeof(*st));
	if (!st)
		return SBI_ENOMEM;

	SPIN_LOCK_INIT(st->lock);
	st->head = 0;
	st->tail = 0;
	dom->virq_priv = st;
#else  /* per-(domain,hart) */
	nharts = sbi_virq_platform_hart_count();
	sbi_printf("[VIRQ] number of harts: %d\n", nharts);

	p = sbi_zalloc(sizeof(*p) + nharts * sizeof(p->st[0]));
	if (!p)
		return SBI_ENOMEM;

	p->nharts = nharts;
	for (i = 0; i < nharts; i++) {
		SPIN_LOCK_INIT(p->st[i].lock);
		p->st[i].head = 0;
		p->st[i].tail = 0;
	}
	dom->virq_priv = p;
#endif

	return SBI_OK;
}

void sbi_virq_domain_exit(struct sbi_domain *dom)
{
	if (!dom || !dom->virq_priv)
		return;

	sbi_free(dom->virq_priv);
	dom->virq_priv = NULL;
}

u32 sbi_virq_irq_to_hwirq(u32 irq)
{
	return __irq_to_hwirq(irq);
}

u32 sbi_virq_hwirq_to_irq(u32 hwirq)
{
	return __hwirq_to_irq(hwirq);
}

int sbi_virq_map_irq(u32 irq, u32 hwirq)
{
	u32 max_hwirq;

	if (!irq || !hwirq ||
	    irq >= SBI_VIRQ_MAX_IRQS || hwirq >= SBI_IRQCHIP_MAX_HWIRQS)
		return SBI_EINVAL;

	spin_lock(&virq_lock);

	max_hwirq = sbi_irqchip_get_max_src();
	if (__irq_to_hwirq(irq) > max_hwirq) {
		spin_unlock(&virq_lock);
		return SBI_EINVAL;
	}

	/* Prevent conflicting mappings */
	if (irq_to_hwirq[irq] || hwirq_to_irq[hwirq]) {
		spin_unlock(&virq_lock);
		return SBI_EALREADY;
	}

	irq_to_hwirq[irq] = hwirq;
	hwirq_to_irq[hwirq] = irq;

	spin_unlock(&virq_lock);
	sbi_printf("[VIRQ] Map hwirq:%u to IRQ:%u\n", hwirq, irq);

	return SBI_OK;
}

int sbi_virq_unmap_irq(u32 irq)
{
	u32 hwirq;

	if (!irq || irq >= SBI_VIRQ_MAX_IRQS)
		return SBI_EINVAL;

	spin_lock(&virq_lock);

	hwirq = irq_to_hwirq[irq];
	if (hwirq) {
		irq_to_hwirq[irq] = 0;
		if (hwirq < SBI_IRQCHIP_MAX_HWIRQS)
			hwirq_to_irq[hwirq] = 0;
	}

	spin_unlock(&virq_lock);
	sbi_printf("[VIRQ] Unmap IRQ:%u from hwirq:%u\n", irq, hwirq);

	return SBI_OK;
}
