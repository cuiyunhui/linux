// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "riscv-cbqri: " fmt

#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/iopoll.h>
#include <linux/slab.h>

#include "internal.h"

LIST_HEAD(cbqri_hw_controllers);

static struct cbqri_resctrl_resource cbqri_resources[RDT_NUM_RESOURCES];

static u64 cbqri_readq(struct cbqri_controller *ctrl, u32 offset)
{
	return readq(ctrl->base + offset);
}

static void cbqri_writeq(struct cbqri_controller *ctrl, u32 offset, u64 value)
{
	writeq(value, ctrl->base + offset);
}

static int cbqri_wait_busy(struct cbqri_controller *ctrl, u32 offset)
{
	u64 value;

	return read_poll_timeout(cbqri_readq, value,
				 !(value & (CBQRI_CTL_BUSY_MASK << CBQRI_CTL_BUSY_SHIFT)),
				 1, 1000, false, ctrl, offset);
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

static struct cbqri_resctrl_resource *cbqri_cache_resource(u32 level)
{
	if (level == 2)
		return &cbqri_resources[RDT_RESOURCE_L2];
	if (level == 3)
		return &cbqri_resources[RDT_RESOURCE_L3];
	return NULL;
}

static struct cbqri_component *
cbqri_find_component(struct cbqri_resctrl_resource *resource, u32 id)
{
	struct cbqri_component *component;

	list_for_each_entry(component, &resource->components, list) {
		if (component->id == id)
			return component;
	}

	return NULL;
}

static int cbqri_add_component_controller(struct cbqri_component *component,
					  struct cbqri_controller *ctrl)
{
	struct cbqri_component_ctrl *entry;

	list_for_each_entry(entry, &component->controllers, list) {
		if (entry->ctrl == ctrl)
			return 0;
	}

	entry = kzalloc_obj(*entry, GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->ctrl = ctrl;
	list_add_tail(&entry->list, &component->controllers);
	return 0;
}

static int cbqri_add_cache_component(struct cbqri_controller *ctrl,
				     struct cbqri_resource_info *info)
{
	struct cbqri_resctrl_resource *resource;
	struct cbqri_component *component;

	resource = cbqri_cache_resource(info->cache.cache_level);
	if (!resource)
		return -EOPNOTSUPP;

	component = cbqri_find_component(resource, info->cache.cache_id);
	if (component) {
		if (!cpumask_equal(&component->cpu_mask, &info->cache.cpu_mask))
			return -EINVAL;
	} else {
		component = kzalloc_obj(*component, GFP_KERNEL);
		if (!component)
			return -ENOMEM;

		component->id = info->cache.cache_id;
		component->level = info->cache.cache_level;
		cpumask_copy(&component->cpu_mask, &info->cache.cpu_mask);
		INIT_LIST_HEAD(&component->controllers);
		list_add_tail(&component->list, &resource->components);
	}

	resource->num_closid = min(resource->num_closid, ctrl->info->rcid_count);
	resource->capacity_ctrl.r_ctrl.bitmap.cbm_len =
		min(resource->capacity_ctrl.r_ctrl.bitmap.cbm_len,
		    (unsigned int)ctrl->cc.ncblks);

	return cbqri_add_component_controller(component, ctrl);
}

static void cbqri_init_cache_resource(struct cbqri_resctrl_resource *resource,
				      enum resctrl_res_level rid)
{
	struct resctrl_ctrl *ctrl = &resource->capacity_ctrl.r_ctrl;
	struct rdt_resource *r = &resource->res;

	r->rid = rid;
	r->name = rid == RDT_RESOURCE_L2 ? "L2" : "L3";
	r->alloc_scope = rid == RDT_RESOURCE_L2 ? RESCTRL_L2_CACHE :
							 RESCTRL_L3_CACHE;
	INIT_LIST_HEAD(&r->controls);
	INIT_LIST_HEAD(&r->mon_domains);
	INIT_LIST_HEAD(&resource->components);
	resource->num_closid = U32_MAX;

	ctrl->scope = r->alloc_scope;
	ctrl->type = RESCTRL_CTRL_BITMAP;
	ctrl->name = RESCTRL_CTRL_NAME_DEF;
	INIT_LIST_HEAD(&ctrl->domains);
	INIT_LIST_HEAD(&ctrl->emulated_by);
	ctrl->bitmap.cbm_len = UINT_MAX;
	ctrl->bitmap.min_cbm_bits = 1;
	__set_bit(RESCTRL_BITMAP_FLAG_SPARSE, ctrl->bitmap.flags);
	resource->capacity_ctrl.kind = CBQRI_CTRL_CAPACITY;
}

static int cbqri_setup_capacity_domain(struct cbqri_resctrl_resource *resource,
				       struct cbqri_component *component)
{
	struct resctrl_ctrl *ctrl = &resource->capacity_ctrl.r_ctrl;
	struct cbqri_ctrl_domain *domain;
	u32 default_value;
	int ret;
	u32 i;

	domain = kzalloc_obj(*domain, GFP_KERNEL);
	if (!domain)
		return -ENOMEM;

	domain->ctrl_val = kcalloc(resource->num_closid,
				   sizeof(*domain->ctrl_val), GFP_KERNEL);
	if (!domain->ctrl_val) {
		kfree(domain);
		return -ENOMEM;
	}

	domain->component = component;
	domain->domain.hdr.id = component->id;
	domain->domain.hdr.rid = resource->res.rid;
	domain->domain.hdr.type = RESCTRL_CTRL_DOMAIN;
	cpumask_copy(&domain->domain.hdr.cpu_mask, &component->cpu_mask);

	default_value = GENMASK(ctrl->bitmap.cbm_len - 1, 0);
	for (i = 0; i < resource->num_closid; i++)
		domain->ctrl_val[i] = default_value;

	ret = resctrl_online_ctrl_domain(&resource->res, ctrl, &domain->domain);
	if (ret) {
		kfree(domain->ctrl_val);
		kfree(domain);
		return ret;
	}

	list_add_tail_rcu(&domain->domain.hdr.list, &ctrl->domains);
	return 0;
}

static int cbqri_finalize_capacity_resource(struct cbqri_resctrl_resource *resource)
{
	struct resctrl_ctrl *ctrl = &resource->capacity_ctrl.r_ctrl;
	struct cbqri_component *component;
	int ret;

	if (list_empty(&resource->components))
		return 0;
	if (!resource->num_closid || resource->num_closid == U32_MAX)
		return -EINVAL;
	if (!ctrl->bitmap.cbm_len || ctrl->bitmap.cbm_len > 32)
		return -EOPNOTSUPP;

	ctrl->bitmap.shareable_bits = GENMASK(ctrl->bitmap.cbm_len - 1, 0);
	list_add_tail(&ctrl->entry, &resource->res.controls);
	resource->res.alloc_capable = true;

	list_for_each_entry(component, &resource->components, list) {
		ret = cbqri_setup_capacity_domain(resource, component);
		if (ret)
			return ret;
	}

	return 0;
}

int cbqri_init_capacity_controls(void)
{
	struct cbqri_resource_info *info;
	struct cbqri_controller *ctrl;
	int ret;

	cbqri_init_cache_resource(&cbqri_resources[RDT_RESOURCE_L2],
				  RDT_RESOURCE_L2);
	cbqri_init_cache_resource(&cbqri_resources[RDT_RESOURCE_L3],
				  RDT_RESOURCE_L3);

	list_for_each_entry(ctrl, &cbqri_hw_controllers, list) {
		if (ctrl->info->type != CBQRI_CONTROLLER_TYPE_CAPACITY ||
		    !ctrl->info->rcid_count)
			continue;

		list_for_each_entry(info, &ctrl->info->resources, list) {
			if (info->id_type != CBQRI_RESOURCE_ID_PROCESSOR_CACHE)
				continue;
			ret = cbqri_add_cache_component(ctrl, info);
			if (ret)
				goto out_remove;
		}
	}

	ret = cbqri_finalize_capacity_resource(&cbqri_resources[RDT_RESOURCE_L2]);
	if (ret)
		goto out_remove;
	ret = cbqri_finalize_capacity_resource(&cbqri_resources[RDT_RESOURCE_L3]);
	if (ret)
		goto out_remove;

	return 0;

out_remove:
	cbqri_remove_capacity_controls();
	return ret;
}

static int cbqri_capacity_op(struct cbqri_controller *ctrl, u32 op,
			     u32 rcid, enum resctrl_conf_type type)
{
	u64 value;
	u32 status;

	value = op | ((u64)rcid << CBQRI_CTL_RCID_SHIFT);
	if (type == CDP_CODE)
		value |= (u64)CBQRI_AT_CODE << CBQRI_CTL_AT_SHIFT;

	cbqri_writeq(ctrl, CBQRI_CC_ALLOC_CTL_OFF, value);
	if (cbqri_wait_busy(ctrl, CBQRI_CC_ALLOC_CTL_OFF))
		return -ETIMEDOUT;

	value = cbqri_readq(ctrl, CBQRI_CC_ALLOC_CTL_OFF);
	status = (value >> CBQRI_CTL_STATUS_SHIFT) & CBQRI_CTL_STATUS_MASK;
	return status == CBQRI_CC_ALLOC_CTL_STATUS_SUCCESS ? 0 : -EIO;
}

int cbqri_capacity_set(struct cbqri_ctrl_domain *domain, u32 closid,
		       enum resctrl_conf_type type, u32 value)
{
	struct cbqri_component_ctrl *entry;
	u32 op = CBQRI_CC_ALLOC_CTL_OP_CONFIG_LIMIT;
	int ret;

	list_for_each_entry(entry, &domain->component->controllers, list) {
		cbqri_writeq(entry->ctrl, CBQRI_CC_BLOCK_MASK_OFF, value);
		ret = cbqri_capacity_op(entry->ctrl, op, closid, type);
		if (ret)
			return ret;
	}

	domain->ctrl_val[closid] = value;
	return 0;
}

int cbqri_capacity_get(struct cbqri_ctrl_domain *domain, u32 closid,
		       enum resctrl_conf_type type, u32 *value)
{
	struct cbqri_component_ctrl *entry;
	u32 op = CBQRI_CC_ALLOC_CTL_OP_READ_LIMIT;
	u32 first = 0;
	u32 current_value;
	bool have_value = false;
	int ret;

	list_for_each_entry(entry, &domain->component->controllers, list) {
		ret = cbqri_capacity_op(entry->ctrl, op, closid, type);
		if (ret)
			return ret;
		current_value = cbqri_readq(entry->ctrl, CBQRI_CC_BLOCK_MASK_OFF);
		if (have_value && current_value != first)
			return -EUCLEAN;
		first = current_value;
		have_value = true;
	}

	if (!have_value)
		return -ENODEV;

	domain->ctrl_val[closid] = first;
	*value = first;
	return 0;
}

void cbqri_remove_capacity_controls(void)
{
	struct cbqri_component_ctrl *entry, *entry_tmp;
	struct cbqri_component *component, *component_tmp;
	struct cbqri_ctrl_domain *domain, *domain_tmp;
	struct cbqri_resctrl_resource *resource;
	struct resctrl_ctrl *ctrl;
	int rid;

	for (rid = RDT_RESOURCE_L3; rid <= RDT_RESOURCE_L2; rid++) {
		resource = &cbqri_resources[rid];
		ctrl = &resource->capacity_ctrl.r_ctrl;

		list_for_each_entry_safe(domain, domain_tmp, &ctrl->domains,
					 domain.hdr.list) {
			list_del(&domain->domain.hdr.list);
			resctrl_offline_ctrl_domain(&resource->res, ctrl,
						    &domain->domain);
			kfree(domain->ctrl_val);
			kfree(domain);
		}

		list_for_each_entry_safe(component, component_tmp,
					 &resource->components, list) {
			list_for_each_entry_safe(entry, entry_tmp,
						 &component->controllers, list) {
				list_del(&entry->list);
				kfree(entry);
			}
			list_del(&component->list);
			kfree(component);
		}
	}
}
