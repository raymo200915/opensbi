// SPDX-License-Identifier: BSD-2-Clause
/*
 * wgchecker model support
 *
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#include <libfdt.h>
#include <sbi/riscv_io.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_string.h>
#include <wgchecker.h>
#include <sbi_utils/fdt/fdt_helper.h>

struct wgchecker_checker {
	char name[32];
	u64 mmio_base;
	u64 mmio_size;
	u32 slot_count;
	u32 range_count;
	struct wgchecker_range *ranges;
};

struct wgchecker_platform_ctx {
	u32 checker_count;
	struct wgchecker_checker *checkers;
};

static struct wgchecker_platform_ctx *wgchecker_platform;

static void wgchecker_free_platform_ctx(struct wgchecker_platform_ctx *platform)
{
	u32 i;

	if (!platform)
		return;

	for (i = 0; i < platform->checker_count; i++)
		sbi_free(platform->checkers[i].ranges);

	sbi_free(platform->checkers);
	sbi_free(platform);
}

static void wgchecker_write64(u64 addr, u64 val)
{
#if __riscv_xlen != 32
	writeq(val, (void *)(unsigned long)addr);
#else
	writel((u32)val, (void *)(unsigned long)addr);
	writel((u32)(val >> 32), (void *)(unsigned long)(addr + 4));
#endif
}

static void wgchecker_write32(u64 addr, u32 val)
{
	writel(val, (void *)(unsigned long)addr);
}

static u32 wgchecker_encode_slot_cfg(u32 policy, u32 a_field)
{
	u32 cfg = a_field & WGCHECKER_SLOT_CFG_A_MASK;

	if (policy & WGCHECKER_CONFIG_ER)
		cfg |= WGCHECKER_SLOT_CFG_ER;
	if (policy & WGCHECKER_CONFIG_EW)
		cfg |= WGCHECKER_SLOT_CFG_EW;
	if (policy & WGCHECKER_CONFIG_IR)
		cfg |= WGCHECKER_SLOT_CFG_IR;
	if (policy & WGCHECKER_CONFIG_IW)
		cfg |= WGCHECKER_SLOT_CFG_IW;
	if (policy & WGCHECKER_CONFIG_L)
		cfg |= WGCHECKER_SLOT_CFG_LOCK;

	return cfg;
}

static u64 wgchecker_slot_addr_encode(u64 addr)
{
	return addr >> 2;
}

static bool wgchecker_range_is_aligned(u64 base, u64 size)
{
	if (!size)
		return false;

	if (base & (WGCHECKER_MIN_ALIGN - 1))
		return false;
	if (size & (WGCHECKER_MIN_ALIGN - 1))
		return false;

	return true;
}

static void wgchecker_sort_ranges(struct wgchecker_checker *checker)
{
	struct wgchecker_range tmp;
	u32 i, j;

	for (i = 1; i < checker->range_count; i++) {
		tmp = checker->ranges[i];
		j = i;
		while (j > 0 && checker->ranges[j - 1].base > tmp.base) {
			checker->ranges[j] = checker->ranges[j - 1];
			j--;
		}
		checker->ranges[j] = tmp;
	}
}

static int wgchecker_compact_ranges(struct wgchecker_checker *checker)
{
	struct wgchecker_range *prev, *cur;
	u64 prev_end, cur_end;
	u32 i, out = 0;

	if (!checker->range_count)
		return 0;

	wgchecker_sort_ranges(checker);

	for (i = 0; i < checker->range_count; i++) {
		cur = &checker->ranges[i];
		cur_end = cur->base + cur->size;
		if (cur_end <= cur->base)
			return SBI_EINVAL;

		if (!out) {
			checker->ranges[out++] = *cur;
			continue;
		}

		prev = &checker->ranges[out - 1];
		prev_end = prev->base + prev->size;
		if (cur->base < prev_end)
			return SBI_EINVAL;

		if (cur->base == prev_end && cur->perm == prev->perm &&
		    cur->cfg == prev->cfg) {
			prev->size += cur->size;
			continue;
		}

		checker->ranges[out++] = *cur;
	}

	checker->range_count = out;
	return 0;
}

static bool
wgchecker_access_rule_is_full_checker(const struct wgchecker_checker *checker)
{
	return checker->slot_count == 1 && checker->range_count == 1;
}

static int wgchecker_get_slot_count(const struct wgchecker_checker *checker)
{
	return readl((void *)(unsigned long)(checker->mmio_base +
					     WGCHECKER_MMIO_NSLOTS));
}

static bool wgchecker_node_is_compatible(const void *fdt, int nodeoff)
{
	return !fdt_node_check_compatible(fdt, nodeoff, WGCHECKER_COMPAT) ||
	       !fdt_node_check_compatible(fdt, nodeoff, WGCHECKER_QEMU_COMPAT);
}

static bool wgchecker_is_partition_node(const void *fdt, int nodeoff)
{
	const char *name = fdt_get_name(fdt, nodeoff, NULL);

	return name && !sbi_strncmp(name, "partition@", 10);
}

static int wgchecker_parse_partition_region(const void *fdt, int nodeoff,
					     u64 *base, u64 *size)
{
	const fdt32_t *prop;
	int len;

	if (!base || !size)
		return SBI_EINVAL;

	prop = fdt_getprop(fdt, nodeoff, WGCHECKER_PROP_WG_REGION, &len);
	if (!prop)
		return SBI_ENOENT;
	if (len != (int)(4 * sizeof(fdt32_t)))
		return SBI_EINVAL;

	*base = ((u64)fdt32_to_cpu(prop[0]) << 32) | fdt32_to_cpu(prop[1]);
	*size = ((u64)fdt32_to_cpu(prop[2]) << 32) | fdt32_to_cpu(prop[3]);
	return 0;
}

static int wgchecker_parse_partition(const void *fdt, int checker_node,
				      int part_node,
				      struct wgchecker_range *range)
{
	u32 part_id, cfg;
	u64 perm;
	int rc;

	rc = fdt_parse_u32(fdt, part_node, "reg", &part_id);
	if (rc)
		return rc;
	if (part_id == WGCHECKER_PARTITION_UNPROTECTED_ID) {
		sbi_printf("[WG] checker %s uses reserved partition id 0x%x\n",
			   fdt_get_name(fdt, checker_node, NULL), part_id);
		return SBI_EINVAL;
	}

	rc = wgchecker_parse_partition_region(fdt, part_node, &range->base,
						 &range->size);
	if (rc)
		return rc;

	rc = fdt_parse_u64(fdt, part_node, WGCHECKER_PROP_SLOT_PERMISSIONS,
			   &perm);
	if (rc)
		return rc;
	range->perm = perm;

	rc = fdt_parse_u32(fdt, part_node, WGCHECKER_PROP_SLOT_CONFIG, &cfg);
	if (rc)
		return rc;
	if (cfg & ~WGCHECKER_CONFIG_MASK) {
		sbi_printf("[WG] checker %s invalid config=0x%x\n",
			   fdt_get_name(fdt, checker_node, NULL), cfg);
		return SBI_EINVAL;
	}
	range->cfg = cfg;

	if (!wgchecker_range_is_aligned(range->base, range->size)) {
		sbi_printf("[WG] checker %s unaligned base=0x%llx size=0x%llx\n",
			   fdt_get_name(fdt, checker_node, NULL),
			   (unsigned long long)range->base,
			   (unsigned long long)range->size);
		return SBI_EINVAL;
	}

	return 0;
}

static int wgchecker_validate_cell_property(const void *fdt, int nodeoff,
					    const char *property, u32 expected)
{
	const fdt32_t *val;
	int len;

	val = fdt_getprop(fdt, nodeoff, property, &len);
	if (!val || len != (int)sizeof(*val)) {
		sbi_printf("[WG] checker %s missing or malformed %s\n",
			   fdt_get_name(fdt, nodeoff, NULL), property);
		return SBI_EINVAL;
	}

	if (fdt32_to_cpu(val[0]) != expected) {
		sbi_printf("[WG] checker %s invalid %s=%u\n",
			   fdt_get_name(fdt, nodeoff, NULL), property,
			   fdt32_to_cpu(val[0]));
		return SBI_EINVAL;
	}

	return 0;
}

static int wgchecker_parse_checker(void *fdt, int checker_node,
				    struct wgchecker_checker *checker)
{
	u64 base = 0, size = 0;
	u32 nranges = 0;
	int part_node, rc;

	rc = wgchecker_validate_cell_property(fdt, checker_node,
					     "#address-cells",
					     WGCHECKER_ADDRESS_CELLS);
	if (rc)
		return rc;

	rc = wgchecker_validate_cell_property(fdt, checker_node,
					     "#size-cells",
					     WGCHECKER_SIZE_CELLS);
	if (rc)
		return rc;

	rc = wgchecker_validate_cell_property(fdt, checker_node,
					     "#access-controller-cells",
					     WGCHECKER_ACCESS_CONTROLLER_CELLS);
	if (rc)
		return rc;

	rc = fdt_get_node_addr_size(fdt, checker_node, 0, &base, &size);
	if (rc)
		return rc;

	checker->mmio_base = base;
	checker->mmio_size = size;
	checker->slot_count = wgchecker_get_slot_count(checker);
	if (!checker->slot_count) {
		sbi_printf("[WG] checker %s reported zero slots\n",
			   fdt_get_name(fdt, checker_node, NULL));
		return SBI_EINVAL;
	}
	sbi_snprintf(checker->name, sizeof(checker->name), "%s",
		     fdt_get_name(fdt, checker_node, NULL));

	fdt_for_each_subnode(part_node, fdt, checker_node) {
		if (!fdt_node_is_enabled(fdt, part_node) ||
		    !wgchecker_is_partition_node(fdt, part_node))
			continue;
		nranges++;
	}

	if (!nranges)
		return 0;

	checker->ranges = sbi_calloc(sizeof(*checker->ranges), nranges);
	if (!checker->ranges)
		return SBI_ENOMEM;

	checker->range_count = 0;
	fdt_for_each_subnode(part_node, fdt, checker_node) {
		if (!fdt_node_is_enabled(fdt, part_node) ||
		    !wgchecker_is_partition_node(fdt, part_node))
			continue;
		rc = wgchecker_parse_partition(fdt, checker_node, part_node,
						&checker->ranges[checker->range_count]);
		if (rc)
			return rc;
		checker->range_count++;
	}

	return wgchecker_compact_ranges(checker);
}

static void wgchecker_program_clear_slots(const struct wgchecker_checker *checker)
{
	u32 slot;

	for (slot = 1; slot < checker->slot_count; slot++) {
		wgchecker_write64(checker->mmio_base + WGCHECKER_MMIO_SLOT_BASE +
				   slot * WGCHECKER_MMIO_SLOT_STRIDE +
				   WGCHECKER_MMIO_SLOT_ADDR, 0);
		wgchecker_write64(checker->mmio_base + WGCHECKER_MMIO_SLOT_BASE +
				   slot * WGCHECKER_MMIO_SLOT_STRIDE +
				   WGCHECKER_MMIO_SLOT_PERM, 0);
		wgchecker_write32(checker->mmio_base + WGCHECKER_MMIO_SLOT_BASE +
				   slot * WGCHECKER_MMIO_SLOT_STRIDE +
				   WGCHECKER_MMIO_SLOT_CFG, 0);
	}
}

static void wgchecker_program_clear_slots_from(const struct wgchecker_checker *checker,
						u32 first_slot)
{
	u32 slot;

	if (first_slot >= checker->slot_count)
		return;

	for (slot = first_slot; slot < checker->slot_count; slot++) {
		wgchecker_write64(checker->mmio_base + WGCHECKER_MMIO_SLOT_BASE +
				   slot * WGCHECKER_MMIO_SLOT_STRIDE +
				   WGCHECKER_MMIO_SLOT_ADDR, 0);
		wgchecker_write64(checker->mmio_base + WGCHECKER_MMIO_SLOT_BASE +
				   slot * WGCHECKER_MMIO_SLOT_STRIDE +
				   WGCHECKER_MMIO_SLOT_PERM, 0);
		wgchecker_write32(checker->mmio_base + WGCHECKER_MMIO_SLOT_BASE +
				   slot * WGCHECKER_MMIO_SLOT_STRIDE +
				   WGCHECKER_MMIO_SLOT_CFG, 0);
	}
}

static int wgchecker_program_checker(const struct wgchecker_checker *checker)
{
	u64 prev_end = 0;
	u32 required_slots = 0, slot = 1, i;

	wgchecker_write64(checker->mmio_base + WGCHECKER_MMIO_ERRCAUSE, 0);
	wgchecker_write64(checker->mmio_base + WGCHECKER_MMIO_ERRADDR, 0);

	if (wgchecker_access_rule_is_full_checker(checker)) {
		wgchecker_program_clear_slots(checker);
		wgchecker_write64(checker->mmio_base + WGCHECKER_MMIO_SLOT_BASE +
				   checker->slot_count * WGCHECKER_MMIO_SLOT_STRIDE +
				   WGCHECKER_MMIO_SLOT_PERM,
				   checker->ranges[0].perm);
		wgchecker_write32(checker->mmio_base + WGCHECKER_MMIO_SLOT_BASE +
				   checker->slot_count * WGCHECKER_MMIO_SLOT_STRIDE +
				   WGCHECKER_MMIO_SLOT_CFG,
				   wgchecker_encode_slot_cfg(checker->ranges[0].cfg,
							      WGCHECKER_SLOT_CFG_A_TOR));
		return 0;
	}

	for (i = 0; i < checker->range_count; i++) {
		if (!i || checker->ranges[i].base != prev_end)
			required_slots++;
		required_slots++;
		prev_end = checker->ranges[i].base + checker->ranges[i].size;
	}

	if (required_slots > checker->slot_count - 1)
		return SBI_EINVAL;

	prev_end = 0;
	for (i = 0; i < checker->range_count; i++) {
		const struct wgchecker_range *range = &checker->ranges[i];
		u64 end = range->base + range->size;

		if (!i || range->base != prev_end) {
			wgchecker_write64(checker->mmio_base +
					   WGCHECKER_MMIO_SLOT_BASE +
					   slot * WGCHECKER_MMIO_SLOT_STRIDE +
					   WGCHECKER_MMIO_SLOT_ADDR,
					   wgchecker_slot_addr_encode(range->base));
			wgchecker_write64(checker->mmio_base +
					   WGCHECKER_MMIO_SLOT_BASE +
					   slot * WGCHECKER_MMIO_SLOT_STRIDE +
					   WGCHECKER_MMIO_SLOT_PERM, 0);
			wgchecker_write32(checker->mmio_base +
					   WGCHECKER_MMIO_SLOT_BASE +
					   slot * WGCHECKER_MMIO_SLOT_STRIDE +
					   WGCHECKER_MMIO_SLOT_CFG,
					   WGCHECKER_SLOT_CFG_A_OFF);
			slot++;
		}

		wgchecker_write64(checker->mmio_base + WGCHECKER_MMIO_SLOT_BASE +
				   slot * WGCHECKER_MMIO_SLOT_STRIDE +
				   WGCHECKER_MMIO_SLOT_ADDR,
				   wgchecker_slot_addr_encode(end));
		wgchecker_write64(checker->mmio_base + WGCHECKER_MMIO_SLOT_BASE +
				   slot * WGCHECKER_MMIO_SLOT_STRIDE +
				   WGCHECKER_MMIO_SLOT_PERM, range->perm);
		wgchecker_write32(checker->mmio_base + WGCHECKER_MMIO_SLOT_BASE +
				   slot * WGCHECKER_MMIO_SLOT_STRIDE +
				   WGCHECKER_MMIO_SLOT_CFG,
				   wgchecker_encode_slot_cfg(range->cfg,
							      WGCHECKER_SLOT_CFG_A_TOR));
		prev_end = end;
		slot++;
	}

	/*
	 * Keep the reset-time trusted-WID bypass slot alive until the new
	 * rule set is fully programmed, otherwise the DRAM checker can deny
	 * OpenSBI's own RAM accesses mid-update.
	 */
	wgchecker_program_clear_slots_from(checker, slot);

	return 0;
}

