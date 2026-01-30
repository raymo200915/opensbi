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
#include <sbi/sbi_string.h>
#include <sbi/sbi_timer.h>
#include <sbi/sbi_virq.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_domain_context.h>
#include <sbi/riscv_asm.h>
#include <sbi/riscv_encoding.h>
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
	u32 bmap_nbits; /* virq range: [0..nbits-1] */

	/* reverse table: virq -> endpoint */
	struct virq_chunk **chunks;
	u32 chunks_cap; /* number of chunk pointers */

	/* forward table: vector of mappings, linear search */
	struct map_node *nodes;
	u32 nodes_cnt;
	u32 nodes_cap;
};

struct sbi_virq_map_list {
	u32 channel_id;
	struct sbi_virq_map map;
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

static struct sbi_virq_map g_virq_map; /* channel 0 */
static struct sbi_virq_map_list *g_virq_maps;
static u32 g_virq_maps_cnt;
static u32 g_virq_maps_cap;
static spinlock_t g_virq_maps_lock;
static struct sbi_virq_router g_router;
static bool g_virq_inited;

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

int sbi_virq_route_add(struct sbi_domain *dom, u32 hwirq, u32 channel_id)
{
	int rc;

	if (!dom)
		return SBI_EINVAL;

	spin_lock(&g_router.lock);

	/* Reject duplicates to keep routing unambiguous */
	for (u32 i = 0; i < g_router.cnt; i++) {
		if (g_router.rules[i].hwirq == hwirq) {
			spin_unlock(&g_router.lock);
			sbi_printf("[VIRQ] duplicate rule: hwirq %u already " \
				   "mapped to dom=%s\n", hwirq,
				   g_router.rules[i].dom ?
				   g_router.rules[i].dom->name : "?");
			return SBI_EALREADY;
		}
	}

	rc = router_ensure_cap(g_router.cnt + 1);
	if (rc) {
		spin_unlock(&g_router.lock);
		return rc;
	}

	g_router.rules[g_router.cnt].hwirq = hwirq;
	g_router.rules[g_router.cnt].dom   = dom;
	g_router.rules[g_router.cnt].channel_id = channel_id;
	g_router.cnt++;

	spin_unlock(&g_router.lock);

	sbi_printf("[VIRQ] add route rule: hwirq %u route to dom (%s)\n",
		   hwirq, dom->name);

	return SBI_OK;
}

int sbi_virq_route_lookup(u32 hwirq, struct sbi_domain **out_dom,
			  u32 *out_channel_id)
{
	/* Fast path: no rules */
	if (!g_router.cnt) {
		if (out_dom)
			*out_dom = &root;
		if (out_channel_id)
			*out_channel_id = 0;
		return SBI_OK;
	}

	spin_lock(&g_router.lock);
	for (u32 i = 0; i < g_router.cnt; i++) {
		if (hwirq == g_router.rules[i].hwirq) {
			struct sbi_domain *d = g_router.rules[i].dom;
			u32 cid = g_router.rules[i].channel_id;
			spin_unlock(&g_router.lock);
			if (out_dom)
				*out_dom = d ? d : &root;
			if (out_channel_id)
				*out_channel_id = cid;
			return SBI_OK;
		}
	}
	spin_unlock(&g_router.lock);

	if (out_dom)
		*out_dom = &root;
	if (out_channel_id)
		*out_channel_id = 0;
	return SBI_OK;
}

static inline void virq_state_init(struct sbi_domain_virq_state *st)
{
	SPIN_LOCK_INIT(st->lock);
	st->head = 0;
	st->tail = 0;
	st->inflight = 0;
	st->return_to_prev = false;
	st->return_timer_pending = false;
}

static inline
struct sbi_domain_virq_state *domain_virq_thishart(struct sbi_domain *dom)
{
	unsigned long hartidx = sbi_hartid_to_hartindex(current_hartid());
	struct sbi_domain_virq_priv *p;

	p = (struct sbi_domain_virq_priv *)dom->virq_priv;
	if (!p || hartidx >= p->nharts)
		return NULL;

