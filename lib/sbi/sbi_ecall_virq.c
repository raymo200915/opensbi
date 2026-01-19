/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 RISCstar Solutions.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#include <sbi/sbi_console.h>
#include <sbi/sbi_ecall.h>
#include <sbi/sbi_ecall_interface.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_virq.h>

static int sbi_ecall_virq_handler(unsigned long extid,
					   unsigned long funcid,
					   struct sbi_trap_regs *regs,
					   struct sbi_ecall_return *out)
{
	(void)extid;
	(void)regs;

	sbi_printf("[ECALL VIRQ] VIRQ ecall handler, funcid: %ld\n", funcid);

	switch (funcid) {
	case SBI_EXT_VIRQ_POP:
		out->value = (unsigned long)sbi_virq_pop_thishart();
		return SBI_OK;
	case SBI_EXT_VIRQ_COMPLETE:
		u32 hwirq = (u32)regs->a0;

		sbi_virq_complete_thishart(hwirq);
		regs->a0 = 0;
		return SBI_OK;
	default:
		return SBI_ENOTSUPP;
	}
}

struct sbi_ecall_extension ecall_virq;

static int sbi_ecall_virq_register_extensions(void)
{
	int ret;

	ret = sbi_ecall_register_extension(&ecall_virq);
	sbi_printf("[ECALL VIRQ] register VIRQ ecall extensions, ret=%d\n", ret);
	return ret;
}

struct sbi_ecall_extension ecall_virq = {
	.name        = "virq",
	.extid_start = SBI_EXT_VIRQ,
	.extid_end   = SBI_EXT_VIRQ,
	.register_extensions    = sbi_ecall_virq_register_extensions,
	.handle      = sbi_ecall_virq_handler,
};
