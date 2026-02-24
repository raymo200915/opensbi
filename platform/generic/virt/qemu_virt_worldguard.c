/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * QEMU virt WorldGuard registration shim
 *
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#include <libfdt.h>
#include <sbi/sbi_error.h>
#include <sbi/sbi_hwiso_test.h>
#include <worldguard.h>

#ifdef CONFIG_SBIUNIT
extern const struct sbi_hwiso_test_ops qemu_virt_worldguard_test_ops;
#endif

int qemu_virt_worldguard_register(void *fdt)
{
	int rc;

	if (!fdt)
		return 0;

	if (fdt_node_check_compatible(fdt, 0, "riscv-virtio") &&
	    fdt_node_check_compatible(fdt, 0, "qemu,virt"))
		return 0;

	rc = worldguard_register();
	if (rc)
		return rc;

#ifdef CONFIG_SBIUNIT
	rc = sbi_hwiso_test_register(worldguard_ops_get(),
				     &qemu_virt_worldguard_test_ops);
	if (rc && rc != SBI_EALREADY)
		return rc;
#endif

	return 0;
}