	return p->st_by_hart[hartidx];
}

static inline bool q_full(struct sbi_domain_virq_state *st)
{
	return ((st->tail + 1) % VIRQ_QSIZE) == st->head;
}

static inline bool q_empty(struct sbi_domain_virq_state *st)
{
	return st->head == st->tail;
}

static inline void virq_set_domain_return_flag(struct sbi_domain *dom,
					       bool return_to_prev)
{
	struct sbi_domain_virq_state *st = domain_virq_thishart(dom);

	if (!st)
		return;

	spin_lock(&st->lock);
	st->return_to_prev = return_to_prev;
	spin_unlock(&st->lock);
}

static void virq_schedule_return_timer(struct sbi_domain_virq_state *st)
{
	u64 next_event;
	const struct sbi_timer_device *tdev;

	if (!st)
		return;

	tdev = sbi_timer_get_device();
	if (!tdev || !tdev->timer_freq || !tdev->timer_event_start)
		return;

	if (misa_extension('S'))
		csr_clear(CSR_MIDELEG, MIP_MTIP);

	next_event = sbi_timer_value() +
		((u64)tdev->timer_freq / 1000) * VIRQ_RETURN_TO_PREV_DELAY_MS;

	sbi_printf("[VIRQ] schedule return timer hart%lu next=0x%lx\n",
		   (unsigned long)current_hartindex(),
		   (unsigned long)next_event);
	sbi_timer_event_start_mmode(next_event);
}

static u32 sbi_virq_platform_hart_count(void)
{
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
	const struct sbi_platform *plat = sbi_platform_ptr(scratch);

	return sbi_platform_hart_count(plat);
}

static int bmap_alloc_one(struct sbi_virq_map *m, u32 *out_virq)
{
	u32 v;

	for (v = 0; v < m->bmap_nbits; v++) {
		if (!bitmap_test(m->bmap, (int)v)) {
			bitmap_set(m->bmap, (int)v, 1);
			*out_virq = v;
			return 0;
		}
	}

	return SBI_ENOSPC;
}

static int bmap_alloc_specific(struct sbi_virq_map *m, u32 virq)
{
	if (virq >= m->bmap_nbits)
		return SBI_EINVAL;
	if (bitmap_test(m->bmap, (int)virq))
		return SBI_EALREADY;
	bitmap_set(m->bmap, (int)virq, 1);

	return 0;
}

static void bmap_free_one(struct sbi_virq_map *m, u32 virq)
{
	if (virq < m->bmap_nbits)
		bitmap_clear(m->bmap, (int)virq, 1);
}

static int chunks_ensure_cap(struct sbi_virq_map *m, u32 new_bmap_nbits)
{
	u32 new_chunks_cap =
		(new_bmap_nbits + VIRQ_CHUNK_SIZE - 1U) >> VIRQ_CHUNK_SHIFT;
	struct virq_chunk **newp;

	if (new_chunks_cap <= m->chunks_cap)
		return 0;

	newp = sbi_zalloc((size_t)new_chunks_cap * sizeof(*newp));
	if (!newp)
		return SBI_ENOMEM;

	if (m->chunks) {
		sbi_memcpy(newp, m->chunks,
			   (size_t)m->chunks_cap * sizeof(*newp));
		sbi_free(m->chunks);
	}

	m->chunks = newp;
	m->chunks_cap = new_chunks_cap;

	return 0;
}

static int bmap_grow(struct sbi_virq_map *m, u32 new_nbits)
{
	unsigned long *newmap;

	if (new_nbits <= m->bmap_nbits)
		return 0;

	newmap = sbi_zalloc(bitmap_estimate_size((int)new_nbits));
	if (!newmap)
		return SBI_ENOMEM;

	bitmap_zero(newmap, (int)new_nbits);
	bitmap_copy(newmap, m->bmap, (int)m->bmap_nbits);

	sbi_free(m->bmap);
	m->bmap = newmap;
	m->bmap_nbits = new_nbits;

	return chunks_ensure_cap(m, new_nbits);
}

static struct virq_entry *rev_get_or_alloc(struct sbi_virq_map *m, u32 virq)
{
	u32 ci = virq >> VIRQ_CHUNK_SHIFT;
	u32 off = virq & VIRQ_CHUNK_MASK;

	if (ci >= m->chunks_cap)
		return NULL;

	if (!m->chunks[ci]) {
		m->chunks[ci] = sbi_zalloc(sizeof(struct virq_chunk));
		if (!m->chunks[ci])
			return NULL;
	}
	return &m->chunks[ci]->e[off];
}

static struct virq_entry *rev_get_existing(struct sbi_virq_map *m, u32 virq)
{
	u32 ci = virq >> VIRQ_CHUNK_SHIFT;
	u32 off = virq & VIRQ_CHUNK_MASK;

