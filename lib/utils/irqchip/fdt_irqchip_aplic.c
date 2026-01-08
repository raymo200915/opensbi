/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2021 Western Digital Corporation or its affiliates.
 * Copyright (c) 2022 Ventana Micro Systems Inc.
 *
 * Authors:
 *   Anup Patel <anup.patel@wdc.com>
 */

#include <libfdt.h>
#include <sbi/riscv_asm.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_heap.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi_utils/irqchip/fdt_irqchip.h>
#include <sbi_utils/irqchip/aplic.h>

#ifdef APLIC_M_MODE_TEST //APLIC_QEMU_WIRED_TEST

static int force_uart_parent_to_root(void *fdt, u32 root_phandle)
{
	int uart_off = fdt_path_offset(fdt, "/soc/serial@10000000");
	if (uart_off < 0) {
		sbi_printf("APLIC TEST: UART node not found (off=%d)\n", uart_off);
		return uart_off;
	}

	if (!root_phandle) {
		sbi_printf("APLIC TEST: root phandle is 0\n");
		return SBI_EINVAL;
	}

	int rc = fdt_setprop_u32(fdt, uart_off, "interrupt-parent", root_phandle);
	if (rc) {
		sbi_printf("APLIC TEST: fdt_setprop_u32 failed rc=%d\n", rc);
		return rc;
	}

	/* Readback verify */
	{
		int len = 0;
		const fdt32_t *ip = fdt_getprop(fdt, uart_off, "interrupt-parent", &len);
		if (ip && len >= 4)
			sbi_printf("APLIC TEST: UART interrupt-parent now phandle=%u\n",
				   fdt32_to_cpu(*ip));
		else
			sbi_printf("APLIC TEST: UART interrupt-parent readback failed\n");
	}

	return 0;
}

static int find_root_aplic_node(const void *fdt)
{
	int off = -1;

	/*
	 * Prefer a semantic marker if present: root node often has "riscv,children".
	 * If not present in your DT, we fall back to matching reg base 0x0c000000.
	 */
	for (off = fdt_next_node(fdt, -1, NULL);
	     off >= 0;
	     off = fdt_next_node(fdt, off, NULL)) {

		if (!fdt_getprop(fdt, off, "riscv,children", NULL))
			continue;

		/* root APLIC should be compatible with riscv,aplic (or aplic-imsic) */
		if (fdt_node_check_compatible(fdt, off, "riscv,aplic") == 0 ||
		    fdt_node_check_compatible(fdt, off, "riscv,aplic-imsic") == 0)
			return off;
	}

	/* Test-only fallback for QEMU virt: root APLIC reg base 0x0c000000 */
	for (off = fdt_next_node(fdt, -1, NULL);
	     off >= 0;
	     off = fdt_next_node(fdt, off, NULL)) {

		if (fdt_node_check_compatible(fdt, off, "riscv,aplic") &&
		    fdt_node_check_compatible(fdt, off, "riscv,aplic-imsic"))
			continue;

		int len = 0;
		const fdt64_t *reg = fdt_getprop(fdt, off, "reg", &len);
		if (!reg || len < (int)(2 * sizeof(fdt64_t)))
			continue;

		u64 base = fdt64_to_cpu(reg[0]);
		if (base == 0x0c000000ULL)
			return off;
	}

	return -FDT_ERR_NOTFOUND;
}

static int init_root_aplic_once(void *fdt)
{
	static int done;
	int rc, i;
	int root_off;
	struct aplic_data *pd;
	u32 root_phandle;

	if (done)
		return 0;
	done = 1;

	root_off = find_root_aplic_node(fdt);
	if (root_off < 0) {
		sbi_printf("APLIC TEST: root APLIC node not found (err=%d)\n", root_off);
		return SBI_ENOENT;
	}

	pd = sbi_zalloc(sizeof(*pd));
	if (!pd)
		return SBI_ENOMEM;

	rc = fdt_parse_aplic_node(fdt, root_off, pd);
	if (rc) {
		sbi_printf("APLIC TEST: fdt_parse_aplic_node(root) failed rc=%d\n", rc);
		sbi_free(pd);
		return rc;
	}

	sbi_printf("APLIC TEST: root parsed addr=0x%lx targets_mmode=%d (nodeoff=%d)\n",
		   pd->addr, pd->targets_mmode, root_off);

	/* A1: clear delegation so wired sources stay in root */
	for (i = 0; i < APLIC_MAX_DELEGATE; i++) {
		pd->delegate[i].first_irq   = 0;
		pd->delegate[i].last_irq    = 0;
		pd->delegate[i].child_index = 0;
	}

	/* A1: patch UART interrupt-parent to root */
	root_phandle = fdt_get_phandle(fdt, root_off);
	sbi_printf("APLIC TEST: root phandle=%u\n", root_phandle);
	(void)force_uart_parent_to_root(fdt, root_phandle);

	/* Now init root aplic */
	rc = aplic_cold_irqchip_init(pd);
	if (rc) {
		sbi_printf("APLIC TEST: aplic_cold_irqchip_init(root) failed rc=%d\n", rc);
		sbi_free(pd);
		return rc;
	}

	sbi_printf("APLIC TEST: root aplic init done\n");
	return 0;
}
#endif

static int irqchip_aplic_cold_init(const void *fdt, int nodeoff,
				   const struct fdt_match *match)
{
	int rc;
	struct aplic_data *pd;

	sbi_printf("APLIC: matched node compatible %s\n", match->compatible);

#ifdef APLIC_M_MODE_TEST //APLIC_QEMU_WIRED_TEST
	/*
	 * IMPORTANT:
	 * Even if the framework only calls us for the child domain,
	 * we proactively locate+init the root domain once, and apply A1 patches.
	 */
	(void)init_root_aplic_once((void *)fdt);
#endif
	pd = sbi_zalloc(sizeof(*pd));
	if (!pd)
		return SBI_ENOMEM;

	rc = fdt_parse_aplic_node(fdt, nodeoff, pd);
	if (rc)
		goto fail_free_data;

	sbi_printf("APLIC: parsed addr=0x%lx targets_mmode=%d\n",
		   pd->addr, pd->targets_mmode);

	rc = aplic_cold_irqchip_init(pd);
	if (rc)
		goto fail_free_data;

	sbi_printf("APLIC: irqchip_aplic_cold_init done\n");
	return 0;

fail_free_data:
	sbi_free(pd);
	return rc;
}

static const struct fdt_match irqchip_aplic_match[] = {
	{ .compatible = "riscv,aplic" },
	{ },
};

const struct fdt_driver fdt_irqchip_aplic = {
	.match_table = irqchip_aplic_match,
	.init = irqchip_aplic_cold_init,
};
