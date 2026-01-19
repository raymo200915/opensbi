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
#include <sbi/sbi_intc.h>
#include <sbi/sbi_hartmask.h>
#include <sbi/sbi_platform.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_sse.h>
#include <sbi/sbi_virq.h>
#include <sbi/riscv_asm.h>
#include <sbi/riscv_locks.h>

/*
 * Keep the queue small for bring-up. If it overflows, we drop and warn.
 * Later we can replace with a bitmap, per-hwirq pending state, etc.
 */
#define SBI_VIRQ_QSIZE  32

struct sbi_domain_virq_state {
	spinlock_t lock;
	u32 head;
	u32 tail;
	u32 q[SBI_VIRQ_QSIZE];
};

#ifdef SINGLE_QUEUE_PER_DOMAIN

static inline struct sbi_domain_virq_state *domain_virq(struct sbi_domain *dom)
{
	return (struct sbi_domain_virq_state *)dom->virq_priv;
}

#else /* per-(domain,hart) */

struct sbi_domain_virq_priv {
	u32 nharts;
	struct sbi_domain_virq_state st[];
};

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

int sbi_virq_enqueue(struct sbi_domain *dom, u32 hwirq)
{
	struct sbi_domain_virq_state *st;

	if (!dom || !hwirq)
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
		sbi_printf("[VIRQ] drop hwirq %u (queue full)\n", hwirq);
		return SBI_ENOMEM;
	}
	st->q[st->tail] = hwirq;
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
	u32 hwirq = 0;

	if (!st)
		return 0;

	spin_lock(&st->lock);
	if (!q_empty(st)) {
		hwirq = st->q[st->head];
		st->head = (st->head + 1) % SBI_VIRQ_QSIZE;
		sbi_printf("[VIRQ] Get hwirq %d\n", hwirq);
	} else {
		sbi_printf("[VIRQ] IRQ queue is empty\n");
	}
	spin_unlock(&st->lock);

	return hwirq;
}

void sbi_virq_complete_thishart(u32 hwirq)
{
	/* Unmask the wired source now that S-mode has cleared the device */
	sbi_intc_unmask_irq(hwirq);
	sbi_printf("[VIRQ] Unmask hwirq %d\n", hwirq);
}

int sbi_virq_courier_handler(u32 hwirq, void *priv)
{
	struct sbi_domain *dom = (struct sbi_domain *)priv;
	int rc;

	/* Mask to avoid level-trigger storm before S-mode clears device */
	sbi_intc_mask_irq(hwirq);
	sbi_printf("[VIRQ] Mask hwirq %d before S-domain done\n", hwirq);

	rc = sbi_virq_enqueue(dom, hwirq);
	if (!rc) {
		notify_smode_thishart();
		sbi_printf("[VIRQ] SSE injected\n");
	} else {
		/* enqueue failed; re-enable to avoid deadlock */
		sbi_intc_unmask_irq(hwirq);
	}

	return rc;
}

int sbi_virq_bind_hwirq_to_domain(u32 hwirq, struct sbi_domain *dom)
{
	return sbi_intc_set_handler(hwirq, sbi_virq_courier_handler, dom);
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
