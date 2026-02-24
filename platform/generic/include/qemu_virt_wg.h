/*
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 RISCstar Solutions Corporation.
 *
 * Author: Raymond Mao <raymond.mao@riscstar.com>
 */

#ifndef __QEMU_VIRT_WG_H__
#define __QEMU_VIRT_WG_H__

#include <sbi/sbi_types.h>

/*
 * QEMU virt WorldGuard model definitions
 */
#define QEMU_VIRT_WG_COMPAT			"sifive,wgchecker2"
#define QEMU_VIRT_WG_CPU_COMPAT			"riscv,wgcpu"
#define QEMU_VIRT_WG_CPU_NODE			"worldguard"
#define QEMU_VIRT_WG_CFG_NODE			"worldguard_cfg"

#define QEMU_VIRT_WG_PROP_SLOT_COUNT		"sifive,slot-count"
#define QEMU_VIRT_WG_PROP_SUBORDINATES		"sifive,subordinates"
#define QEMU_VIRT_WG_PROP_WID			"worldguard,wid"
#define QEMU_VIRT_WG_PROP_WIDLIST		"worldguard,widlist"
#define QEMU_VIRT_WG_PROP_MWID			"mwid"
#define QEMU_VIRT_WG_PROP_MWIDLIST		"mwidlist"
#define QEMU_VIRT_WG_PROP_PERMS			"perms"

/*
 * The current QEMU wgChecker model uses a 64-bit permission register with
 * 2 bits per world, so the current software model tracks at most 32 WIDs.
 */
#define QEMU_VIRT_WG_MAX_WIDS			32

/* The current QEMU wgChecker model requires 4 KiB slot alignment. */
#define QEMU_VIRT_WG_MIN_ALIGN			0x1000ULL

/* Current QEMU wgChecker MMIO register layout. */
#define QEMU_VIRT_WG_MMIO_NSLOTS		0x008
#define QEMU_VIRT_WG_MMIO_ERRCAUSE		0x010
#define QEMU_VIRT_WG_MMIO_ERRADDR		0x018
#define QEMU_VIRT_WG_MMIO_SLOT_BASE		0x020
#define QEMU_VIRT_WG_MMIO_SLOT_STRIDE		0x020
#define QEMU_VIRT_WG_MMIO_SLOT_ADDR		0x000
#define QEMU_VIRT_WG_MMIO_SLOT_PERM		0x008
#define QEMU_VIRT_WG_MMIO_SLOT_CFG		0x010

/* Current QEMU wgChecker slot cfg.A[1:0] encoding. */
#define QEMU_VIRT_WG_SLOT_CFG_A_MASK		0x3
#define QEMU_VIRT_WG_SLOT_CFG_A_OFF		0x0
#define QEMU_VIRT_WG_SLOT_CFG_A_TOR		0x1

struct qemu_virt_wg_range {
	u64 base;
	u64 size;
	u64 perm;
};

#endif
