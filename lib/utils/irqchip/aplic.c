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
#include <sbi/sbi_heap.h>
#include <sbi/sbi_virq.h>
#include <sbi_utils/irqchip/aplic.h>

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

static SBI_LIST_HEAD(aplic_list);
static void aplic_writel_msicfg(struct aplic_msicfg_data *msicfg,
				void *msicfgaddr, void *msicfgaddrH);

#ifdef APLIC_QEMU_WIRED_TEST

#define UART_QEMU_MMIO 0x10000000UL

static void aplic_test_uart_handler(void)
{
	volatile u8 *uart = (volatile u8 *)UART_QEMU_MMIO;

	/* Drain RX FIFO to clear the interrupt source */
	while (uart[0x05] & 0x01) {        /* LSR.DR */
		u8 ch = uart[0x00];            /* RBR */

		sbi_printf("[APLIC TEST] UART got '%c'(0x%02x)\n",
			   (ch >= 32 && ch < 127) ? ch : '.', ch);
	}

	/* (Optional) read IIR to acknowledge on some models */
	(void)uart[0x02]; /* IIR is at offset 2 when DLAB=0; */
}

static void aplic_hwirq_test_run(unsigned long aplic_addr)
{
	volatile u8 *uart = (volatile u8 *)UART_QEMU_MMIO;

	/* UART: enable RX interrupt */
	uart[0x02] = 0x07;          /* FCR enable+clear */
	uart[0x04] |= (1 << 3);     /* MCR.OUT2 */
	uart[0x01] |= 0x01;         /* IER.ERBFI */
	while (uart[0x05] & 0x01)   /* drain */
		(void)uart[0x00];

	sbi_printf("[APLIC TEST] Setup done. Type keys now.\n");
}
#endif

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

#ifdef APLIC_QEMU_WIRED_TEST
static int aplic_hwirq_handler(u32 hwirq, void *opaque)
{
	(void)opaque;

	sbi_printf("[APLIC] Enter registered hwirq %u raw handler callback\n",
		   hwirq);

	if (hwirq == 10)
		aplic_test_uart_handler();

	return SBI_OK;
}
#endif

static inline void *aplic_idc_base(unsigned long aplic_addr, u32 idc_index)
{
	return (void *)(aplic_addr + APLIC_IDC_BASE +
			(unsigned long)idc_index * APLIC_IDC_SIZE);
}

static void aplic_hwirq_mask(struct sbi_irqchip_device *chip, u32 hwirq)
{
	struct aplic_data *w = chip->chip_priv;

	if (!w || !hwirq)
		return;

	if (!w->addr || hwirq > w->num_source)
		return;

	/* Disable source */
	writel(hwirq, (void *)(w->addr + APLIC_CLRIENUM));
}

static void aplic_hwirq_unmask(struct sbi_irqchip_device *chip, u32 hwirq)
{
	struct aplic_data *w = chip->chip_priv;

	if (!w || !hwirq)
		return;

	if (!w->addr || hwirq > w->num_source)
		return;

	/* Enable source */
	writel(hwirq, (void *)(w->addr + APLIC_SETIENUM));
}

static int aplic_hwirq_claim(struct sbi_irqchip_device *chip, u32 *hwirq)
{
	struct aplic_data *w = chip->chip_priv;
	u32 hartid = current_hartid();
	int hidx = sbi_hartid_to_hartindex(hartid);
	void *idc;
	u32 v, id;

	if (!w || !hwirq)
		return SBI_EINVAL;

	if (!w->addr || hidx < 0 || (u32)hidx >= w->num_idc)
		return SBI_ENODEV;

	idc = aplic_idc_base(w->addr, (u32)hidx);

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
	if (id > w->num_source)
		return SBI_EINVAL;

	*hwirq = id;

	return SBI_OK;
}

static void aplic_hwirq_eoi(struct sbi_irqchip_device *chip, u32 hwirq)
{
	struct aplic_data *w = chip->chip_priv;
	u32 hartid = current_hartid();
	int hidx = sbi_hartid_to_hartindex(hartid);
	void *idc;

	sbi_printf("[APLIC] Enter regitered EOI of hwirq %u\n", hwirq);

	if (!w || !w->addr)
		return;
	if (hidx < 0 || (u32)hidx >= w->num_idc)
		return;

	idc = aplic_idc_base(w->addr, (u32)hidx);

	/* QEMU workaround: clear pending after source deassert for level IRQ */
	writel(hwirq, idc + APLIC_CLRIPNUM);
}

static void aplic_set_target(struct sbi_irqchip_device *chip, u32 hwirq,
			     u32 hart_idx)
{
	unsigned long idc;
	struct aplic_data *w = chip->chip_priv;

	idc = w->addr + APLIC_IDC_BASE + hart_idx * APLIC_IDC_SIZE;

	writel((hart_idx << APLIC_TARGET_HART_IDX_SHIFT) |
	       APLIC_DEFAULT_PRIORITY,
	       (void *)(w->addr + APLIC_TARGET_BASE + (hwirq - 1) * 4));

	/* IDC delivery */
	writel(APLIC_ENABLE_IDELIVERY, (void *)(idc + APLIC_IDC_IDELIVERY));
	writel(APLIC_ENABLE_ITHRESHOLD, (void *)(idc + APLIC_IDC_ITHRESHOLD));
}

