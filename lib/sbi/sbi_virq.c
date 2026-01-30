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
#include <sbi/sbi_string.h>
#include <sbi/sbi_virq.h>
#include <sbi/riscv_asm.h>
#include <sbi/riscv_locks.h>

struct map_node {
	u32 chip_uid;
	u32 hwirq;
	u32 virq;
};

struct sbi_virq_map {
	spinlock_t lock;

	/* allocator bitmap */
	unsigned long *bmap;
	u32 bmap_nbits; /* virq range: [0..nbits-1], virq 0 reserved */

	/* reverse table: virq -> endpoint */
	struct virq_chunk **chunks;
	u32 chunks_cap; /* number of chunk pointers */

	/* forward table: vector of mappings, linear search */
	struct map_node *nodes;
	u32 nodes_cnt;
	u32 nodes_cap;
};

/*
 * HWIRQ -> Domain routing rules
 */

struct sbi_virq_router {
	spinlock_t lock;
	struct sbi_virq_route_rule *rules;
	u32 cnt;
	u32 cap;
};

static struct sbi_virq_map g_virq_map;
static struct sbi_virq_router g_router;

void sbi_virq_route_reset(void)
{
	spin_lock(&g_router.lock);
	if (g_router.rules) {
		sbi_free(g_router.rules);
		g_router.rules = NULL;
	}
	g_router.cnt = 0;
	g_router.cap = 0;
	spin_unlock(&g_router.lock);
}

static int router_ensure_cap(u32 need)
{
	struct sbi_virq_route_rule *newp;
	u32 newcap;

	if (g_router.cap >= need)
		return 0;

	newcap = g_router.cap ? (g_router.cap << 1) : 8;
	while (newcap < need)
		newcap <<= 1;

	newp = sbi_zalloc((size_t)newcap * sizeof(*newp));
	if (!newp)
		return SBI_ENOMEM;

	if (g_router.rules) {
		sbi_memcpy(newp, g_router.rules,
			   (size_t)g_router.cnt * sizeof(*newp));
		sbi_free(g_router.rules);
	}

	g_router.rules = newp;
	g_router.cap = newcap;

	return SBI_OK;
}

int sbi_virq_route_add_range(struct sbi_domain *dom, u32 first, u32 count,
			     u32 hartid)
{
	u32 last;
	u32 hart_index;
	int rc;

	if (!dom || !count)
		return SBI_EINVAL;

	hart_index = sbi_hartid_to_hartindex(hartid);
	if (!sbi_hartindex_valid(hart_index))
		return SBI_EINVAL;
	if (!sbi_domain_is_assigned_hart(dom, hart_index))
		return SBI_EINVAL;

	last = first + count - 1;
	if (last < first)
		return SBI_EINVAL;

	spin_lock(&g_router.lock);

	/* Reject overlaps to keep routing unambiguous */
	for (u32 i = 0; i < g_router.cnt; i++) {
		u32 a1 = g_router.rules[i].first;
		u32 a2 = g_router.rules[i].last;
		bool overlap = !(last < a1 || first > a2);
		if (overlap) {
			spin_unlock(&g_router.lock);
			sbi_printf("[VIRQ] overlap: new [%u..%u] with " \
				   "existing [%u..%u] dom=%s\n", first, last,
				   a1, a2, g_router.rules[i].dom ?
				   g_router.rules[i].dom->name : "?");
			return SBI_EALREADY;
		}
	}

	rc = router_ensure_cap(g_router.cnt + 1);
	if (rc) {
		spin_unlock(&g_router.lock);
		return rc;
	}

	g_router.rules[g_router.cnt].first = first;
	g_router.rules[g_router.cnt].last  = last;
	g_router.rules[g_router.cnt].hartid = hartid;
	g_router.rules[g_router.cnt].hart_index = hart_index;
	g_router.rules[g_router.cnt].dom   = dom;
	g_router.cnt++;

	spin_unlock(&g_router.lock);

	sbi_printf("[VIRQ] add route rule: hwirq [%u..%u] route to dom (%s) " \
		   "hartid=%u\n", first, last, dom->name, hartid);

	return SBI_OK;
}

