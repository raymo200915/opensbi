/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#ifndef __PLATFORM_GENERIC_WGCHECKER_H__
#define __PLATFORM_GENERIC_WGCHECKER_H__

#include <sbi/sbi_types.h>

#define WGCHECKER_COMPAT			"sifive,wgchecker2"
#define WGCHECKER_QEMU_COMPAT			"qemu,wgchecker2"
#define WGCHECKER_ADDRESS_CELLS			1
#define WGCHECKER_SIZE_CELLS			0
#define WGCHECKER_ACCESS_CONTROLLER_CELLS	1
#define WGCHECKER_PARTITION_UNPROTECTED_ID	0xffffffffU
#define WGCHECKER_PROP_WG_REGION		"sifive,wg-region"
#define WGCHECKER_PROP_SLOT_PERMISSIONS	"sifive,slot-permissions"
#define WGCHECKER_PROP_SLOT_CONFIG		"sifive,slot-config"

/*
 * The current wgchecker model uses a 64-bit permission register with
 * 2 bits per world, so the current checker model tracks at most 32 WIDs.
 */
#define WGCHECKER_MAX_WIDS			32

/* The current wgchecker model requires 4 KiB slot alignment. */
#define WGCHECKER_MIN_ALIGN			0x1000ULL

/* Current wgchecker MMIO register layout. */
#define WGCHECKER_MMIO_NSLOTS			0x008
#define WGCHECKER_MMIO_ERRCAUSE		0x010
#define WGCHECKER_MMIO_ERRADDR			0x018
#define WGCHECKER_MMIO_SLOT_BASE		0x020
#define WGCHECKER_MMIO_SLOT_STRIDE		0x020
#define WGCHECKER_MMIO_SLOT_ADDR		0x000
#define WGCHECKER_MMIO_SLOT_PERM		0x008
#define WGCHECKER_MMIO_SLOT_CFG		0x010

/*
 * RFC access-controller config bits. These are the DT-visible policy bits
 * carried by the final "config" cell in each access-controllers specifier.
 */
#define WGCHECKER_CONFIG_ER			0x01U
#define WGCHECKER_CONFIG_EW			0x02U
#define WGCHECKER_CONFIG_IR			0x04U
#define WGCHECKER_CONFIG_IW			0x08U
#define WGCHECKER_CONFIG_L			0x10U
#define WGCHECKER_CONFIG_MASK			0x1fU

#define WGCHECKER_CONFIG_ERR_RW		\
	(WGCHECKER_CONFIG_ER | WGCHECKER_CONFIG_EW)
#define WGCHECKER_CONFIG_ERR_IRQ_RW		\
	(WGCHECKER_CONFIG_ERR_RW | WGCHECKER_CONFIG_IR | \
	 WGCHECKER_CONFIG_IW)

/* RFC permission bitmap helpers: two bits per WID, read then write. */
#define WGCHECKER_PERM_R(_wid)		(1ULL << (2U * (_wid)))
#define WGCHECKER_PERM_W(_wid)		(1ULL << (2U * (_wid) + 1U))

static inline u64 wgchecker_perm_rw(u32 wid)
{
	return WGCHECKER_PERM_R(wid) | WGCHECKER_PERM_W(wid);
}

/* Current wgchecker MMIO slot cfg.A[1:0] encoding. */
#define WGCHECKER_SLOT_CFG_A_MASK		0x3
#define WGCHECKER_SLOT_CFG_A_OFF		0x0
#define WGCHECKER_SLOT_CFG_A_TOR		0x1
#define WGCHECKER_SLOT_CFG_A_NAPOT		0x3
#define WGCHECKER_SLOT_CFG_ER			(1U << 8)
#define WGCHECKER_SLOT_CFG_EW			(1U << 9)
#define WGCHECKER_SLOT_CFG_IR			(1U << 10)
#define WGCHECKER_SLOT_CFG_IW			(1U << 11)
#define WGCHECKER_SLOT_CFG_LOCK		(1U << 31)

struct wgchecker_range {
	u64 base;
	u64 size;
	u64 perm;
	u32 cfg;
};

u32 wgchecker_count_platform_checkers(void *fdt);
int wgchecker_init(void *fdt);
void wgchecker_cleanup(void);
u32 wgchecker_checker_count(void);

#endif
