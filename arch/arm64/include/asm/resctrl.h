/* SPDX-License-Identifier: GPL-2.0 */
#include <linux/resctrl_types.h>
#include <linux/arm_mpam.h>

enum resctrl_id_mode resctrl_arch_id_mode(enum resctrl_id_type type);

int resctrl_arch_release_ctrl(u32 closid);

static inline void resctrl_arch_release_rmid(u32 closid, u32 rmid)
{
}
