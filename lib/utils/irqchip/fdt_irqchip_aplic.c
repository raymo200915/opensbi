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
#include <sbi/sbi_scratch.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <sbi_utils/irqchip/fdt_irqchip.h>
#include <sbi_utils/irqchip/aplic.h>

static int irqchip_aplic_update_idc_map(const void *fdt, int nodeoff,
					struct aplic_data *pd)
{
	int i, err, count, cpu_offset, cpu_intc_offset;
	u32 phandle, hartid, hartindex;
	const fdt32_t *val;

	val = fdt_getprop(fdt, nodeoff, "interrupts-extended", &count);
	if (!val || count < sizeof(fdt32_t))
		return SBI_EINVAL;
	count = count / sizeof(fdt32_t);

	for (i = 0; i < count; i += 2) {
		phandle = fdt32_to_cpu(val[i]);

		cpu_intc_offset = fdt_node_offset_by_phandle(fdt, phandle);
		if (cpu_intc_offset < 0)
			continue;

		cpu_offset = fdt_parent_offset(fdt, cpu_intc_offset);
		if (cpu_offset < 0)
			continue;

		err = fdt_parse_hart_id(fdt, cpu_offset, &hartid);
		if (err)
			continue;

		hartindex = sbi_hartid_to_hartindex(hartid);
		if (hartindex == -1U)
			continue;

		pd->idc_map[i / 2] = hartindex;
	}

	return 0;
}

static u32 irqchip_aplic_domain_boot_hartindex(void *fdt, int domain_offset)
{
	int len, cpu_offset;
	const fdt32_t *val;

	val = fdt_getprop(fdt, domain_offset, "boot-hart", &len);
	if (val && len >= 4) {
		cpu_offset = fdt_node_offset_by_phandle(fdt,
							fdt32_to_cpu(*val));
		if (cpu_offset >= 0) {
			u32 hartid;
			if (!fdt_parse_hart_id(fdt, cpu_offset, &hartid)) {
				u32 hidx = sbi_hartid_to_hartindex(hartid);
				if (sbi_hartindex_valid(hidx))
					return hidx;
			}
		}
	}

	return current_hartindex();
}

static void irqchip_aplic_fill_hwirq_targets_from_sysirq(const void *fdt,
							 int aplic_nodeoff,
							 struct aplic_data *pd)
{
	int chosen_off, nodeoff;
	int len, rc, index;
	const fdt32_t *val;
	u32 boot_hartindex;

	if (!fdt || aplic_nodeoff < 0 || !pd || !pd->hwirq_target_hartindex)
		return;

	chosen_off = fdt_path_offset(fdt, "/chosen/opensbi-domains");
	if (chosen_off < 0)
		return;

	fdt_for_each_subnode(nodeoff, fdt, chosen_off) {
		if (fdt_node_check_compatible(fdt, nodeoff,
					      "opensbi,mpxy-sysirq"))
			continue;

		val = fdt_getprop(fdt, nodeoff, "opensbi,domain", &len);
		if (!val || len < 4)
			continue;

		rc = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(*val));
		if (rc < 0)
			continue;

		boot_hartindex = irqchip_aplic_domain_boot_hartindex((void *)fdt,
								     rc);

		for (index = 0; ; index++) {
			struct fdt_phandle_args args;

			rc = fdt_parse_phandle_with_args(fdt, nodeoff,
							 "interrupts-extended",
							 "#interrupt-cells",
							 index, &args);
			if (rc)
				break;
			if (args.args_count < 1)
				continue;
			if (args.node_offset != aplic_nodeoff)
				continue;

			u32 hwirq = args.args[0];
			if (!hwirq || hwirq > pd->num_source)
				continue;

			if (pd->hwirq_target_hartindex[hwirq] == -1U)
				pd->hwirq_target_hartindex[hwirq] =
					boot_hartindex;
		}
	}
}

static int irqchip_aplic_cold_init(const void *fdt, int nodeoff,
				   const struct fdt_match *match)
{
	int rc;
	struct aplic_data *pd;

	sbi_printf("APLIC: matched node compatible %s\n", match->compatible);

	pd = sbi_zalloc(sizeof(*pd));
	if (!pd)
		return SBI_ENOMEM;

	rc = fdt_parse_aplic_node(fdt, nodeoff, pd);
	if (rc)
		goto fail_free_data;

	sbi_printf("APLIC: parsed addr=0x%lx targets_mmode=%d\n",
		   pd->addr, pd->targets_mmode);

	if (pd->num_idc) {
		pd->idc_map = sbi_zalloc(sizeof(*pd->idc_map) * pd->num_idc);
		if (!pd->idc_map) {
			rc = SBI_ENOMEM;
			goto fail_free_data;
		}

		rc = irqchip_aplic_update_idc_map(fdt, nodeoff, pd);
		if (rc)
			goto fail_free_idc_map;
	}

	/* Precompute target hartindex per HWIRQ from DT. */
	if (pd->targets_mmode) {
		u32 i;

		pd->hwirq_target_hartindex =
			sbi_zalloc(sizeof(*pd->hwirq_target_hartindex) *
				   (pd->num_source + 1));
		if (!pd->hwirq_target_hartindex) {
			rc = SBI_ENOMEM;
			goto fail_free_idc_map;
		}

		for (i = 0; i <= pd->num_source; i++)
			pd->hwirq_target_hartindex[i] = -1U;

		irqchip_aplic_fill_hwirq_targets_from_sysirq(fdt, nodeoff, pd);
	}

	rc = aplic_cold_irqchip_init(pd);
	if (rc)
		goto fail_free_idc_map;

	sbi_printf("APLIC: irqchip aplic cold init done\n");
	return 0;

fail_free_idc_map:
	if (pd->hwirq_target_hartindex)
		sbi_free(pd->hwirq_target_hartindex);
	if (pd->num_idc)
		sbi_free(pd->idc_map);
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
