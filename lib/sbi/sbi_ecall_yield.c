// SPDX-License-Identifier: BSD-2-Clause
/*
 * Copyright (c) 2026 RISCstar Solutions.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#include <sbi/sbi_ecall.h>
#include <sbi/sbi_ecall_interface.h>
#include <sbi/sbi_error.h>
#include <sbi_utils/sysirq/rpmi_sysirq.h>

static int sbi_ecall_yield_handler(unsigned long extid, unsigned long funcid,
				   struct sbi_trap_regs *regs,
				   struct sbi_ecall_return *out)
{
	u32 completed = 0;
	int rc;

	if (funcid != SBI_EXT_YIELD_YIELD)
		return SBI_ENOTSUPP;
	rc = rpmi_sysirq_complete_thishart(&completed);
	out->value = completed;
	return rc;
}

struct sbi_ecall_extension ecall_yield;

static int sbi_ecall_yield_register_extensions(void)
{
	if (!rpmi_sysirq_available())
		return SBI_OK;
	return sbi_ecall_register_extension(&ecall_yield);
}

struct sbi_ecall_extension ecall_yield = {
	.name = "yield",
	.extid_start = SBI_EXT_YIELD,
	.extid_end = SBI_EXT_YIELD,
	.register_extensions = sbi_ecall_yield_register_extensions,
	.handle = sbi_ecall_yield_handler,
};
