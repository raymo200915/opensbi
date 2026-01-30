/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Virtual IRQ (VIRQ) courier/routing layer for OpenSBI.
 *
 * This header defines:
 *   1) VIRQ number allocation and (chip_uid,hwirq) <-> VIRQ mapping
 *   2) HWIRQ -> Domain routing rules (from DeviceTree "opensbi,host-irqs")
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
 *   - "opensbi,host-irqs" is treated as *routing metadata* only. It does not
 *     encode which privilege level receives the interrupt. Hardware delivery
 *     (MEI vs SEI) is determined by platform IRQ topology and interrupt-parent.
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
 * Keep the VIRQ pending queue small for bring-up.
 *
 * Current implementation behavior when queue overflows:
 *   - Drop the incoming VIRQ
 *   - Return SBI_ENOMEM
 */
#define VIRQ_QSIZE  32

/*
 * Reverse mapping table is chunked to avoid a single large static array.
 * VIRQ is used as an index into a chunk; chunks are allocated on demand.
 */
#define VIRQ_CHUNK_SHIFT   6U
#define VIRQ_CHUNK_SIZE    (1U << VIRQ_CHUNK_SHIFT)
#define VIRQ_CHUNK_MASK    (VIRQ_CHUNK_SIZE - 1U)

/* Minimum growth step for forward mapping vector and related metadata. */
#define VEC_GROW_MIN       16U

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
 * A routing rule maps a closed interval of HWIRQs [first..last] to a domain.
 *
 * Rules are populated once during cold boot while parsing the DT
 * opensbi-domains configuration (property "opensbi,host-irqs").
 *
 * DT encodes ranges as:
 *   opensbi,host-irqs = <first_hwirq count hartid> ...
 *
 * Internally they are converted to:
 *   [first .. last] (inclusive), where:
 *       last = first + count - 1
 *
 * Policy notes:
 *   - Range overlap is rejected and returns SBI_EALREADY.
 *   - If no rule matches, routing falls back to the root domain (&root).
 */
struct sbi_virq_route_rule {
	u32 first;
	u32 last;                 /* inclusive */
	u32 hartid;               /* target hart id */
	u32 hart_index;           /* target hart index */
	struct sbi_domain *dom;   /* owner domain */
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
	u32 q[VIRQ_QSIZE];

	/* Cached irqchip device. */
	struct sbi_irqchip_device *chip;
};

/*
 * Per-domain private VIRQ context.
 *
 * Attached to struct sbi_domain and contains per-hart states.
 */
struct sbi_domain_virq_priv {
	/* number of harts in the domain */
	u32 nharts;

	/* per-hart VIRQ state array */
	struct sbi_domain_virq_state st[];
};

/* Courier binding used when enqueuing a VIRQ. */
struct sbi_virq_courier_binding {
	/* destination domain */
	struct sbi_domain *dom;

	/* destination hart index */
	u32 hart_index;

	/* irqchip device that asserted the HWIRQ */
	struct sbi_irqchip_device *chip;

	/* VIRQ number to enqueue */
	u32 virq;
};

/*------------------------------------------------------------------------------
 * Public APIs
 *----------------------------------------------------------------------------*/

/*
 * Initialize the VIRQ allocator/mapping state.
 *
 * @init_virq_cap:
 *   Initial capacity in VIRQ bits (e.g., 256). Implementation may grow beyond.
 *
 * Return:
 *   SBI_OK on success
 *   SBI_ENOMEM on allocation failure
 */
int sbi_virq_map_init(u32 init_virq_cap);

/*
 * Create or get a stable mapping for (chip_uid, hwirq) -> VIRQ.
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
 *   Output pointer receiving the mapped/allocated VIRQ (non-zero on success).
 *
 * Return:
 *   SBI_OK on success
 *   SBI_ENOMEM on allocation failure
 *   SBI_ENOSPC if allocator cannot allocate
 *   SBI_EINVAL on invalid parameters
 */
int sbi_virq_map_one(u32 chip_uid, u32 hwirq,
		     bool allow_identity, u32 identity_limit, u32 *out_virq);

/*
 * Lookup existing mapping: (chip_uid, hwirq) -> VIRQ.
 *
 * @chip_uid: irqchip unique id
 * @hwirq:    host hwirq number
 * @out_virq: output VIRQ (non-zero on success)
 *
 * Return:
 *   SBI_OK if found
 *   SBI_ENOENT if not mapped
 *   SBI_EINVAL on invalid input
 */
int sbi_virq_hwirq2virq(u32 chip_uid, u32 hwirq, u32 *out_virq);

/*
 * Reverse lookup: VIRQ -> (chip_uid, hwirq).
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
 *   SBI_EINVAL if virq is 0, out of range, not allocated, or reverse entry
 *     missing
 */
int sbi_virq_virq2hwirq(u32 virq, u32 *out_chip_uid, u32 *out_hwirq);

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
 * Add a routing rule: [first .. first+count-1] -> (dom, hartid).
 *
 * @dom:
 *   Target domain that should receive HWIRQs in this range.
 *
 * @first:
 *   First HWIRQ number (inclusive).
 *
 * @count:
 *   Number of HWIRQs in the range.
 *
 * @hartid:
 *   Target hart id for this range.
 *
 * Return:
 *   SBI_OK on success
 *   SBI_EINVAL on invalid parameters
 *   SBI_ENOMEM on allocation failure
 *   SBI_EALREADY if the new range overlaps an existing rule
 */
int sbi_virq_route_add_range(struct sbi_domain *dom, u32 first, u32 count,
			     u32 hartid);

/*
 * Lookup destination (domain, hart) for a given HWIRQ.
 *
 * @hwirq:
 *   Incoming host HWIRQ number.
 *
 * Return:
 *   SBI_OK on success
 *   SBI_EINVAL on invalid parameters
 *
 * Notes:
 *   If no rule matches, defaults to (&root, current hart).
 */
int sbi_virq_route_lookup(u32 hwirq, struct sbi_domain **out_dom,
			  u32 *out_hartid, u32 *out_hartindex);

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
 * Pop the next pending VIRQ for the domain assigned to a specific hart.
 *
 * @hartid:
 *   Target hart id whose domain queue will be popped.
 *
 * Return:
 *   0 if none pending or state not available
 *   otherwise a non-zero VIRQ number
 */
u32 sbi_virq_pop(u32 hartid);

/*
 * Complete a previously couriered VIRQ for the specified hart/domain.
 *
 * @hartid:
 *   Target hart id.
 *
 * @virq:
 *   VIRQ to complete.
 */
void sbi_virq_complete(u32 hartid, u32 virq);

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
 *
 * @init_virq_cap:
 *   Initial VIRQ bitmap capacity in bits
 *
 * Return:
 *   SBI_OK on success
 *   SBI_ENOMEM on allocation failure
 *   Other SBI_E* error codes propagated from mapping init
 */
int sbi_virq_init(u32 init_virq_cap);

#endif
