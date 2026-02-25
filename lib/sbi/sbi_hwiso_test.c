/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */
#include <sbi/riscv_asm.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_domain_context.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_string.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_unit_test.h>

static struct sbi_domain *hwiso_find_domain(const char *name)
{
	u32 i;
	struct sbi_domain *dom;

	sbi_domain_for_each(i, dom) {
		if (!sbi_strcmp(dom->name, name))
			return dom;
	}

	return NULL;
}

static void hwiso_snapshot_context(struct sbi_context *ctx)
{
	struct sbi_trap_regs *trap_regs;
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();

	trap_regs = (struct sbi_trap_regs *)(csr_read(CSR_MSCRATCH) -
					     SBI_TRAP_REGS_SIZE);
	ctx->regs = *trap_regs;
	ctx->sstatus = csr_read(CSR_SSTATUS);
	ctx->sie = csr_read(CSR_SIE);
	ctx->stvec = csr_read(CSR_STVEC);
	ctx->sscratch = csr_read(CSR_SSCRATCH);
	ctx->sepc = csr_read(CSR_SEPC);
	ctx->scause = csr_read(CSR_SCAUSE);
	ctx->stval = csr_read(CSR_STVAL);
	ctx->sip = csr_read(CSR_SIP);
	ctx->satp = csr_read(CSR_SATP);
	ctx->scounteren = 0;
	ctx->senvcfg = 0;
	if (sbi_hart_priv_version(scratch) >= SBI_HART_PRIV_VER_1_10)
		ctx->scounteren = csr_read(CSR_SCOUNTEREN);
	if (sbi_hart_priv_version(scratch) >= SBI_HART_PRIV_VER_1_12)
		ctx->senvcfg = csr_read(CSR_SENVCFG);
	ctx->initialized = true;
}

static struct sbi_context *hwiso_ensure_context(struct sbiunit_test_case *test,
						struct sbi_domain *dom,
						struct sbi_context *tmpl,
						u32 hartindex)
{
	struct sbi_context *dom_ctx;

	dom_ctx = sbi_hartindex_to_domain_context(hartindex, dom);
	if (!dom_ctx) {
		dom_ctx = sbi_zalloc(sizeof(*dom_ctx));
		SBIUNIT_ASSERT_NE(test, dom_ctx, NULL);
		dom_ctx->dom = dom;
		dom->hartindex_to_context_table[hartindex] = dom_ctx;
	}

	if (tmpl) {
		*dom_ctx = *tmpl;
		dom_ctx->dom = dom;
		dom->hartindex_to_context_table[hartindex] = dom_ctx;
	}

	return dom_ctx;
}

static void hwiso_domain_switch_test(struct sbiunit_test_case *test)
{
	struct sbi_domain *cur_dom = sbi_domain_thishart_ptr();
	struct sbi_context *ctx = sbi_domain_context_thishart_ptr();
	struct sbi_domain *dom0 = hwiso_find_domain("domain@0");
	struct sbi_domain *dom1 = hwiso_find_domain("domain@1");
	struct sbi_domain *root_dom = &root;
	struct sbi_context *cur_ctx;
	struct sbi_context tmpl_ctx;
	u32 hartindex = sbi_hartid_to_hartindex(current_hartid());
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
	bool debug_on = false;

	SBIUNIT_ASSERT_NE(test, cur_dom, NULL);
	SBIUNIT_ASSERT_NE(test, dom0, NULL);
	SBIUNIT_ASSERT_NE(test, dom1, NULL);
	SBIUNIT_ASSERT_NE(test, root_dom, NULL);

	if (scratch->options & SBI_SCRATCH_DEBUG_PRINTS)
		debug_on = true;
	else
		scratch->options |= SBI_SCRATCH_DEBUG_PRINTS;

	if (!ctx) {
		cur_ctx = sbi_zalloc(sizeof(*cur_ctx));
		SBIUNIT_ASSERT_NE(test, cur_ctx, NULL);
		cur_ctx->dom = cur_dom;
		cur_dom->hartindex_to_context_table[hartindex] = cur_ctx;
		ctx = cur_ctx;
	}

	hwiso_snapshot_context(ctx);
	tmpl_ctx = *ctx;

	(void)hwiso_ensure_context(test, cur_dom, NULL, hartindex);
	(void)hwiso_ensure_context(test, dom0, &tmpl_ctx, hartindex);
	(void)hwiso_ensure_context(test, dom1, &tmpl_ctx, hartindex);
	(void)hwiso_ensure_context(test, root_dom, &tmpl_ctx, hartindex);

	if (cur_dom == dom0) {
		SBIUNIT_ASSERT_EQ(test, sbi_domain_context_enter(dom1), 0);
		SBIUNIT_ASSERT_EQ(test, sbi_domain_context_enter(root_dom), 0);
	} else if (cur_dom == dom1) {
		SBIUNIT_ASSERT_EQ(test, sbi_domain_context_enter(dom0), 0);
		SBIUNIT_ASSERT_EQ(test, sbi_domain_context_enter(root_dom), 0);
	} else {
		SBIUNIT_ASSERT_EQ(test, sbi_domain_context_enter(dom0), 0);
		SBIUNIT_ASSERT_EQ(test, sbi_domain_context_enter(dom1), 0);
		SBIUNIT_ASSERT_EQ(test, sbi_domain_context_enter(root_dom), 0);
	}

	if (!debug_on)
		scratch->options &= ~SBI_SCRATCH_DEBUG_PRINTS;
}

static struct sbiunit_test_case hwiso_test_cases[] = {
	SBIUNIT_TEST_CASE(hwiso_domain_switch_test),
	SBIUNIT_END_CASE,
};

SBIUNIT_TEST_SUITE(hwiso_test_suite, hwiso_test_cases);
