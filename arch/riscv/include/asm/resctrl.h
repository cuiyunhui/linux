/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_RISCV_RESCTRL_H
#define _ASM_RISCV_RESCTRL_H

#include <linux/riscv_qos.h>

int resctrl_arch_release_ctrl(u32 closid);
void resctrl_arch_release_rmid(u32 rmid);

static inline bool resctrl_arch_cdp_consumes_closids(void)
{
	return false;
}

#endif /* _ASM_RISCV_RESCTRL_H */
