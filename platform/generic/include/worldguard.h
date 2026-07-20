/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#ifndef __PLATFORM_GENERIC_WORLDGUARD_H__
#define __PLATFORM_GENERIC_WORLDGUARD_H__

#include <sbi/sbi_types.h>
#include <sbi/sbi_domain.h>

#define WORLDGUARD_CPUS_PROP_NWORLDS	"riscv,nworlds"
#define WORLDGUARD_CPUS_PROP_TRUSTEDWID	"sifive,trustedwid"

#define WORLDGUARD_PROP_WID		"worldguard,wid"
#define WORLDGUARD_PROP_WIDLIST		"worldguard,widlist"
#define WORLDGUARD_CPU_PROP_PMWID	"riscv,pmwid"
#define WORLDGUARD_CPU_PROP_PMWIDLIST	"riscv,pmwidlist"
#define WORLDGUARD_CPU_PROP_PMLWIDLIST	"riscv,pmlwidlist"

struct wg_cpu_defaults {
	u32 pmwid;
	u32 pmwidlist_mask;
	u32 pmlwidlist_mask;
};

struct worldguard_domain_state {
	bool has_wid;
	u32 wid;
	u32 widlist_mask;
};

int worldguard_setup(const void *fdt);

#ifdef CONFIG_SBIUNIT
int worldguard_test_check_runtime_state(bool runtime_enabled);
int worldguard_test_check_domain_state(const struct sbi_domain *dom,
				       bool expect_state, u32 wid,
				       u32 widlist_mask);
#endif

#endif
