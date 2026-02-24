/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * QEMU virt WorldGuard hardware isolation support
 *
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#include <libfdt.h>
#include <sbi/riscv_asm.h>
#include <sbi/riscv_encoding.h>
#include <sbi/riscv_io.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_hartmask.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_hwiso.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_string.h>
#include <qemu_virt_wg.h>
#include <sbi_utils/fdt/fdt_helper.h>

struct wg_checker {
	char name[32];
	u64 mmio_base;
	u64 mmio_size;
	u32 slot_count;
	u32 subordinate_count;
	bool full_checker_rule;
	u64 full_checker_perm;
	u32 range_count;
	struct qemu_virt_wg_range *ranges;
};

struct wg_cpu_defaults {
	u32 trusted_wid;
	u32 nworlds;
	u32 valid_wid_mask;
};

struct wg_platform_ctx {
	u32 checker_count;
	u32 hart_count;
	bool checker_enabled;
	bool runtime_enabled;
	struct wg_checker *checkers;
	struct wg_cpu_defaults *hart_defaults;
};

struct wg_domain_ctx {
	bool has_wid;
	u32 wid;
	u32 widlist_count;
	u32 widlist_mask;
	u32 widlist[QEMU_VIRT_WG_MAX_WIDS];
};

static struct wg_platform_ctx *wg_platform;

static void wg_free_platform_ctx(struct wg_platform_ctx *platform)
{
	u32 i;

	if (!platform)
		return;

	for (i = 0; i < platform->checker_count; i++)
		sbi_free(platform->checkers[i].ranges);

	sbi_free(platform->checkers);
	sbi_free(platform->hart_defaults);
	sbi_free(platform);
}

static bool wg_runtime_enabled(void)
{
	return wg_platform && wg_platform->runtime_enabled;
}

static u64 wg_read_cells(const fdt32_t *cells, int count)
{
	u64 val = 0;
	int i;

	for (i = 0; i < count; i++)
		val = (val << 32) | fdt32_to_cpu(cells[i]);

	return val;
}

static void wg_write64(u64 addr, u64 val)
{
#if __riscv_xlen != 32
	writeq(val, (void *)(unsigned long)addr);
#else
	writel((u32)val, (void *)(unsigned long)addr);
	writel((u32)(val >> 32), (void *)(unsigned long)(addr + 4));
#endif
}

static void wg_write32(u64 addr, u32 val)
{
	writel(val, (void *)(unsigned long)addr);
}

static u64 wg_slot_addr_encode(u64 addr)
{
	return addr >> 2;
}

static u64 wg_wid_mask(u32 wid)
{
	return (wid < 32) ? (1ULL << wid) : 0;
}

static bool wg_range_is_aligned(u64 base, u64 size)
{
	if (!size)
		return false;

	if (base & (QEMU_VIRT_WG_MIN_ALIGN - 1))
		return false;
	if (size & (QEMU_VIRT_WG_MIN_ALIGN - 1))
		return false;

	return true;
}

