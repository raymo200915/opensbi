/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#ifndef __PLATFORM_GENERIC_QEMU_VIRT_WORLDGUARD_H__
#define __PLATFORM_GENERIC_QEMU_VIRT_WORLDGUARD_H__

int qemu_virt_worldguard_setup(const void *fdt);

#ifdef CONFIG_SBIUNIT
#include <sbi/sbi_hart_protection_test.h>

extern const struct sbi_hart_protection_test_ops qemu_virt_worldguard_test_ops;
#endif

#endif
