/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/arm_mpam.h>

static inline bool resctrl_arch_cdp_consumes_closids(void)
{
	return true;
}

static inline int resctrl_arch_release_ctrl(u32 closid)
{
	return 0;
}

static inline void resctrl_arch_release_rmid(u32 rmid)
{
}