static void wg_sort_ranges(struct wg_checker *checker)
{
	struct qemu_virt_wg_range tmp;
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

static int wg_compact_ranges(struct wg_checker *checker)
{
	struct qemu_virt_wg_range *prev, *cur;
	u64 prev_end, cur_end;
	u32 i, out = 0;

	if (!checker->range_count)
		return 0;

	wg_sort_ranges(checker);

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

static int wg_get_reg_cells(void *fdt, int resource_node,
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

static int wg_count_reg_entries(void *fdt, int resource_node, int reg_node)
{
	const fdt32_t *reg;
	int addr_cells, size_cells, entry_cells, len, rc;

	rc = wg_get_reg_cells(fdt, resource_node, &addr_cells, &size_cells);
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

static int wg_parse_perms(void *fdt, int cfg_node, u64 **out_perms,
			   u32 *out_count)
{
	const fdt32_t *perms;
	u64 *vals;
	int len, i, count;

	*out_perms = NULL;
	*out_count = 0;

	perms = fdt_getprop(fdt, cfg_node, QEMU_VIRT_WG_PROP_PERMS, &len);
	if (!perms || len <= 0)
		return 0;

	/* QEMU virt WG permissions are always encoded as 64-bit <hi lo> cells. */
	if (len % (2 * (int)sizeof(fdt32_t)))
		return SBI_EINVAL;

	count = len / (2 * (int)sizeof(fdt32_t));
	vals = sbi_calloc(sizeof(*vals), count);
	if (!vals)
		return SBI_ENOMEM;

	for (i = 0; i < count; i++, perms += 2)
		vals[i] = wg_read_cells(perms, 2);

	*out_perms = vals;
	*out_count = count;
	return 0;
}

static int wg_fill_ranges(void *fdt, int resource_node, int reg_node,
			  const u64 *perms, u32 perm_count,
			  struct qemu_virt_wg_range *ranges, u32 range_count)
{
	const fdt32_t *reg;
	u64 base, size;
	int addr_cells, size_cells, entry_cells, len, i, rc;

	rc = wg_get_reg_cells(fdt, resource_node, &addr_cells, &size_cells);
	if (rc)
		return rc;

	reg = fdt_getprop(fdt, reg_node, "reg", &len);
	if (!reg || len <= 0)
		return SBI_EINVAL;

	entry_cells = addr_cells + size_cells;
	for (i = 0; i < (int)range_count; i++, reg += entry_cells) {
		base = wg_read_cells(reg, addr_cells);
		size = wg_read_cells(reg + addr_cells, size_cells);
		if (!wg_range_is_aligned(base, size))
			return SBI_EINVAL;

		ranges[i].base = base;
		ranges[i].size = size;
		ranges[i].perm = perms[(perm_count == 1) ? 0 : i];
	}

	return 0;
}

static int wg_parse_checker_rules(void *fdt, int checker_node,
				  struct wg_checker *checker)
{
	const fdt32_t *subs;
	u64 *perms = NULL;
	int cfg_node, len, i, rc = 0, reg_count;
	u32 perm_count = 0;
	int child;

	subs = fdt_getprop(fdt, checker_node,
			   QEMU_VIRT_WG_PROP_SUBORDINATES, &len);
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
				   checker->name, i, fdt32_to_cpu(subs[i]),
				   child);
			rc = child;
			goto err;
		}

		cfg_node = fdt_subnode_offset(fdt, child,
					      QEMU_VIRT_WG_CFG_NODE);
		if (cfg_node < 0)
			continue;

		rc = wg_parse_perms(fdt, cfg_node, &perms, &perm_count);
		if (rc)
			goto err;
		if (!perm_count)
			continue;

		reg_count = wg_count_reg_entries(fdt, child, cfg_node);
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
			reg_count = wg_count_reg_entries(fdt, child, child);
		if (reg_count <= 0)
			goto err;

		if (perm_count != 1 && perm_count != (u32)reg_count)
			goto err;
		if (checker->full_checker_rule)
			goto err;
		if (checker->range_count + reg_count > checker->slot_count)
			goto err;

		rc = wg_fill_ranges(fdt, child,
				     (fdt_getprop(fdt, cfg_node, "reg", NULL) ?
				      cfg_node : child),
				     perms, perm_count,
				     &checker->ranges[checker->range_count],
				     reg_count);
		sbi_free(perms);
		perms = NULL;
		if (rc)
			goto err;

		checker->range_count += reg_count;
	}

	if (checker->full_checker_rule)
		return 0;

	return wg_compact_ranges(checker);

err:
	sbi_free(perms);
	return rc ? rc : SBI_EINVAL;
}

static int wg_parse_checker(void *fdt, int checker_node,
			    struct wg_checker *checker)
{
	const fdt32_t *val;
	u64 base = 0, size = 0;
	int len, rc;

	rc = fdt_get_node_addr_size(fdt, checker_node, 0, &base, &size);
	if (rc)
		return rc;

	val = fdt_getprop(fdt, checker_node,
			  QEMU_VIRT_WG_PROP_SLOT_COUNT, &len);
	if (!val || len < (int)sizeof(fdt32_t))
		return SBI_EINVAL;

	checker->mmio_base = base;
	checker->mmio_size = size;
	checker->slot_count = fdt32_to_cpu(val[0]);
	sbi_snprintf(checker->name, sizeof(checker->name), "%s",
		     fdt_get_name(fdt, checker_node, NULL));

	return wg_parse_checker_rules(fdt, checker_node, checker);
}

static void wg_program_clear_slots(const struct wg_checker *checker)
{
	u32 slot;

	for (slot = 1; slot < checker->slot_count; slot++) {
		wg_write64(checker->mmio_base + QEMU_VIRT_WG_MMIO_SLOT_BASE +
			   slot * QEMU_VIRT_WG_MMIO_SLOT_STRIDE +
			   QEMU_VIRT_WG_MMIO_SLOT_ADDR, 0);
		wg_write64(checker->mmio_base + QEMU_VIRT_WG_MMIO_SLOT_BASE +
			   slot * QEMU_VIRT_WG_MMIO_SLOT_STRIDE +
			   QEMU_VIRT_WG_MMIO_SLOT_PERM, 0);
		wg_write32(checker->mmio_base + QEMU_VIRT_WG_MMIO_SLOT_BASE +
			   slot * QEMU_VIRT_WG_MMIO_SLOT_STRIDE +
			   QEMU_VIRT_WG_MMIO_SLOT_CFG, 0);
	}

}

static void wg_program_clear_last_slot(const struct wg_checker *checker)
{
	wg_write64(checker->mmio_base + QEMU_VIRT_WG_MMIO_SLOT_BASE +
		   checker->slot_count * QEMU_VIRT_WG_MMIO_SLOT_STRIDE +
		   QEMU_VIRT_WG_MMIO_SLOT_PERM, 0);
	wg_write32(checker->mmio_base + QEMU_VIRT_WG_MMIO_SLOT_BASE +
		   checker->slot_count * QEMU_VIRT_WG_MMIO_SLOT_STRIDE +
		   QEMU_VIRT_WG_MMIO_SLOT_CFG, 0);
}

static void wg_program_clear_slots_from(const struct wg_checker *checker,
					u32 first_slot)
{
	u32 slot;

	if (first_slot >= checker->slot_count)
		return;

	for (slot = first_slot; slot < checker->slot_count; slot++) {
		wg_write64(checker->mmio_base + QEMU_VIRT_WG_MMIO_SLOT_BASE +
			   slot * QEMU_VIRT_WG_MMIO_SLOT_STRIDE +
			   QEMU_VIRT_WG_MMIO_SLOT_ADDR, 0);
		wg_write64(checker->mmio_base + QEMU_VIRT_WG_MMIO_SLOT_BASE +
			   slot * QEMU_VIRT_WG_MMIO_SLOT_STRIDE +
			   QEMU_VIRT_WG_MMIO_SLOT_PERM, 0);
		wg_write32(checker->mmio_base + QEMU_VIRT_WG_MMIO_SLOT_BASE +
			   slot * QEMU_VIRT_WG_MMIO_SLOT_STRIDE +
			   QEMU_VIRT_WG_MMIO_SLOT_CFG, 0);
	}
}

static int wg_program_checker(const struct wg_checker *checker)
{
	u64 prev_end = 0;
	u32 required_slots = 0, slot = 1, i;

	wg_write64(checker->mmio_base + QEMU_VIRT_WG_MMIO_ERRCAUSE, 0);
	wg_write64(checker->mmio_base + QEMU_VIRT_WG_MMIO_ERRADDR, 0);

	if (checker->full_checker_rule) {
		wg_program_clear_slots(checker);
		wg_write64(checker->mmio_base + QEMU_VIRT_WG_MMIO_SLOT_BASE +
			   checker->slot_count * QEMU_VIRT_WG_MMIO_SLOT_STRIDE +
			   QEMU_VIRT_WG_MMIO_SLOT_PERM,
			   checker->full_checker_perm);
		wg_write32(checker->mmio_base + QEMU_VIRT_WG_MMIO_SLOT_BASE +
			   checker->slot_count * QEMU_VIRT_WG_MMIO_SLOT_STRIDE +
			   QEMU_VIRT_WG_MMIO_SLOT_CFG,
			   QEMU_VIRT_WG_SLOT_CFG_A_TOR);
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
		const struct qemu_virt_wg_range *range = &checker->ranges[i];
		u64 end = range->base + range->size;

		if (!i || range->base != prev_end) {
			wg_write64(checker->mmio_base +
				   QEMU_VIRT_WG_MMIO_SLOT_BASE +
				   slot * QEMU_VIRT_WG_MMIO_SLOT_STRIDE +
				   QEMU_VIRT_WG_MMIO_SLOT_ADDR,
				   wg_slot_addr_encode(range->base));
			wg_write64(checker->mmio_base +
				   QEMU_VIRT_WG_MMIO_SLOT_BASE +
				   slot * QEMU_VIRT_WG_MMIO_SLOT_STRIDE +
				   QEMU_VIRT_WG_MMIO_SLOT_PERM, 0);
			wg_write32(checker->mmio_base +
				   QEMU_VIRT_WG_MMIO_SLOT_BASE +
				   slot * QEMU_VIRT_WG_MMIO_SLOT_STRIDE +
				   QEMU_VIRT_WG_MMIO_SLOT_CFG,
				   QEMU_VIRT_WG_SLOT_CFG_A_OFF);
			slot++;
		}

		wg_write64(checker->mmio_base + QEMU_VIRT_WG_MMIO_SLOT_BASE +
			   slot * QEMU_VIRT_WG_MMIO_SLOT_STRIDE +
			   QEMU_VIRT_WG_MMIO_SLOT_ADDR,
			   wg_slot_addr_encode(end));
		wg_write64(checker->mmio_base + QEMU_VIRT_WG_MMIO_SLOT_BASE +
			   slot * QEMU_VIRT_WG_MMIO_SLOT_STRIDE +
			   QEMU_VIRT_WG_MMIO_SLOT_PERM,
			   range->perm);
		wg_write32(checker->mmio_base + QEMU_VIRT_WG_MMIO_SLOT_BASE +
			   slot * QEMU_VIRT_WG_MMIO_SLOT_STRIDE +
			   QEMU_VIRT_WG_MMIO_SLOT_CFG,
			   QEMU_VIRT_WG_SLOT_CFG_A_TOR);
		prev_end = end;
		slot++;
	}

	/*
	 * Keep the reset-time trusted-WID bypass slot alive until the new
	 * rule set is fully programmed, otherwise the DRAM checker can deny
	 * OpenSBI's own RAM accesses mid-update.
	 */
	wg_program_clear_slots_from(checker, slot);
	wg_program_clear_last_slot(checker);

	return 0;
}

static void wg_free_platform(void)
{
	if (!wg_platform)
		return;

	wg_free_platform_ctx(wg_platform);
	wg_platform = NULL;
}

static void wg_init_cpu_defaults(struct wg_platform_ctx *platform)
{
	u32 i;

	if (!platform || !platform->hart_defaults)
		return;

	for (i = 0; i < platform->hart_count; i++) {
		platform->hart_defaults[i].trusted_wid = 0;
		platform->hart_defaults[i].nworlds = 1;
		platform->hart_defaults[i].valid_wid_mask = 0x1;
	}
}

static int wg_parse_wid_prop(void *fdt, int node, const char *prop_name,
			     u32 *out_wid)
{
	const fdt32_t *prop;
	int len;

	if (!out_wid)
		return SBI_EINVAL;

	prop = fdt_getprop(fdt, node, prop_name, &len);
	if (!prop)
		return SBI_ENOENT;
	if (len != (int)sizeof(fdt32_t))
		return SBI_EINVAL;

	*out_wid = fdt32_to_cpu(prop[0]);
	if (*out_wid >= QEMU_VIRT_WG_MAX_WIDS)
		return SBI_EINVAL;

	return 0;
}

static int wg_parse_widlist(void *fdt, int node, const char *prop_name,
			    u32 *out_mask, u32 *out_wids, u32 *out_count)
{
	const fdt32_t *prop;
	u32 mask = 0, count = 0, wid;
	int len, i;

	if (!out_mask || !out_count)
		return SBI_EINVAL;

	*out_mask = 0;
	*out_count = 0;

	prop = fdt_getprop(fdt, node, prop_name, &len);
	if (!prop)
		return 0;
	if (len < 0 || (len % (int)sizeof(fdt32_t)))
		return SBI_EINVAL;

	count = len / sizeof(fdt32_t);
	if (count > QEMU_VIRT_WG_MAX_WIDS)
		return SBI_EINVAL;

	for (i = 0; i < (int)count; i++) {
		wid = fdt32_to_cpu(prop[i]);
		if (wid >= QEMU_VIRT_WG_MAX_WIDS)
			return SBI_EINVAL;
		if (mask & wg_wid_mask(wid))
			return SBI_EINVAL;

		mask |= wg_wid_mask(wid);
		if (out_wids)
			out_wids[i] = wid;
	}

	*out_mask = mask;
	*out_count = count;
	return 0;
}

static int wg_parse_cpu_defaults(void *fdt, struct wg_platform_ctx *platform)
{
	struct wg_cpu_defaults *cpu_defaults;
	u32 hartid, hartindex, max_wid, widlist_count;
	int cpus_offset, cpu_offset, wgcpu, rc;

	if (!fdt || !platform || !platform->hart_defaults)
		return 0;

	cpus_offset = fdt_path_offset(fdt, "/cpus");
	if (cpus_offset < 0)
		return 0;

	fdt_for_each_subnode(cpu_offset, fdt, cpus_offset) {
		if (fdt_parse_hart_id(fdt, cpu_offset, &hartid))
			continue;

		hartindex = sbi_hartid_to_hartindex(hartid);
		if (!sbi_hartindex_valid(hartindex) ||
		    hartindex >= platform->hart_count)
			continue;

		wgcpu = fdt_subnode_offset(fdt, cpu_offset,
					   QEMU_VIRT_WG_CPU_NODE);
		if (wgcpu < 0 || fdt_node_check_compatible(
					 fdt, wgcpu, QEMU_VIRT_WG_CPU_COMPAT))
			continue;

		cpu_defaults = &platform->hart_defaults[hartindex];
		rc = wg_parse_wid_prop(fdt, wgcpu, QEMU_VIRT_WG_PROP_MWID,
				       &cpu_defaults->trusted_wid);
		if (rc)
			return rc;

		max_wid = cpu_defaults->trusted_wid;
		rc = wg_parse_widlist(fdt, wgcpu, QEMU_VIRT_WG_PROP_MWIDLIST,
				      &cpu_defaults->valid_wid_mask, NULL,
				      &widlist_count);
		if (rc)
			return rc;

		cpu_defaults->valid_wid_mask |=
			wg_wid_mask(cpu_defaults->trusted_wid);
		if (cpu_defaults->valid_wid_mask) {
			u32 wid;

			for (wid = 0; wid < QEMU_VIRT_WG_MAX_WIDS; wid++) {
				if (cpu_defaults->valid_wid_mask & (1U << wid))
					max_wid = wid;
			}
		}

		cpu_defaults->nworlds = max_wid + 1;
	}

	return 0;
}

static bool wg_has_cpu_runtime(void *fdt)
{
	u32 hartid;
	int cpus_offset, cpu_offset, wgcpu;

	if (!fdt)
		return false;

	cpus_offset = fdt_path_offset(fdt, "/cpus");
	if (cpus_offset < 0)
		return false;

	fdt_for_each_subnode(cpu_offset, fdt, cpus_offset) {
		if (fdt_parse_hart_id(fdt, cpu_offset, &hartid))
			continue;

		wgcpu = fdt_subnode_offset(fdt, cpu_offset,
					   QEMU_VIRT_WG_CPU_NODE);
		if (wgcpu < 0)
			continue;
		if (fdt_node_check_compatible(fdt, wgcpu,
					      QEMU_VIRT_WG_CPU_COMPAT))
			continue;

		return true;
	}

	return false;
}

static u32 wg_count_platform_checkers(void *fdt)
{
	int checker_node;
	u32 count = 0;

	if (!fdt)
		return 0;

	checker_node = -1;
	while (true) {
		checker_node = fdt_node_offset_by_compatible(
			fdt, checker_node, QEMU_VIRT_WG_COMPAT);
		if (checker_node < 0)
			break;
		if (fdt_getprop(fdt, checker_node,
				QEMU_VIRT_WG_PROP_SUBORDINATES, NULL))
			count++;
	}

	return count;
}

static int wg_validate_domain_ctx(const struct sbi_domain *dom,
				  const struct wg_domain_ctx *ctx)
{
	const struct wg_cpu_defaults *cpu_defaults;
	u32 hartindex;

	if (!wg_platform || !dom || !ctx || dom == &root || !dom->possible_harts)
		return 0;

	for (hartindex = 0; hartindex < wg_platform->hart_count; hartindex++) {
		if (!sbi_hartmask_test_hartindex(hartindex, dom->possible_harts))
			continue;

		cpu_defaults = &wg_platform->hart_defaults[hartindex];
		if (!(cpu_defaults->valid_wid_mask & wg_wid_mask(ctx->wid)))
			return SBI_EINVAL;
		if (ctx->widlist_mask & ~cpu_defaults->valid_wid_mask)
			return SBI_EINVAL;
	}

	return 0;
}

static const struct wg_cpu_defaults *wg_current_cpu_defaults(void)
{
	u32 hartindex;

	if (!wg_platform || !wg_platform->hart_defaults)
		return NULL;

	hartindex = sbi_hartid_to_hartindex(current_hartid());
	if (!sbi_hartindex_valid(hartindex) ||
	    hartindex >= wg_platform->hart_count)
		return NULL;

	return &wg_platform->hart_defaults[hartindex];
}

static void wg_program_wid_state(u32 mlwid, u32 mwiddeleg, u32 slwid)
{
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();

	if (!sbi_hart_has_extension(scratch, SBI_HART_EXT_SMWG))
		return;

	if (!sbi_hart_has_extension(scratch, SBI_HART_EXT_SSWG)) {
		csr_write(CSR_MLWID, mlwid);
		return;
	}

	csr_write(CSR_MWIDDELEG, 0);
	csr_write(CSR_MLWID, mlwid);
	if (mwiddeleg) {
		csr_write(CSR_MWIDDELEG, mwiddeleg);
		csr_write(CSR_SLWID, slwid);
	}
}

static int wg_init(void *fdt)
{
	struct wg_platform_ctx *platform;
	int checker_node, rc;
	u32 count, idx = 0;
	bool has_runtime;

	wg_free_platform();

	if (!fdt)
		return 0;

	count = wg_count_platform_checkers(fdt);
	has_runtime = wg_has_cpu_runtime(fdt);
	if (!count && !has_runtime)
		return 0;

	platform = sbi_zalloc(sizeof(*platform));
	if (!platform)
		return SBI_ENOMEM;

	platform->hart_count = sbi_scratch_last_hartindex() + 1;
	platform->checker_count = count;
	platform->checker_enabled = !!count;
	platform->runtime_enabled = has_runtime;
	platform->checkers = sbi_calloc(sizeof(*platform->checkers), count);
	if (count && !platform->checkers) {
		sbi_free(platform);
		return SBI_ENOMEM;
	}

	platform->hart_defaults = sbi_calloc(sizeof(*platform->hart_defaults),
					      platform->hart_count);
	if (!platform->hart_defaults) {
		wg_free_platform_ctx(platform);
		return SBI_ENOMEM;
	}

	wg_init_cpu_defaults(platform);
	rc = wg_parse_cpu_defaults(fdt, platform);
	if (rc) {
		wg_free_platform_ctx(platform);
		return rc;
	}

	if (platform->checker_enabled) {
		checker_node = -1;
		while (true) {
			checker_node = fdt_node_offset_by_compatible(
				fdt, checker_node, QEMU_VIRT_WG_COMPAT);
			if (checker_node < 0)
				break;
			if (!fdt_getprop(fdt, checker_node,
					 QEMU_VIRT_WG_PROP_SUBORDINATES, NULL))
				continue;

			rc = wg_parse_checker(fdt, checker_node,
					      &platform->checkers[idx]);
			if (rc) {
				sbi_printf("[WG] failed to parse checker %s err=%d\n",
					   fdt_get_name(fdt, checker_node, NULL),
					   rc);
				wg_free_platform_ctx(platform);
				return rc;
			}

			rc = wg_program_checker(&platform->checkers[idx]);
			if (rc) {
				sbi_printf("[WG] failed to program checker %s err=%d\n",
					   platform->checkers[idx].name, rc);
				wg_free_platform_ctx(platform);
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
	}

	wg_platform = platform;
	return 0;
}

static int wg_domain_init(void *fdt, int domain_offset,
			  struct sbi_domain *dom, void **out_ctx)
{
	struct wg_domain_ctx *ctx;
	int hoff, child, rc;
	bool found = false;

	if (!out_ctx)
		return SBI_EINVAL;

	*out_ctx = NULL;
	if (!wg_runtime_enabled())
		return 0;
	if (!fdt || domain_offset < 0)
		return 0;

	hoff = fdt_subnode_offset(fdt, domain_offset, "hw-isolation");
	if (hoff < 0)
		return (dom == &root) ? 0 : SBI_EINVAL;

	fdt_for_each_subnode(child, fdt, hoff) {
		if (fdt_node_check_compatible(
				fdt, child, QEMU_VIRT_WG_COMPAT))
			continue;
		found = true;
		break;
	}

	if (!found)
		return (dom == &root) ? 0 : SBI_EINVAL;

	ctx = sbi_zalloc(sizeof(*ctx));
	if (!ctx)
		return SBI_ENOMEM;

	rc = wg_parse_wid_prop(fdt, child, QEMU_VIRT_WG_PROP_WID, &ctx->wid);
	if (rc)
		goto err_free_ctx;
	ctx->has_wid = true;

	rc = wg_parse_widlist(fdt, child, QEMU_VIRT_WG_PROP_WIDLIST,
			      &ctx->widlist_mask, ctx->widlist,
			      &ctx->widlist_count);
	if (rc)
		goto err_free_ctx;

	rc = wg_validate_domain_ctx(dom, ctx);
	if (rc)
		goto err_free_ctx;

	*out_ctx = ctx;
	return 0;

err_free_ctx:
	sbi_free(ctx);
	return rc;
}

static u32 wg_fallback_wid(void)
{
	const struct wg_cpu_defaults *cpu_defaults = wg_current_cpu_defaults();

	return cpu_defaults ? cpu_defaults->trusted_wid : 0;
}

static u32 wg_valid_wid_mask(void)
{
	const struct wg_cpu_defaults *cpu_defaults = wg_current_cpu_defaults();

	return cpu_defaults ? cpu_defaults->valid_wid_mask :
			      (u32)wg_wid_mask(wg_fallback_wid());
}

static u32 wg_select_slwid(u32 widlist_mask, bool has_wid, u32 wid, u32 fallback)
{
	u32 i;

	if (!widlist_mask)
		return fallback;

	if (has_wid && (wg_wid_mask(wid) & widlist_mask))
		return wid;

	for (i = 0; i < 32; i++) {
		if (widlist_mask & (1U << i))
			return i;
	}

	return fallback;
}

static void wg_domain_exit(const struct sbi_domain *src,
			   const struct sbi_domain *dst, void *ctx)
{
	u32 mlwid = wg_fallback_wid();

	(void)ctx;
	if (!wg_runtime_enabled())
		return;

	wg_program_wid_state(mlwid, 0, mlwid);

	sbi_printf("[WG] domain_exit src=%s dst=%s mlwid=%u mwiddeleg=0x0\n",
		   src ? src->name : "<null>",
		   dst ? dst->name : "<null>", mlwid);
}

static void wg_domain_enter(const struct sbi_domain *dst,
			    const struct sbi_domain *src, void *ctx)
{
	struct wg_domain_ctx *dctx = ctx;
	u32 valid_mask = wg_valid_wid_mask();
	u32 mlwid = wg_fallback_wid();
	u32 mwiddeleg = 0;
	u32 slwid = mlwid;

	(void)src;
	if (!wg_runtime_enabled())
		return;

	if (dctx && dctx->has_wid && (wg_wid_mask(dctx->wid) & valid_mask))
		mlwid = dctx->wid;

	if (dctx)
		mwiddeleg = dctx->widlist_mask & valid_mask;
	slwid = wg_select_slwid(mwiddeleg, dctx && dctx->has_wid,
				dctx ? dctx->wid : 0, mlwid);

	wg_program_wid_state(mlwid, mwiddeleg, slwid);

	sbi_printf("[WG] domain_enter dst=%s mlwid=%u mwiddeleg=0x%x",
		   dst ? dst->name : "<null>", mlwid, mwiddeleg);
	sbi_printf(" slwid=%u\n", slwid);
}

static void wg_domain_cleanup(struct sbi_domain *dom, void *ctx)
{
	(void)dom;
	sbi_free(ctx);
}

static const struct sbi_hwiso_ops wg_ops = {
	.name = QEMU_VIRT_WG_COMPAT,
	.init = wg_init,
	.domain_init = wg_domain_init,
	.domain_exit = wg_domain_exit,
	.domain_enter = wg_domain_enter,
	.domain_cleanup = wg_domain_cleanup,
};

int qemu_virt_hwiso_register(void *fdt)
{
	int rc;

	if (!fdt)
		return 0;

	if (fdt_node_check_compatible(fdt, 0, "riscv-virtio") &&
	    fdt_node_check_compatible(fdt, 0, "qemu,virt"))
		return 0;

	rc = sbi_hwiso_register(&wg_ops);
	if (rc)
		return rc;

	return 0;
}

