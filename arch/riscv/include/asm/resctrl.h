/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_RISCV_RESCTRL_H
#define _ASM_RISCV_RESCTRL_H

#include <linux/riscv_qos.h>

static inline int resctrl_arch_release_ctrl(u32 closid)
{
	return 0;
}

#endif /* _ASM_RISCV_RESCTRL_H */
