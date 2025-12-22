/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2025 RISCstar Solutions.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#include <sbi/sbi_error.h>
#include <sbi/sbi_intc.h>
#include <sbi/sbi_console.h>
#include <sbi/riscv_locks.h>

struct sbi_intc_desc {
	sbi_intc_irq_handler_t handler;
	void *priv;
};

static struct {
	const struct sbi_intc_provider_ops *ops;
	void *ctx;
	u32 max_hwirq;
} g_provider;

/*
 * Keep it simple for now: a fixed table. (Can be made dynamic later.)
 * hwirq 0 is reserved, wired controllers (e.g. APLIC), use source IDs in the
 * range 1..N, so the table is sized as (N + 1).
 */
#define SBI_INTC_MAX_HWIRQS  1025
static struct sbi_intc_desc irq_table[SBI_INTC_MAX_HWIRQS];

static spinlock_t intc_lock = SPIN_LOCK_INITIALIZER;

int sbi_intc_register_provider(const struct sbi_intc_provider_ops *ops,
			       void *ctx, u32 max_hwirq)
{
	if (!ops || !ops->claim || !ops->complete)
		return SBI_EINVAL;

	/* Bound max_hwirq to our table size for now */
	if (!max_hwirq || max_hwirq >= SBI_INTC_MAX_HWIRQS)
		return SBI_EINVAL;

	spin_lock(&intc_lock);
	g_provider.ops = ops;
	g_provider.ctx = ctx;
	g_provider.max_hwirq = max_hwirq;
	spin_unlock(&intc_lock);

	return SBI_OK;
}

int sbi_intc_set_handler(u32 hwirq, sbi_intc_irq_handler_t handler, void *priv)
{
	u32 max_hwirq;

	if (!handler)
		return SBI_EINVAL;
	/* hwirq 0 is reserved */
	if (!hwirq || hwirq >= SBI_INTC_MAX_HWIRQS)
		return SBI_EINVAL;

	spin_lock(&intc_lock);
	max_hwirq = g_provider.max_hwirq;
	if (g_provider.ops && hwirq > max_hwirq) {
		spin_unlock(&intc_lock);
		return SBI_EINVAL;
	}
	irq_table[hwirq].handler = handler;
	irq_table[hwirq].priv = priv;
	spin_unlock(&intc_lock);

	return SBI_OK;
}

int sbi_intc_clear_handler(u32 hwirq)
{
	u32 max_hwirq;

	/* hwirq 0 is reserved */
	if (!hwirq || hwirq >= SBI_INTC_MAX_HWIRQS)
		return SBI_EINVAL;

	spin_lock(&intc_lock);
	max_hwirq = g_provider.max_hwirq;
	if (g_provider.ops && hwirq > max_hwirq) {
		spin_unlock(&intc_lock);
		return SBI_EINVAL;
	}
	irq_table[hwirq].handler = NULL;
	irq_table[hwirq].priv = NULL;
	spin_unlock(&intc_lock);

	return SBI_OK;
}

int sbi_intc_handle_external_irq(void)
{
	const struct sbi_intc_provider_ops *ops;
	void *ctx;
	u32 max_hwirq;

	/*
	 * Avoid locks in the IRQ fast path: take a snapshot of provider fields.
	 * Registration happens during cold init.
	 */
	ops = g_provider.ops;
	ctx = g_provider.ctx;
	max_hwirq = g_provider.max_hwirq;

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

		sbi_printf("[INTC] claim hwirq=%u\n", hwirq);

		if (hwirq > max_hwirq || hwirq >= SBI_INTC_MAX_HWIRQS) {
			/* Provider returned an out-of-range hwirq; avoid MMIO corruption */
			sbi_printf("[INTC] invalid wired IRQ %u (max %u)", hwirq, max_hwirq);
			break;
		}

		if (!hwirq || hwirq > max_hwirq || hwirq >= SBI_INTC_MAX_HWIRQS) {
			/* Complete anyway to avoid stuck IRQs */
			ops->complete(ctx, hwirq);
			continue;
		}

		/* Snapshot handler without locking; writes only happen at init */
		sbi_intc_irq_handler_t handler = irq_table[hwirq].handler;
		void *priv = irq_table[hwirq].priv;

		if (handler)
			handler(hwirq, priv);
		else
			sbi_printf("[INTC] unhandled wired IRQ %u\n", hwirq);

		ops->complete(ctx, hwirq);
		sbi_printf("[INTC] complete hwirq=%u\n", hwirq);
	}

	return SBI_OK;
}
