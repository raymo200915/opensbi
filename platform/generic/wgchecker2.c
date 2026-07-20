// SPDX-License-Identifier: BSD-2-Clause
/*
 * wgchecker2 model support
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
#include <wgchecker2.h>
#include <sbi_utils/fdt/fdt_helper.h>

struct wgchecker2_checker {
	char name[32];
	u64 mmio_base;
	u64 mmio_size;
	u32 slot_count;
	u32 subordinate_count;
	bool full_checker_rule;
	u64 full_checker_perm;
	u32 range_count;
	struct wgchecker2_range *ranges;
};

struct wgchecker2_platform_ctx {
	u32 checker_count;
	struct wgchecker2_checker *checkers;
};

static struct wgchecker2_platform_ctx *wgchecker2_platform;

static void wgchecker2_free_platform_ctx(struct wgchecker2_platform_ctx *platform)
{
	u32 i;

	if (!platform)
		return;

	for (i = 0; i < platform->checker_count; i++)
		sbi_free(platform->checkers[i].ranges);

	sbi_free(platform->checkers);
	sbi_free(platform);
}

static u64 wgchecker2_read_cells(const fdt32_t *cells, int count)
{
	u64 val = 0;
	int i;

	for (i = 0; i < count; i++)
		val = (val << 32) | fdt32_to_cpu(cells[i]);

	return val;
}

static void wgchecker2_write64(u64 addr, u64 val)
{
#if __riscv_xlen != 32
	writeq(val, (void *)(unsigned long)addr);
#else
	writel((u32)val, (void *)(unsigned long)addr);
	writel((u32)(val >> 32), (void *)(unsigned long)(addr + 4));
#endif
}

static void wgchecker2_write32(u64 addr, u32 val)
{
	writel(val, (void *)(unsigned long)addr);
}

static u64 wgchecker2_slot_addr_encode(u64 addr)
{
	return addr >> 2;
}

static bool wgchecker2_range_is_aligned(u64 base, u64 size)
{
	if (!size)
		return false;

	if (base & (WGCHECKER2_MIN_ALIGN - 1))
		return false;
	if (size & (WGCHECKER2_MIN_ALIGN - 1))
		return false;

	return true;
}

static void wgchecker2_sort_ranges(struct wgchecker2_checker *checker)
{
	struct wgchecker2_range tmp;
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

static int wgchecker2_compact_ranges(struct wgchecker2_checker *checker)
{
	struct wgchecker2_range *prev, *cur;
	u64 prev_end, cur_end;
	u32 i, out = 0;

	if (!checker->range_count)
		return 0;

	wgchecker2_sort_ranges(checker);

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

		if (cur->base == prev_end && cur->perm == prev->perm) {
			prev->size += cur->size;
			continue;
		}

		checker->ranges[out++] = *cur;
	}

	checker->range_count = out;
	return 0;
}

static int wgchecker2_get_reg_cells(void *fdt, int resource_node,
				     int *addr_cells, int *size_cells)
{
	int parent;

	parent = fdt_parent_offset(fdt, resource_node);
	if (parent < 0)
		return SBI_EINVAL;

	*addr_cells = fdt_address_cells(fdt, parent);
	*size_cells = fdt_size_cells(fdt, parent);
	if (*addr_cells <= 0 || *addr_cells > 2 || *size_cells <= 0 ||
	    *size_cells > 2)
		return SBI_EINVAL;

	return 0;
}

static int wgchecker2_count_reg_entries(void *fdt, int resource_node,
					int reg_node)
{
	const fdt32_t *reg;
	int addr_cells, size_cells, entry_cells, len, rc;

	rc = wgchecker2_get_reg_cells(fdt, resource_node, &addr_cells,
					      &size_cells);
	if (rc)
		return rc;

	reg = fdt_getprop(fdt, reg_node, "reg", &len);
	if (!reg || len <= 0)
		return 0;

	entry_cells = addr_cells + size_cells;
	if (len % (entry_cells * (int)sizeof(fdt32_t)))
		return SBI_EINVAL;

	return len / (entry_cells * (int)sizeof(fdt32_t));
}

static int wgchecker2_parse_perms(void *fdt, int cfg_node, u64 **out_perms,
				  u32 *out_count)
{
	const fdt32_t *perms;
	u64 *vals;
	int len, i, count;

	*out_perms = NULL;
	*out_count = 0;

	perms = fdt_getprop(fdt, cfg_node, WGCHECKER2_PROP_PERMS, &len);
	if (!perms || len <= 0)
		return 0;

	if (len % (2 * (int)sizeof(fdt32_t)))
		return SBI_EINVAL;

	count = len / (2 * (int)sizeof(fdt32_t));
	vals = sbi_calloc(sizeof(*vals), count);
	if (!vals)
		return SBI_ENOMEM;

	for (i = 0; i < count; i++, perms += 2)
		vals[i] = wgchecker2_read_cells(perms, 2);

	*out_perms = vals;
	*out_count = count;
	return 0;
}

static int wgchecker2_fill_ranges(void *fdt, int resource_node, int reg_node,
				  const u64 *perms, u32 perm_count,
				  struct wgchecker2_range *ranges,
				  u32 range_count)
{
	const fdt32_t *reg;
	u64 base, size;
	int addr_cells, size_cells, entry_cells, len, i, rc;

	rc = wgchecker2_get_reg_cells(fdt, resource_node, &addr_cells,
					      &size_cells);
	if (rc)
		return rc;

	reg = fdt_getprop(fdt, reg_node, "reg", &len);
	if (!reg || len <= 0)
		return SBI_EINVAL;

	entry_cells = addr_cells + size_cells;
	for (i = 0; i < (int)range_count; i++, reg += entry_cells) {
		base = wgchecker2_read_cells(reg, addr_cells);
		size = wgchecker2_read_cells(reg + addr_cells, size_cells);
		if (!wgchecker2_range_is_aligned(base, size))
			return SBI_EINVAL;

		ranges[i].base = base;
		ranges[i].size = size;
		ranges[i].perm = perms[(perm_count == 1) ? 0 : i];
	}

	return 0;
}

static int wgchecker2_parse_checker_rules(void *fdt, int checker_node,
					  struct wgchecker2_checker *checker)
{
	const fdt32_t *subs;
	u64 *perms = NULL;
	int cfg_node, len, i, rc = 0, reg_count;
	u32 perm_count = 0;
	int child;

	subs = fdt_getprop(fdt, checker_node, WGCHECKER2_PROP_SUBORDINATES, &len);
	if (!subs || len <= 0)
		return 0;
	if (len % (int)sizeof(fdt32_t))
		goto err;

	checker->subordinate_count = len / sizeof(fdt32_t);
	if (!checker->slot_count)
		goto err;

	checker->ranges = sbi_calloc(sizeof(*checker->ranges),
				     checker->slot_count);
	if (!checker->ranges)
		return SBI_ENOMEM;

	for (i = 0; i < checker->subordinate_count; i++) {
		child = fdt_node_offset_by_phandle(fdt, fdt32_to_cpu(subs[i]));
		if (child < 0) {
			sbi_printf("[WG] checker %s has invalid subordinate"
				   " phandle[%d]=0x%x err=%d\n",
				   checker->name, i, fdt32_to_cpu(subs[i]), child);
			rc = child;
			goto err;
		}

		cfg_node = fdt_subnode_offset(fdt, child, WGCHECKER2_CFG_NODE);
		if (cfg_node < 0)
			continue;

		rc = wgchecker2_parse_perms(fdt, cfg_node, &perms, &perm_count);
		if (rc)
			goto err;
		if (!perm_count)
			continue;

		reg_count = wgchecker2_count_reg_entries(fdt, child, cfg_node);
		if (reg_count < 0)
			goto err;

		if (!reg_count && checker->subordinate_count == 1 &&
		    perm_count == 1) {
			if (checker->range_count)
				goto err;
			checker->full_checker_rule = true;
			checker->full_checker_perm = perms[0];
			sbi_free(perms);
			perms = NULL;
			continue;
		}

		if (!reg_count)
			reg_count = wgchecker2_count_reg_entries(fdt, child, child);
		if (reg_count <= 0)
			goto err;

		if (perm_count != 1 && perm_count != (u32)reg_count)
			goto err;
		if (checker->full_checker_rule)
			goto err;
		if (checker->range_count + reg_count > checker->slot_count)
			goto err;

		rc = wgchecker2_fill_ranges(
			fdt, child,
			(fdt_getprop(fdt, cfg_node, "reg", NULL) ? cfg_node : child),
			perms, perm_count,
			&checker->ranges[checker->range_count], reg_count);
		sbi_free(perms);
		perms = NULL;
		if (rc)
			goto err;

		checker->range_count += reg_count;
	}

	if (checker->full_checker_rule)
		return 0;

	return wgchecker2_compact_ranges(checker);

err:
	sbi_free(perms);
	return rc ? rc : SBI_EINVAL;
}

static int wgchecker2_parse_checker(void *fdt, int checker_node,
				    struct wgchecker2_checker *checker)
{
	const fdt32_t *val;
	u64 base = 0, size = 0;
	int len, rc;

	rc = fdt_get_node_addr_size(fdt, checker_node, 0, &base, &size);
	if (rc)
		return rc;

	val = fdt_getprop(fdt, checker_node, WGCHECKER2_PROP_SLOT_COUNT, &len);
	if (!val || len < (int)sizeof(fdt32_t))
		return SBI_EINVAL;

	checker->mmio_base = base;
	checker->mmio_size = size;
	checker->slot_count = fdt32_to_cpu(val[0]);
	sbi_snprintf(checker->name, sizeof(checker->name), "%s",
		     fdt_get_name(fdt, checker_node, NULL));

	return wgchecker2_parse_checker_rules(fdt, checker_node, checker);
}

static void wgchecker2_program_clear_slots(const struct wgchecker2_checker *checker)
{
	u32 slot;

	for (slot = 1; slot < checker->slot_count; slot++) {
		wgchecker2_write64(checker->mmio_base + WGCHECKER2_MMIO_SLOT_BASE +
				   slot * WGCHECKER2_MMIO_SLOT_STRIDE +
				   WGCHECKER2_MMIO_SLOT_ADDR, 0);
		wgchecker2_write64(checker->mmio_base + WGCHECKER2_MMIO_SLOT_BASE +
				   slot * WGCHECKER2_MMIO_SLOT_STRIDE +
				   WGCHECKER2_MMIO_SLOT_PERM, 0);
		wgchecker2_write32(checker->mmio_base + WGCHECKER2_MMIO_SLOT_BASE +
				   slot * WGCHECKER2_MMIO_SLOT_STRIDE +
				   WGCHECKER2_MMIO_SLOT_CFG, 0);
	}
}

static void wgchecker2_program_clear_last_slot(const struct wgchecker2_checker *checker)
{
	wgchecker2_write64(checker->mmio_base + WGCHECKER2_MMIO_SLOT_BASE +
			   checker->slot_count * WGCHECKER2_MMIO_SLOT_STRIDE +
			   WGCHECKER2_MMIO_SLOT_PERM, 0);
	wgchecker2_write32(checker->mmio_base + WGCHECKER2_MMIO_SLOT_BASE +
			   checker->slot_count * WGCHECKER2_MMIO_SLOT_STRIDE +
			   WGCHECKER2_MMIO_SLOT_CFG, 0);
}

static void wgchecker2_program_clear_slots_from(const struct wgchecker2_checker *checker,
						u32 first_slot)
{
	u32 slot;

	if (first_slot >= checker->slot_count)
		return;

	for (slot = first_slot; slot < checker->slot_count; slot++) {
		wgchecker2_write64(checker->mmio_base + WGCHECKER2_MMIO_SLOT_BASE +
				   slot * WGCHECKER2_MMIO_SLOT_STRIDE +
				   WGCHECKER2_MMIO_SLOT_ADDR, 0);
		wgchecker2_write64(checker->mmio_base + WGCHECKER2_MMIO_SLOT_BASE +
				   slot * WGCHECKER2_MMIO_SLOT_STRIDE +
				   WGCHECKER2_MMIO_SLOT_PERM, 0);
		wgchecker2_write32(checker->mmio_base + WGCHECKER2_MMIO_SLOT_BASE +
				   slot * WGCHECKER2_MMIO_SLOT_STRIDE +
				   WGCHECKER2_MMIO_SLOT_CFG, 0);
	}
}

static int wgchecker2_program_checker(const struct wgchecker2_checker *checker)
{
	u64 prev_end = 0;
	u32 required_slots = 0, slot = 1, i;

	wgchecker2_write64(checker->mmio_base + WGCHECKER2_MMIO_ERRCAUSE, 0);
	wgchecker2_write64(checker->mmio_base + WGCHECKER2_MMIO_ERRADDR, 0);

	if (checker->full_checker_rule) {
		wgchecker2_program_clear_slots(checker);
		wgchecker2_write64(checker->mmio_base + WGCHECKER2_MMIO_SLOT_BASE +
				   checker->slot_count * WGCHECKER2_MMIO_SLOT_STRIDE +
				   WGCHECKER2_MMIO_SLOT_PERM,
				   checker->full_checker_perm);
		wgchecker2_write32(checker->mmio_base + WGCHECKER2_MMIO_SLOT_BASE +
				   checker->slot_count * WGCHECKER2_MMIO_SLOT_STRIDE +
				   WGCHECKER2_MMIO_SLOT_CFG,
				   WGCHECKER2_SLOT_CFG_A_TOR);
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
		const struct wgchecker2_range *range = &checker->ranges[i];
		u64 end = range->base + range->size;

		if (!i || range->base != prev_end) {
			wgchecker2_write64(checker->mmio_base +
					   WGCHECKER2_MMIO_SLOT_BASE +
					   slot * WGCHECKER2_MMIO_SLOT_STRIDE +
					   WGCHECKER2_MMIO_SLOT_ADDR,
					   wgchecker2_slot_addr_encode(range->base));
			wgchecker2_write64(checker->mmio_base +
					   WGCHECKER2_MMIO_SLOT_BASE +
					   slot * WGCHECKER2_MMIO_SLOT_STRIDE +
					   WGCHECKER2_MMIO_SLOT_PERM, 0);
			wgchecker2_write32(checker->mmio_base +
					   WGCHECKER2_MMIO_SLOT_BASE +
					   slot * WGCHECKER2_MMIO_SLOT_STRIDE +
					   WGCHECKER2_MMIO_SLOT_CFG,
					   WGCHECKER2_SLOT_CFG_A_OFF);
			slot++;
		}

		wgchecker2_write64(checker->mmio_base + WGCHECKER2_MMIO_SLOT_BASE +
				   slot * WGCHECKER2_MMIO_SLOT_STRIDE +
				   WGCHECKER2_MMIO_SLOT_ADDR,
				   wgchecker2_slot_addr_encode(end));
		wgchecker2_write64(checker->mmio_base + WGCHECKER2_MMIO_SLOT_BASE +
				   slot * WGCHECKER2_MMIO_SLOT_STRIDE +
				   WGCHECKER2_MMIO_SLOT_PERM, range->perm);
		wgchecker2_write32(checker->mmio_base + WGCHECKER2_MMIO_SLOT_BASE +
				   slot * WGCHECKER2_MMIO_SLOT_STRIDE +
				   WGCHECKER2_MMIO_SLOT_CFG,
				   WGCHECKER2_SLOT_CFG_A_TOR);
		prev_end = end;
		slot++;
	}

	/*
	 * Keep the reset-time trusted-WID bypass slot alive until the new
	 * rule set is fully programmed, otherwise the DRAM checker can deny
	 * OpenSBI's own RAM accesses mid-update.
	 */
	wgchecker2_program_clear_slots_from(checker, slot);
	wgchecker2_program_clear_last_slot(checker);

	return 0;
}

