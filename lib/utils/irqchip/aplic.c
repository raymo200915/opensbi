/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2021 Western Digital Corporation or its affiliates.
 * Copyright (c) 2022 Ventana Micro Systems Inc.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

#include <sbi/riscv_io.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_error.h>
#include <sbi_utils/irqchip/aplic.h>
#include <sbi/sbi_intc.h>

#define APLIC_MAX_IDC			(1UL << 14)
#define APLIC_MAX_SOURCE		1024

#define APLIC_DOMAINCFG		0x0000
#define APLIC_DOMAINCFG_IE		(1 << 8)
#define APLIC_DOMAINCFG_DM		(1 << 2)
#define APLIC_DOMAINCFG_BE		(1 << 0)

#define APLIC_SOURCECFG_BASE		0x0004
#define APLIC_SOURCECFG_D		(1 << 10)
#define APLIC_SOURCECFG_CHILDIDX_MASK	0x000003ff
#define APLIC_SOURCECFG_SM_MASK	0x00000007
#define APLIC_SOURCECFG_SM_INACTIVE	0x0
#define APLIC_SOURCECFG_SM_DETACH	0x1
#define APLIC_SOURCECFG_SM_EDGE_RISE	0x4
#define APLIC_SOURCECFG_SM_EDGE_FALL	0x5
#define APLIC_SOURCECFG_SM_LEVEL_HIGH	0x6
#define APLIC_SOURCECFG_SM_LEVEL_LOW	0x7

#define APLIC_MMSICFGADDR		0x1bc0
#define APLIC_MMSICFGADDRH		0x1bc4
#define APLIC_SMSICFGADDR		0x1bc8
#define APLIC_SMSICFGADDRH		0x1bcc

#define APLIC_xMSICFGADDRH_L		(1UL << 31)
#define APLIC_xMSICFGADDRH_HHXS_MASK	0x1f
#define APLIC_xMSICFGADDRH_HHXS_SHIFT	24
#define APLIC_xMSICFGADDRH_LHXS_MASK	0x7
#define APLIC_xMSICFGADDRH_LHXS_SHIFT	20
#define APLIC_xMSICFGADDRH_HHXW_MASK	0x7
#define APLIC_xMSICFGADDRH_HHXW_SHIFT	16
#define APLIC_xMSICFGADDRH_LHXW_MASK	0xf
#define APLIC_xMSICFGADDRH_LHXW_SHIFT	12
#define APLIC_xMSICFGADDRH_BAPPN_MASK	0xfff

#define APLIC_xMSICFGADDR_PPN_SHIFT	12

#define APLIC_xMSICFGADDR_PPN_HART(__lhxs) \
	((1UL << (__lhxs)) - 1)

#define APLIC_xMSICFGADDR_PPN_LHX_MASK(__lhxw) \
	((1UL << (__lhxw)) - 1)
#define APLIC_xMSICFGADDR_PPN_LHX_SHIFT(__lhxs) \
	((__lhxs))
#define APLIC_xMSICFGADDR_PPN_LHX(__lhxw, __lhxs) \
	(APLIC_xMSICFGADDR_PPN_LHX_MASK(__lhxw) << \
	 APLIC_xMSICFGADDR_PPN_LHX_SHIFT(__lhxs))

#define APLIC_xMSICFGADDR_PPN_HHX_MASK(__hhxw) \
	((1UL << (__hhxw)) - 1)
#define APLIC_xMSICFGADDR_PPN_HHX_SHIFT(__hhxs) \
	((__hhxs) + APLIC_xMSICFGADDR_PPN_SHIFT)
#define APLIC_xMSICFGADDR_PPN_HHX(__hhxw, __hhxs) \
	(APLIC_xMSICFGADDR_PPN_HHX_MASK(__hhxw) << \
	 APLIC_xMSICFGADDR_PPN_HHX_SHIFT(__hhxs))

#define APLIC_SETIP_BASE		0x1c00
#define APLIC_SETIPNUM			0x1cdc

#define APLIC_CLRIP_BASE		0x1d00
#define APLIC_CLRIPNUM			0x1ddc

#define APLIC_SETIE_BASE		0x1e00
#define APLIC_SETIENUM			0x1edc

#define APLIC_CLRIE_BASE		0x1f00
#define APLIC_CLRIENUM			0x1fdc

#define APLIC_SETIPNUM_LE		0x2000
#define APLIC_SETIPNUM_BE		0x2004

#define APLIC_TARGET_BASE		0x3004
#define APLIC_TARGET_HART_IDX_SHIFT	18
#define APLIC_TARGET_HART_IDX_MASK	0x3fff
#define APLIC_TARGET_GUEST_IDX_SHIFT	12
#define APLIC_TARGET_GUEST_IDX_MASK	0x3f
#define APLIC_TARGET_IPRIO_MASK	0xff
#define APLIC_TARGET_EIID_MASK	0x7ff

