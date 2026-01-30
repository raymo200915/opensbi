/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Virtual IRQ (VIRQ) courier/routing layer for OpenSBI.
 *
 * This header defines:
 *   1) VIRQ number allocation and (chip_uid,hwirq) <-> VIRQ mapping
 *   2) HWIRQ -> Domain routing rules (from DeviceTree "opensbi,mpxy-sysirq")
 *   3) Per-(domain,hart) pending queue (push in M-mode, pop/complete in S-mode)
 *
 * High-level design intent:
 *   - All physical host IRQs are handled in M-mode by host irqchip drivers.
 *   - For each incoming HWIRQ, OpenSBI determines the destination domain using
 *     DT-defined routing rules and enqueues a VIRQ into the per-(domain,hart)
 *     pending queue.
 *   - S-mode payload consumes pending VIRQs via pop(), and completes them via
 *     complete(), which unmasks the corresponding host HWIRQ line.
 *
 * Notes:
 *   - "opensbi,mpxy-sysirq" routing is derived from the sysirq node's
 *     "interrupts-extended" entries. It does not encode privilege level
 *     delivery. Hardware delivery (MEI vs SEI) is determined by platform IRQ
 *     topology and interrupt-parent.
 *
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#ifndef __SBI_VIRQ_H__
#define __SBI_VIRQ_H__

#include <sbi/sbi_domain.h>
#include <sbi/sbi_irqchip.h>
#include <sbi/riscv_locks.h>
#include <sbi/sbi_types.h>

/*
 * Current implementation behavior when queue overflows:
 *   - Drop the incoming VIRQ
 *   - Return SBI_ENOMEM
 */
#define VIRQ_QSIZE  64

/*
 * Reverse mapping table is chunked to avoid a single large static array.
 * VIRQ is used as an index into a chunk; chunks are allocated on demand.
 */
#define VIRQ_CHUNK_SHIFT   6U
#define VIRQ_CHUNK_SIZE    (1U << VIRQ_CHUNK_SHIFT)
#define VIRQ_CHUNK_MASK    (VIRQ_CHUNK_SIZE - 1U)

/* Minimum growth step for forward mapping vector and related metadata. */
#define VEC_GROW_MIN       16U

/* Returned by pop when no pending VIRQ is available. */
#define VIRQ_INVALID       0xffffffffU

/*------------------------------------------------------------------------------
 * VIRQ allocator and (chip_uid,hwirq) <-> VIRQ mapping
 *----------------------------------------------------------------------------*/

 /*
 * VIRQ mapping model:
 *   - Forward mapping:  (chip_uid,hwirq) -> VIRQ
 *       Implementation: dynamic vector of entries (linear search).
 *
 *   - Reverse mapping:  VIRQ -> (chip_uid,hwirq)
 *       Implementation: chunked table allocated on demand, O(1) lookup.
 *
 *   - VIRQ number allocation:
 *       Implementation: growable bitmap; capacity expands as needed.
 *
 * Memory usage scales with the number of installed mappings.
 */

/* Entry of reverse mapping table: represents (chip_uid,hwirq) endpoint */
struct virq_entry {
	u32 chip_uid;
	u32 hwirq;
};

/* Chunked reverse mapping table: VIRQ -> (chip_uid,hwirq) */
struct virq_chunk {
	struct virq_entry e[VIRQ_CHUNK_SIZE];
};

/*------------------------------------------------------------------------------
 * HWIRQ -> Domain routing rules
 *----------------------------------------------------------------------------*/

/*
 * A routing rule maps a single HWIRQ to a domain.
 *
 * Rules are populated once during cold boot while parsing the DT
 * opensbi-domains configuration (sysirq node "opensbi,mpxy-sysirq").
 *
 * DT encodes mapping via "interrupts-extended"; the index within this array
 * becomes the VIRQ number for the given MPXY channel.
 *
 * Policy notes:
 *   - Duplicate HWIRQ entries are rejected and return SBI_EALREADY.
 *   - If no rule matches, routing falls back to the root domain (&root).
 */
