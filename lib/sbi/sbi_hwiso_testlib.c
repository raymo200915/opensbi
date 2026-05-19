/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#include <sbi/sbi_error.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_hwiso_test.h>
#include <sbi/sbi_list.h>

struct sbi_hwiso_test_node {
	const struct sbi_hwiso_ops *ops;
	const struct sbi_hwiso_test_ops *test_ops;
	struct sbi_dlist node;
};

static SBI_LIST_HEAD(hwiso_test_ops_list);

static const struct sbi_hwiso_test_ops *
sbi_hwiso_find_test_ops(const struct sbi_hwiso_ops *ops)
{
	struct sbi_hwiso_test_node *entry;

	sbi_list_for_each_entry(entry, &hwiso_test_ops_list, node) {
		if (entry->ops == ops)
			return entry->test_ops;
	}

	return NULL;
}

int sbi_hwiso_test_register(const struct sbi_hwiso_ops *ops,
			    const struct sbi_hwiso_test_ops *test_ops)
{
	struct sbi_hwiso_test_node *node;

	if (!ops || !test_ops)
		return SBI_EINVAL;
	if (sbi_hwiso_find_test_ops(ops))
		return SBI_EALREADY;

	node = sbi_zalloc(sizeof(*node));
	if (!node)
		return SBI_ENOMEM;

	node->ops = ops;
	node->test_ops = test_ops;
	SBI_INIT_LIST_HEAD(&node->node);
	sbi_list_add_tail(&node->node, &hwiso_test_ops_list);

	return 0;
}

void sbi_hwiso_test_boot(struct sbiunit_test_case *test)
{
	struct sbi_hwiso_test_node *entry;

	sbi_list_for_each_entry(entry, &hwiso_test_ops_list, node) {
		if (!entry->test_ops->boot_test)
			continue;
		entry->test_ops->boot_test(test);
	}
}

void sbi_hwiso_test_failure(struct sbiunit_test_case *test)
{
	struct sbi_hwiso_test_node *entry;

	sbi_list_for_each_entry(entry, &hwiso_test_ops_list, node) {
		if (!entry->test_ops->failure_test)
			continue;
		entry->test_ops->failure_test(test);
	}
}

void sbi_hwiso_test_domain_state(struct sbiunit_test_case *test,
				 const struct sbi_domain *dom)
{
	const struct sbi_hwiso_test_ops *test_ops;
	u32 i;

	if (!dom || !dom->hwiso_ctxs)
		return;

	for (i = 0; i < dom->hwiso_ctx_count; i++) {
		if (!dom->hwiso_ctxs[i].ops)
			continue;

		test_ops = sbi_hwiso_find_test_ops(dom->hwiso_ctxs[i].ops);
		if (!test_ops || !test_ops->domain_state_test)
			continue;

		test_ops->domain_state_test(test, dom, dom->hwiso_ctxs[i].ctx);
	}
}

void sbi_hwiso_test_domain_quiesced(struct sbiunit_test_case *test,
				    const struct sbi_domain *dom)
{
	const struct sbi_hwiso_test_ops *test_ops;
	u32 i;

	if (!dom || !dom->hwiso_ctxs)
		return;

	for (i = 0; i < dom->hwiso_ctx_count; i++) {
		if (!dom->hwiso_ctxs[i].ops)
			continue;

		test_ops = sbi_hwiso_find_test_ops(dom->hwiso_ctxs[i].ops);
		if (!test_ops || !test_ops->domain_quiesce_test)
			continue;

		test_ops->domain_quiesce_test(test, dom,
					      dom->hwiso_ctxs[i].ctx);
	}
}
