/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Simple wired interrupt controller (INTC) dispatch for OpenSBI
 *
 * This is a minimal abstraction: wired INTC provider
 * responsible for claim/complete, and a small per-hwirq handler table.
 *
 * Goal: allow any OpenSBI driver to register a handler for a wired IRQ line.
 *
 * Copyright (c) 2025 RISCstar Solutions.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#ifndef __SBI_INTC_H__
#define __SBI_INTC_H__

#include <sbi/sbi_types.h>

typedef int (*sbi_intc_irq_handler_t)(u32 hwirq, void *priv);

struct sbi_intc_provider_ops {
	/*
	 * Claim a pending wired interrupt on current hart.
	 * Returns:
	 *   SBI_OK      : *hwirq is valid
	 *   SBI_ENOENT  : no pending wired interrupt
	 *   <0          : error
	 */
	int (*claim)(void *ctx, u32 *hwirq);

	/*
	 * Complete/acknowledge a previously claimed hwirq (if required by HW).
	 * Some HW may not require an explicit completion.
	 */
	void (*complete)(void *ctx, u32 hwirq);

	/*
	 * Optional: mask/unmask a wired interrupt line.
	 *
	 * These are required for reliable couriering of level-triggered device
	 * interrupts to S-mode: mask in M-mode before enqueueing, and unmask
	 * after S-mode has cleared the device interrupt source.
	 */
	void (*mask)(void *ctx, u32 hwirq);
	void (*unmask)(void *ctx, u32 hwirq);
};

/*
 * Register the active wired interrupt provider.
 * - max_hwirq specifies the highest valid hwirq ID.
 */
int sbi_intc_register_provider(const struct sbi_intc_provider_ops *ops,
			       void *ctx, u32 max_hwirq);

int sbi_intc_set_handler(u32 hwirq, sbi_intc_irq_handler_t handler, void *priv);
int sbi_intc_clear_handler(u32 hwirq);
void sbi_intc_mask_irq(u32 hwirq);
void sbi_intc_unmask_irq(u32 hwirq);
/* Can be used as OpenSBI "external interrupt handler" (irqchip device hook) */
int sbi_intc_handle_external_irq(void);

#endif
