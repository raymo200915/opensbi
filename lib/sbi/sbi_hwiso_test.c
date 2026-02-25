/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Author: OpenAI
 */
#include <sbi/riscv_asm.h>
#include <sbi/sbi_domain.h>
#include <sbi/sbi_domain_context.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_unit_test.h>

static void hwiso_domain_switch_test(struct sbiunit_test_case *test)
{
	struct sbi_trap_regs *trap_regs;
	struct sbi_domain *cur_dom = sbi_domain_thishart_ptr();
	struct sbi_context *ctx = sbi_domain_context_thishart_ptr();
	struct sbi_domain *dom = &root;
	struct sbi_context *dom_ctx, *cur_ctx;
	u32 hartindex = sbi_hartid_to_hartindex(current_hartid());

	SBIUNIT_ASSERT_NE(test, cur_dom, NULL);
	SBIUNIT_ASSERT_NE(test, dom, NULL);

	if (!ctx) {
		cur_ctx = sbi_zalloc(sizeof(*cur_ctx));
		SBIUNIT_ASSERT_NE(test, cur_ctx, NULL);
		cur_ctx->dom = cur_dom;
		cur_dom->hartindex_to_context_table[hartindex] = cur_ctx;
		ctx = cur_ctx;
	}

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
	if (sbi_hart_priv_version(sbi_scratch_thishart_ptr()) >=
	    SBI_HART_PRIV_VER_1_10)
		ctx->scounteren = csr_read(CSR_SCOUNTEREN);
	if (sbi_hart_priv_version(sbi_scratch_thishart_ptr()) >=
	    SBI_HART_PRIV_VER_1_12)
		ctx->senvcfg = csr_read(CSR_SENVCFG);
	ctx->initialized = true;

	dom_ctx = sbi_hartindex_to_domain_context(hartindex, dom);
	if (!dom_ctx) {
		dom_ctx = sbi_zalloc(sizeof(*dom_ctx));
		SBIUNIT_ASSERT_NE(test, dom_ctx, NULL);
		dom_ctx->dom = dom;
		dom->hartindex_to_context_table[hartindex] = dom_ctx;
	}

	dom_ctx->regs = ctx->regs;
	dom_ctx->sstatus = ctx->sstatus;
	dom_ctx->sie = ctx->sie;
	dom_ctx->stvec = ctx->stvec;
	dom_ctx->sscratch = ctx->sscratch;
	dom_ctx->sepc = ctx->sepc;
	dom_ctx->scause = ctx->scause;
	dom_ctx->stval = ctx->stval;
	dom_ctx->sip = ctx->sip;
	dom_ctx->satp = ctx->satp;
	dom_ctx->scounteren = ctx->scounteren;
	dom_ctx->senvcfg = ctx->senvcfg;
	dom_ctx->initialized = true;

	SBIUNIT_ASSERT_EQ(test, sbi_domain_context_enter(dom), 0);
	SBIUNIT_ASSERT_EQ(test, sbi_domain_context_exit(), 0);
}

static struct sbiunit_test_case hwiso_test_cases[] = {
	SBIUNIT_TEST_CASE(hwiso_domain_switch_test),
	SBIUNIT_END_CASE,
};

SBIUNIT_TEST_SUITE(hwiso_test_suite, hwiso_test_cases);
