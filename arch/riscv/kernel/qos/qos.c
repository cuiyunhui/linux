// SPDX-License-Identifier: GPL-2.0-only
#include <linux/slab.h>
#include <linux/err.h>
#include <linux/cpu.h>
#include <linux/cpumask.h>
#include <linux/riscv_qos.h>
#include <linux/smp.h>

#include <asm/csr.h>
#include <asm/qos.h>

#include "internal.h"

/* cached value of srmcfg csr for each cpu */
DEFINE_PER_CPU(u32, cpu_srmcfg);
DEFINE_PER_CPU(u32, cpu_srmcfg_default);

static u32 supervisor_srmcfg_assoc;

static void qos_write_supervisor_assoc(void *info)
{
	u32 value = READ_ONCE(supervisor_srmcfg_assoc);

	csr_write(CSR_SRMCFG_ASSOC, value);
}

void qos_write_supervisor_assoc_local(void)
{
	qos_write_supervisor_assoc(NULL);
}

unsigned long resctrl_arch_get_kernel_modes(void)
{
	unsigned long modes = BIT(RESCTRL_KERNEL_MODE_INHERIT);

	if (riscv_isa_extension_available(NULL, SSQOSASSOC))
		modes |= BIT(RESCTRL_KERNEL_MODE_GLOBAL_CTRL) |
			 BIT(RESCTRL_KERNEL_MODE_GLOBAL_CTRL_MON);

	return modes;
}

int resctrl_arch_set_kernel_mode(enum resctrl_kernel_mode mode,
				 u32 closid, u32 rmid)
{
	u32 value = 0;

	if (mode != RESCTRL_KERNEL_MODE_INHERIT &&
	    !riscv_isa_extension_available(NULL, SSQOSASSOC))
		return -EOPNOTSUPP;
	if ((closid & ~SRMCFG_RCID_MASK) || (rmid & ~SRMCFG_MCID_MASK))
		return -EINVAL;

	switch (mode) {
	case RESCTRL_KERNEL_MODE_INHERIT:
		break;
	case RESCTRL_KERNEL_MODE_GLOBAL_CTRL:
		value = closid | SRMCFG_ASSOC_RCID_EN | SRMCFG_ASSOC_EN;
		break;
	case RESCTRL_KERNEL_MODE_GLOBAL_CTRL_MON:
		value = closid | (rmid << SRMCFG_MCID_SHIFT) |
			SRMCFG_ASSOC_RCID_EN | SRMCFG_ASSOC_MCID_EN |
			SRMCFG_ASSOC_EN;
		break;
	default:
		return -EINVAL;
	}

	WRITE_ONCE(supervisor_srmcfg_assoc, value);
	on_each_cpu(qos_write_supervisor_assoc, NULL, 1);

	return 0;
}

static int __init qos_arch_late_init(void)
{
	int err;

	if (!riscv_isa_extension_available(NULL, SSQOSID))
		return -ENODEV;

	err = qos_resctrl_setup();
	if (err != 0)
		return err;

	cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "qos:online", qos_resctrl_online_cpu,
			  qos_resctrl_offline_cpu);

	return err;
}
late_initcall(qos_arch_late_init);
