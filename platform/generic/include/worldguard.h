/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#ifndef __PLATFORM_GENERIC_WORLDGUARD_H__
#define __PLATFORM_GENERIC_WORLDGUARD_H__

#include <sbi/sbi_domain.h>
#include <sbi/sbi_hwiso.h>
#include <sbi/sbi_types.h>

#define WORLDGUARD_CPU_COMPAT		"riscv,wgcpu"
#define WORLDGUARD_CPU_NODE		"worldguard"

#define WORLDGUARD_PROP_WID		"worldguard,wid"
#define WORLDGUARD_PROP_WIDLIST		"worldguard,widlist"
#define WORLDGUARD_PROP_MWID		"mwid"
#define WORLDGUARD_PROP_MWIDLIST	"mwidlist"

struct wg_cpu_defaults {
	u32 trusted_wid;
	u32 nworlds;
	u32 valid_wid_mask;
};

struct worldguard_domain_ctx {
	bool has_wid;
	u32 wid;
	u32 widlist_mask;
};

int worldguard_register(void);
const struct sbi_hwiso_ops *worldguard_ops_get(void);

#ifdef CONFIG_SBIUNIT
int worldguard_test_check_runtime_state(bool runtime_enabled);
int worldguard_test_check_domain_state(const struct sbi_domain *dom,
				       bool expect_ctx, u32 wid,
				       u32 widlist_mask);
#endif

#endif
