/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Generic WorldGuard runtime support
 *
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#include <libfdt.h>
#include <sbi/riscv_encoding.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_hartmask.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_hwiso.h>
#include <sbi/sbi_scratch.h>
#include <worldguard.h>
#include <wgchecker2.h>
#include <sbi_utils/fdt/fdt_helper.h>

struct worldguard_platform_ctx {
	u32 hart_count;
	bool runtime_enabled;
	struct wg_cpu_defaults *hart_defaults;
};

static struct worldguard_platform_ctx *worldguard_platform;

static u32 worldguard_wid_mask(u32 wid)
{
	return (wid < WGCHECKER2_MAX_WIDS) ? (1U << wid) : 0;
}

static void worldguard_free_platform(void)
{
	if (!worldguard_platform)
		return;

	sbi_free(worldguard_platform->hart_defaults);
	sbi_free(worldguard_platform);
	worldguard_platform = NULL;
}

static bool worldguard_runtime_enabled(void)
{
	return worldguard_platform && worldguard_platform->runtime_enabled;
}

static void worldguard_init_cpu_defaults(struct worldguard_platform_ctx *platform)
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

static int worldguard_parse_wid_prop(void *fdt, int node, const char *prop_name,
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
	if (*out_wid >= WGCHECKER2_MAX_WIDS)
		return SBI_EINVAL;

	return 0;
}

static int worldguard_parse_widlist(void *fdt, int node, const char *prop_name,
				    u32 *out_mask)
{
	const fdt32_t *prop;
	u32 mask = 0, count, wid;
	int len, i;

	if (!out_mask)
		return SBI_EINVAL;

	*out_mask = 0;

	prop = fdt_getprop(fdt, node, prop_name, &len);
	if (!prop)
		return 0;
	if (len < 0 || (len % (int)sizeof(fdt32_t)))
		return SBI_EINVAL;

	count = len / sizeof(fdt32_t);
	if (count > WGCHECKER2_MAX_WIDS)
		return SBI_EINVAL;

	for (i = 0; i < (int)count; i++) {
		wid = fdt32_to_cpu(prop[i]);
		if (wid >= WGCHECKER2_MAX_WIDS)
			return SBI_EINVAL;
		if (mask & worldguard_wid_mask(wid))
			return SBI_EINVAL;

		mask |= worldguard_wid_mask(wid);
	}

	*out_mask = mask;
	return 0;
}

static int worldguard_parse_cpu_defaults(void *fdt,
					 struct worldguard_platform_ctx *platform)
{
	struct wg_cpu_defaults *cpu_defaults;
	u32 hartid, hartindex, max_wid;
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

		wgcpu = fdt_subnode_offset(fdt, cpu_offset, WORLDGUARD_CPU_NODE);
		if (wgcpu < 0 || fdt_node_check_compatible(
					 fdt, wgcpu, WORLDGUARD_CPU_COMPAT))
			continue;

		cpu_defaults = &platform->hart_defaults[hartindex];
		rc = worldguard_parse_wid_prop(fdt, wgcpu, WORLDGUARD_PROP_MWID,
					       &cpu_defaults->trusted_wid);
		if (rc)
			return rc;

		max_wid = cpu_defaults->trusted_wid;
		rc = worldguard_parse_widlist(fdt, wgcpu,
					      WORLDGUARD_PROP_MWIDLIST,
					      &cpu_defaults->valid_wid_mask);
		if (rc)
			return rc;

		cpu_defaults->valid_wid_mask |=
			worldguard_wid_mask(cpu_defaults->trusted_wid);
		if (cpu_defaults->valid_wid_mask) {
			u32 wid;

			for (wid = 0; wid < WGCHECKER2_MAX_WIDS; wid++) {
				if (cpu_defaults->valid_wid_mask & (1U << wid))
					max_wid = wid;
			}
		}

		cpu_defaults->nworlds = max_wid + 1;
	}

	return 0;
}

static bool worldguard_has_cpu_runtime(void *fdt)
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

		wgcpu = fdt_subnode_offset(fdt, cpu_offset, WORLDGUARD_CPU_NODE);
		if (wgcpu < 0)
			continue;
		if (fdt_node_check_compatible(fdt, wgcpu, WORLDGUARD_CPU_COMPAT))
			continue;

		return true;
	}

	return false;
}

