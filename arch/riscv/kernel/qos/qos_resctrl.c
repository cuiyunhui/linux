// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "riscv-cbqri: " fmt

#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/slab.h>

#include "internal.h"

LIST_HEAD(cbqri_hw_controllers);

static u64 cbqri_readq(struct cbqri_controller *ctrl, u32 offset)
{
	return readq(ctrl->base + offset);
}

static int cbqri_read_capacity_caps(struct cbqri_controller *ctrl)
{
	u64 value = cbqri_readq(ctrl, CBQRI_CC_CAPABILITIES_OFF);

	if (!value)
		return -ENODEV;

	ctrl->version_minor = value & CBQRI_CAPABILITIES_VER_MINOR_MASK;
	ctrl->version_major = (value & CBQRI_CAPABILITIES_VER_MAJOR_MASK) >> 4;
	ctrl->cc.ncblks = (value >> CBQRI_CC_CAPABILITIES_NCBLKS_SHIFT) &
			   CBQRI_CC_CAPABILITIES_NCBLKS_MASK;
	ctrl->cc.supports_alloc_op_flush_rcid =
		(value >> CBQRI_CC_CAPABILITIES_FRCID_SHIFT) &
		CBQRI_CC_CAPABILITIES_FRCID_MASK;

	return ctrl->cc.ncblks ? 0 : -EINVAL;
}

static int cbqri_read_bandwidth_caps(struct cbqri_controller *ctrl)
{
	u64 value = cbqri_readq(ctrl, CBQRI_BC_CAPABILITIES_OFF);

	if (!value)
		return -ENODEV;

	ctrl->version_minor = value & CBQRI_CAPABILITIES_VER_MINOR_MASK;
	ctrl->version_major = (value & CBQRI_CAPABILITIES_VER_MAJOR_MASK) >> 4;
	ctrl->bc.nbwblks = (value >> CBQRI_BC_CAPABILITIES_NBWBLKS_SHIFT) &
			    CBQRI_BC_CAPABILITIES_NBWBLKS_MASK;
	ctrl->bc.mrbwb = (value >> CBQRI_BC_CAPABILITIES_MRBWB_SHIFT) &
			  CBQRI_BC_CAPABILITIES_MRBWB_MASK;

	return ctrl->bc.nbwblks ? 0 : -EINVAL;
}

static int cbqri_map_controller(struct cbqri_controller *ctrl,
				struct cbqri_controller_info *info)
{
	int ret;

	ctrl->info = info;
	if (!request_mem_region(info->addr, info->size, "riscv-cbqri"))
		return -EBUSY;

	ctrl->base = ioremap(info->addr, info->size);
	if (!ctrl->base) {
		ret = -ENOMEM;
		goto out_release_region;
	}

	switch (info->type) {
	case CBQRI_CONTROLLER_TYPE_CAPACITY:
		ret = cbqri_read_capacity_caps(ctrl);
		break;
	case CBQRI_CONTROLLER_TYPE_BANDWIDTH:
		ret = cbqri_read_bandwidth_caps(ctrl);
		break;
	default:
		ret = -EINVAL;
		break;
	}
	if (ret)
		goto out_unmap;

	return 0;

out_unmap:
	iounmap(ctrl->base);
	ctrl->base = NULL;
out_release_region:
	release_mem_region(info->addr, info->size);
	return ret;
}

void cbqri_remove_controllers(void)
{
	struct cbqri_controller *ctrl, *tmp;

	list_for_each_entry_safe(ctrl, tmp, &cbqri_hw_controllers, list) {
		list_del(&ctrl->list);
		iounmap(ctrl->base);
		release_mem_region(ctrl->info->addr, ctrl->info->size);
		kfree(ctrl);
	}
}

int cbqri_probe_controllers(void)
{
	struct cbqri_controller_info *info;
	struct cbqri_controller *ctrl;
	int ret;

	list_for_each_entry(info, &cbqri_controllers, list) {
		ctrl = kzalloc_obj(*ctrl, GFP_KERNEL);
		if (!ctrl) {
			ret = -ENOMEM;
			goto out_remove;
		}

		ret = cbqri_map_controller(ctrl, info);
		if (ret) {
			kfree(ctrl);
			goto out_remove;
		}

		INIT_LIST_HEAD(&ctrl->list);
		list_add_tail(&ctrl->list, &cbqri_hw_controllers);
		pr_debug("controller %#llx version %u.%u RCID=%u MCID=%u\n",
			 info->addr, ctrl->version_major, ctrl->version_minor,
			 info->rcid_count, info->mcid_count);
	}

	return 0;

out_remove:
	cbqri_remove_controllers();
	return ret;
}