u32 wgchecker2_count_platform_checkers(void *fdt)
{
	int checker_node;
	u32 count = 0;

	if (!fdt)
		return 0;

	checker_node = -1;
	while (true) {
		checker_node = fdt_node_offset_by_compatible(fdt, checker_node,
						     WGCHECKER2_COMPAT);
		if (checker_node < 0)
			break;
		if (fdt_getprop(fdt, checker_node, WGCHECKER2_PROP_SUBORDINATES,
				NULL))
			count++;
	}

	return count;
}

int wgchecker2_init(void *fdt)
{
	struct wgchecker2_platform_ctx *platform;
	int checker_node, rc;
	u32 count, idx = 0;

	wgchecker2_cleanup();

	if (!fdt)
		return 0;

	count = wgchecker2_count_platform_checkers(fdt);
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

	checker_node = -1;
	while (true) {
		checker_node = fdt_node_offset_by_compatible(fdt, checker_node,
						     WGCHECKER2_COMPAT);
		if (checker_node < 0)
			break;
		if (!fdt_getprop(fdt, checker_node, WGCHECKER2_PROP_SUBORDINATES,
				 NULL))
			continue;

		rc = wgchecker2_parse_checker(fdt, checker_node,
					      &platform->checkers[idx]);
		if (rc) {
			sbi_printf("[WG] failed to parse checker %s err=%d\n",
				   fdt_get_name(fdt, checker_node, NULL), rc);
			wgchecker2_free_platform_ctx(platform);
			return rc;
		}

		rc = wgchecker2_program_checker(&platform->checkers[idx]);
		if (rc) {
			sbi_printf("[WG] failed to program checker %s err=%d\n",
				   platform->checkers[idx].name, rc);
			wgchecker2_free_platform_ctx(platform);
			return rc;
		}

		sbi_printf("[WG] checker %s base=0x%llx slots=%u rules=%u%s\n",
			   platform->checkers[idx].name,
			   (unsigned long long)platform->checkers[idx].mmio_base,
			   platform->checkers[idx].slot_count,
			   platform->checkers[idx].range_count,
			   platform->checkers[idx].full_checker_rule ?
				" full-checker" : "");
		idx++;
	}

	wgchecker2_platform = platform;
	return 0;
}

void wgchecker2_cleanup(void)
{
	if (!wgchecker2_platform)
		return;

	wgchecker2_free_platform_ctx(wgchecker2_platform);
	wgchecker2_platform = NULL;
}

u32 wgchecker2_checker_count(void)
{
	return wgchecker2_platform ? wgchecker2_platform->checker_count : 0;
}
