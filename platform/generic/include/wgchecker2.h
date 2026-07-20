/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#ifndef __PLATFORM_GENERIC_WGCHECKER2_H__
#define __PLATFORM_GENERIC_WGCHECKER2_H__

#include <sbi/sbi_types.h>

#define WGCHECKER2_COMPAT			"sifive,wgchecker2"
#define WGCHECKER2_CFG_NODE			"worldguard_cfg"

#define WGCHECKER2_PROP_SLOT_COUNT		"sifive,slot-count"
#define WGCHECKER2_PROP_SUBORDINATES		"sifive,subordinates"
#define WGCHECKER2_PROP_PERMS			"perms"

/*
 * The current wgchecker2 model uses a 64-bit permission register with
 * 2 bits per world, so the current checker model tracks at most 32 WIDs.
 */
#define WGCHECKER2_MAX_WIDS			32

/* The current wgchecker2 model requires 4 KiB slot alignment. */
#define WGCHECKER2_MIN_ALIGN			0x1000ULL

/* Current wgchecker2 MMIO register layout. */
#define WGCHECKER2_MMIO_NSLOTS			0x008
#define WGCHECKER2_MMIO_ERRCAUSE		0x010
#define WGCHECKER2_MMIO_ERRADDR			0x018
#define WGCHECKER2_MMIO_SLOT_BASE		0x020
#define WGCHECKER2_MMIO_SLOT_STRIDE		0x020
#define WGCHECKER2_MMIO_SLOT_ADDR		0x000
#define WGCHECKER2_MMIO_SLOT_PERM		0x008
#define WGCHECKER2_MMIO_SLOT_CFG		0x010

/* Current wgchecker2 slot cfg.A[1:0] encoding. */
#define WGCHECKER2_SLOT_CFG_A_MASK		0x3
#define WGCHECKER2_SLOT_CFG_A_OFF		0x0
#define WGCHECKER2_SLOT_CFG_A_TOR		0x1

struct wgchecker2_range {
	u64 base;
	u64 size;
	u64 perm;
};

u32 wgchecker2_count_platform_checkers(void *fdt);
int wgchecker2_init(void *fdt);
void wgchecker2_cleanup(void);
u32 wgchecker2_checker_count(void);

#endif