#define APLIC_IDC_BASE			0x4000
#define APLIC_IDC_SIZE			32

#define APLIC_IDC_IDELIVERY		0x00

#define APLIC_IDC_IFORCE		0x04

#define APLIC_IDC_ITHRESHOLD		0x08

#define APLIC_IDC_TOPI			0x18
#define APLIC_IDC_TOPI_ID_SHIFT	16
#define APLIC_IDC_TOPI_ID_MASK	0x3ff
#define APLIC_IDC_TOPI_PRIO_MASK	0xff

#define APLIC_IDC_CLAIMI		0x1c

#define APLIC_DEFAULT_PRIORITY		1
#define APLIC_DISABLE_IDELIVERY		0
#define APLIC_ENABLE_IDELIVERY		1
#define APLIC_DISABLE_ITHRESHOLD	1
#define APLIC_ENABLE_ITHRESHOLD		0

/*
 * Minimal APLIC wired INTC provider (M-mode, IDC path).
 *
 * NOTE: This is intentionally minimal: claim uses CLAIMI (TOPI encoding),
 * and completion is a no-op unless a quirk says otherwise.
 */
struct aplic_wired_ctx {
	unsigned long aplic_addr;  /* MMIO base */
	u32 num_idc;
	u32 num_src;
};

static struct aplic_wired_ctx g_aplic_wired;
static int g_aplic_wired_registered;

static SBI_LIST_HEAD(aplic_list);
static void aplic_writel_msicfg(struct aplic_msicfg_data *msicfg,
				void *msicfgaddr, void *msicfgaddrH);

static void aplic_init(struct aplic_data *aplic)
{
	struct aplic_delegate_data *deleg;
	u32 i, j, tmp;
	int locked;

	/* Set domain configuration to 0 */
	writel(0, (void *)(aplic->addr + APLIC_DOMAINCFG));

	/* Disable all interrupts */
	for (i = 0; i <= aplic->num_source; i += 32)
		writel(-1U, (void *)(aplic->addr + APLIC_CLRIE_BASE +
				     (i / 32) * sizeof(u32)));

	/* Set interrupt type and priority for all interrupts */
	for (i = 1; i <= aplic->num_source; i++) {
		/* Set IRQ source configuration to 0 */
		writel(0, (void *)(aplic->addr + APLIC_SOURCECFG_BASE +
			  (i - 1) * sizeof(u32)));
		/* Set IRQ target hart index and priority to 1 */
		writel(APLIC_DEFAULT_PRIORITY, (void *)(aplic->addr +
						APLIC_TARGET_BASE +
						(i - 1) * sizeof(u32)));
	}

	/* Configure IRQ delegation */
	for (i = 0; i < APLIC_MAX_DELEGATE; i++) {
		deleg = &aplic->delegate[i];
		if (!deleg->first_irq || !deleg->last_irq)
			continue;
		if (aplic->num_source < deleg->first_irq ||
		    aplic->num_source < deleg->last_irq)
			continue;
		if (deleg->child_index > APLIC_SOURCECFG_CHILDIDX_MASK)
			continue;
		if (deleg->first_irq > deleg->last_irq) {
			tmp = deleg->first_irq;
			deleg->first_irq = deleg->last_irq;
			deleg->last_irq = tmp;
		}
		for (j = deleg->first_irq; j <= deleg->last_irq; j++)
			writel(APLIC_SOURCECFG_D | deleg->child_index,
			       (void *)(aplic->addr + APLIC_SOURCECFG_BASE +
			       (j - 1) * sizeof(u32)));
	}

	/* Default initialization of IDC structures */
	for (i = 0; i < aplic->num_idc; i++) {
		writel(0, (void *)(aplic->addr + APLIC_IDC_BASE +
				   i * APLIC_IDC_SIZE + APLIC_IDC_IDELIVERY));
		writel(0, (void *)(aplic->addr + APLIC_IDC_BASE +
				   i * APLIC_IDC_SIZE + APLIC_IDC_IFORCE));
		writel(APLIC_DISABLE_ITHRESHOLD, (void *)(aplic->addr +
						  APLIC_IDC_BASE +
						  (i * APLIC_IDC_SIZE) +
						  APLIC_IDC_ITHRESHOLD));
	}

	/* MSI configuration */
	locked = readl((void *)(aplic->addr + APLIC_MMSICFGADDRH)) & APLIC_xMSICFGADDRH_L;
	if (aplic->targets_mmode && aplic->has_msicfg_mmode && !locked) {
		aplic_writel_msicfg(&aplic->msicfg_mmode,
				    (void *)(aplic->addr + APLIC_MMSICFGADDR),
				    (void *)(aplic->addr + APLIC_MMSICFGADDRH));
	}
	if (aplic->targets_mmode && aplic->has_msicfg_smode && !locked) {
		aplic_writel_msicfg(&aplic->msicfg_smode,
				    (void *)(aplic->addr + APLIC_SMSICFGADDR),
				    (void *)(aplic->addr + APLIC_SMSICFGADDRH));
	}
}