struct sbi_virq_route_rule {
	u32 hwirq;
	struct sbi_domain *dom;   /* owner domain */
	u32 channel_id;           /* VIRQ space/channel */
};

/*
 * Courier context passed as 'opaque' to sbi_virq_courier_handler(), created
 * per host irqchip.
 *
 * The courier handler needs to:
 *   - map (chip_uid,hwirq) -> VIRQ
 *   - mask/unmask HWIRQ using the correct irqchip device
 * Therefore the irqchip device pointer is carried here.
 */
struct sbi_virq_courier_ctx {
	struct sbi_irqchip_device *chip;
};

/*------------------------------------------------------------------------------
 * Per-(domain,hart) pending VIRQ state and queue management
 *----------------------------------------------------------------------------*/

/*
 * Per-(domain,hart) VIRQ state.
 *
 * Locking:
 *   - lock protects head/tail and q[].
 *
 * Queue semantics:
 *   - q[] stores VIRQs pending handling for this (domain,hart).
 *   - enqueue is performed by M-mode (courier handler) according to route rule
 *     populated from DT.
 *   - pop/complete is performed by S-mode payload running in the destination
 *     domain on the current hart.
 *   - chip caches the irqchip device for unmasking on complete().
 */
struct sbi_domain_virq_state {
	spinlock_t lock;
	u32 head;
	u32 tail;

	/* Pending VIRQ ring buffer. */
	struct {
		u32 virq;
		u32 channel_id;
		struct sbi_irqchip_device *chip;
	} q[VIRQ_QSIZE];

	/* Last popped entry for completion. */
	u32 last_pop_virq;
	u32 last_pop_channel_id;
	struct sbi_irqchip_device *last_pop_chip;

	/* Deferred SSE notify when target domain not ready. */
	bool deferred_notify;
	u32 deferred_virq;

	/* Return to previous domain after SSE completion. */
	bool return_to_prev;
};

/*
 * Per-domain private VIRQ context.
 *
 * Attached to struct sbi_domain and contains per-hart states.
 */
struct sbi_domain_virq_priv {
	/* number of platform harts */
	u32 nharts;

	/* number of allocated per-hart states */
	u32 st_count;

	/* per-hart VIRQ state pointer array (indexed by hart index) */
	struct sbi_domain_virq_state *st_by_hart[];
};

/* Courier binding used when enqueuing a VIRQ. */
struct sbi_virq_courier_binding {
	/* destination domain */
	struct sbi_domain *dom;

	/* irqchip device that asserted the HWIRQ */
	struct sbi_irqchip_device *chip;

	/* VIRQ space/channel ID */
	u32 channel_id;

	/* VIRQ number to enqueue */
	u32 virq;
};

/*------------------------------------------------------------------------------
 * Public APIs
 *----------------------------------------------------------------------------*/

/*
 * Initialize a per-channel VIRQ map.
 *
 * @channel_id:
 *   VIRQ space/channel ID (0 is the default channel).
 *
 * @init_virq_cap:
 *   Initial capacity in VIRQ bits (e.g., 256). Implementation may grow beyond.
 *
 * Return:
 *   SBI_OK on success
 *   SBI_ENOMEM on allocation failure
 */
int sbi_virq_map_init(u32 channel_id, u32 init_virq_cap);

/*
 * Create or get a stable mapping for (channel_id, chip_uid, hwirq) -> VIRQ.
 *
 * @channel_id:
 *   Paravirt channel ID; VIRQ numbering is local to each channel.
 *
 * @chip_uid:
 *   Unique 32-bit ID of the host irqchip device.
 *
 * @hwirq:
 *   Host HWIRQ number as produced by the irqchip driver (e.g. APLIC claim ID).
 *
 * @allow_identity:
 *   If true, allocator may attempt VIRQ == hwirq for small ranges.
 *
 * @identity_limit:
 *   Upper bound (exclusive) for identity mapping trial: hwirq < identity_limit.
 *
 * @out_virq:
 *   Output pointer receiving the mapped/allocated VIRQ (0 is valid).
 *
 * Return:
 *   SBI_OK on success
 *   SBI_ENOMEM on allocation failure
 *   SBI_ENOSPC if allocator cannot allocate
 *   SBI_EINVAL on invalid parameters
 */
