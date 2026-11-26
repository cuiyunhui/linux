/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/resctrl_types.h>
#include <linux/arm_mpam.h>

enum resctrl_id_mode resctrl_arch_ctrl_id_mode(void);
int resctrl_arch_release_ctrl(u32 closid);
