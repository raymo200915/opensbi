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
#include <sbi/sbi_hart_protection_test.h>
#include <worldguard.h>

int qemu_virt_worldguard_setup(const void *fdt)
{
	int rc;

#ifdef CONFIG_SBIUNIT
	int chosen_offset;
#endif

	if (!fdt)
		return 0;

	if (fdt_node_check_compatible(fdt, 0, "riscv-virtio") &&
	    fdt_node_check_compatible(fdt, 0, "qemu,virt"))
		return 0;

	rc = worldguard_setup(fdt);
	if (rc)
		return rc;

#ifdef CONFIG_SBIUNIT
	chosen_offset = fdt_path_offset(fdt, "/chosen");
	if (worldguard_is_enabled() && chosen_offset >= 0 &&
	    fdt_getprop(fdt, chosen_offset, "opensbi,worldguard-sbiunit", NULL)) {
		rc = sbi_hart_protection_test_register(&qemu_virt_worldguard_test_ops);
		if (rc && rc != SBI_EALREADY)
			return rc;
	}
#endif

	return 0;
}