void aplic_reinit_all(void)
{
	struct aplic_data *aplic;

	sbi_list_for_each_entry(aplic, &aplic_list, node)
		aplic_init(aplic);
}

static void aplic_writel_msicfg(struct aplic_msicfg_data *msicfg,
				void *msicfgaddr, void *msicfgaddrH)
{
	u32 val;
	unsigned long base_ppn;

	/* Compute the MSI base PPN */
	base_ppn = msicfg->base_addr >> APLIC_xMSICFGADDR_PPN_SHIFT;
	base_ppn &= ~APLIC_xMSICFGADDR_PPN_HART(msicfg->lhxs);
	base_ppn &= ~APLIC_xMSICFGADDR_PPN_LHX(msicfg->lhxw, msicfg->lhxs);
	base_ppn &= ~APLIC_xMSICFGADDR_PPN_HHX(msicfg->hhxw, msicfg->hhxs);

	/* Write the lower MSI config register */
	writel((u32)base_ppn, msicfgaddr);

	/* Write the upper MSI config register */
	val = (((u64)base_ppn) >> 32) &
		APLIC_xMSICFGADDRH_BAPPN_MASK;
	val |= (msicfg->lhxw & APLIC_xMSICFGADDRH_LHXW_MASK)
		<< APLIC_xMSICFGADDRH_LHXW_SHIFT;
	val |= (msicfg->hhxw & APLIC_xMSICFGADDRH_HHXW_MASK)
		<< APLIC_xMSICFGADDRH_HHXW_SHIFT;
	val |= (msicfg->lhxs & APLIC_xMSICFGADDRH_LHXS_MASK)
		<< APLIC_xMSICFGADDRH_LHXS_SHIFT;
	val |= (msicfg->hhxs & APLIC_xMSICFGADDRH_HHXS_MASK)
		<< APLIC_xMSICFGADDRH_HHXS_SHIFT;
	writel(val, msicfgaddrH);
}

static int aplic_check_msicfg(struct aplic_msicfg_data *msicfg)
{
	if (APLIC_xMSICFGADDRH_LHXS_MASK < msicfg->lhxs)
		return SBI_EINVAL;

	if (APLIC_xMSICFGADDRH_LHXW_MASK < msicfg->lhxw)
		return SBI_EINVAL;

	if (APLIC_xMSICFGADDRH_HHXS_MASK < msicfg->hhxs)
		return SBI_EINVAL;

	if (APLIC_xMSICFGADDRH_HHXW_MASK < msicfg->hhxw)
		return SBI_EINVAL;

	return 0;
}

static inline void *aplic_idc_base(unsigned long aplic_addr, u32 idc_index)
{
	return (void *)(aplic_addr + APLIC_IDC_BASE +
			(unsigned long)idc_index * APLIC_IDC_SIZE);
}

static int aplic_wired_claim(void *ctx, u32 *hwirq)
{
	struct aplic_wired_ctx *w = ctx;
	u32 hartid = current_hartid();
	int hidx = sbi_hartid_to_hartindex(hartid);
	void *idc;
	u32 v, id;

	if (!w || !hwirq)
		return SBI_EINVAL;

	if (!w->aplic_addr || hidx < 0 || (u32)hidx >= w->num_idc)
		return SBI_ENODEV;

	idc = aplic_idc_base(w->aplic_addr, (u32)hidx);

	/*
	 * Read CLAIMI: returns TOPI value.
	 * ID==0 means spurious interrupt (spec-defined).
	 */
	v = readl(idc + APLIC_IDC_CLAIMI); /* dequeue */
	/*
	 * QEMU workaround: Read CLAIMI a second time since QEMU's APLIC model
	 * currently has a bug and may not clear pending on deassert after the
	 * first reading.
	 */
	if (readl(idc + APLIC_IDC_CLAIMI) != v)
		return SBI_ENOENT;

	id = (v >> APLIC_IDC_TOPI_ID_SHIFT) & APLIC_IDC_TOPI_ID_MASK;

	/* ID==0 means spurious / no pending wired interrupt */
	if (!id)
		return SBI_ENOENT;

	/* Bound check against DT-discovered num_src */
	if (id > w->num_src)
		return SBI_EINVAL;

	*hwirq = id;

	/* mask */
	writel(id, idc + APLIC_CLRIENUM);

	return SBI_OK;
}

