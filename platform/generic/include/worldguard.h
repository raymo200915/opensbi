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

#define WORLDGUARD_PROP_WID		"worldguard,wid"
#define WORLDGUARD_PROP_WIDLIST		"worldguard,widlist"
#define WORLDGUARD_CPU_PROP_PMWID	"riscv,pmwid"
#define WORLDGUARD_CPU_PROP_PMWIDLIST	"riscv,pmwidlist"
#define WORLDGUARD_CPU_PROP_PMLWIDLIST	"riscv,pmlwidlist"
#define WORLDGUARD_MAX_WIDS		64

struct wg_cpu_defaults {
	u32 pmwid;
	u64 pmwidlist_mask;
	u64 pmlwidlist_mask;
};

struct worldguard_domain_state {
	bool has_wid;
	u32 wid;
	u64 widlist_mask;
};

int worldguard_setup(const void *fdt);
bool worldguard_is_enabled(void);

#endif