	if (ci >= m->chunks_cap || !m->chunks[ci])
		return NULL;
	return &m->chunks[ci]->e[off];
}

static void rev_clear(struct sbi_virq_map *m, u32 virq)
{
	struct virq_entry *e = rev_get_existing(m, virq);
	if (e) {
		e->chip_uid = 0;
		e->hwirq = 0;
	}
}

static int vec_ensure_cap(struct sbi_virq_map *m, u32 need_cnt)
{
	struct map_node *newp;
	u32 newcap;

	if (m->nodes_cap >= need_cnt)
		return 0;

	newcap = m->nodes_cap ? (m->nodes_cap << 1) :
		VEC_GROW_MIN;
	while (newcap < need_cnt)
		newcap <<= 1;

	newp = sbi_zalloc((size_t)newcap * sizeof(*newp));
	if (!newp)
		return SBI_ENOMEM;

	if (m->nodes) {
		sbi_memcpy(newp, m->nodes,
			   (size_t)m->nodes_cnt * sizeof(*newp));
		sbi_free(m->nodes);
	}

	m->nodes = newp;
	m->nodes_cap = newcap;

	return 0;
}

static int forward_find_idx(struct sbi_virq_map *m,
			    u32 chip_uid, u32 hwirq, u32 *out_idx)
{
	u32 i;
	for (i = 0; i < m->nodes_cnt; i++) {
		if (m->nodes[i].chip_uid == chip_uid &&
		    m->nodes[i].hwirq == hwirq) {
			*out_idx = i;
			return 0;
		}
	}

	return SBI_ENOENT;
}

static int virq_map_init_one(struct sbi_virq_map *m, u32 init_virq_cap)
{
	int rc;

	sbi_memset(m, 0, sizeof(*m));
	SPIN_LOCK_INIT(m->lock);

	if (init_virq_cap < 8U)
		init_virq_cap = 8U;

	m->bmap_nbits = init_virq_cap;
	m->bmap =
		sbi_zalloc(bitmap_estimate_size((int)m->bmap_nbits));
	if (!m->bmap)
		return SBI_ENOMEM;

	bitmap_zero(m->bmap, (int)m->bmap_nbits);

	rc = chunks_ensure_cap(m, m->bmap_nbits);
	if (rc)
		return rc;

	return SBI_OK;
}

static struct sbi_virq_map *virq_map_get(u32 channel_id, bool create,
					 u32 init_virq_cap)
{
	u32 i;
	struct sbi_virq_map_list *newp;

	if (channel_id == 0)
		return &g_virq_map;

	spin_lock(&g_virq_maps_lock);
	for (i = 0; i < g_virq_maps_cnt; i++) {
		if (g_virq_maps[i].channel_id == channel_id) {
			spin_unlock(&g_virq_maps_lock);
			return &g_virq_maps[i].map;
		}
	}
	if (!create) {
		spin_unlock(&g_virq_maps_lock);
		return NULL;
	}

	if (g_virq_maps_cnt == g_virq_maps_cap) {
		u32 newcap = g_virq_maps_cap ? (g_virq_maps_cap << 1) : 4;
		newp = sbi_zalloc((size_t)newcap * sizeof(*newp));
		if (!newp) {
			spin_unlock(&g_virq_maps_lock);
			return NULL;
		}
		if (g_virq_maps) {
			sbi_memcpy(newp, g_virq_maps,
				   (size_t)g_virq_maps_cnt * sizeof(*newp));
			sbi_free(g_virq_maps);
		}
		g_virq_maps = newp;
		g_virq_maps_cap = newcap;
	}

	g_virq_maps[g_virq_maps_cnt].channel_id = channel_id;
	if (virq_map_init_one(&g_virq_maps[g_virq_maps_cnt].map,
			      init_virq_cap)) {
		spin_unlock(&g_virq_maps_lock);
		return NULL;
	}
	g_virq_maps_cnt++;
	spin_unlock(&g_virq_maps_lock);

	return &g_virq_maps[g_virq_maps_cnt - 1].map;
}

int sbi_virq_map_init(u32 channel_id, u32 init_virq_cap)
{
	if (channel_id == 0)
		return virq_map_init_one(&g_virq_map, init_virq_cap);
	SPIN_LOCK_INIT(g_virq_maps_lock);
	return virq_map_get(channel_id, true, init_virq_cap) ?
		SBI_OK : SBI_ENOMEM;
}

int sbi_virq_map_one(u32 channel_id, u32 chip_uid, u32 hwirq,
		     bool allow_identity, u32 identity_limit,
		     u32 *out_virq)
{
	u32 idx, virq = 0;
	int rc;
	struct sbi_virq_map *m;

	m = virq_map_get(channel_id, true, 0);
	if (!m)
		return SBI_ENOMEM;

	spin_lock(&m->lock);
	/* already mapped? */
	rc = forward_find_idx(m, chip_uid, hwirq, &idx);
	if (!rc) {
		*out_virq = m->nodes[idx].virq;
		sbi_printf("[VIRQ] found existing mapping: " \
			   "(hwirq %u, chip_uid %u) -> virq %u\n",
			   hwirq, chip_uid, virq);
		spin_unlock(&m->lock);
		return 0;
	}

	/* ensure vector capacity for new node */
	rc = vec_ensure_cap(m, m->nodes_cnt + 1U);
	if (rc) {
		spin_unlock(&m->lock);
		return rc;
	}

	/* optional identity */
	if (allow_identity && hwirq < identity_limit) {
		/* ensure bitmap covers this virq */
		if (hwirq >= m->bmap_nbits) {
			u32 new_nbits = m->bmap_nbits;
			while (new_nbits <= hwirq)
				new_nbits <<= 1;
			rc = bmap_grow(m, new_nbits);
			if (rc) {
				spin_unlock(&m->lock);
				return rc;
			}
		}

		rc = bmap_alloc_specific(m, hwirq);
		if (!rc)
			virq = hwirq;
		else if (rc != SBI_EALREADY) {
			spin_unlock(&m->lock);
			return rc;
		}
		/* if EEXIST, fallthrough to normal allocation */
	}

	/* allocate new virq if identity not taken */
	if (!virq) {
		rc = bmap_alloc_one(m, &virq);
		if (rc == SBI_ENOSPC) {
			rc = bmap_grow(m, m->bmap_nbits << 1);
			if (rc) {
				spin_unlock(&m->lock);
				return rc;
			}
			rc = bmap_alloc_one(m, &virq);
		}
		if (rc) {
			spin_unlock(&m->lock);
			return rc;
		}
	}

	/* install reverse mapping */
	{
		struct virq_entry *e = rev_get_or_alloc(m, virq);
		if (!e) {
			bmap_free_one(m, virq);
			spin_unlock(&m->lock);
			return SBI_ENOMEM;
		}
		e->chip_uid = chip_uid;
		e->hwirq = hwirq;
	}

	/* append forward node */
	m->nodes[m->nodes_cnt].chip_uid = chip_uid;
	m->nodes[m->nodes_cnt].hwirq = hwirq;
	m->nodes[m->nodes_cnt].virq = virq;
	m->nodes_cnt++;

	*out_virq = virq;
	sbi_printf("[VIRQ] new mapping: (hwirq %u, chip_uid %u) -> VIRQ %u\n",
		   hwirq, chip_uid, virq);
	spin_unlock(&m->lock);

	return SBI_OK;
}

int sbi_virq_map_set(u32 channel_id, u32 chip_uid, u32 hwirq, u32 virq)
{
	struct sbi_virq_map *m;
	u32 idx;
	int rc;

	m = virq_map_get(channel_id, true, virq + 1U);
	if (!m)
		return SBI_ENOMEM;

	spin_lock(&m->lock);
	rc = forward_find_idx(m, chip_uid, hwirq, &idx);
	if (!rc) {
		spin_unlock(&m->lock);
		return (m->nodes[idx].virq == virq) ? SBI_OK : SBI_EALREADY;
	}

	if (virq >= m->bmap_nbits) {
		u32 new_nbits = m->bmap_nbits;
		while (new_nbits <= virq)
			new_nbits <<= 1;
		rc = bmap_grow(m, new_nbits);
		if (rc) {
			spin_unlock(&m->lock);
			return rc;
		}
	}

	rc = bmap_alloc_specific(m, virq);
	if (rc == SBI_EALREADY) {
		struct virq_entry *e = rev_get_existing(m, virq);

		if (!e || e->chip_uid != chip_uid || e->hwirq != hwirq) {
			spin_unlock(&m->lock);
			return SBI_EALREADY;
		}

		spin_unlock(&m->lock);
		return SBI_OK;
	} else if (rc) {
		spin_unlock(&m->lock);
		return rc;
	}

	rc = vec_ensure_cap(m, m->nodes_cnt + 1U);
	if (rc) {
		spin_unlock(&m->lock);
		return rc;
	}

	{
		struct virq_entry *e = rev_get_or_alloc(m, virq);
		if (!e) {
			bmap_free_one(m, virq);
			spin_unlock(&m->lock);
			return SBI_ENOMEM;
		}
		e->chip_uid = chip_uid;
		e->hwirq = hwirq;
	}

	m->nodes[m->nodes_cnt].chip_uid = chip_uid;
	m->nodes[m->nodes_cnt].hwirq = hwirq;
	m->nodes[m->nodes_cnt].virq = virq;
	m->nodes_cnt++;
	sbi_printf("[VIRQ] set mapping: (hwirq %u, chip_uid %u) -> VIRQ %u\n",
		   hwirq, chip_uid, virq);
	spin_unlock(&m->lock);

	return SBI_OK;
}

int sbi_virq_map_ensure_cap(u32 channel_id, u32 min_virq_cap)
{
	struct sbi_virq_map *m;
	u32 new_nbits;
	int rc = SBI_OK;

	if (min_virq_cap < 8U)
		min_virq_cap = 8U;

	if (channel_id == 0) {
		m = &g_virq_map;
		if (!m->bmap)
			return SBI_EINVAL;
	} else {
		m = virq_map_get(channel_id, true, min_virq_cap);
		if (!m)
			return SBI_ENOMEM;
	}

	if (m->bmap_nbits >= min_virq_cap)
		return SBI_OK;

	spin_lock(&m->lock);
	new_nbits = m->bmap_nbits ? m->bmap_nbits : 8U;
	while (new_nbits < min_virq_cap)
		new_nbits <<= 1;
	rc = bmap_grow(m, new_nbits);
	spin_unlock(&m->lock);

	return rc;
}

int sbi_virq_hwirq2virq(u32 channel_id, u32 chip_uid, u32 hwirq,
			u32 *out_virq)
{
	u32 idx;
	int rc;
	struct sbi_virq_map *m;

	m = virq_map_get(channel_id, false, 0);
	if (!m)
		return SBI_ENOENT;

	spin_lock(&m->lock);
	rc = forward_find_idx(m, chip_uid, hwirq, &idx);
	if (!rc)
		*out_virq = m->nodes[idx].virq;
	spin_unlock(&m->lock);

	return rc;
}

int sbi_virq_virq2hwirq(u32 channel_id, u32 virq,
			u32 *out_chip_uid, u32 *out_hwirq)
{
	struct virq_entry *e;
	struct sbi_virq_map *m;

	m = virq_map_get(channel_id, false, 0);
	if (!m)
		return SBI_EINVAL;

	spin_lock(&m->lock);

	if (virq >= m->bmap_nbits ||
	    !bitmap_test(m->bmap, (int)virq)) {
		spin_unlock(&m->lock);
		return SBI_EINVAL;
	}

	e = rev_get_existing(m, virq);
	if (!e) {
		spin_unlock(&m->lock);
		return SBI_EINVAL;
	}

	*out_chip_uid = e->chip_uid;
	*out_hwirq = e->hwirq;

	spin_unlock(&m->lock);

	return SBI_OK;
}

int sbi_virq_unmap_one(u32 virq)
{
	struct virq_entry *e;
	u32 idx, last;
	int rc;
	struct sbi_virq_map *m = &g_virq_map;

	spin_lock(&m->lock);

	if (virq >= m->bmap_nbits ||
	    !bitmap_test(m->bmap, (int)virq)) {
		spin_unlock(&m->lock);
		return SBI_EINVAL;
	}

	e = rev_get_existing(m, virq);
	if (!e) {
		spin_unlock(&m->lock);
		return SBI_EINVAL;
	}

	/* find forward node corresponding to this virq (linear) */
	rc = SBI_ENOENT;
	for (idx = 0; idx < m->nodes_cnt; idx++) {
		if (m->nodes[idx].virq == virq) {
			/* optionally also check endpoint matches e */
			rc = 0;
			break;
		}
	}
	if (rc) {
		/* inconsistent state */
		spin_unlock(&m->lock);
		return SBI_EINVAL;
	}

	/* remove node: swap with last */
	last = m->nodes_cnt - 1U;
	if (idx != last)
		m->nodes[idx] = m->nodes[last];
	m->nodes_cnt--;

	/* clear reverse + free virq id */
	rev_clear(m, virq);
	bmap_free_one(m, virq);

	spin_unlock(&m->lock);

	return SBI_OK;
}

static void virq_map_uninit_one(struct sbi_virq_map *m)
{
	u32 i;

	spin_lock(&m->lock);

	/* free reverse chunks */
	if (m->chunks) {
		for (i = 0; i < m->chunks_cap; i++) {
			if (m->chunks[i])
				sbi_free(m->chunks[i]);
		}
		sbi_free(m->chunks);
		m->chunks = NULL;
		m->chunks_cap = 0;
	}

	/* free forward vector */
	if (m->nodes) {
		sbi_free(m->nodes);
		m->nodes = NULL;
		m->nodes_cnt = 0;
		m->nodes_cap = 0;
	}

	/* free bitmap */
	if (m->bmap) {
		sbi_free(m->bmap);
		m->bmap = NULL;
		m->bmap_nbits = 0;
	}

	spin_unlock(&m->lock);
}

void sbi_virq_map_uninit(void)
{
	u32 i;

	virq_map_uninit_one(&g_virq_map);

	spin_lock(&g_virq_maps_lock);
	for (i = 0; i < g_virq_maps_cnt; i++)
		virq_map_uninit_one(&g_virq_maps[i].map);
	if (g_virq_maps) {
		sbi_free(g_virq_maps);
		g_virq_maps = NULL;
		g_virq_maps_cnt = 0;
		g_virq_maps_cap = 0;
	}
	spin_unlock(&g_virq_maps_lock);
}

int sbi_virq_enqueue(struct sbi_virq_courier_binding *c)
{
	struct sbi_domain_virq_state *st;

	if (!c->dom || c->virq == VIRQ_INVALID)
		return SBI_EINVAL;

	st = domain_virq_thishart(c->dom);
	if (!st)
		return SBI_ENODEV;

	sbi_printf("[VIRQ] Get queue for (domain,hartidx): (%s,%u)\n",
		   c->dom->name, sbi_hartid_to_hartindex(current_hartid()));

	spin_lock(&st->lock);
	if (q_full(st)) {
		spin_unlock(&st->lock);
		sbi_printf("[VIRQ] drop VIRQ %u (queue full)\n", c->virq);
		return SBI_ENOSPC;
	}

	sbi_printf("[VIRQ] Push VIRQ %d to queue\n", c->virq);
	st->q[st->tail].virq = c->virq;
	st->q[st->tail].channel_id = c->channel_id;
	st->q[st->tail].chip = c->chip;
	st->tail = (st->tail + 1) % VIRQ_QSIZE;
	spin_unlock(&st->lock);

	return SBI_OK;
}

u32 sbi_virq_pop_thishart(void)
{
	struct sbi_domain *dom = sbi_domain_thishart_ptr();
	struct sbi_domain_virq_state *st;
	u32 virq = VIRQ_INVALID;

	if (!dom)
		return VIRQ_INVALID;

	st = domain_virq_thishart(dom);
	if (!st)
		return VIRQ_INVALID;

	sbi_printf("[VIRQ] Get queue for (domain,hartidx): (%s,%u)\n",
		   dom->name, sbi_hartid_to_hartindex(current_hartid()));

	spin_lock(&st->lock);
	if (!q_empty(st)) {
		virq = st->q[st->head].virq;
		st->last_pop_virq = virq;
		st->last_pop_channel_id = st->q[st->head].channel_id;
		st->last_pop_chip = st->q[st->head].chip;
		st->inflight++;
		st->head = (st->head + 1) % VIRQ_QSIZE;
		sbi_printf("[VIRQ] Pop VIRQ %d from queue\n", virq);
	} else {
		sbi_printf("[VIRQ] VIRQ queue is empty\n");
		virq = VIRQ_INVALID;
	}
	spin_unlock(&st->lock);

	if (virq == VIRQ_INVALID) {
		if (sbi_irqchip_notify_smode_get())
			sbi_irqchip_notify_smode_clear();
	}

	return virq;
}

void sbi_virq_complete_thishart(u32 virq)
{
	struct sbi_domain *dom = sbi_domain_thishart_ptr();
	struct sbi_domain_virq_state *st;
	u32 hwirq;
	u32 chip_uid;
	u32 channel_id;
	struct sbi_irqchip_device *chip;
	bool schedule_return = false;

	if (virq == VIRQ_INVALID)
		return;

	if (!dom)
		return;

	st = domain_virq_thishart(dom);
	if (!st)
		return;

	sbi_printf("[VIRQ] Get queue for (domain,hartidx): (%s,%u)\n",
		   dom->name, sbi_hartid_to_hartindex(current_hartid()));

	sbi_printf("[VIRQ] Complete VIRQ %d from queue\n", virq);

	spin_lock(&st->lock);
	channel_id = st->last_pop_channel_id;
	chip = st->last_pop_chip;
	if (st->last_pop_virq == virq) {
		st->last_pop_virq = 0;
		st->last_pop_channel_id = 0;
		st->last_pop_chip = NULL;
		if (st->inflight)
			st->inflight--;
	}
	if (st->return_to_prev && q_empty(st) && !st->inflight &&
	    !st->return_timer_pending) {
		st->return_timer_pending = true;
		schedule_return = true;
	}
	spin_unlock(&st->lock);

	if (!chip)
		return;

	sbi_virq_virq2hwirq(channel_id, virq, &chip_uid, &hwirq);
	if (chip_uid != chip->id) {
		sbi_printf("[VIRQ] chip_uid %d does not match to chip->id %d\n",
			   chip_uid, chip->id);
	}
	if (chip->hwirq_eoi) {
		sbi_printf("[IRQCHIP] Calling EOI of hwirq %u\n", hwirq);
		chip->hwirq_eoi(chip, hwirq);
	}
	sbi_irqchip_unmask_hwirq(chip, hwirq);

	if (schedule_return) {
		if (sbi_irqchip_notify_smode_get())
			sbi_irqchip_notify_smode_clear();
		virq_schedule_return_timer(st);
	}
}

void sbi_virq_return_to_prev_if_needed(void)
{
	struct sbi_domain *dom = sbi_domain_thishart_ptr();
	struct sbi_domain_virq_state *st;
	bool do_return = false;

	if (!dom)
		return;

	st = domain_virq_thishart(dom);
	if (!st)
		return;

	spin_lock(&st->lock);
	if (st->return_to_prev && q_empty(st)) {
		st->return_to_prev = false;
		do_return = true;
	}
	spin_unlock(&st->lock);

	if (!do_return)
		return;

	sbi_printf("[VIRQ] return_to_prev after VIRQ queue drained on hart%lu\n",
		   (unsigned long)current_hartindex());
	sbi_domain_context_request_return_to_prev();
}

void sbi_virq_return_to_prev_from_timer(void)
{
	struct sbi_domain *dom = sbi_domain_thishart_ptr();
	struct sbi_domain_virq_state *st;
	bool do_return = false;

	sbi_timer_event_stop_mmode();

	if (!dom)
		return;

	st = domain_virq_thishart(dom);
	if (!st)
		return;

	spin_lock(&st->lock);
	if (st->return_timer_pending) {
		if (st->return_to_prev && q_empty(st) && !st->inflight) {
			st->return_to_prev = false;
			st->return_timer_pending = false;
			do_return = true;
		} else {
			st->return_timer_pending = false;
		}
	}
	spin_unlock(&st->lock);

	if (misa_extension('S'))
		csr_set(CSR_MIDELEG, MIP_MTIP);

	if (!do_return)
		return;

	sbi_printf("[VIRQ] return_to_prev timer fired on hart%lu\n",
		   (unsigned long)current_hartindex());
	sbi_domain_context_request_return_to_prev();
}

bool sbi_virq_return_timer_pending(void)
{
	struct sbi_domain *dom = sbi_domain_thishart_ptr();
	struct sbi_domain_virq_state *st;
	bool pending;

	if (!dom)
		return false;

	st = domain_virq_thishart(dom);
	if (!st)
		return false;

	spin_lock(&st->lock);
	pending = st->return_timer_pending;
	spin_unlock(&st->lock);

	return pending;
}

int sbi_virq_courier_handler(u32 hwirq, void *opaque)
{
	struct sbi_virq_courier_ctx *ctx =
		(struct sbi_virq_courier_ctx *)opaque;
	struct sbi_domain *dom;
	struct sbi_virq_courier_binding courier;
	u32 channel_id = 0;
	u32 virq = 0;
	int rc;
	struct sbi_domain *curr_dom;

	if (!ctx || !ctx->chip)
		return SBI_EINVAL;

	/* Route purely by HWIRQ -> Domain/channel rules (from FDT). */
	rc = sbi_virq_route_lookup(hwirq, &dom, &channel_id);
	if (rc || !dom)
		return SBI_EINVAL;

	curr_dom = sbi_domain_thishart_ptr();
	sbi_printf("[VIRQ] virq courier hart%lu curr=%s target=%s hwirq=%u\n",
		   (unsigned long)current_hartindex(),
		   curr_dom ? curr_dom->name : "?", dom->name, hwirq);

	/* Allocate/Get a stable VIRQ for (chip_uid, hwirq). */
	rc = sbi_virq_map_one(channel_id, ctx->chip->id, hwirq,
			      false, 0, &virq);
	if (rc)
		return rc;

	sbi_printf("[VIRQ] route hwirq %u, chip_uid %u -> dom (%s), channel %u,"
		   " VIRQ %u\n", hwirq, ctx->chip->id, dom->name, channel_id,
		   virq);

	/*
	 * Mask to avoid level-trigger storm before S-mode clears device source.
	 * S-mode will call sbi_virq_complete_thishart(virq) to unmask.
	 */
	sbi_irqchip_mask_hwirq(ctx->chip, hwirq);

	courier.dom  = dom;
	courier.chip = ctx->chip;
	courier.channel_id = channel_id;
	courier.virq = virq;

	rc = sbi_virq_enqueue(&courier);
	if (rc) {
		/* enqueue failed; re-enable to avoid deadlock */
		sbi_irqchip_unmask_hwirq(ctx->chip, hwirq);
		return rc;
	}

	/*
	 * Notify S-mode on notification rising edge.
	 *
	 * If the target is the current domain, operate on the live CSR.
	 * Otherwise, set the pending bit in the target domain context
	 * before switching (covers first-entry). After switching, set the
	 * live CSR only if needed (covers already-initialized targets).
	 */
	if (dom != curr_dom) {
		bool already = sbi_domain_context_pending_notify_smode(
			dom, current_hartindex());
		if (!already)
			sbi_printf("[VIRQ] S-mode pending notify\n");

		/* Mark return_to_prev for VIRQ-driven domain switch. */
		virq_set_domain_return_flag(dom, true);
		sbi_printf("[VIRQ] virq courier switching hart%lu %s -> %s\n",
			   current_hartindex(), curr_dom->name, dom->name);
		rc = sbi_domain_context_enter(dom);
		if (rc) {
			sbi_printf("[VIRQ] virq courier switch failed, rc=%d\n",
				   rc);
			/* Switch failed; do not defer EOI */
			sbi_irqchip_unmask_hwirq(ctx->chip, hwirq);
			if (ctx->chip->hwirq_eoi) {
				sbi_printf("[IRQCHIP] Calling EOI, hwirq %u\n",
					   hwirq);
				ctx->chip->hwirq_eoi(ctx->chip, hwirq);
			}
			return SBI_OK;
		}

		/*
		 * If the domain was already initialized,
		 * sbi_domain_context_enter() returns and CSR_SIP reflect
		 * dom_ctx->sip. For robustness, set the live notify bit if it
		 * is still clear.
		 */
		if (!sbi_irqchip_notify_smode_get()) {
			rc = sbi_irqchip_notify_smode_set();
			if (rc) {
				/*
				 * notification failed; re-enable to avoid
				 * deadlock
				 */
				sbi_irqchip_unmask_hwirq(ctx->chip, hwirq);
				return rc;
			}
			sbi_printf("[VIRQ] S-mode notified\n");
		}
	} else if (!sbi_irqchip_notify_smode_get()) {
		rc = sbi_irqchip_notify_smode_set();
		if (rc) {
			/* notification failed; re-enable to avoid deadlock */
			sbi_irqchip_unmask_hwirq(ctx->chip, hwirq);
			return rc;
		}
		sbi_printf("[VIRQ] S-mode notified\n");
	}

	/*
	 * Return SBI_EALREADY to defer EOI until VIRQ COMPLETE so S-mode
	 * notification can be delivered to the target domain.
	 */
	return SBI_EALREADY;
}

int sbi_virq_domain_init(struct sbi_domain *dom)
{
	struct sbi_domain_virq_priv *p;
	u32 i, k, nharts, st_count;
	struct sbi_domain_virq_state *st_base;
	size_t alloc_size;

	if (!dom)
		return SBI_EINVAL;

	if (dom->virq_priv)
		return SBI_OK;

	sbi_printf("[VIRQ] Init per-domain VIRQ courier state for %s\n",
		   dom->name);

	nharts = sbi_virq_platform_hart_count();
	st_count = dom->possible_harts ?
		   (u32)sbi_hartmask_weight(dom->possible_harts) : nharts;
	sbi_printf("[VIRQ] number of harts: %d\n", nharts);

	alloc_size = sizeof(*p) +
		     nharts * sizeof(p->st_by_hart[0]) +
		     st_count * sizeof(struct sbi_domain_virq_state);
	p = sbi_zalloc(alloc_size);
	if (!p) {
		return SBI_ENOMEM;
	}

	p->nharts = nharts;
	p->st_count = st_count;
	st_base = (struct sbi_domain_virq_state *)(p->st_by_hart + nharts);

	if (!dom->possible_harts) {
		for (i = 0; i < nharts; i++) {
			p->st_by_hart[i] = &st_base[i];
			virq_state_init(p->st_by_hart[i]);
		}
	} else {
		for (i = 0; i < nharts; i++)
			p->st_by_hart[i] = NULL;
		k = 0;
		sbi_hartmask_for_each_hartindex(i, dom->possible_harts) {
			if (k >= st_count)
				break;
			p->st_by_hart[i] = &st_base[k++];
			virq_state_init(p->st_by_hart[i]);
		}
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
	int rc = SBI_OK;

	if (g_virq_inited)
		return SBI_EALREADY;

	rc = sbi_virq_map_init(0, init_virq_cap);
	if (rc)
		return rc;

	SPIN_LOCK_INIT(g_virq_maps_lock);
	SPIN_LOCK_INIT(g_router.lock);
	sbi_virq_route_reset();
	g_virq_inited = true;
	return rc;
}

bool sbi_virq_is_inited(void)
{
	return g_virq_inited;
}
