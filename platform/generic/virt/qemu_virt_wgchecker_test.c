/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */
#include <libfdt.h>
#include <sbi/riscv_encoding.h>
#include <sbi/riscv_io.h>
#include <sbi/sbi_console.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_hwiso.h>
#include <sbi/sbi_hwiso_test.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_unit_test.h>
#include <sbi/sbi_unpriv.h>
#include <sbi_utils/fdt/fdt_helper.h>
#include <wgchecker2.h>
#include <worldguard.h>

struct qemu_virt_wg_slot_expect {
	bool check_addr;
	u64 addr;
	u64 perm;
	u32 cfg;
};

struct qemu_virt_wg_checker_expect {
	const char *path;
	u32 slot_count;
	struct qemu_virt_wg_slot_expect *slots;
	struct qemu_virt_wg_slot_expect last_slot;
};

struct qemu_virt_wg_expect {
	u32 wid;
	u32 widlist_mask;
	bool check_slwid;
	u32 slwid;
};

#define QEMU_VIRT_WG_DENIED_STORE_ADDR		0xc0001000UL

static u64 qemu_virt_wg_mmio_read64(unsigned long addr)
{
#if __riscv_xlen != 32
	return readq((void *)addr);
#else
	return readl((void *)addr) | ((u64)readl((void *)(addr + 4)) << 32);
#endif
}

static struct qemu_virt_wg_slot_expect qemu_virt_wg_dram_slots[] = {
		{ .check_addr = true, .addr = 0x20000000ULL, .perm = 0, .cfg = WGCHECKER2_SLOT_CFG_A_OFF },
		{ .check_addr = true, .addr = 0x30000000ULL, .perm = 0xcf, .cfg = WGCHECKER2_SLOT_CFG_A_TOR },
		{ .check_addr = true, .addr = 0x30400000ULL, .perm = 0xcc, .cfg = WGCHECKER2_SLOT_CFG_A_TOR },
		{ .check_addr = true, .addr = 0x40000000ULL, .perm = 0xcf, .cfg = WGCHECKER2_SLOT_CFG_A_TOR },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
};

static struct qemu_virt_wg_slot_expect qemu_virt_wg_zero_slots[] = {
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
	{ .check_addr = true, .addr = 0, .perm = 0, .cfg = 0 },
};

static const struct qemu_virt_wg_checker_expect qemu_virt_wg_checker_expects[] = {
	{
		.path = "/soc/wgchecker@6000000",
		.slot_count = 16,
		.slots = qemu_virt_wg_dram_slots,
		.last_slot = { .perm = 0, .cfg = 0 },
	},
	{
		.path = "/soc/wgchecker@6001000",
		.slot_count = 16,
		.slots = qemu_virt_wg_zero_slots,
		.last_slot = { .perm = 0xc3, .cfg = WGCHECKER2_SLOT_CFG_A_TOR },
	},
	{
		.path = "/soc/wgchecker@6002000",
		.slot_count = 1,
		.slots = NULL,
		.last_slot = { .perm = 0xc0, .cfg = WGCHECKER2_SLOT_CFG_A_TOR },
	},
};

static const struct qemu_virt_wg_expect qemu_virt_wg_root_expect = {
	.wid = 3,
	.widlist_mask = 0,
	.check_slwid = false,
};

static const struct qemu_virt_wg_expect qemu_virt_wg_domain0_expect = {
	.wid = 0,
	.widlist_mask = 0xb,
	.check_slwid = true,
	.slwid = 0,
};

static const struct qemu_virt_wg_expect qemu_virt_wg_domain1_expect = {
	.wid = 1,
	.widlist_mask = 0xa,
	.check_slwid = true,
	.slwid = 1,
};

static struct sbi_domain *qemu_virt_wg_find_domain(const char *name)
{
	u32 i;
	struct sbi_domain *dom;

	sbi_domain_for_each(i, dom) {
		if (!sbi_strcmp(dom->name, name))
			return dom;
	}

	return NULL;
}