int sbi_virq_map_one(u32 channel_id, u32 chip_uid, u32 hwirq,
		     bool allow_identity, u32 identity_limit, u32 *out_virq);

/*
 * Force a mapping for (channel_id, chip_uid, hwirq) -> VIRQ.
 *
 * @channel_id:
 *   Paravirt channel ID; VIRQ numbering is local to each channel.
 *
 * @chip_uid:
 *   Unique 32-bit ID of the host irqchip device.
 *
 * @hwirq:
 *   Host HWIRQ number as produced by the irqchip driver.
 *
 * @virq:
 *   VIRQ number to assign (0 is valid).
 *
 * Return:
 *   SBI_OK on success
 *   SBI_ENOMEM on allocation failure
 *   SBI_EINVAL on invalid parameters
 *   SBI_EALREADY if a different mapping already exists
 */
int sbi_virq_map_set(u32 channel_id, u32 chip_uid, u32 hwirq, u32 virq);

/*
 * Ensure VIRQ map capacity for a given channel.
 *
 * @channel_id:
 *   Paravirt channel ID.
 *
 * @min_virq_cap:
 *   Minimum VIRQ bitmap capacity in bits (will be rounded up).
 *
 * Return:
 *   SBI_OK on success
 *   SBI_EINVAL if the map is not initialized (channel 0)
 *   SBI_ENOMEM on allocation failure
 */
int sbi_virq_map_ensure_cap(u32 channel_id, u32 min_virq_cap);

/*
 * Lookup existing mapping: (channel_id, chip_uid, hwirq) -> VIRQ.
 *
 * @channel_id:
 *   Paravirt channel ID; VIRQ numbering is local to each channel.
 *
 * @chip_uid:
 *   Irqchip unique id.
 *
 * @hwirq:
 *   Host hwirq number.
 *
 * @out_virq:
 *   Output VIRQ (0 is valid).
 *
 * Return:
 *   SBI_OK if found
 *   SBI_ENOENT if not mapped
 *   SBI_EINVAL on invalid input
 */
int sbi_virq_hwirq2virq(u32 channel_id, u32 chip_uid, u32 hwirq,
			u32 *out_virq);

/*
 * Reverse lookup: (channel_id, VIRQ) -> (chip_uid, hwirq).
 *
 * @channel_id:
 *   Paravirt channel ID; VIRQ numbering is local to each channel.
 *
 * @virq:
 *   VIRQ number to look up.
 *
 * @out_chip_uid:
 *   Output pointer receiving irqchip unique id.
 *
 * @out_hwirq:
 *   Output pointer receiving host hwirq number.
 *
 * Return:
 *   SBI_OK on success
 *   SBI_EINVAL if virq is VIRQ_INVALID, out of range, not allocated, or
 *     reverse entry missing
 */
int sbi_virq_virq2hwirq(u32 channel_id, u32 virq,
			u32 *out_chip_uid, u32 *out_hwirq);

/*
 * Unmap a single VIRQ mapping and free the VIRQ number.
 *
 * @virq:
 *   VIRQ number to unmap.
 *
 * Return:
 *   SBI_OK on success
 *   SBI_EINVAL if virq is invalid or state is inconsistent
 */
int sbi_virq_unmap_one(u32 virq);

/*
 * Uninitialize the VIRQ mapping allocator and free all resources.
 *
 * Notes:
 *   - This frees bitmap, forward vector, and reverse chunks.
 */
void sbi_virq_map_uninit(void);

/*
 * Reset all HWIRQ->Domain routing rules (frees the rule array).
 *
 * Typical usage:
 *   - Called once at cold boot during init before parsing DT domains.
 */
void sbi_virq_route_reset(void);

