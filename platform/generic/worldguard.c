// SPDX-License-Identifier: BSD-2-Clause
/*
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
#include <sbi/sbi_domain_data.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_hart_protection.h>
#include <sbi/sbi_hartmask.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_string.h>
#include <sbi_utils/fdt/fdt_domain.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <wgchecker2.h>
#include <worldguard.h>

struct worldguard_platform_ctx {
	u32 hart_count;
	bool runtime_enabled;
	struct wg_cpu_defaults *hart_defaults;
};

static struct worldguard_platform_ctx *worldguard_platform;
static bool worldguard_hprot_registered;
static bool worldguard_domain_data_registered;

static int worldguard_domain_data_setup(struct sbi_domain *dom,
					struct sbi_domain_data *data,
					void *data_ptr);
static void worldguard_domain_data_cleanup(struct sbi_domain *dom,
					   struct sbi_domain_data *data,
					   void *data_ptr);

static struct sbi_domain_data worldguard_domain_data;

static u32 worldguard_wid_mask(u32 wid)
{
	return (wid < WGCHECKER2_MAX_WIDS) ? (1U << wid) : 0;
}

static bool worldguard_runtime_enabled(void)
{
	return worldguard_platform && worldguard_platform->runtime_enabled;
}

static void worldguard_free_platform(void)
{
	if (!worldguard_platform)
		return;

	sbi_free(worldguard_platform->hart_defaults);
	sbi_free(worldguard_platform);
	worldguard_platform = NULL;
}

static void worldguard_init_cpu_defaults(struct worldguard_platform_ctx *platform)
{
	u32 i;

	if (!platform || !platform->hart_defaults)
		return;

	for (i = 0; i < platform->hart_count; i++) {
		platform->hart_defaults[i].trusted_wid = 0;
		platform->hart_defaults[i].valid_wid_mask = 0x1;
	}
}

static int worldguard_parse_cpu_defaults(const void *fdt,
					 struct worldguard_platform_ctx *platform)
{
	struct wg_cpu_defaults *cpu_defaults;
	u32 hartid, hartindex;
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
		if (wgcpu < 0 || fdt_node_check_compatible(fdt, wgcpu,
							   WORLDGUARD_CPU_COMPAT))
			continue;

		cpu_defaults = &platform->hart_defaults[hartindex];
		rc = fdt_parse_u32(fdt, wgcpu, WORLDGUARD_PROP_MWID,
				   &cpu_defaults->trusted_wid);
		if (rc)
			return rc;
		if (cpu_defaults->trusted_wid >= WGCHECKER2_MAX_WIDS)
			return SBI_EINVAL;

		rc = fdt_parse_u32_array_bitmask(fdt, wgcpu,
						 WORLDGUARD_PROP_MWIDLIST,
						 WGCHECKER2_MAX_WIDS,
						 &cpu_defaults->valid_wid_mask);
		if (rc)
			return rc;

		cpu_defaults->valid_wid_mask |=
			worldguard_wid_mask(cpu_defaults->trusted_wid);
	}

	return 0;
}

static bool worldguard_has_cpu_runtime(const void *fdt)
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

static int worldguard_validate_domain_state(const struct sbi_domain *dom,
					    const struct worldguard_domain_state *state)
{
	const struct wg_cpu_defaults *cpu_defaults;
	u32 hartindex;

	if (!worldguard_platform || !dom || !state || dom == &root ||
	    !dom->possible_harts)
		return 0;

	for (hartindex = 0; hartindex < worldguard_platform->hart_count;
	     hartindex++) {
		if (!dom->possible_harts ||
		    !sbi_hartmask_test_hartindex(hartindex, dom->possible_harts))
			continue;

		cpu_defaults = &worldguard_platform->hart_defaults[hartindex];
		if (!(cpu_defaults->valid_wid_mask & worldguard_wid_mask(state->wid)))
			return SBI_EINVAL;
		if (state->widlist_mask & ~cpu_defaults->valid_wid_mask)
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

static int worldguard_parse_domain_state(const void *fdt,
					 const struct sbi_domain *dom,
					 struct worldguard_domain_state *state)
{
	int domain_offset, hoff, child, rc;
	bool found = false;

	sbi_memset(state, 0, sizeof(*state));
	if (!worldguard_runtime_enabled())
		return 0;
	if (!fdt || !dom || dom == &root)
		return 0;

	domain_offset = fdt_find_domain_offset(fdt, dom);
	if (domain_offset < 0)
		return (domain_offset == SBI_ENOENT) ? SBI_EINVAL : domain_offset;

	hoff = fdt_subnode_offset(fdt, domain_offset, "hw-isolation");
	if (hoff < 0)
		return SBI_EINVAL;

	fdt_for_each_subnode(child, fdt, hoff) {
		if (fdt_node_check_compatible(fdt, child, WGCHECKER2_COMPAT))
			continue;
		found = true;
		break;
	}

	if (!found)
		return SBI_EINVAL;

	rc = fdt_parse_u32(fdt, child, WORLDGUARD_PROP_WID, &state->wid);
	if (rc)
		return rc;
	if (state->wid >= WGCHECKER2_MAX_WIDS)
		return SBI_EINVAL;
	state->has_wid = true;

	rc = fdt_parse_u32_array_bitmask(fdt, child, WORLDGUARD_PROP_WIDLIST,
					 WGCHECKER2_MAX_WIDS,
					 &state->widlist_mask);
	if (rc)
		return rc;

	return worldguard_validate_domain_state(dom, state);
}

static int worldguard_domain_data_setup(struct sbi_domain *dom,
					struct sbi_domain_data *data,
					void *data_ptr)
{
	(void)data;

	return worldguard_parse_domain_state(fdt_get_address(), dom, data_ptr);
}

static void worldguard_domain_data_cleanup(struct sbi_domain *dom,
					   struct sbi_domain_data *data,
					   void *data_ptr)
{
	(void)dom;
	(void)data;
	(void)data_ptr;
}

static int worldguard_hprot_configure(struct sbi_scratch *scratch,
				      struct sbi_domain *dom)
{
	struct worldguard_domain_state *state = NULL;
	u32 valid_mask = worldguard_valid_wid_mask();
	u32 mlwid = worldguard_fallback_wid();
	u32 mwiddeleg = 0;
	u32 slwid = mlwid;

	(void)scratch;
	if (!worldguard_runtime_enabled())
		return 0;

	state = sbi_domain_data_ptr(dom, &worldguard_domain_data);
	if (state && state->has_wid &&
	    (worldguard_wid_mask(state->wid) & valid_mask))
		mlwid = state->wid;
	if (state)
		mwiddeleg = state->widlist_mask & valid_mask;

	slwid = worldguard_select_slwid(mwiddeleg, state && state->has_wid,
					state ? state->wid : 0, mlwid);
	worldguard_program_wid_state(mlwid, mwiddeleg, slwid);

	sbi_printf("[WG] configure dom=%s mlwid=%u mwiddeleg=0x%x slwid=%u\n",
		   dom ? dom->name : "<null>", mlwid, mwiddeleg, slwid);

	return 0;
}

static void worldguard_hprot_unconfigure(struct sbi_scratch *scratch,
					 struct sbi_domain *dom)
{
	(void)scratch;
	(void)dom;

	if (!worldguard_runtime_enabled())
		return;

	worldguard_program_wid_state(worldguard_fallback_wid(), 0,
				     worldguard_fallback_wid());

	sbi_printf("[WG] unconfigure dom=%s mlwid=%u mwiddeleg=0x0\n",
		   dom ? dom->name : "<null>", worldguard_fallback_wid());
}

static struct sbi_hart_protection worldguard_hprot = {
	.name = "wgid",
	.type = SBI_HART_PROTECTION_TYPE_ID,
	.rating = 1,
	.configure = worldguard_hprot_configure,
	.unconfigure = worldguard_hprot_unconfigure,
};

int worldguard_setup(const void *fdt)
{
	struct worldguard_platform_ctx *platform = NULL;
	u32 checker_count;
	bool has_runtime, data_registered_now = false;
	int rc;

	if (worldguard_platform)
		return 0;
	if (!fdt)
		return 0;

	checker_count = wgchecker2_count_platform_checkers((void *)fdt);
	has_runtime = worldguard_has_cpu_runtime(fdt);
	if (!checker_count && !has_runtime)
		return 0;

	if (has_runtime && !worldguard_hprot_registered) {
		rc = sbi_hart_protection_register(&worldguard_hprot);
		if (rc && rc != SBI_EALREADY)
			return rc;
		worldguard_hprot_registered = true;
	}

	if (has_runtime && !worldguard_domain_data_registered) {
		worldguard_domain_data.data_size =
			sizeof(struct worldguard_domain_state);
		worldguard_domain_data.data_setup = worldguard_domain_data_setup;
		worldguard_domain_data.data_cleanup =
			worldguard_domain_data_cleanup;
		rc = sbi_domain_register_data(&worldguard_domain_data);
		if (rc)
			return rc;
		worldguard_domain_data_registered = true;
		data_registered_now = true;
	}

	platform = sbi_zalloc(sizeof(*platform));
	if (!platform) {
		rc = SBI_ENOMEM;
		goto err;
	}

	platform->hart_count = sbi_hart_count();
	platform->runtime_enabled = has_runtime;
	platform->hart_defaults = sbi_calloc(sizeof(*platform->hart_defaults),
					     platform->hart_count);
	if (!platform->hart_defaults) {
		rc = SBI_ENOMEM;
		goto err;
	}

	worldguard_init_cpu_defaults(platform);
	rc = worldguard_parse_cpu_defaults(fdt, platform);
	if (rc)
		goto err;

	worldguard_platform = platform;

	if (checker_count) {
		rc = wgchecker2_init((void *)fdt);
		if (rc)
			goto err;
	}

	return 0;

err:
	if (worldguard_platform == platform)
		worldguard_platform = NULL;
	worldguard_free_platform();
	wgchecker2_cleanup();
	if (data_registered_now) {
		sbi_domain_unregister_data(&worldguard_domain_data);
		worldguard_domain_data_registered = false;
	}
	return rc;
}

#ifdef CONFIG_SBIUNIT
int worldguard_test_check_runtime_state(bool runtime_enabled)
{
	if (!worldguard_platform)
		return runtime_enabled ? SBI_ENOENT : 0;
	if (worldguard_platform->runtime_enabled != runtime_enabled)
		return SBI_EINVAL;

	return 0;
}

int worldguard_test_check_domain_state(const struct sbi_domain *dom,
				       bool expect_state, u32 wid,
				       u32 widlist_mask)
{
	struct worldguard_domain_state *state;

	state = sbi_domain_data_ptr((struct sbi_domain *)dom,
				    &worldguard_domain_data);
	if (!expect_state)
		return state && state->has_wid ? SBI_EINVAL : 0;
	if (!state || !state->has_wid)
		return SBI_ENOENT;
	if (state->wid != wid || state->widlist_mask != widlist_mask)
		return SBI_EINVAL;

	return 0;
}
#endif
