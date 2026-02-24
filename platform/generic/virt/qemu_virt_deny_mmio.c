/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * QEMU virt deny-mmio hardware isolation mechanism
 *
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#include <libfdt.h>
#include <sbi/riscv_asm.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_hwiso.h>
#include <sbi/sbi_math.h>
#include <sbi/sbi_scratch.h>

#define QEMU_HWISO_MAX_REGIONS		4

struct qemu_hwiso_region {
	u64 base;
	u32 order;
};

struct qemu_hwiso_ctx {
	u32 region_count;
	struct qemu_hwiso_region regions[QEMU_HWISO_MAX_REGIONS];
};

struct qemu_hwiso_hart_state {
	u8 count;
	u8 entries[QEMU_HWISO_MAX_REGIONS];
};

static unsigned long qemu_hwiso_state_offset;

static int qemu_hwiso_init(void *fdt)
{
	(void)fdt;
	sbi_printf("[QEMU] qemu_hwiso_init\n");

	if (qemu_hwiso_state_offset)
		return 0;

	qemu_hwiso_state_offset = sbi_scratch_alloc_offset(
					sizeof(struct qemu_hwiso_hart_state));
	if (!qemu_hwiso_state_offset)
		return SBI_ENOMEM;

	return 0;
}

static int qemu_hwiso_parse_node(void *fdt, int node_offset,
				 struct qemu_hwiso_ctx *ctx)
{
	const u32 *val;
	u64 base, size;
	u32 order, count, i;
	int len;

	val = fdt_getprop(fdt, node_offset, "deny-mmio", &len);
	if (!val || len <= 0)
		return 0;

	if (len % 16)
		return SBI_EINVAL;

	count = len / 16;
	if (!count || count > QEMU_HWISO_MAX_REGIONS)
		return SBI_EINVAL;

	for (i = 0; i < count; i++) {
		base = fdt32_to_cpu(val[i * 4]);
		base = (base << 32) | fdt32_to_cpu(val[(i * 4) + 1]);
		size = fdt32_to_cpu(val[(i * 4) + 2]);
		size = (size << 32) | fdt32_to_cpu(val[(i * 4) + 3]);

		if (!size || (size & (size - 1)))
			return SBI_EINVAL;

		order = log2roundup(size);
		if (order < PMP_SHIFT || order > __riscv_xlen)
			return SBI_EINVAL;

		if (base & (size - 1))
			return SBI_EINVAL;

		ctx->regions[i].base = base;
		ctx->regions[i].order = order;
	}

	ctx->region_count = count;

	return 0;
}

static int qemu_hwiso_domain_init(void *fdt, int domain_offset,
				  struct sbi_domain *dom, void **out_ctx)
{
	int hoff, child, rc;
	struct qemu_hwiso_ctx *ctx;

	sbi_printf("[QEMU] qemu_hwiso_domain_init: %s\n",
		   dom ? dom->name : "<null>");

	if (!out_ctx)
		return SBI_EINVAL;

	*out_ctx = NULL;

	if (!fdt || (domain_offset < 0) || !dom)
		return 0;

	hoff = fdt_subnode_offset(fdt, domain_offset, "hw-isolation");
	if (hoff < 0)
		return 0;

	fdt_for_each_subnode(child, fdt, hoff) {
		if (fdt_node_check_compatible(
				fdt, child,
				"opensbi,qemu-virt-deny-mmio"))
			continue;

		ctx = sbi_zalloc(sizeof(*ctx));
		if (!ctx)
			return SBI_ENOMEM;

		rc = qemu_hwiso_parse_node(fdt, child, ctx);
		if (rc) {
			sbi_free(ctx);
			return rc;
		}

		*out_ctx = ctx;
		return 0;
	}

	return 0;
}

