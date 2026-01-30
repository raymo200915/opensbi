/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2022 Ventana Micro Systems Inc.
 *
 * Authors:
 *   Anup Patel <apatel@ventanamicro.com>
 */

#include <sbi/sbi_console.h>
#include <sbi/sbi_irqchip.h>
#include <sbi/sbi_list.h>
#include <sbi/sbi_platform.h>
#include <sbi/riscv_locks.h>

static SBI_LIST_HEAD(irqchip_list);

static struct {
	const struct sbi_irqchip_provider_ops *ops;
	void *ctx;
	u32 max_src;
} g_provider;

/* irqchip handler descriptor */
struct sbi_irqchip_desc {
	sbi_irqchip_irq_handler_t handler;
	void *priv;
};

static struct sbi_irqchip_desc irq_table[SBI_IRQCHIP_MAX_HWIRQS];
static spinlock_t irqchip_lock = SPIN_LOCK_INITIALIZER;

static int default_irqfn(void)
{
	return SBI_ENODEV;
}

static int (*ext_irqfn)(void) = default_irqfn;

int sbi_irqchip_process(void)
{
	return ext_irqfn();
}

void sbi_irqchip_add_device(struct sbi_irqchip_device *dev)
{
	sbi_list_add_tail(&dev->node, &irqchip_list);

	if (dev->irq_handle)
		ext_irqfn = dev->irq_handle;
}

int sbi_irqchip_init(struct sbi_scratch *scratch, bool cold_boot)
{
	int rc;
	const struct sbi_platform *plat = sbi_platform_ptr(scratch);
	struct sbi_irqchip_device *dev;

	if (cold_boot) {
		rc = sbi_platform_irqchip_init(plat);
		if (rc)
			return rc;
	}

	sbi_list_for_each_entry(dev, &irqchip_list, node) {
		if (!dev->warm_init)
			continue;
		rc = dev->warm_init(dev);
		if (rc)
			return rc;
	}

	if (ext_irqfn != default_irqfn)
		csr_set(CSR_MIE, MIP_MEIP);

	return 0;
}

void sbi_irqchip_exit(struct sbi_scratch *scratch)
{
	if (ext_irqfn != default_irqfn)
		csr_clear(CSR_MIE, MIP_MEIP);
}

int sbi_irqchip_register_provider(const struct sbi_irqchip_provider_ops *ops,
                        	  void *ctx, u32 max_src)
{
	if (!ops || !ops->claim || !ops->complete)
		return SBI_EINVAL;

	/* Bound max_hwirq to our table size for now */
	if (!max_src || max_src >= SBI_IRQCHIP_MAX_HWIRQS)
		return SBI_EINVAL;

	spin_lock(&irqchip_lock);
	g_provider.ops = ops;
	g_provider.ctx = ctx;
	g_provider.max_src = max_src;
	spin_unlock(&irqchip_lock);

	return SBI_OK;
}

u32 sbi_irqchip_get_max_src(void)
{
	if (g_provider.max_src < SBI_IRQCHIP_MAX_HWIRQS)
		return g_provider.max_src;

	return SBI_IRQCHIP_MAX_HWIRQS;
}

int sbi_irqchip_set_handler(u32 hwirq, sbi_irqchip_irq_handler_t handler,
			    void *priv)
{
	u32 max_hwirq;

	if (!handler)
		return SBI_EINVAL;
	/* hwirq 0 is reserved */
	if (!hwirq || hwirq >= SBI_IRQCHIP_MAX_HWIRQS)
		return SBI_EINVAL;

	spin_lock(&irqchip_lock);
	max_hwirq = g_provider.max_src;
	if (g_provider.ops && hwirq > max_hwirq) {
		spin_unlock(&irqchip_lock);
		return SBI_EINVAL;
	}
	irq_table[hwirq].handler = handler;
	irq_table[hwirq].priv = priv;
	spin_unlock(&irqchip_lock);

	return SBI_OK;
}

int sbi_irqchip_clear_handler(u32 hwirq)
{
	u32 max_hwirq;

	/* hwirq 0 is reserved */
	if (!hwirq || hwirq >= SBI_IRQCHIP_MAX_HWIRQS)
		return SBI_EINVAL;

	spin_lock(&irqchip_lock);
	max_hwirq = g_provider.max_src;
	if (g_provider.ops && hwirq > max_hwirq) {
		spin_unlock(&irqchip_lock);
		return SBI_EINVAL;
	}
	irq_table[hwirq].handler = NULL;
	irq_table[hwirq].priv = NULL;
	spin_unlock(&irqchip_lock);

	return SBI_OK;
}

void sbi_irqchip_mask_irq(u32 hwirq)
{
 	const struct sbi_irqchip_provider_ops *ops = g_provider.ops;
 	void *ctx = g_provider.ctx;
 	u32 max_hwirq = g_provider.max_src;
 
 	if (!ops || !ops->mask)
 		return;

 	if (!hwirq || hwirq > max_hwirq || hwirq >= SBI_IRQCHIP_MAX_HWIRQS)
 		return;

	sbi_printf("[IRQCHIP] mask hwirq %u\n", hwirq);
 	ops->mask(ctx, hwirq);
}
 
void sbi_irqchip_unmask_irq(u32 hwirq)
{
 	const struct sbi_irqchip_provider_ops *ops = g_provider.ops;
 	void *ctx = g_provider.ctx;
 	u32 max_hwirq = g_provider.max_src;
 
 	if (!ops || !ops->unmask)
 		return;

 	if (!hwirq || hwirq > max_hwirq || hwirq >= SBI_IRQCHIP_MAX_HWIRQS)
 		return;

	sbi_printf("[IRQCHIP] unmask hwirq %u\n", hwirq);
	ops->unmask(ctx, hwirq);
}

int sbi_irqchip_handle_external_irq(void)
{
	const struct sbi_irqchip_provider_ops *ops;
	void *ctx;
	u32 max_hwirq;

	/*
	 * Avoid locks in the IRQ fast path: take a snapshot of provider fields.
	 * Registration happens during cold init.
	 */
	ops = g_provider.ops;
	ctx = g_provider.ctx;
	max_hwirq = g_provider.max_src;

	if (!ops)
		return SBI_ENODEV;

	for (;;) {
		u32 hwirq = 0;
		int rc = ops->claim(ctx, &hwirq);

		if (rc == SBI_ENOENT)
			break;
		if (rc)
			return rc;

		if (!hwirq)
			break;

		sbi_printf("[IRQCHIP] claim hwirq %u\n", hwirq);

		if (hwirq > max_hwirq || hwirq >= SBI_IRQCHIP_MAX_HWIRQS) {
			/* Provider returned an out-of-range hwirq; avoid MMIO corruption */
			sbi_printf("[IRQCHIP] hwirq %u (max %u)\n", hwirq, max_hwirq);
			break;
		}

		/* Snapshot handler without locking; writes only happen at init */
		sbi_irqchip_irq_handler_t handler = irq_table[hwirq].handler;
		void *priv = irq_table[hwirq].priv;

		if (handler) {
			sbi_printf("[IRQCHIP] calling handler for hwirq %u\n", hwirq);
			handler(priv);
		}
		else
			sbi_printf("[IRQCHIP] unhandled hwirq %u\n", hwirq);

		ops->complete(ctx, hwirq);
		sbi_printf("[IRQCHIP] complete hwirq %u\n", hwirq);
	}

	return SBI_OK;
}