static const struct qemu_virt_wg_expect *
qemu_virt_wg_domain_expect(const struct sbi_domain *dom)
{
	if (dom == &root)
		return &qemu_virt_wg_root_expect;
	if (dom && !sbi_strcmp(dom->name, "root"))
		return &qemu_virt_wg_root_expect;
	if (!dom)
		return NULL;
	if (!sbi_strcmp(dom->name, "domain@0"))
		return &qemu_virt_wg_domain0_expect;
	if (!sbi_strcmp(dom->name, "domain@1"))
		return &qemu_virt_wg_domain1_expect;

	return NULL;
}

static void qemu_virt_wg_assert_checker(struct sbiunit_test_case *test,
					const struct qemu_virt_wg_checker_expect *expect)
{
	void *fdt = fdt_get_address();
	u64 base, size, addr, perm;
	u32 cfg, slot;
	int node, rc;

	node = fdt_path_offset(fdt, expect->path);
	SBIUNIT_ASSERT(test, node >= 0);

	rc = fdt_get_node_addr_size(fdt, node, 0, &base, &size);
	SBIUNIT_ASSERT_EQ(test, rc, 0);
	(void)size;

	SBIUNIT_ASSERT_EQ(test,
				  readl((void *)(unsigned long)
					(base + WGCHECKER2_MMIO_NSLOTS)),
			  expect->slot_count);
	SBIUNIT_ASSERT_EQ(test,
				  qemu_virt_wg_mmio_read64(base + WGCHECKER2_MMIO_ERRCAUSE),
			  0UL);
	SBIUNIT_ASSERT_EQ(test,
				  qemu_virt_wg_mmio_read64(base + WGCHECKER2_MMIO_ERRADDR),
			  0UL);

	for (slot = 1; slot < expect->slot_count; slot++) {
		addr = qemu_virt_wg_mmio_read64(
				base + WGCHECKER2_MMIO_SLOT_BASE +
				slot * WGCHECKER2_MMIO_SLOT_STRIDE +
				WGCHECKER2_MMIO_SLOT_ADDR);
		perm = qemu_virt_wg_mmio_read64(
				base + WGCHECKER2_MMIO_SLOT_BASE +
				slot * WGCHECKER2_MMIO_SLOT_STRIDE +
				WGCHECKER2_MMIO_SLOT_PERM);
		cfg = readl((void *)(unsigned long)
				    (base + WGCHECKER2_MMIO_SLOT_BASE +
				     slot * WGCHECKER2_MMIO_SLOT_STRIDE +
				     WGCHECKER2_MMIO_SLOT_CFG)) &
			      WGCHECKER2_SLOT_CFG_A_MASK;

		if (expect->slots[slot - 1].check_addr)
			SBIUNIT_ASSERT_EQ(test, addr, expect->slots[slot - 1].addr);
		SBIUNIT_ASSERT_EQ(test, perm, expect->slots[slot - 1].perm);
		SBIUNIT_ASSERT_EQ(test, cfg, expect->slots[slot - 1].cfg);
	}

	perm = qemu_virt_wg_mmio_read64(
			base + WGCHECKER2_MMIO_SLOT_BASE +
			expect->slot_count * WGCHECKER2_MMIO_SLOT_STRIDE +
			WGCHECKER2_MMIO_SLOT_PERM);
	cfg = readl((void *)(unsigned long)
		    (base + WGCHECKER2_MMIO_SLOT_BASE +
		     expect->slot_count * WGCHECKER2_MMIO_SLOT_STRIDE +
		     WGCHECKER2_MMIO_SLOT_CFG)) &
		      WGCHECKER2_SLOT_CFG_A_MASK;
	SBIUNIT_ASSERT_EQ(test, perm, expect->last_slot.perm);
	SBIUNIT_ASSERT_EQ(test, cfg, expect->last_slot.cfg);
}

static void qemu_virt_wg_assert_state(struct sbiunit_test_case *test,
				      const struct sbi_domain *dom, void *ctx)
{
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
	const struct qemu_virt_wg_expect *expect;

	(void)ctx;

	if (!sbi_hart_has_extension(scratch, SBI_HART_EXT_SMWG))
		return;
	expect = qemu_virt_wg_domain_expect(dom);
	if (!expect)
		return;

	SBIUNIT_ASSERT_EQ(test, csr_read(CSR_MLWID), expect->wid);

	if (!sbi_hart_has_extension(scratch, SBI_HART_EXT_SSWG))
		return;

	SBIUNIT_ASSERT_EQ(test, csr_read(CSR_MWIDDELEG), expect->widlist_mask);
	if (!expect->check_slwid)
		return;

	SBIUNIT_ASSERT_EQ(test, csr_read(CSR_SLWID), expect->slwid);
}

