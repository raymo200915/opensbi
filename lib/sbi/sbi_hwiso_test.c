/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */
#include <sbi/riscv_encoding.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_domain_context.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_hwiso.h>
#include <sbi/sbi_hwiso_test.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_unit_test.h>

static u32 hwiso_collect_switch_domains(struct sbi_domain *current,
					struct sbi_domain **targets,
					u32 max_targets)
{
	u32 i;
	u32 count = 0;
	struct sbi_domain *dom;

	if (!targets || !max_targets)
		return 0;

	sbi_domain_for_each(i, dom) {
		if (dom == current || dom == &root)
			continue;

		targets[count++] = dom;
		if (count == max_targets)
			return count;
	}

	if (current != &root && count < max_targets)
		targets[count++] = &root;

	return count;
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

static void hwiso_boot_test(struct sbiunit_test_case *test)
{
	sbi_hwiso_test_boot(test);
}

static void hwiso_domain_switch_test(struct sbiunit_test_case *test)
{
	struct sbi_domain *cur_dom = sbi_domain_thishart_ptr();
	struct sbi_context *ctx = sbi_domain_context_thishart_ptr();
	struct sbi_domain *switch_doms[2];
	struct sbi_context *cur_ctx;
	struct sbi_context tmpl_ctx;
	u32 switch_count;
	u32 i;
	u32 hartindex = sbi_hartid_to_hartindex(current_hartid());
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
	bool debug_on = false;

	SBIUNIT_ASSERT_NE(test, cur_dom, NULL);

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
	switch_count = hwiso_collect_switch_domains(cur_dom, switch_doms, 2);

	(void)hwiso_ensure_context(test, cur_dom, NULL, hartindex);
	for (i = 0; i < switch_count; i++)
		(void)hwiso_ensure_context(test, switch_doms[i], &tmpl_ctx,
					   hartindex);

	if (!switch_count)
		goto out;

	sbi_hwiso_domain_exit(cur_dom, switch_doms[0]);
	sbi_hwiso_test_domain_quiesced(test, cur_dom);
	sbi_hwiso_domain_enter(switch_doms[0], cur_dom);
	sbi_hwiso_test_domain_state(test, switch_doms[0]);
	sbi_hwiso_domain_enter(cur_dom, switch_doms[0]);
	sbi_hwiso_test_domain_state(test, cur_dom);

	for (i = 0; i < switch_count; i++) {
		SBIUNIT_ASSERT_EQ(test, sbi_domain_context_enter(switch_doms[i]), 0);
		sbi_hwiso_test_domain_state(test, switch_doms[i]);
	}

	SBIUNIT_ASSERT_EQ(test, sbi_domain_context_enter(cur_dom), 0);
	sbi_hwiso_test_domain_state(test, cur_dom);

out:
	if (!debug_on)
		scratch->options &= ~SBI_SCRATCH_DEBUG_PRINTS;
}

static struct sbiunit_test_case hwiso_test_cases[] = {
	SBIUNIT_TEST_CASE(hwiso_boot_test),
	SBIUNIT_TEST_CASE(hwiso_domain_switch_test),
	SBIUNIT_END_CASE,
};

SBIUNIT_TEST_SUITE(hwiso_test_suite, hwiso_test_cases);