static int worldguard_validate_domain_ctx(const struct sbi_domain *dom,
					  const struct worldguard_domain_ctx *ctx)
{
	const struct wg_cpu_defaults *cpu_defaults;
	u32 hartindex;

	if (!worldguard_platform || !dom || !ctx || dom == &root ||
	    !dom->possible_harts)
		return 0;

	for (hartindex = 0; hartindex < worldguard_platform->hart_count;
	     hartindex++) {
		if (!sbi_hartmask_test_hartindex(hartindex, dom->possible_harts))
			continue;

		cpu_defaults = &worldguard_platform->hart_defaults[hartindex];
		if (!(cpu_defaults->valid_wid_mask & worldguard_wid_mask(ctx->wid)))
			return SBI_EINVAL;
		if (ctx->widlist_mask & ~cpu_defaults->valid_wid_mask)
			return SBI_EINVAL;
	}

	return 0;
}

static const struct wg_cpu_defaults *worldguard_current_cpu_defaults(void)
{
	u32 hartindex;

	if (!worldguard_platform || !worldguard_platform->hart_defaults)
		return NULL;

	hartindex = sbi_hartid_to_hartindex(current_hartid());
	if (!sbi_hartindex_valid(hartindex) ||
	    hartindex >= worldguard_platform->hart_count)
		return NULL;

	return &worldguard_platform->hart_defaults[hartindex];
}

static u32 worldguard_fallback_wid(void)
{
	const struct wg_cpu_defaults *cpu_defaults =
		worldguard_current_cpu_defaults();

	return cpu_defaults ? cpu_defaults->trusted_wid : 0;
}

static u32 worldguard_valid_wid_mask(void)
{
	const struct wg_cpu_defaults *cpu_defaults =
		worldguard_current_cpu_defaults();

	return cpu_defaults ? cpu_defaults->valid_wid_mask :
			      worldguard_wid_mask(worldguard_fallback_wid());
}

static u32 worldguard_select_slwid(u32 widlist_mask, bool has_wid, u32 wid,
				   u32 fallback)
{
	u32 i;

	if (!widlist_mask)
		return fallback;

	if (has_wid && (worldguard_wid_mask(wid) & widlist_mask))
		return wid;

	for (i = 0; i < WGCHECKER2_MAX_WIDS; i++) {
		if (widlist_mask & (1U << i))
			return i;
	}

	return fallback;
}

static void worldguard_program_wid_state(u32 mlwid, u32 mwiddeleg, u32 slwid)
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

static int worldguard_init(void *fdt)
{
	struct worldguard_platform_ctx *platform;
	u32 checker_count;
	bool has_runtime;
	int rc;

	wgchecker2_cleanup();
	worldguard_free_platform();

	if (!fdt)
		return 0;

	checker_count = wgchecker2_count_platform_checkers(fdt);
	has_runtime = worldguard_has_cpu_runtime(fdt);
	if (!checker_count && !has_runtime)
		return 0;

	platform = sbi_zalloc(sizeof(*platform));
	if (!platform)
		return SBI_ENOMEM;

	platform->hart_count = sbi_scratch_last_hartindex() + 1;
	platform->runtime_enabled = has_runtime;
	platform->hart_defaults = sbi_calloc(sizeof(*platform->hart_defaults),
					      platform->hart_count);
	if (!platform->hart_defaults) {
		sbi_free(platform);
		return SBI_ENOMEM;
	}

	worldguard_init_cpu_defaults(platform);
	rc = worldguard_parse_cpu_defaults(fdt, platform);
	if (rc) {
		sbi_free(platform->hart_defaults);
		sbi_free(platform);
		return rc;
	}

	worldguard_platform = platform;

	if (checker_count) {
		rc = wgchecker2_init(fdt);
		if (rc) {
			worldguard_free_platform();
			wgchecker2_cleanup();
			return rc;
		}
	}

	return 0;
}

static int worldguard_domain_init(void *fdt, int domain_offset,
				  struct sbi_domain *dom, void **out_ctx)
{
	struct worldguard_domain_ctx *ctx;
	int hoff, child, rc;
	bool found = false;

	if (!out_ctx)
		return SBI_EINVAL;

	*out_ctx = NULL;
	if (!worldguard_runtime_enabled())
		return 0;
	if (!fdt || domain_offset < 0)
		return 0;

	hoff = fdt_subnode_offset(fdt, domain_offset, "hw-isolation");
	if (hoff < 0)
		return (dom == &root) ? 0 : SBI_EINVAL;

	fdt_for_each_subnode(child, fdt, hoff) {
		if (fdt_node_check_compatible(fdt, child, WGCHECKER2_COMPAT))
			continue;
		found = true;
		break;
	}

	if (!found)
		return (dom == &root) ? 0 : SBI_EINVAL;

