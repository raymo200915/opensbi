/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * QEMU virt WorldGuard checker demo hardware isolation mechanism
 *
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#include <libfdt.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_hwiso.h>
#include <sbi/sbi_string.h>

#define WG_DEMO_MAX_WIDS	8

struct wg_demo_ctx {
	bool has_wid;
	u32 wid;
	u32 widlist_count;
	u32 widlist[WG_DEMO_MAX_WIDS];
};

static u64 wg_demo_read_cells(const fdt32_t *cells, int count)
{
	u64 val = 0;
	int i;

	for (i = 0; i < count; i++)
		val = (val << 32) | fdt32_to_cpu(cells[i]);

	return val;
}

static void wg_demo_print_reg(void *fdt, int node)
{
	const fdt32_t *reg;
	int parent, addr_cells, size_cells, entry_cells;
	int len, i, entries;
	u64 base, size;

	parent = fdt_parent_offset(fdt, node);
	if (parent < 0)
		return;

	addr_cells = fdt_address_cells(fdt, parent);
	size_cells = fdt_size_cells(fdt, parent);
	if (addr_cells <= 0 || addr_cells > 2 || size_cells <= 0 ||
	    size_cells > 2)
		return;

	reg = fdt_getprop(fdt, node, "reg", &len);
	if (!reg || len <= 0)
		return;

	entry_cells = addr_cells + size_cells;
	entries = len / (entry_cells * sizeof(fdt32_t));
	for (i = 0; i < entries; i++) {
		base = wg_demo_read_cells(reg, addr_cells);
		size = wg_demo_read_cells(reg + addr_cells, size_cells);
		sbi_printf("[WG]   reg[%d] base=0x%llx size=0x%llx\n",
			   i, (unsigned long long)base,
			   (unsigned long long)size);
		reg += entry_cells;
	}
}

static void wg_demo_print_perms(void *fdt, int cfg)
{
	const fdt32_t *perms;
	int len, i, count;

	perms = fdt_getprop(fdt, cfg, "perms", &len);
	if (!perms || len <= 0)
		return;

	count = len / sizeof(fdt32_t);
	sbi_printf("[WG]   perms:");
	for (i = 0; i < count; i++)
		sbi_printf(" 0x%x", fdt32_to_cpu(perms[i]));
	sbi_printf("\n");
}

static void wg_demo_print_cfg_ranges(void *fdt, int cfg)
{
	const fdt32_t *reg;
	int len, i, entries;
	u64 base, size;

	reg = fdt_getprop(fdt, cfg, "reg", &len);
	if (!reg || len <= 0)
		return;

	if (len % (4 * sizeof(fdt32_t))) {
		sbi_printf("[WG]   invalid worldguard_cfg reg length %d\n",
			   len);
		return;
	}

	entries = len / (4 * sizeof(fdt32_t));
	for (i = 0; i < entries; i++) {
		base = wg_demo_read_cells(reg, 2);
		size = wg_demo_read_cells(reg + 2, 2);
		sbi_printf("[WG]   range[%d] base=0x%llx size=0x%llx\n",
			   i, (unsigned long long)base,
			   (unsigned long long)size);
		reg += 4;
	}
}

static void wg_demo_print_worldguard_cfg(void *fdt, int node)
{
	int cfg;

	cfg = fdt_subnode_offset(fdt, node, "worldguard_cfg");
	if (cfg < 0)
		return;

	sbi_printf("[WG] worldguard_cfg for %s\n",
		   fdt_get_name(fdt, node, NULL));
	wg_demo_print_reg(fdt, node);
	wg_demo_print_cfg_ranges(fdt, cfg);
	wg_demo_print_perms(fdt, cfg);
}

static int wg_demo_init(void *fdt)
{
	const fdt32_t *subs;
	const char *device_type;
	int checker, len, i, count, node;

	sbi_printf("[WG] wg_demo_init\n");

	if (!fdt)
		return 0;

	checker = -1;
	while (true) {
		checker = fdt_node_offset_by_compatible(fdt, checker,
							"sifive,wgchecker2");
		if (checker < 0)
			break;

		subs = fdt_getprop(fdt, checker, "sifive,subordinates", &len);
		if (!subs || len <= 0)
			continue;

		sbi_printf("[WG] checker %s\n",
			   fdt_get_name(fdt, checker, NULL));
		wg_demo_print_reg(fdt, checker);

		count = len / sizeof(fdt32_t);
		for (i = 0; i < count; i++) {
			node = fdt_node_offset_by_phandle(
					fdt, fdt32_to_cpu(subs[i]));
			if (node < 0)
				continue;

			wg_demo_print_worldguard_cfg(fdt, node);
		}
	}

	fdt_for_each_subnode(node, fdt, 0) {
		device_type = fdt_getprop(fdt, node, "device_type", NULL);
		if (device_type && !sbi_strcmp(device_type, "memory"))
			wg_demo_print_worldguard_cfg(fdt, node);
	}

	return 0;
}

