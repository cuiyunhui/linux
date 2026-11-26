/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/arm_mpam.h>

#include <linux/resctrl_types.h>

static inline enum resctrl_id_mode resctrl_arch_ctrl_id_mode(void)
{
	return RESCTRL_IDENTITY;
}