int sbi_virq_route_lookup(u32 hwirq, struct sbi_domain **out_dom,
			  u32 *out_hartid, u32 *out_hartindex)
{
	if (!out_dom || !out_hartid || !out_hartindex)
		return SBI_EINVAL;

	/* Fast path: no rules */
	if (!g_router.cnt)
		goto fallback;

	spin_lock(&g_router.lock);
	for (u32 i = 0; i < g_router.cnt; i++) {
		if (hwirq >= g_router.rules[i].first &&
		    hwirq <= g_router.rules[i].last) {
			struct sbi_domain *d = g_router.rules[i].dom;
			*out_dom = d ? d : &root;
			*out_hartid = g_router.rules[i].hartid;
			*out_hartindex = g_router.rules[i].hart_index;
			spin_unlock(&g_router.lock);
			return SBI_OK;
		}
	}
	spin_unlock(&g_router.lock);

fallback:
	*out_dom = &root;
	*out_hartid = current_hartid();
	*out_hartindex = current_hartindex();
	return SBI_OK;
}

static inline
struct sbi_domain_virq_state *domain_virq_thishart(struct sbi_domain *dom,
						   u32 hart_index)
{
	struct sbi_domain_virq_priv *p;

	p = (struct sbi_domain_virq_priv *)dom->virq_priv;
	if (!p || hart_index >= p->nharts)
		return NULL;

	sbi_printf("[VIRQ] Get queue for domain %s, hartidx:%u, hartid:%u\n",
		   dom->name, hart_index,
		   sbi_hartindex_to_hartid(hart_index));

	return &p->st[hart_index];
}

static inline bool q_full(struct sbi_domain_virq_state *st)
{
	return ((st->tail + 1) % VIRQ_QSIZE) == st->head;
}

static inline bool q_empty(struct sbi_domain_virq_state *st)
{
	return st->head == st->tail;
}

static int notify_smode_hartid(u32 hartid)
{
	int ret;
	u64 event = SBI_SSE_EVENT_LOCAL_SOFTWARE;

	ret = sbi_sse_inject_event_to_hart(event, hartid);
	sbi_printf("[VIRQ] Inject SSE event 0x%lx to hartid %u, ret:%d\n",
		   event, hartid, ret);

	return ret;
}

static u32 sbi_virq_platform_hart_count(void)
{
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
	const struct sbi_platform *plat = sbi_platform_ptr(scratch);

	return sbi_platform_hart_count(plat);
}

static int bmap_alloc_one(u32 *out_virq)
{
	u32 v;

	for (v = 1; v < g_virq_map.bmap_nbits; v++) {
		if (!bitmap_test(g_virq_map.bmap, (int)v)) {
			bitmap_set(g_virq_map.bmap, (int)v, 1);
			*out_virq = v;
			return 0;
		}
	}

	return SBI_ENOSPC;
}

static int bmap_alloc_specific(u32 virq)
{
	if (virq == 0 || virq >= g_virq_map.bmap_nbits)
		return SBI_EINVAL;
	if (bitmap_test(g_virq_map.bmap, (int)virq))
		return SBI_EALREADY;
	bitmap_set(g_virq_map.bmap, (int)virq, 1);

	return 0;
}

static void bmap_free_one(u32 virq)
{
	if (virq < g_virq_map.bmap_nbits)
		bitmap_clear(g_virq_map.bmap, (int)virq, 1);
}

static int chunks_ensure_cap(u32 new_bmap_nbits)
{
	u32 new_chunks_cap =
		(new_bmap_nbits + VIRQ_CHUNK_SIZE - 1U) >> VIRQ_CHUNK_SHIFT;
	struct virq_chunk **newp;

	if (new_chunks_cap <= g_virq_map.chunks_cap)
		return 0;

	newp = sbi_zalloc((size_t)new_chunks_cap * sizeof(*newp));
	if (!newp)
		return SBI_ENOMEM;

	if (g_virq_map.chunks) {
		sbi_memcpy(newp, g_virq_map.chunks,
			   (size_t)g_virq_map.chunks_cap * sizeof(*newp));
		sbi_free(g_virq_map.chunks);
	}

	g_virq_map.chunks = newp;
	g_virq_map.chunks_cap = new_chunks_cap;

	return 0;
}

