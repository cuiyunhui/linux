/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __VDSO_PAGE_H
#define __VDSO_PAGE_H

#include <uapi/linux/const.h>

/*
 * PTE_SHIFT determines the MMU page size.
 *
 * Note: This definition is required because PTE_SHIFT is used
 * in several places throughout the codebase.
 */
#define PTE_SHIFT      CONFIG_PTE_SHIFT

#define PTE_SIZE	(_AC(1,UL) << CONFIG_PTE_SHIFT)

#if !defined(CONFIG_64BIT)
/*
 * Applies only to 32-bit architectures.
 *
 * Subtle: (1 << CONFIG_PTE_SHIFT) is an int, not an unsigned long.
 * So if we assign PTE_MASK to a larger type it gets extended the
 * way we want (i.e. with 1s in the high bits) while masking a
 * 64-bit value such as phys_addr_t.
 */
#define PTE_MASK	(~((1 << CONFIG_PTE_SHIFT) - 1))
#else
#define PTE_MASK	(~(PTE_SIZE - 1))
#endif

#define PAGE_SHIFT	PTE_SHIFT
#define PAGE_SIZE	PTE_SIZE
#define PAGE_MASK	PTE_MASK
#endif	/* __VDSO_PAGE_H */
