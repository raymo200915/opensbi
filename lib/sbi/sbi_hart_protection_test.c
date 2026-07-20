// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */
#include <sbi/sbi_domain.h>
#include <sbi/sbi_domain_context.h>
#include <sbi/sbi_hart.h>
#include <sbi/sbi_hart_protection.h>
#include <sbi/sbi_hart_protection_test.h>
#include <sbi/sbi_scratch.h>
#include <sbi/sbi_unit_test.h>

static u32 hprot_collect_switch_domains(struct sbi_domain *current,
					struct sbi_domain **targets,
					u32 max_targets)
{
	u32 count = 0;
	u32 hartindex = current_hartindex();
	struct sbi_domain *dom;

	if (!targets || !max_targets)
		return 0;

	sbi_domain_for_each(dom) {
		if (dom == current || dom == &root)
			continue;
		if (!dom->possible_harts ||
		    !sbi_hartmask_test_hartindex(hartindex, dom->possible_harts))
			continue;

		targets[count++] = dom;
		if (count == max_targets)
			return count;
	}

	if (current != &root && count < max_targets)
		targets[count++] = &root;

	return count;
}

static void hprot_boot_test(struct sbiunit_test_case *test)
{
	sbi_hart_protection_test_boot(test);
}

static void hprot_domain_switch_test(struct sbiunit_test_case *test)
{
	struct sbi_domain *cur_dom = sbi_domain_thishart_ptr();
	struct sbi_domain *active_dom;
	struct sbi_domain *switch_doms[2];
	u32 switch_count;
	u32 i;
	struct sbi_scratch *scratch = sbi_scratch_thishart_ptr();
	bool debug_on = false;

	SBIUNIT_ASSERT_NE(test, cur_dom, NULL);

	if (scratch->options & SBI_SCRATCH_DEBUG_PRINTS)
		debug_on = true;
	else
		scratch->options |= SBI_SCRATCH_DEBUG_PRINTS;

	switch_count = hprot_collect_switch_domains(cur_dom, switch_doms, 2);

	if (!switch_count)
		goto out;

	sbi_hart_protection_unconfigure(scratch, cur_dom);
	sbi_hart_protection_test_domain_quiesced(test, cur_dom);
	SBIUNIT_ASSERT_EQ(test,
			  sbi_hart_protection_configure(scratch, switch_doms[0]),
			  0);
	sbi_hart_protection_test_domain_state(test, switch_doms[0]);
	sbi_hart_protection_unconfigure(scratch, switch_doms[0]);
	sbi_hart_protection_test_domain_quiesced(test, switch_doms[0]);
	SBIUNIT_ASSERT_EQ(test,
			  sbi_hart_protection_configure(scratch, cur_dom), 0);
	sbi_hart_protection_test_domain_state(test, cur_dom);

	active_dom = cur_dom;
	for (i = 0; i < switch_count; i++) {
		SBIUNIT_ASSERT_EQ(test,
				  sbi_hart_protection_reconfigure(scratch,
								  active_dom,
								  switch_doms[i]),
				  0);
		sbi_hart_protection_test_domain_state(test, switch_doms[i]);
		active_dom = switch_doms[i];
	}

	if (active_dom != cur_dom) {
		SBIUNIT_ASSERT_EQ(test,
				  sbi_hart_protection_reconfigure(scratch,
								  active_dom,
								  cur_dom),
				  0);
		sbi_hart_protection_test_domain_state(test, cur_dom);
	}

out:
	if (!debug_on)
		scratch->options &= ~SBI_SCRATCH_DEBUG_PRINTS;
}

static struct sbiunit_test_case hprot_test_cases[] = {
	SBIUNIT_TEST_CASE(hprot_boot_test),
	SBIUNIT_TEST_CASE(hprot_domain_switch_test),
	SBIUNIT_END_CASE,
};

SBIUNIT_TEST_SUITE(hart_protection_test_suite, hprot_test_cases);