u32 wgchecker_count_platform_checkers(void *fdt)
{
	int checker_node;
	u32 count = 0;

	if (!fdt)
		return 0;

	for (checker_node = 0; checker_node >= 0;
	     checker_node = fdt_next_node(fdt, checker_node, NULL)) {
		if (!wgchecker_node_is_compatible(fdt, checker_node) ||
		    !fdt_node_is_enabled(fdt, checker_node))
			continue;
		if (fdt_getprop(fdt, checker_node, "#access-controller-cells", NULL))
			count++;
	}

	return count;
}

int wgchecker_init(void *fdt)
{
	struct wgchecker_platform_ctx *platform;
	int checker_node, rc;
	u32 count, idx = 0;

	wgchecker_cleanup();

	if (!fdt)
		return 0;

	count = wgchecker_count_platform_checkers(fdt);
	if (!count)
		return 0;

	platform = sbi_zalloc(sizeof(*platform));
	if (!platform)
		return SBI_ENOMEM;

	platform->checker_count = count;
	platform->checkers = sbi_calloc(sizeof(*platform->checkers), count);
	if (!platform->checkers) {
		sbi_free(platform);
		return SBI_ENOMEM;
	}

	for (checker_node = 0; checker_node >= 0;
	     checker_node = fdt_next_node(fdt, checker_node, NULL)) {
		if (!wgchecker_node_is_compatible(fdt, checker_node) ||
		    !fdt_node_is_enabled(fdt, checker_node))
			continue;
		if (!fdt_getprop(fdt, checker_node, "#access-controller-cells", NULL))
			continue;

		rc = wgchecker_parse_checker(fdt, checker_node,
					      &platform->checkers[idx]);
		if (rc) {
			sbi_printf("[WG] failed to parse checker %s err=%d\n",
				   fdt_get_name(fdt, checker_node, NULL), rc);
			wgchecker_free_platform_ctx(platform);
			return rc;
		}

		rc = wgchecker_program_checker(&platform->checkers[idx]);
		if (rc) {
			sbi_printf("[WG] failed to program checker %s err=%d\n",
				   platform->checkers[idx].name, rc);
			wgchecker_free_platform_ctx(platform);
			return rc;
		}
		idx++;
	}

	wgchecker_platform = platform;
	return 0;
}

void wgchecker_cleanup(void)
{
	if (!wgchecker_platform)
		return;

	wgchecker_free_platform_ctx(wgchecker_platform);
	wgchecker_platform = NULL;
}

u32 wgchecker_checker_count(void)
{
	return wgchecker_platform ? wgchecker_platform->checker_count : 0;
}