static int bmap_grow(u32 new_nbits)
{
	unsigned long *newmap;

	if (new_nbits <= g_virq_map.bmap_nbits)
		return 0;

	newmap = sbi_zalloc(bitmap_estimate_size((int)new_nbits));
	if (!newmap)
		return SBI_ENOMEM;

	bitmap_zero(newmap, (int)new_nbits);
	bitmap_copy(newmap, g_virq_map.bmap, (int)g_virq_map.bmap_nbits);

	sbi_free(g_virq_map.bmap);
	g_virq_map.bmap = newmap;
	g_virq_map.bmap_nbits = new_nbits;

	return chunks_ensure_cap(new_nbits);
}

static struct virq_entry *rev_get_or_alloc(u32 virq)
{
	u32 ci = virq >> VIRQ_CHUNK_SHIFT;
	u32 off = virq & VIRQ_CHUNK_MASK;

	if (ci >= g_virq_map.chunks_cap)
		return NULL;

	if (!g_virq_map.chunks[ci]) {
		g_virq_map.chunks[ci] = sbi_zalloc(sizeof(struct virq_chunk));
		if (!g_virq_map.chunks[ci])
			return NULL;
	}
	return &g_virq_map.chunks[ci]->e[off];
}

static struct virq_entry *rev_get_existing(u32 virq)
{
	u32 ci = virq >> VIRQ_CHUNK_SHIFT;
	u32 off = virq & VIRQ_CHUNK_MASK;

	if (ci >= g_virq_map.chunks_cap || !g_virq_map.chunks[ci])
		return NULL;
	return &g_virq_map.chunks[ci]->e[off];
}

static void rev_clear(u32 virq)
{
	struct virq_entry *e = rev_get_existing(virq);
	if (e) {
		e->chip_uid = 0;
		e->hwirq = 0;
	}
}

static int vec_ensure_cap(u32 need_cnt)
{
	struct map_node *newp;
	u32 newcap;

	if (g_virq_map.nodes_cap >= need_cnt)
		return 0;

	newcap = g_virq_map.nodes_cap ? (g_virq_map.nodes_cap << 1) :
		VEC_GROW_MIN;
	while (newcap < need_cnt)
		newcap <<= 1;

	newp = sbi_zalloc((size_t)newcap * sizeof(*newp));
	if (!newp)
		return SBI_ENOMEM;

	if (g_virq_map.nodes) {
		sbi_memcpy(newp, g_virq_map.nodes,
			   (size_t)g_virq_map.nodes_cnt * sizeof(*newp));
		sbi_free(g_virq_map.nodes);
	}

	g_virq_map.nodes = newp;
	g_virq_map.nodes_cap = newcap;

	return 0;
}

static int forward_find_idx(u32 chip_uid, u32 hwirq, u32 *out_idx)
{
	u32 i;
	for (i = 0; i < g_virq_map.nodes_cnt; i++) {
		if (g_virq_map.nodes[i].chip_uid == chip_uid &&
		    g_virq_map.nodes[i].hwirq == hwirq) {
			*out_idx = i;
			return 0;
		}
	}

	return SBI_ENOENT;
}

int sbi_virq_map_init(u32 init_virq_cap)
{
	int rc;

	sbi_memset(&g_virq_map, 0, sizeof(g_virq_map));
	SPIN_LOCK_INIT(g_virq_map.lock);

	if (init_virq_cap < 8U)
		init_virq_cap = 8U;

	g_virq_map.bmap_nbits = init_virq_cap;
	g_virq_map.bmap =
		sbi_zalloc(bitmap_estimate_size((int)g_virq_map.bmap_nbits));
	if (!g_virq_map.bmap)
		return SBI_ENOMEM;

	bitmap_zero(g_virq_map.bmap, (int)g_virq_map.bmap_nbits);
	bitmap_set(g_virq_map.bmap, 0, 1); /* reserve virq 0 */

	rc = chunks_ensure_cap(g_virq_map.bmap_nbits);
	if (rc)
		return rc;

	return SBI_OK;
}

