/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _ASM_RISCV_RESCTRL_H
#define _ASM_RISCV_RESCTRL_H

#include <linux/resctrl_types.h>
#include <linux/types.h>

struct rdt_resource;
struct task_struct;

bool resctrl_arch_alloc_capable(void);
void resctrl_arch_enable_alloc(void);
void resctrl_arch_disable_alloc(void);
bool resctrl_arch_mon_capable(void);
void resctrl_arch_enable_mon(void);
void resctrl_arch_disable_mon(void);
unsigned int resctrl_arch_round_mon_val(unsigned int value);
void resctrl_arch_set_cpu_default_closid_rmid(int cpu, u32 closid, u32 rmid);
void resctrl_arch_set_closid_rmid(struct task_struct *task, u32 closid,
				  u32 rmid);
bool resctrl_arch_match_closid(struct task_struct *task, u32 closid);
bool resctrl_arch_match_rmid(struct task_struct *task, u32 closid, u32 rmid);
void resctrl_arch_sync_cpu_closid_rmid(void *info);
void resctrl_arch_sched_in(struct task_struct *task);
void resctrl_arch_rmid_idx_decode(u32 idx, u32 *closid, u32 *rmid);
u32 resctrl_arch_rmid_idx_encode(u32 closid, u32 rmid);

int resctrl_arch_release_ctrl(u32 closid);

void resctrl_arch_release_rmid(u32 closid, u32 rmid);

static inline void *resctrl_arch_mon_ctx_alloc(struct rdt_resource *r,
					       enum resctrl_event_id eventid)
{
	return NULL;
}

static inline void resctrl_arch_mon_ctx_free(struct rdt_resource *r,
					     enum resctrl_event_id eventid,
					     void *ctx)
{
}

#endif /* _ASM_RISCV_RESCTRL_H */
