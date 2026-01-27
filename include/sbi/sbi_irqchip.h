/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 Ventana Micro Systems Inc.
 *
 * Authors:
 *   Anup Patel <apatel@ventanamicro.com>
 */

#ifndef __SBI_IRQCHIP_H__
#define __SBI_IRQCHIP_H__

#include <sbi/sbi_list.h>
#include <sbi/sbi_types.h>

struct sbi_scratch;

/** irqchip hardware device */
struct sbi_irqchip_device {
	/** Node in the list of irqchip devices */
	struct sbi_dlist node;

	/** Initialize per-hart state for the current hart */
	int (*warm_init)(struct sbi_irqchip_device *dev);

	/** Handle an IRQ from this irqchip */
	int (*irq_handle)(void);
};

/* Handler for a specified hwirq */
typedef int (*sbi_irqchip_irq_handler_t)(void *priv);

/* Provider operations */
struct sbi_irqchip_provider_ops {
	/*
	 * Claim a pending wired interrupt on current hart.
	 * Returns:
	 *   SBI_OK      : *hwirq is valid
	 *   SBI_ENOENT  : no pending wired interrupt
	 *   <0          : error
	 */
	int (*claim)(void *ctx, u32 *hwirq);

	/*
	 * Complete/acknowledge a previously claimed wired interrupt
	 * (if required by HW).
	 * Some HW may not require an explicit completion.
	 */
	void (*complete)(void *ctx, u32 hwirq);

	/*
	 * mask/unmask a wired interrupt line.
	 *
	 * These are required for reliable couriering of level-triggered device
	 * interrupts to S-mode: mask in M-mode before enqueueing, and unmask
	 * after S-mode has cleared the device interrupt source.
	 */
	void (*mask)(void *ctx, u32 hwirq);
	void (*unmask)(void *ctx, u32 hwirq);
};

/**
 * Process external interrupts
 *
 * This function is called by sbi_trap_handler() to handle external
 * interrupts.
 *
 * @param regs pointer for trap registers
 */
int sbi_irqchip_process(void);

/** Register an irqchip device to receive callbacks */
void sbi_irqchip_add_device(struct sbi_irqchip_device *dev);

/** Initialize interrupt controllers */
int sbi_irqchip_init(struct sbi_scratch *scratch, bool cold_boot);

/** Exit interrupt controllers */
void sbi_irqchip_exit(struct sbi_scratch *scratch);

/*
 * Register the active wired interrupt provider.
 * - max_hwirq specifies the highest valid hwirq ID.
 */
int sbi_irqchip_register_provider(const struct sbi_irqchip_provider_ops *ops,
				  void *ctx, u32 max_hwirq);

u32 sbi_irqchip_get_max_src(void);

/* Set/clear handler for a specified hwirq */
int sbi_irqchip_set_handler(u32 hwirq, sbi_irqchip_irq_handler_t handler,
			    void *priv);
int sbi_irqchip_clear_handler(u32 hwirq);

void sbi_irqchip_mask_irq(u32 hwirq);
void sbi_irqchip_unmask_irq(u32 hwirq);

/* external interrupt handler (irqchip device hook) */
int sbi_irqchip_handle_external_irq(void);

#endif