static int aplic_hwirq_setup(struct sbi_irqchip_device *chip, u32 hwirq)
{
	struct aplic_data *w = chip->chip_priv;

	/* APLIC: sourcecfg/target/enable */
	writel(APLIC_SOURCECFG_SM_LEVEL_HIGH,
	       (void *)(w->addr + APLIC_SOURCECFG_BASE + (hwirq - 1) * 4));

	writel(hwirq, (void *)(w->addr + APLIC_SETIENUM));

	/* Direct mode for aia=aplic: DM=0 => don't set DM bit */
	writel(APLIC_DOMAINCFG_IE | APLIC_DOMAINCFG_BE,
	       (void *)(w->addr + APLIC_DOMAINCFG));

	/* Enable MEIE + global MIE */
	csr_set(CSR_MIE, (1UL << 11));  /* MEIE */
	csr_set(CSR_MSTATUS, MSTATUS_MIE);

	return SBI_OK;
}

static int aplic_process_hwirqs(struct sbi_irqchip_device *chip)
{
	if (!chip)
		return SBI_ENODEV;

	for (;;) {
		u32 hwirq = 0;
		int rc = aplic_hwirq_claim(chip, &hwirq);

		if (rc == SBI_ENOENT)
			break;
		if (rc)
			return rc;

		if (!hwirq)
			break;

		sbi_printf("[APLIC] IDC_TOPI_ID from CLAIMI (hwirq) %u\n",
			   hwirq);

		if (hwirq > chip->num_hwirq) {
			sbi_printf("[APLIC] hwirq %u > max (num_hwirq) %u)\n",
				   hwirq, chip->num_hwirq);
			break;
		}

		sbi_irqchip_process_hwirq(chip, hwirq);
	}

	return SBI_OK;
}

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

	if (aplic->num_idc) {
		for (i = 0; i < aplic->num_idc; i++) {
			sbi_printf("[APLIC] hart index %u\n", aplic->idc_map[i]);
			sbi_hartmask_set_hartindex(aplic->idc_map[i],
						   &aplic->irqchip.target_harts);
		}
	} else {
		sbi_hartmask_set_all(&aplic->irqchip.target_harts);
	}

	/* Register irqchip device */
	aplic->irqchip.id = aplic->unique_id;
	aplic->irqchip.num_hwirq = aplic->num_source + 1;
	aplic->irqchip.chip_priv = aplic;
	aplic->irqchip.hwirq_mask = aplic_hwirq_mask;
	aplic->irqchip.hwirq_unmask = aplic_hwirq_unmask;
	aplic->irqchip.hwirq_eoi = aplic_hwirq_eoi;
	/*
	 * Only the domain that directly injects interrupts into M-mode external
	 * interrupt line should provide process_hwirqs().
	 *
	 * The other domain (e.g. S-mode) may still be registered so that its
	 * other ops (mask/unmask/config/etc.) can be used, but it must not
	 * claim to be the external interrupt line provider.
	 */
	if (aplic->targets_mmode)
		aplic->irqchip.process_hwirqs = aplic_process_hwirqs;
	aplic->irqchip.hwirq_setup = aplic_hwirq_setup;
	rc = sbi_irqchip_add_device(&aplic->irqchip);
	if (rc)
		return rc;

	/* Attach to the aplic list */
	sbi_list_add_tail(&aplic->node, &aplic_list);
#ifdef APLIC_QEMU_WIRED_TEST
	rc = sbi_irqchip_register_handler(&aplic->irqchip, 1, aplic->num_source,
					  aplic_hwirq_handler, NULL);
	if (rc)
		return rc;

	/* Enable test in M-mode before jumping to any payload */
	aplic_hwirq_test_run(aplic->addr);
#elif APLIC_QEMU_VIRQ_TEST
	/*
	 * Register one global courier handler on the M-mode host irqchip.
	 * Domain routing is resolved in VIRQ layer (hwirq -> domain queue).
	 */
	if (aplic->targets_mmode) {
		struct sbi_virq_courier_ctx *ctx;

		rc = sbi_virq_init(aplic->num_source);
		if (rc)
			return rc;

		ctx = sbi_zalloc(sizeof(*ctx));
		if (!ctx)
			return SBI_ENOMEM;

		ctx->chip = &aplic->irqchip;
		aplic_set_target(&aplic->irqchip, 10, !aplic->num_idc ? 0 :
				 aplic->num_idc - 1);
		rc = sbi_irqchip_register_handler(&aplic->irqchip,
						  10, 1,
						  sbi_virq_courier_handler,
						  ctx);
		if (rc)
			return rc;
	}
#endif
	return 0;
}