static void aplic_wired_complete(void *ctx, u32 hwirq)
{
	struct aplic_wired_ctx *w = ctx;
	u32 hartid = current_hartid();
	int hidx = sbi_hartid_to_hartindex(hartid);
	void *idc;

	if (!w || !w->aplic_addr)
		return;
	if (hidx < 0 || (u32)hidx >= w->num_idc)
		return;

	idc = aplic_idc_base(w->aplic_addr, (u32)hidx);

	/* QEMU workaround: clear pending after source deassert for level IRQ */
	writel(hwirq, idc + APLIC_CLRIPNUM);

	/* unmask */
	writel(hwirq, idc + APLIC_SETIENUM);
}

static const struct sbi_intc_provider_ops aplic_wired_intc_ops = {
	.claim    = aplic_wired_claim,
	.complete = aplic_wired_complete,
};

int aplic_cold_irqchip_init(struct aplic_data *aplic)
{
	int rc;
	struct aplic_delegate_data *deleg;
	u32 first_deleg_irq, last_deleg_irq, i;

	/* Sanity checks */
	if (!aplic ||
	    !aplic->num_source || APLIC_MAX_SOURCE <= aplic->num_source ||
	    APLIC_MAX_IDC <= aplic->num_idc)
		return SBI_EINVAL;
	if (aplic->targets_mmode && aplic->has_msicfg_mmode) {
		rc = aplic_check_msicfg(&aplic->msicfg_mmode);
		if (rc)
			return rc;
	}
	if (aplic->targets_mmode && aplic->has_msicfg_smode) {
		rc = aplic_check_msicfg(&aplic->msicfg_smode);
		if (rc)
			return rc;
	}

	/* Init the APLIC registers */
	aplic_init(aplic);

	/*
	 * Add APLIC region to the root domain if:
	 * 1) It targets M-mode of any HART directly or via MSIs
	 * 2) All interrupts are delegated to some child APLIC
	 */
	first_deleg_irq = -1U;
	last_deleg_irq = 0;
	for (i = 0; i < APLIC_MAX_DELEGATE; i++) {
		deleg = &aplic->delegate[i];
		if (deleg->first_irq < first_deleg_irq)
			first_deleg_irq = deleg->first_irq;
		if (last_deleg_irq < deleg->last_irq)
			last_deleg_irq = deleg->last_irq;
	}

	if (aplic->targets_mmode ||
	    ((first_deleg_irq < last_deleg_irq) &&
	    (last_deleg_irq == aplic->num_source) &&
	    (first_deleg_irq == 1))) {
		rc = sbi_domain_root_add_memrange(aplic->addr, aplic->size, PAGE_SIZE,
						  SBI_DOMAIN_MEMREGION_MMIO |
						  SBI_DOMAIN_MEMREGION_M_READABLE |
						  SBI_DOMAIN_MEMREGION_M_WRITABLE);
		if (rc)
			return rc;
	}

	/*
	 * If APLIC targets M-mode (wired via IDC), we can use sbi_intc as the
	 * external interrupt handler. Do it once for now.
	 *
	 * IMPORTANT: Do NOT register a second irqchip device (avoids double
	 * handling). Instead, redirect this APLIC irqchip's irq_handle.
	 */
	if (aplic->targets_mmode && !g_aplic_wired_registered) {
		sbi_printf("M-mode: Register APLIC wired via IDC\n");

		g_aplic_wired.aplic_addr = aplic->addr;
		g_aplic_wired.num_idc    = aplic->num_idc;
		g_aplic_wired.num_src    = aplic->num_source;

		/* Register INTC provider for wired interrupts (claim/complete via IDC.CLAIMI) */
		sbi_intc_register_provider(&aplic_wired_intc_ops,
					   &g_aplic_wired,
					   g_aplic_wired.num_src);
		/*
		* CRITICAL:
		* Override the irqchip handler for the *existing* root APLIC irqchip device.
		* Otherwise OpenSBI will keep calling the original APLIC stub handler and
		* you'll see "unhandled local interrupt (error -1000)" on MEIP.
		*/
		aplic->irqchip.irq_handle = sbi_intc_handle_external_irq;

		g_aplic_wired_registered = 1;
	}

	/* Register irqchip device */
	sbi_irqchip_add_device(&aplic->irqchip);

	/* Attach to the aplic list */
	sbi_list_add_tail(&aplic->node, &aplic_list);

	return 0;
}
