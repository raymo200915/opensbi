/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Temporary vendor SBI extension for "virtual wired IRQ" bring-up.
 *
 * This allows S-mode software to pop the next pending hwirq that M-mode has
 * couried into the current domain. It is expected to be replaced by trap &
 * emulate of APLIC MMIO (CLAIMI) in later steps.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#include <sbi/sbi_ecall.h>
#include <sbi/sbi_ecall_interface.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_trap.h>
#include <sbi/sbi_virq.h>

static int sbi_ecall_riscstar_virq_handler(unsigned long extid,
					   unsigned long funcid,
					   struct sbi_trap_regs *regs,
					   struct sbi_ecall_return *out)
{
	(void)extid;
	(void)regs;

	switch (funcid) {
	case SBI_EXT_RISCSTAR_VIRQ_POP:
		out->value = (unsigned long)sbi_virq_pop_thishart();
		return SBI_OK;
	case SBI_EXT_VENDOR_VIRQ_COMPLETE:
		u32 hwirq = (u32)regs->a0;

		sbi_virq_complete_thishart(hwirq);
		regs->a0 = 0;
		return SBI_OK;
	default:
		return SBI_ENOTSUPP;
	}
}

struct sbi_ecall_extension ecall_riscstar_virq;

static int sbi_ecall_riscstar_virq_register_extensions(void)
{
	return sbi_ecall_register_extension(&ecall_riscstar_virq);
}

struct sbi_ecall_extension ecall_riscstar_virq = {
	.name			= "virq",
	.extid_start = SBI_EXT_RISCSTAR_VIRQ,
	.extid_end   = SBI_EXT_RISCSTAR_VIRQ,
	.register_extensions	= sbi_ecall_riscstar_virq_register_extensions,
	.handle      = sbi_ecall_riscstar_virq_handler,
};
