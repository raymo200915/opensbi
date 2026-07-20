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
#include <wgchecker.h>
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

static u64 worldguard_wid_mask(u32 wid)
{
	return (wid < WORLDGUARD_MAX_WIDS) ? (1ULL << wid) : 0;
}

static int worldguard_parse_wid_bitmap_u64(const void *fdt, int nodeoff,
					   const char *prop_name,
					   u32 max_bits, u64 *out_mask)
{
	u64 mask64;
	int rc;

	if (!out_mask || !max_bits || max_bits > WORLDGUARD_MAX_WIDS)
		return SBI_EINVAL;

	*out_mask = 0;
	rc = fdt_parse_u64(fdt, nodeoff, prop_name, &mask64);
	if (rc)
		return rc;
	if (max_bits < WORLDGUARD_MAX_WIDS && (mask64 >> max_bits))
		return SBI_EINVAL;

	*out_mask = mask64;
	return 0;
}

static int worldguard_parse_wid_u32_or_u64(const void *fdt, int nodeoff,
					   const char *prop_name, u32 max_wid,
					   u32 *out_wid)
{
	const fdt32_t *prop;
	u64 wid64;
	int len;

	if (!out_wid)
		return SBI_EINVAL;

	prop = fdt_getprop(fdt, nodeoff, prop_name, &len);
	if (!prop)
		return SBI_ENOENT;
	if (len == (int)sizeof(fdt32_t))
		wid64 = fdt32_to_cpu(prop[0]);
	else if (len == (int)(2 * sizeof(fdt32_t)))
		wid64 = ((u64)fdt32_to_cpu(prop[0]) << 32) |
			fdt32_to_cpu(prop[1]);
	else
		return SBI_EINVAL;
	if (wid64 >= max_wid)
		return SBI_EINVAL;

	*out_wid = (u32)wid64;
	return 0;
}

static int worldguard_parse_wid_list(const void *fdt, int nodeoff,
					     const char *prop_name, u64 *out_mask)
{
	const fdt32_t *prop;
	u32 i, wid;
	int len;

	if (!out_mask)
		return SBI_EINVAL;

	prop = fdt_getprop(fdt, nodeoff, prop_name, &len);
	if (!prop || !len || len % sizeof(*prop))
		return SBI_EINVAL;

	*out_mask = 0;
	for (i = 0; i < len / sizeof(*prop); i++) {
		wid = fdt32_to_cpu(prop[i]);
		if (wid >= WORLDGUARD_MAX_WIDS)
			return SBI_EINVAL;
		*out_mask |= worldguard_wid_mask(wid);
	}

	return 0;
}

static bool worldguard_runtime_enabled(void)
{
	return worldguard_platform && worldguard_platform->runtime_enabled;
}

static void worldguard_free_platform(struct worldguard_platform_ctx *platform)
{
	if (!platform)
		return;

	sbi_free(platform->hart_defaults);
	sbi_free(platform);
}

static int worldguard_parse_cpu_defaults(const void *fdt,
					 struct worldguard_platform_ctx *platform)
{
	struct wg_cpu_defaults *cpu_defaults;
	u32 hartid, hartindex;
	int cpus_offset, cpu_offset, rc;

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

		cpu_defaults = &platform->hart_defaults[hartindex];
		rc = worldguard_parse_wid_u32_or_u64(fdt, cpu_offset,
						     WORLDGUARD_CPU_PROP_PMWID,
						     WORLDGUARD_MAX_WIDS,
						     &cpu_defaults->pmwid);
		if (rc && rc != SBI_ENOENT)
			return rc;

		rc = worldguard_parse_wid_bitmap_u64(fdt, cpu_offset,
					     WORLDGUARD_CPU_PROP_PMWIDLIST,
					     WORLDGUARD_MAX_WIDS,
					     &cpu_defaults->pmwidlist_mask);
		if (rc && rc != SBI_ENOENT)
			return rc;