	ctx = sbi_zalloc(sizeof(*ctx));
	if (!ctx)
		return SBI_ENOMEM;

	rc = worldguard_parse_wid_prop(fdt, child, WORLDGUARD_PROP_WID,
				       &ctx->wid);
	if (rc)
		goto err_free_ctx;
	ctx->has_wid = true;

	rc = worldguard_parse_widlist(fdt, child, WORLDGUARD_PROP_WIDLIST,
				      &ctx->widlist_mask);
	if (rc)
		goto err_free_ctx;

	rc = worldguard_validate_domain_ctx(dom, ctx);
	if (rc)
		goto err_free_ctx;

	*out_ctx = ctx;
	return 0;

err_free_ctx:
	sbi_free(ctx);
	return rc;
}

static void worldguard_domain_exit(const struct sbi_domain *src,
				   const struct sbi_domain *dst, void *ctx)
{
	u32 mlwid = worldguard_fallback_wid();

	(void)ctx;
	if (!worldguard_runtime_enabled())
		return;

	worldguard_program_wid_state(mlwid, 0, mlwid);

	sbi_printf("[WG] domain_exit src=%s dst=%s mlwid=%u mwiddeleg=0x0\n",
		   src ? src->name : "<null>",
		   dst ? dst->name : "<null>", mlwid);
}

static void worldguard_domain_enter(const struct sbi_domain *dst,
				    const struct sbi_domain *src, void *ctx)
{
	struct worldguard_domain_ctx *dctx = ctx;
	u32 valid_mask = worldguard_valid_wid_mask();
	u32 mlwid = worldguard_fallback_wid();
	u32 mwiddeleg = 0;
	u32 slwid = mlwid;

	(void)src;
	if (!worldguard_runtime_enabled())
		return;

	if (dctx && dctx->has_wid && (worldguard_wid_mask(dctx->wid) & valid_mask))
		mlwid = dctx->wid;

	if (dctx)
		mwiddeleg = dctx->widlist_mask & valid_mask;
	slwid = worldguard_select_slwid(mwiddeleg, dctx && dctx->has_wid,
					dctx ? dctx->wid : 0, mlwid);

	worldguard_program_wid_state(mlwid, mwiddeleg, slwid);

	sbi_printf("[WG] domain_enter dst=%s mlwid=%u mwiddeleg=0x%x",
		   dst ? dst->name : "<null>", mlwid, mwiddeleg);
	sbi_printf(" slwid=%u\n", slwid);
}

static void worldguard_domain_cleanup(struct sbi_domain *dom, void *ctx)
{
	(void)dom;
	sbi_free(ctx);
}

static const struct sbi_hwiso_ops worldguard_ops = {
	.name = WGCHECKER2_COMPAT,
	.init = worldguard_init,
	.domain_init = worldguard_domain_init,
	.domain_exit = worldguard_domain_exit,
	.domain_enter = worldguard_domain_enter,
	.domain_cleanup = worldguard_domain_cleanup,
};

int worldguard_register(void)
{
	return sbi_hwiso_register(&worldguard_ops);
}

const struct sbi_hwiso_ops *worldguard_ops_get(void)
{
	return &worldguard_ops;
}

#ifdef CONFIG_SBIUNIT
static struct worldguard_domain_ctx *
worldguard_test_find_domain_ctx(const struct sbi_domain *dom)
{
	u32 i;

	if (!dom || !dom->hwiso_ctxs)
		return NULL;

	for (i = 0; i < dom->hwiso_ctx_count; i++) {
		if (dom->hwiso_ctxs[i].ops != &worldguard_ops)
			continue;

		return dom->hwiso_ctxs[i].ctx;
	}

	return NULL;
}

int worldguard_test_check_runtime_state(bool runtime_enabled)
{
	if (!worldguard_platform)
		return runtime_enabled ? SBI_ENOENT : 0;
	if (worldguard_platform->runtime_enabled != runtime_enabled)
		return SBI_EINVAL;

	return 0;
}

int worldguard_test_check_domain_state(const struct sbi_domain *dom,
				       bool expect_ctx, u32 wid,
				       u32 widlist_mask)
{
	struct worldguard_domain_ctx *ctx = worldguard_test_find_domain_ctx(dom);

	if (!expect_ctx)
		return ctx ? SBI_EINVAL : 0;
	if (!ctx || !ctx->has_wid)
		return SBI_ENOENT;
	if (ctx->wid != wid || ctx->widlist_mask != widlist_mask)
		return SBI_EINVAL;

	return 0;
}
#endif