int sbi_virq_map_one(u32 chip_uid, u32 hwirq, bool allow_identity,
		     u32 identity_limit, u32 *out_virq)
{
	u32 idx, virq = 0;
	int rc;

	spin_lock(&g_virq_map.lock);

	/* already mapped? */
	rc = forward_find_idx(chip_uid, hwirq, &idx);
	if (!rc) {
		*out_virq = g_virq_map.nodes[idx].virq;
		sbi_printf("[VIRQ] found existing mapping: " \
			   "(hwirq %u, chip_uid %u) -> virq %u\n",
			   hwirq, chip_uid, virq);
		spin_unlock(&g_virq_map.lock);
		return 0;
	}

	/* ensure vector capacity for new node */
	rc = vec_ensure_cap(g_virq_map.nodes_cnt + 1U);
	if (rc) {
		spin_unlock(&g_virq_map.lock);
		return rc;
	}

	/* optional identity */
	if (allow_identity && hwirq != 0 && hwirq < identity_limit) {
		/* ensure bitmap covers this virq */
		if (hwirq >= g_virq_map.bmap_nbits) {
			u32 new_nbits = g_virq_map.bmap_nbits;
			while (new_nbits <= hwirq)
				new_nbits <<= 1;
			rc = bmap_grow(new_nbits);
			if (rc) {
				spin_unlock(&g_virq_map.lock);
				return rc;
			}
		}

		rc = bmap_alloc_specific(hwirq);
		if (!rc)
			virq = hwirq;
		else if (rc != SBI_EALREADY) {
			spin_unlock(&g_virq_map.lock);
			return rc;
		}
		/* if EEXIST, fallthrough to normal allocation */
	}

	/* allocate new virq if identity not taken */
	if (!virq) {
		rc = bmap_alloc_one(&virq);
		if (rc == SBI_ENOSPC) {
			rc = bmap_grow(g_virq_map.bmap_nbits << 1);
			if (rc) {
				spin_unlock(&g_virq_map.lock);
				return rc;
			}
			rc = bmap_alloc_one(&virq);
		}
		if (rc) {
			spin_unlock(&g_virq_map.lock);
			return rc;
		}
	}

	/* install reverse mapping */
	{
		struct virq_entry *e = rev_get_or_alloc(virq);
		if (!e) {
			bmap_free_one(virq);
			spin_unlock(&g_virq_map.lock);
			return SBI_ENOMEM;
		}
		e->chip_uid = chip_uid;
		e->hwirq = hwirq;
	}

	/* append forward node */
	g_virq_map.nodes[g_virq_map.nodes_cnt].chip_uid = chip_uid;
	g_virq_map.nodes[g_virq_map.nodes_cnt].hwirq = hwirq;
	g_virq_map.nodes[g_virq_map.nodes_cnt].virq = virq;
	g_virq_map.nodes_cnt++;

	*out_virq = virq;
	sbi_printf("[VIRQ] new mapping: (hwirq %u, chip_uid %u) -> VIRQ %u\n",
		   hwirq, chip_uid, virq);
	spin_unlock(&g_virq_map.lock);

	return SBI_OK;
}

int sbi_virq_hwirq2virq(u32 chip_uid, u32 hwirq, u32 *out_virq)
{
	u32 idx;
	int rc;

	spin_lock(&g_virq_map.lock);
	rc = forward_find_idx(chip_uid, hwirq, &idx);
	if (!rc)
		*out_virq = g_virq_map.nodes[idx].virq;
	spin_unlock(&g_virq_map.lock);

	return rc;
}

int sbi_virq_virq2hwirq(u32 virq, u32 *out_chip_uid, u32 *out_hwirq)
{
	struct virq_entry *e;

	spin_lock(&g_virq_map.lock);

	if (virq == 0 || virq >= g_virq_map.bmap_nbits ||
	    !bitmap_test(g_virq_map.bmap, (int)virq)) {
		spin_unlock(&g_virq_map.lock);
		return SBI_EINVAL;
	}

	e = rev_get_existing(virq);
	if (!e) {
		spin_unlock(&g_virq_map.lock);
		return SBI_EINVAL;
	}

	*out_chip_uid = e->chip_uid;
	*out_hwirq = e->hwirq;

	spin_unlock(&g_virq_map.lock);

	return SBI_OK;
}

