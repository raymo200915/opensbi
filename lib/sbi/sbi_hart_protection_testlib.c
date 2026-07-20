// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#include <sbi/sbi_error.h>
#include <sbi/sbi_heap.h>
#include <sbi/sbi_hart_protection_test.h>
#include <sbi/sbi_list.h>

struct sbi_hart_protection_test_node {
	const struct sbi_hart_protection_test_ops *test_ops;
	struct sbi_dlist node;
};

static SBI_LIST_HEAD(hart_protection_test_ops_list);

int sbi_hart_protection_test_register(
	const struct sbi_hart_protection_test_ops *test_ops)
{
	struct sbi_hart_protection_test_node *node;
	struct sbi_hart_protection_test_node *entry;

	if (!test_ops)
		return SBI_EINVAL;

	sbi_list_for_each_entry(entry, &hart_protection_test_ops_list, node) {
		if (entry->test_ops == test_ops)
			return SBI_EALREADY;
	}

	node = sbi_zalloc(sizeof(*node));
	if (!node)
		return SBI_ENOMEM;

	node->test_ops = test_ops;
	SBI_INIT_LIST_HEAD(&node->node);
	sbi_list_add_tail(&node->node, &hart_protection_test_ops_list);

	return 0;
}

void sbi_hart_protection_test_boot(struct sbiunit_test_case *test)
{
	struct sbi_hart_protection_test_node *entry;

	sbi_list_for_each_entry(entry, &hart_protection_test_ops_list, node) {
		if (!entry->test_ops->boot_test)
			continue;
		entry->test_ops->boot_test(test);
	}
}

void sbi_hart_protection_test_failure(struct sbiunit_test_case *test)
{
	struct sbi_hart_protection_test_node *entry;

	sbi_list_for_each_entry(entry, &hart_protection_test_ops_list, node) {
		if (!entry->test_ops->failure_test)
			continue;
		entry->test_ops->failure_test(test);
	}
}

void sbi_hart_protection_test_domain_state(struct sbiunit_test_case *test,
					   const struct sbi_domain *dom)
{
	struct sbi_hart_protection_test_node *entry;

	if (!dom)
		return;

	sbi_list_for_each_entry(entry, &hart_protection_test_ops_list, node) {
		if (!entry->test_ops->domain_state_test)
			continue;
		entry->test_ops->domain_state_test(test, dom);
	}
}

void sbi_hart_protection_test_domain_quiesced(
	struct sbiunit_test_case *test, const struct sbi_domain *dom)
{
	struct sbi_hart_protection_test_node *entry;

	if (!dom)
		return;

	sbi_list_for_each_entry(entry, &hart_protection_test_ops_list, node) {
		if (!entry->test_ops->domain_quiesce_test)
			continue;
		entry->test_ops->domain_quiesce_test(test, dom);
	}
}
