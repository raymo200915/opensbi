/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#ifndef __SBI_HART_PROTECTION_TEST_H__
#define __SBI_HART_PROTECTION_TEST_H__

#include <sbi/sbi_domain.h>
#include <sbi/sbi_unit_test.h>

#ifdef CONFIG_SBIUNIT
struct sbi_hart_protection_test_ops {
	void (*boot_test)(struct sbiunit_test_case *test);
	void (*failure_test)(struct sbiunit_test_case *test);
	void (*domain_state_test)(struct sbiunit_test_case *test,
				  const struct sbi_domain *dom);
	void (*domain_quiesce_test)(struct sbiunit_test_case *test,
				    const struct sbi_domain *dom);
};

int sbi_hart_protection_test_register(const struct sbi_hart_protection_test_ops
				      *test_ops);
void sbi_hart_protection_test_boot(struct sbiunit_test_case *test);
void sbi_hart_protection_test_failure(struct sbiunit_test_case *test);
void sbi_hart_protection_test_domain_state(struct sbiunit_test_case *test,
					   const struct sbi_domain *dom);
void sbi_hart_protection_test_domain_quiesced(struct sbiunit_test_case *test,
					      const struct sbi_domain *dom);
#endif

#endif