int sbi_virq_unmap_one(u32 virq)
{
	struct virq_entry *e;
	u32 idx, last;
	int rc;

	spin_lock(&g_virq_map.lock);

	if (virq == 0 || virq >= g_virq_map.bmap_nbits ||
	    !bitmap_test(g_virq_map.bmap, (int)virq)) {
		spin_unlock(&g_virq_map.lock);
		return SBI_EINVAL;
	}

	e = rev_get_existing(virq);
	if (!e) {
		spin_unlock(&g_virq_map.lock);
		return SBI_EINVAL;
	}

	/* find forward node corresponding to this virq (linear) */
	rc = SBI_ENOENT;
	for (idx = 0; idx < g_virq_map.nodes_cnt; idx++) {
		if (g_virq_map.nodes[idx].virq == virq) {
			/* optionally also check endpoint matches e */
			rc = 0;
			break;
		}
	}
	if (rc) {
		/* inconsistent state */
		spin_unlock(&g_virq_map.lock);
		return SBI_EINVAL;
	}

	/* remove node: swap with last */
	last = g_virq_map.nodes_cnt - 1U;
	if (idx != last)
		g_virq_map.nodes[idx] = g_virq_map.nodes[last];
	g_virq_map.nodes_cnt--;

	/* clear reverse + free virq id */
	rev_clear(virq);
	bmap_free_one(virq);

	spin_unlock(&g_virq_map.lock);

	return SBI_OK;
}

void sbi_virq_map_uninit(void)
{
	u32 i;

	spin_lock(&g_virq_map.lock);

	/* free reverse chunks */
	if (g_virq_map.chunks) {
		for (i = 0; i < g_virq_map.chunks_cap; i++) {
			if (g_virq_map.chunks[i])
				sbi_free(g_virq_map.chunks[i]);
		}
		sbi_free(g_virq_map.chunks);
		g_virq_map.chunks = NULL;
		g_virq_map.chunks_cap = 0;
	}

	/* free forward vector */
	if (g_virq_map.nodes) {
		sbi_free(g_virq_map.nodes);
		g_virq_map.nodes = NULL;
		g_virq_map.nodes_cnt = 0;
		g_virq_map.nodes_cap = 0;
	}

	/* free bitmap */
	if (g_virq_map.bmap) {
		sbi_free(g_virq_map.bmap);
		g_virq_map.bmap = NULL;
		g_virq_map.bmap_nbits = 0;
	}

	spin_unlock(&g_virq_map.lock);
}

int sbi_virq_enqueue(struct sbi_virq_courier_binding *c)
{
	struct sbi_domain_virq_state *st;

	if (!c->dom || !c->virq)
		return SBI_EINVAL;

	st = domain_virq_thishart(c->dom, c->hart_index);

	if (!st)
		return SBI_ENODEV;

	spin_lock(&st->lock);
	if (q_full(st)) {
		spin_unlock(&st->lock);
		sbi_printf("[VIRQ] drop VIRQ %u (queue full)\n", c->virq);
		return SBI_ENOSPC;
	}
	st->chip = c->chip;
	st->q[st->tail] = c->virq;
	st->tail = (st->tail + 1) % VIRQ_QSIZE;
	spin_unlock(&st->lock);

	return SBI_OK;
}

u32 sbi_virq_pop(u32 hartid)
{
	u32 hartindex = sbi_hartid_to_hartindex(hartid);
	struct sbi_domain *dom;
	struct sbi_domain_virq_state *st;

	u32 virq = 0;

	if (!sbi_hartindex_valid(hartindex))
		return 0;

	dom = sbi_hartindex_to_domain(hartindex);
	st = dom ? domain_virq_thishart(dom, hartindex) : NULL;

	if (!st)
		return 0;

	spin_lock(&st->lock);
	if (!q_empty(st)) {
		virq = st->q[st->head];
		st->head = (st->head + 1) % VIRQ_QSIZE;
		sbi_printf("[VIRQ] Get VIRQ %d from queue (hartid %u)\n",
			   virq, hartid);
	} else {
		sbi_printf("[VIRQ] VIRQ queue is empty (hartid %u)\n", hartid);
	}
	spin_unlock(&st->lock);

	return virq;
}