/*
 * Add a routing rule: hwirq -> dom with channel_id.
 *
 * @dom:
 *   Target domain that should receive HWIRQs in this range.
 *
 * @hwirq:
 *   HWIRQ number to route.
 *
 * @channel_id:
 *   Paravirt channel ID for VIRQ mapping and SSE injection.
 *
 * Return:
 *   SBI_OK on success
 *   SBI_EINVAL on invalid parameters
 *   SBI_ENOMEM on allocation failure
 *   SBI_EALREADY if the HWIRQ already has a rule
 */
int sbi_virq_route_add(struct sbi_domain *dom, u32 hwirq, u32 channel_id);

/*
 * Lookup destination domain for a given HWIRQ.
 *
 * @hwirq:
 *   Incoming host HWIRQ number.
 *
 * @out_dom:
 *   Output pointer receiving destination domain. If no rule matches, &root
 *   is returned.
 *
 * @out_channel_id:
 *   Output pointer receiving channel id if non-NULL.
 *
 * Return:
 *   SBI_OK on success
 *   SBI_EINVAL on invalid parameters
 */
int sbi_virq_route_lookup(u32 hwirq, struct sbi_domain **out_dom,
			  u32 *out_channel_id);

/*
 * Enqueue a VIRQ for the destination domain on the current hart.
 *
 * @c:
 *   Courier binding containing:
 *     - c->dom  : destination domain
 *     - c->chip : irqchip device pointer
 *     - c->virq : VIRQ number
 *
 * Return:
 *   SBI_OK on success
 *   SBI_EINVAL on invalid parameters
 *   SBI_ENODEV if per-(domain,hart) state is not available
 *   SBI_ENOMEM if queue is full
 */
int sbi_virq_enqueue(struct sbi_virq_courier_binding *c);

/*
 * Pop the next pending VIRQ for the current domain on the current hart.
 *
 * Return:
 *   VIRQ_INVALID if none pending or state not available
 *   otherwise a VIRQ number (zero is legal)
 */
u32 sbi_virq_pop_thishart(void);

/*
 * Complete a previously couriered VIRQ for the current domain/hart.
 *
 * @virq:
 *   VIRQ to complete.
 */
void sbi_virq_complete_thishart(u32 virq);

/* Return to previous domain if a VIRQ-driven switch is pending. */
void sbi_virq_return_to_prev_if_needed(void);

/* Notify S-mode if a deferred SSE injection is pending. */
void sbi_virq_notify_smode_deferred_thishart(void);

/*
 * Courier handler intended to be registered by host irqchip driver.
 *
 * @hwirq:
 *   Incoming host HWIRQ number asserted on the irqchip.
 *
 * @opaque:
 *   Point to a valid struct sbi_virq_courier_ctx, which provides the
 *   irqchip device pointer used for mapping and mask/unmask.
 *
 * Return:
 *   SBI_OK on success
 *   SBI_EINVAL on invalid parameters
 *   Other SBI_E* propagated from mapping or enqueue
 */
int sbi_virq_courier_handler(u32 hwirq, void *opaque);

/*
 * Initialize per-domain VIRQ state.
 *
 * @dom:
 *   Domain to initialize.
 *
 * Return:
 *   SBI_OK on success
 *   SBI_EINVAL on invalid parameters
 *   SBI_ENOMEM on allocation failure
 */
int sbi_virq_domain_init(struct sbi_domain *dom);

/*
 * Free per-domain VIRQ state.
 *
 * @dom:
 *   Free the per-domain VIRQ state.
 */
void sbi_virq_domain_exit(struct sbi_domain *dom);

/*
 * Initialize VIRQ subsystem (mapping allocator + route rules).
 * Must be called once before parsing sysirq DT nodes.
 *
 * @init_virq_cap:
 *   Initial VIRQ bitmap capacity in bits
 *
 * Return:
 *   SBI_OK on success
 *   SBI_EALREADY if called more than once
 *   SBI_ENOMEM on allocation failure
 *   Other SBI_E* error codes propagated from mapping init
 */
int sbi_virq_init(u32 init_virq_cap);

/*
 * Query whether the VIRQ subsystem is initialized.
 */
bool sbi_virq_is_inited(void);

#endif