static void qemu_virt_wg_assert_quiesced(struct sbiunit_test_case *test,
					 const struct sbi_domain *dom, void *ctx)
{
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();

	(void)dom;
	(void)ctx;

	if (!sbi_hart_has_extension(scratch, SBI_HART_EXT_SMWG))
		return;

	SBIUNIT_ASSERT_EQ(test, csr_read(CSR_MLWID), qemu_virt_wg_root_expect.wid);

	if (!sbi_hart_has_extension(scratch, SBI_HART_EXT_SSWG))
		return;

	SBIUNIT_ASSERT_EQ(test, csr_read(CSR_MWIDDELEG), 0UL);
}

static void qemu_virt_wg_boot_test(struct sbiunit_test_case *test)
{
	struct sbi_domain *dom0, *dom1;
	u32 i;

	for (i = 0;
	     i < (sizeof(qemu_virt_wg_checker_expects) /
		  sizeof(qemu_virt_wg_checker_expects[0]));
	     i++)
		qemu_virt_wg_assert_checker(test, &qemu_virt_wg_checker_expects[i]);

	SBIUNIT_ASSERT_EQ(test, worldguard_test_check_runtime_state(true), 0);
	SBIUNIT_ASSERT_EQ(test, wgchecker2_checker_count(), 3);

	dom0 = qemu_virt_wg_find_domain("domain@0");
	dom1 = qemu_virt_wg_find_domain("domain@1");
	SBIUNIT_ASSERT_NE(test, dom0, NULL);
	SBIUNIT_ASSERT_NE(test, dom1, NULL);
	SBIUNIT_ASSERT_EQ(test,
			  worldguard_test_check_domain_state(
				dom0, true, qemu_virt_wg_domain0_expect.wid,
				qemu_virt_wg_domain0_expect.widlist_mask),
			  0);
	SBIUNIT_ASSERT_EQ(test,
			  worldguard_test_check_domain_state(
				dom1, true, qemu_virt_wg_domain1_expect.wid,
				qemu_virt_wg_domain1_expect.widlist_mask),
			  0);
	SBIUNIT_ASSERT_EQ(test,
			  worldguard_test_check_domain_state(&root, false, 0, 0),
			  0);
}

static void qemu_virt_wg_failure_test(struct sbiunit_test_case *test)
{
	struct sbi_domain *cur_dom = sbi_domain_thishart_ptr();
	struct sbi_domain *dom0 = qemu_virt_wg_find_domain("domain@0");
	struct sbi_trap_info trap = { 0 };

	SBIUNIT_ASSERT_NE(test, cur_dom, NULL);
	SBIUNIT_ASSERT_NE(test, dom0, NULL);

	if (cur_dom != dom0) {
		sbi_hwiso_domain_exit(cur_dom, dom0);
		sbi_hwiso_domain_enter(dom0, cur_dom);
	}

	sbi_store_u32((u32 *)QEMU_VIRT_WG_DENIED_STORE_ADDR, 0x5a5aa5a5U,
		      &trap);

	if (cur_dom != dom0) {
		sbi_hwiso_domain_exit(dom0, cur_dom);
		sbi_hwiso_domain_enter(cur_dom, dom0);
	}

	sbi_printf("[WG TEST] failure trap cause=0x%lx tval=0x%lx\n",
		   trap.cause, trap.tval);

	SBIUNIT_ASSERT_EQ(test, trap.cause, CAUSE_STORE_ACCESS);
	SBIUNIT_ASSERT_EQ(test, trap.tval, QEMU_VIRT_WG_DENIED_STORE_ADDR);
}

const struct sbi_hwiso_test_ops qemu_virt_worldguard_test_ops = {
	.boot_test = qemu_virt_wg_boot_test,
	.failure_test = qemu_virt_wg_failure_test,
	.domain_state_test = qemu_virt_wg_assert_state,
	.domain_quiesce_test = qemu_virt_wg_assert_quiesced,
};
