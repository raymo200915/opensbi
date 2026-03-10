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
#include <sbi_utils/fdt/fdt_domain.h>
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

struct hwirq_target_fill_info {
	struct aplic_data *pd;
};

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

static int irqchip_aplic_fill_hwirq_targets(void *fdt, int domain_offset,
					    void *opaque)
{
	int len;
	const fdt32_t *val;
	struct hwirq_target_fill_info *info = opaque;
	struct aplic_data *pd;
	u32 boot_hartindex;

	if (!fdt || !info || !info->pd)
		return 0;

	pd = info->pd;
	boot_hartindex = irqchip_aplic_domain_boot_hartindex(fdt, domain_offset);

	val = fdt_getprop(fdt, domain_offset, "opensbi,host-irqs", &len);
	if (val && len >= 8) {
		u32 i, pairs = len / 8;
		for (i = 0; i < pairs; i++) {
			u32 first = fdt32_to_cpu(val[i * 2]);
			u32 count = fdt32_to_cpu(val[i * 2 + 1]);
			u32 last = first + count - 1;
			u32 hwirq;

			for (hwirq = first; hwirq <= last; hwirq++) {
				if (hwirq == 0 || hwirq > pd->num_source)
					continue;
				if (pd->hwirq_target_hartindex[hwirq] == -1U)
					pd->hwirq_target_hartindex[hwirq] = boot_hartindex;
			}
		}
	}

	return 0;
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
		struct hwirq_target_fill_info info = {
			.pd = pd,
		};
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

		fdt_iterate_each_domain((void *)fdt, &info,
					irqchip_aplic_fill_hwirq_targets);
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