static void qemu_hwiso_domain_exit(const struct sbi_domain *src,
				   const struct sbi_domain *dst, void *ctx)
{
	struct qemu_hwiso_ctx *qctx = ctx;
	struct qemu_hwiso_hart_state *state;
	u8 i;

	(void)dst;
	sbi_printf("[QEMU] qemu_hwiso_domain_exit: from %s\n",
		   src ? src->name : "<null>");

	if (!qemu_hwiso_state_offset)
		return;

	state = sbi_scratch_thishart_offset_ptr(qemu_hwiso_state_offset);
	if (state->count > QEMU_HWISO_MAX_REGIONS) {
		state->count = 0;
		return;
	}
	for (i = 0; i < state->count; i++)
		pmp_disable(state->entries[i]);
	state->count = 0;

	sbi_printf("[QEMU] qemu-virt-hwiso: deny list cleared\n");
	if (qctx && qctx->region_count) {
		for (i = 0; i < qctx->region_count; i++) {
			unsigned long long size_kb = 1ULL <<
						     qctx->regions[i].order;

			size_kb >>= 10;
			sbi_printf("  [%u] base=0x%lx order=%u (size=0x%llxKB)\n",
				    i,
				    (unsigned long)qctx->regions[i].base,
				    qctx->regions[i].order,
				    size_kb);
		}
	} else {
		sbi_printf("  (none)\n");
	}
}

static void qemu_hwiso_domain_enter(const struct sbi_domain *dst,
				    const struct sbi_domain *src, void *ctx)
{
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
	struct qemu_hwiso_ctx *qctx = ctx;
	struct qemu_hwiso_hart_state *state;
	unsigned int pmp_count, pmp_log2gran;
	int idx, found;
	u32 i;

	(void)src;
	sbi_printf("[QEMU] qemu_hwiso_domain_enter: to %s\n",
		   dst ? dst->name : "<null>");

	if (!qemu_hwiso_state_offset || !qctx || !qctx->region_count)
		return;

	state = sbi_scratch_thishart_offset_ptr(qemu_hwiso_state_offset);

	pmp_count = sbi_hart_pmp_count(scratch);
	pmp_log2gran = sbi_hart_pmp_log2gran(scratch);
	state->count = 0;

	for (i = 0; i < qctx->region_count; i++) {
		if (qctx->regions[i].order < pmp_log2gran)
			continue;

		found = -1;
		for (idx = (int)pmp_count - 1; idx >= 0; idx--) {
			if (idx == SBI_SMEPMP_RESV_ENTRY)
				continue;
			if (is_pmp_entry_mapped(idx))
				continue;
			found = idx;
			break;
		}

		if (found < 0)
			break;

		if (!pmp_set(found, 0, qctx->regions[i].base,
			     qctx->regions[i].order)) {
			state->entries[state->count++] = found;
			if (state->count >= QEMU_HWISO_MAX_REGIONS)
				break;
		}
	}

	if (state->count) {
		sbi_printf("[QEMU] qemu-virt-hwiso: deny list applied (%u entries)\n",
			    state->count);
		for (i = 0; i < qctx->region_count; i++) {
			unsigned long long size_kb = 1ULL <<
						     qctx->regions[i].order;

			size_kb >>= 10;
			sbi_printf("  [%u] base=0x%lx order=%u (size=0x%llxKB)\n",
				    i,
				    (unsigned long)qctx->regions[i].base,
				    qctx->regions[i].order,
				    size_kb);
		}
	}
}

static void qemu_hwiso_domain_cleanup(struct sbi_domain *dom, void *ctx)
{
	(void)dom;
	sbi_printf("[QEMU] qemu_hwiso_domain_cleanup: %s\n",
		   dom ? dom->name : "<null>");
	sbi_free(ctx);
}

static const struct sbi_hwiso_ops qemu_virt_deny_mmio_ops = {
	.name = "opensbi,qemu-virt-deny-mmio",
	.init = qemu_hwiso_init,
	.domain_init = qemu_hwiso_domain_init,
	.domain_exit = qemu_hwiso_domain_exit,
	.domain_enter = qemu_hwiso_domain_enter,
	.domain_cleanup = qemu_hwiso_domain_cleanup,
};

int qemu_virt_hwiso_register(void *fdt)
{
	if (!fdt)
		return 0;

	if (fdt_node_check_compatible(fdt, 0, "riscv-virtio") &&
	    fdt_node_check_compatible(fdt, 0, "qemu,virt"))
		return 0;

	return sbi_hwiso_register(&qemu_virt_deny_mmio_ops);
}