static int wg_demo_domain_init(void *fdt, int domain_offset,
			       struct sbi_domain *dom, void **out_ctx)
{
	const fdt32_t *val;
	struct wg_demo_ctx *ctx;
	int hoff, child, len, i, count;

	sbi_printf("[WG] wg_demo_domain_init\n");

	if (!out_ctx)
		return SBI_EINVAL;

	*out_ctx = NULL;

	ctx = sbi_zalloc(sizeof(*ctx));
	if (!ctx)
		return SBI_ENOMEM;

	if (fdt && domain_offset >= 0) {
		hoff = fdt_subnode_offset(fdt, domain_offset, "hw-isolation");
		if (hoff >= 0) {
			fdt_for_each_subnode(child, fdt, hoff) {
				if (fdt_node_check_compatible(
						fdt, child, "sifive,wgchecker2"))
					continue;

				val = fdt_getprop(fdt, child,
						  "worldguard,wid", &len);
				if (val && len >= sizeof(fdt32_t)) {
					ctx->has_wid = true;
					ctx->wid = fdt32_to_cpu(val[0]);
				}

				val = fdt_getprop(fdt, child,
						  "worldguard,widlist", &len);
				if (val && len > 0) {
					count = len / sizeof(fdt32_t);
					if (count > WG_DEMO_MAX_WIDS)
						count = WG_DEMO_MAX_WIDS;
					ctx->widlist_count = count;
					for (i = 0; i < count; i++)
						ctx->widlist[i] =
							fdt32_to_cpu(val[i]);
				}

				break;
			}
		}
	}

	sbi_printf("[WG] domain_init %s wid=%s",
		   dom ? dom->name : "<null>",
		   ctx->has_wid ? "" : "<none>");
	if (ctx->has_wid)
		sbi_printf("%u", ctx->wid);
	sbi_printf(" widlist_count=%u\n", ctx->widlist_count);

	*out_ctx = ctx;

	return 0;
}

static void wg_demo_domain_exit(const struct sbi_domain *src,
				const struct sbi_domain *dst, void *ctx)
{
	(void)ctx;

	sbi_printf("[WG] wg_demo_domain_exit\n");

	sbi_printf("[WG] domain_exit src=%s dst=%s\n",
		   src ? src->name : "<null>",
		   dst ? dst->name : "<null>");
}

static void wg_demo_domain_enter(const struct sbi_domain *dst,
				 const struct sbi_domain *src, void *ctx)
{
	struct wg_demo_ctx *wctx = ctx;
	u32 i;

	(void)src;

	sbi_printf("[WG] wg_demo_domain_enter\n");

	sbi_printf("[WG] domain_enter dst=%s", dst ? dst->name : "<null>");
	if (wctx && wctx->has_wid)
		sbi_printf(" wid=%u", wctx->wid);
	if (wctx && wctx->widlist_count) {
		sbi_printf(" widlist=");
		for (i = 0; i < wctx->widlist_count; i++)
			sbi_printf("%s%u", i ? "," : "", wctx->widlist[i]);
	}
	sbi_printf("\n");
}

static void wg_demo_domain_cleanup(struct sbi_domain *dom, void *ctx)
{
	sbi_printf("[WG] wg_demo_domain_cleanup\n");

	sbi_printf("[WG] domain_cleanup %s\n",
		   dom ? dom->name : "<null>");
	sbi_free(ctx);
}

static const struct sbi_hwiso_ops wgchecker_demo_ops = {
	.name = "sifive,wgchecker2",
	.init = wg_demo_init,
	.domain_init = wg_demo_domain_init,
	.domain_exit = wg_demo_domain_exit,
	.domain_enter = wg_demo_domain_enter,
	.domain_cleanup = wg_demo_domain_cleanup,
};

int qemu_virt_hwiso_register(void *fdt)
{
	if (!fdt)
		return 0;

	if (fdt_node_check_compatible(fdt, 0, "riscv-virtio") &&
	    fdt_node_check_compatible(fdt, 0, "qemu,virt"))
		return 0;

	return sbi_hwiso_register(&wgchecker_demo_ops);
}
