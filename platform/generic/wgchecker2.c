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

static u64 wgchecker2_read_u32_args(const u32 *cells, int count)
{
	u64 val = 0;
	int i;

	for (i = 0; i < count; i++)
		val = (val << 32) | cells[i];

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
wgchecker2_access_rule_is_full_checker(const struct wgchecker2_checker *checker)
{
	return checker->slot_count == 1 && checker->range_count == 1;
}

static int wgchecker2_get_slot_count(const struct wgchecker2_checker *checker)
{
	return readl((void *)(unsigned long)(checker->mmio_base +
					     WGCHECKER2_MMIO_NSLOTS));
}

static int wgchecker2_collect_rule_from_node(const void *fdt, int nodeoff,
					     int checker_node,
					     struct wgchecker2_range *range,
					     u32 *nranges,
					     u32 max_ranges)
{
	struct fdt_phandle_args args;
	u32 index = 0;
	int rc;

	if (!fdt || nodeoff < 0 || !nranges)
		return SBI_EINVAL;

	while (true) {
		rc = fdt_parse_phandle_with_args(fdt, nodeoff,
						 WGCHECKER2_PROP_ACCESS_CONTROLLERS,
						 "#access-controller-cells",
						 index++, &args);
		if (rc == SBI_ENOENT)
			return 0;
		if (rc) {
			sbi_printf("[WG] access-controllers parse failed at %s rc=%d\n",
				   fdt_get_name(fdt, nodeoff, NULL), rc);
			return rc;
		}
		if (args.node_offset != checker_node)
			continue;
		if (args.args_count != WGCHECKER2_ACCESS_CONTROLLER_CELLS) {
			sbi_printf("[WG] checker %s args_count=%d expected=%d\n",
				   fdt_get_name(fdt, checker_node, NULL),
				   args.args_count,
				   WGCHECKER2_ACCESS_CONTROLLER_CELLS);
			return SBI_EINVAL;
		}
		if (!range) {
			(*nranges)++;
			continue;
		}
		if (*nranges >= max_ranges) {
			sbi_printf("[WG] checker %s range overflow nranges=%u max=%u\n",
				   fdt_get_name(fdt, checker_node, NULL),
				   *nranges, max_ranges);
			return SBI_EINVAL;
		}

		range[*nranges].base = wgchecker2_read_u32_args(&args.args[0], 2);
		range[*nranges].size = wgchecker2_read_u32_args(&args.args[2], 2);
		range[*nranges].perm = wgchecker2_read_u32_args(&args.args[4], 2);
		range[*nranges].cfg = args.args[6];
		if (!wgchecker2_range_is_aligned(range[*nranges].base,
						 range[*nranges].size)) {
			sbi_printf("[WG] checker %s unaligned base=0x%llx size=0x%llx\n",
				   fdt_get_name(fdt, checker_node, NULL),
				   (unsigned long long)range[*nranges].base,
				   (unsigned long long)range[*nranges].size);
			return SBI_EINVAL;
		}
		(*nranges)++;
	}
}

static int wgchecker2_parse_checker(void *fdt, int checker_node,
				    struct wgchecker2_checker *checker)
{
	u64 base = 0, size = 0;
	u32 nranges = 0;
	int nodeoff, depth = 0, rc;
	const fdt32_t *val;
	int len;

	val = fdt_getprop(fdt, checker_node, "#access-controller-cells", &len);
	if (!val || len < (int)sizeof(fdt32_t))
		return SBI_ENODEV;
	if (fdt32_to_cpu(val[0]) != WGCHECKER2_ACCESS_CONTROLLER_CELLS) {
		sbi_printf("[WG] checker %s invalid #access-controller-cells=%u\n",
			   fdt_get_name(fdt, checker_node, NULL),
			   fdt32_to_cpu(val[0]));
		return SBI_EINVAL;
	}

	rc = fdt_get_node_addr_size(fdt, checker_node, 0, &base, &size);
	if (rc)
		return rc;

	checker->mmio_base = base;
	checker->mmio_size = size;
	checker->slot_count = wgchecker2_get_slot_count(checker);
	if (!checker->slot_count) {
		sbi_printf("[WG] checker %s reported zero slots\n",
			   fdt_get_name(fdt, checker_node, NULL));
		return SBI_EINVAL;
	}
	sbi_snprintf(checker->name, sizeof(checker->name), "%s",
		     fdt_get_name(fdt, checker_node, NULL));

	for (nodeoff = 0; nodeoff >= 0;
	     nodeoff = fdt_next_node(fdt, nodeoff, &depth)) {
		if (nodeoff == checker_node || !fdt_node_is_enabled(fdt, nodeoff))
			continue;
		rc = wgchecker2_collect_rule_from_node(fdt, nodeoff,
						       checker_node, NULL,
						       &nranges, 0);
		if (rc)
			return rc;
	}

	if (!nranges)
		return 0;

	checker->ranges = sbi_calloc(sizeof(*checker->ranges), nranges);
	if (!checker->ranges)
		return SBI_ENOMEM;

	checker->range_count = 0;
	depth = 0;
	for (nodeoff = 0; nodeoff >= 0;
	     nodeoff = fdt_next_node(fdt, nodeoff, &depth)) {
		if (nodeoff == checker_node || !fdt_node_is_enabled(fdt, nodeoff))
			continue;
		rc = wgchecker2_collect_rule_from_node(fdt, nodeoff,
						       checker_node,
						       checker->ranges,
						       &checker->range_count,
						       nranges);
		if (rc)
			return rc;
	}

	return wgchecker2_compact_ranges(checker);
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

	if (wgchecker2_access_rule_is_full_checker(checker)) {
		wgchecker2_program_clear_slots(checker);
		wgchecker2_write64(checker->mmio_base + WGCHECKER2_MMIO_SLOT_BASE +
				   checker->slot_count * WGCHECKER2_MMIO_SLOT_STRIDE +
				   WGCHECKER2_MMIO_SLOT_PERM,
				   checker->ranges[0].perm);
		wgchecker2_write32(checker->mmio_base + WGCHECKER2_MMIO_SLOT_BASE +
				   checker->slot_count * WGCHECKER2_MMIO_SLOT_STRIDE +
				   WGCHECKER2_MMIO_SLOT_CFG,
				   checker->ranges[0].cfg);
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
				   range->cfg);
		prev_end = end;
		slot++;
	}

	/*
	 * Keep the reset-time trusted-WID bypass slot alive until the new
	 * rule set is fully programmed, otherwise the DRAM checker can deny
	 * OpenSBI's own RAM accesses mid-update.
	 */
	wgchecker2_program_clear_slots_from(checker, slot);

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
		if (fdt_getprop(fdt, checker_node, "#access-controller-cells",
				NULL) && fdt_node_is_enabled(fdt, checker_node))
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
		if (!fdt_node_is_enabled(fdt, checker_node))
			continue;
		if (!fdt_getprop(fdt, checker_node, "#access-controller-cells",
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
