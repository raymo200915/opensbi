// SPDX-License-Identifier: BSD-2-Clause
/*
 * QEMU virt WorldGuard registration shim
 *
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#include <libfdt.h>
#include <qemu_virt_worldguard.h>
#include <sbi/sbi_error.h>
#include <worldguard.h>

int qemu_virt_worldguard_setup(const void *fdt)
{
	if (!fdt)
		return 0;

	if (fdt_node_check_compatible(fdt, 0, "riscv-virtio") &&
	    fdt_node_check_compatible(fdt, 0, "qemu,virt"))
		return 0;

	return worldguard_setup(fdt);
}
