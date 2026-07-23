/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2014 Regents of the University of California
 */

#include <linux/init.h>
#include <linux/linkage.h>
#include <asm/page.h>

#ifndef __VDSO_PATH
#define __VDSO_PATH "arch/riscv/kernel/vdso/vdso.so"
#endif

	__PAGE_ALIGNED_DATA

	.globl vdso_start, vdso_end
	.balign PG_SIZE
vdso_start:
	.incbin __VDSO_PATH
	.balign PG_SIZE
vdso_end:

	.previous