void sbi_virq_complete(u32 hartid, u32 virq)
{
	u32 hartindex = sbi_hartid_to_hartindex(hartid);
	struct sbi_domain *dom;
	struct sbi_domain_virq_state *st;
	u32 hwirq;
	u32 chip_uid;

	sbi_printf("[VIRQ] Complete VIRQ %d\n", virq);

	if (!sbi_hartindex_valid(hartindex))
		return;

	dom = sbi_hartindex_to_domain(hartindex);
	st = dom ? domain_virq_thishart(dom, hartindex) : NULL;
	if (!st)
		return;

	sbi_virq_virq2hwirq(virq, &chip_uid, &hwirq);
	if (chip_uid != st->chip->id) {
		sbi_printf("[VIRQ] chip_uid %d does not match to chip->id %d\n",
			   chip_uid, st->chip->id);
	}
	sbi_irqchip_unmask_hwirq(st->chip, hwirq);
}

int sbi_virq_courier_handler(u32 hwirq, void *opaque)
{
	struct sbi_virq_courier_ctx *ctx =
		(struct sbi_virq_courier_ctx *)opaque;
	struct sbi_domain *dom;
	struct sbi_virq_courier_binding courier;
	u32 virq = 0;
	u32 hartid = 0;
	u32 hartindex = 0;
	int rc;

	if (!ctx || !ctx->chip)
		return SBI_EINVAL;

	/* Route purely by HWIRQ -> (domain,hart) rules (from FDT). */
	rc = sbi_virq_route_lookup(hwirq, &dom, &hartid, &hartindex);
	if (rc)
		return rc;

	/* Allocate/Get a stable VIRQ for (chip_uid, hwirq). */
	rc = sbi_virq_map_one(ctx->chip->id, hwirq, 0, 0, &virq);
	if (rc)
		return rc;

	sbi_printf("[VIRQ] route hwirq %u, chip_uid %u -> dom (%s), " \
		   "hartid %u, VIRQ %u\n",
		   hwirq, ctx->chip->id, dom->name, hartid, virq);

	/*
	 * Mask to avoid level-trigger storm before S-mode clears device source.
	 * S-mode will call sbi_virq_complete(hartid, virq) to unmask.
	 */
	sbi_irqchip_mask_hwirq(ctx->chip, hwirq);

	courier.dom  = dom;
	courier.hart_index = hartindex;
	courier.chip = ctx->chip;
	courier.virq = virq;

	rc = sbi_virq_enqueue(&courier);
	if (rc) {
		/* enqueue failed; re-enable to avoid deadlock */
		sbi_irqchip_unmask_hwirq(ctx->chip, hwirq);
		return rc;
	}

	rc = notify_smode_hartid(hartid);
	if (rc) {
		/* notification failed; re-enable to avoid deadlock */
		sbi_irqchip_unmask_hwirq(ctx->chip, hwirq);
		return rc;
	}
	sbi_printf("[VIRQ] SSE injected\n");

	return SBI_OK;
}

int sbi_virq_domain_init(struct sbi_domain *dom)
{
	struct sbi_domain_virq_priv *p;
	u32 i, nharts;

	if (!dom)
		return SBI_EINVAL;

	if (dom->virq_priv)
		return SBI_OK;

	sbi_printf("[VIRQ] Init per-domain VIRQ courier state\n");

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

	return SBI_OK;
}

void sbi_virq_domain_exit(struct sbi_domain *dom)
{
	if (!dom || !dom->virq_priv)
		return;

	sbi_free(dom->virq_priv);
	dom->virq_priv = NULL;
}

int sbi_virq_init(u32 init_virq_cap)
{
	int rc;

	rc = sbi_virq_map_init(init_virq_cap);
	if (rc)
		return rc;

	SPIN_LOCK_INIT(g_router.lock);
	sbi_virq_route_reset();

	return SBI_OK;
}