		rc = worldguard_parse_wid_bitmap_u64(fdt, cpu_offset,
					     WORLDGUARD_CPU_PROP_PMLWIDLIST,
					     WORLDGUARD_MAX_WIDS,
					     &cpu_defaults->pmlwidlist_mask);
		if (rc && rc != SBI_ENOENT)
			return rc;

		cpu_defaults->pmwidlist_mask |=
			worldguard_wid_mask(cpu_defaults->pmwid);
	}

	return 0;
}

static bool worldguard_has_cpu_runtime(const void *fdt)
{
	u32 hartid;
	int cpus_offset, cpu_offset;

	if (!fdt)
		return false;

	cpus_offset = fdt_path_offset(fdt, "/cpus");
	if (cpus_offset < 0)
		return false;

	fdt_for_each_subnode(cpu_offset, fdt, cpus_offset) {
		if (fdt_parse_hart_id(fdt, cpu_offset, &hartid))
			continue;
		if (fdt_getprop(fdt, cpu_offset, WORLDGUARD_CPU_PROP_PMWID, NULL) ||
		    fdt_getprop(fdt, cpu_offset, WORLDGUARD_CPU_PROP_PMWIDLIST,
				NULL) ||
		    fdt_getprop(fdt, cpu_offset, WORLDGUARD_CPU_PROP_PMLWIDLIST,
				NULL))
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
		if (!sbi_hartmask_test_hartindex(hartindex, dom->possible_harts))
			continue;

		cpu_defaults = &worldguard_platform->hart_defaults[hartindex];
		if (!(cpu_defaults->pmwidlist_mask &
		      worldguard_wid_mask(state->wid)))
			return SBI_EINVAL;
		if (state->widlist_mask & ~cpu_defaults->pmlwidlist_mask)
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

	return cpu_defaults ? cpu_defaults->pmwid : 0;
}

static u64 worldguard_valid_pmwid_mask(void)
{
	const struct wg_cpu_defaults *cpu_defaults =
		worldguard_current_cpu_defaults();

	return cpu_defaults ? cpu_defaults->pmwidlist_mask :
			      worldguard_wid_mask(worldguard_fallback_wid());
}

static u64 worldguard_valid_pmlwid_mask(void)
{
	const struct wg_cpu_defaults *cpu_defaults =
		worldguard_current_cpu_defaults();

	return cpu_defaults ? cpu_defaults->pmlwidlist_mask :
			      worldguard_wid_mask(worldguard_fallback_wid());
}

static u32 worldguard_select_slwid(u64 widlist_mask, bool has_wid, u32 wid,
				   u32 fallback)
{
	u32 i;

	if (!widlist_mask)
		return fallback;

	if (has_wid && (worldguard_wid_mask(wid) & widlist_mask))
		return wid;

	for (i = 0; i < WORLDGUARD_MAX_WIDS; i++) {
		if (widlist_mask & (1ULL << i))
			return i;
	}

	return fallback;
}

static void worldguard_program_wid_state(u32 mlwid, u64 mlwidlist,
					 u64 mwiddeleg, u32 slwid)
{
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();

	if (!sbi_hart_has_extension(scratch, SBI_HART_EXT_SMLWID))
		return;

	if (sbi_hart_has_extension(scratch, SBI_HART_EXT_SMLWIDLIST))
		csr_write(CSR_MLWIDLIST, mlwidlist);

	if (!sbi_hart_has_extension(scratch, SBI_HART_EXT_SMWIDDELEG) ||
	    !sbi_hart_has_extension(scratch, SBI_HART_EXT_SSWID)) {
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
		return 0;

	fdt_for_each_subnode(child, fdt, hoff) {
		if (fdt_node_check_compatible(fdt, child, WGCHECKER_COMPAT))
			continue;
		found = true;
		break;
	}

	if (!found)
		return 0;

	rc = fdt_parse_u32(fdt, child, WORLDGUARD_PROP_WID, &state->wid);
	if (rc)
		return rc;
	if (state->wid >= WORLDGUARD_MAX_WIDS)
		return SBI_EINVAL;
	state->has_wid = true;

	rc = worldguard_parse_wid_list(fdt, child, WORLDGUARD_PROP_WIDLIST,
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
	u64 valid_pmwid_mask = worldguard_valid_pmwid_mask();
	u64 valid_pmlwid_mask = worldguard_valid_pmlwid_mask();
	u32 mlwid = worldguard_fallback_wid();
	u64 mlwidlist = valid_pmwid_mask;
	u64 mwiddeleg = 0;
	u32 slwid = mlwid;

	(void)scratch;
	if (!worldguard_runtime_enabled())
		return 0;

	state = sbi_domain_data_ptr(dom, &worldguard_domain_data);
	if (state && state->has_wid &&
	    (worldguard_wid_mask(state->wid) & valid_pmwid_mask))
		mlwid = state->wid;
	if (state && state->has_wid)
		mwiddeleg = state->widlist_mask & valid_pmlwid_mask;

	slwid = worldguard_select_slwid(mwiddeleg, state && state->has_wid,
					state ? state->wid : 0, mlwid);
	worldguard_program_wid_state(mlwid, mlwidlist, mwiddeleg, slwid);
	return 0;
}

static void worldguard_hprot_unconfigure(struct sbi_scratch *scratch,
					 struct sbi_domain *dom)
{
	(void)scratch;
	(void)dom;

	if (!worldguard_runtime_enabled())
		return;

	worldguard_program_wid_state(worldguard_fallback_wid(),
				     worldguard_valid_pmwid_mask(), 0,
				     worldguard_fallback_wid());
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
	bool hprot_registered_now = false;
	int rc;

	if (worldguard_platform)
		return 0;
	if (!fdt)
		return 0;

	checker_count = wgchecker_count_platform_checkers((void *)fdt);
	has_runtime = worldguard_has_cpu_runtime(fdt);
	if (!checker_count && !has_runtime)
		return 0;

	if (has_runtime && !worldguard_hprot_registered) {
		rc = sbi_hart_protection_register(&worldguard_hprot);
		if (rc)
			return rc;
		worldguard_hprot_registered = true;
		hprot_registered_now = true;
	}

	if (has_runtime && !worldguard_domain_data_registered) {
		worldguard_domain_data.data_size =
			sizeof(struct worldguard_domain_state);
		worldguard_domain_data.data_setup = worldguard_domain_data_setup;
		worldguard_domain_data.data_cleanup =
			worldguard_domain_data_cleanup;
		rc = sbi_domain_register_data(&worldguard_domain_data);
		if (rc)
			goto err;
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

	rc = worldguard_parse_cpu_defaults(fdt, platform);
	if (rc) {
		sbi_printf("%s: worldguard_parse_cpu_defaults failed (error %d)\n",
			   __func__, rc);
		goto err;
	}

	worldguard_platform = platform;

	/*
	 * Program the boot hart into the trusted/root world before
	 * enabling checker rules that may restrict DRAM access.
	 */
	if (has_runtime)
		worldguard_program_wid_state(worldguard_fallback_wid(),
					     worldguard_valid_pmwid_mask(), 0,
					     worldguard_fallback_wid());

	if (checker_count) {
		rc = wgchecker_init((void *)fdt);
		if (rc) {
			sbi_printf("%s: wgchecker_init failed (error %d)\n",
				   __func__, rc);
			goto err;
		}
	}

	return 0;

err:
	if (worldguard_platform == platform)
		worldguard_platform = NULL;
	worldguard_free_platform(platform);
	wgchecker_cleanup();
	if (data_registered_now) {
		sbi_domain_unregister_data(&worldguard_domain_data);
		worldguard_domain_data_registered = false;
	}
	if (hprot_registered_now) {
		sbi_hart_protection_unregister(&worldguard_hprot);
		worldguard_hprot_registered = false;
	}
	return rc;
}

bool worldguard_is_enabled(void)
{
	return worldguard_runtime_enabled() && wgchecker_checker_count();
}
