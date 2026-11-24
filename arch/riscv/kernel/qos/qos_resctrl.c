// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "riscv-cbqri: " fmt

#include <linux/bitfield.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/iopoll.h>
#include <linux/percpu.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/topology.h>

#include <asm/qos.h>

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
	domain->ctrl = &resource->capacity_ctrl;
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

static void cbqri_init_bandwidth_resource(struct cbqri_resctrl_resource *resource)
{
	struct resctrl_ctrl *reserved = &resource->reserved_ctrl.r_ctrl;
	struct resctrl_ctrl *weight = &resource->weight_ctrl.r_ctrl;
	struct rdt_resource *r = &resource->res;

	r->rid = RDT_RESOURCE_MBA;
	r->name = "MB";
	r->alloc_scope = RESCTRL_L3_NODE;
	r->bw_throttle_mode = THREAD_THROTTLE_UNDEFINED;
	INIT_LIST_HEAD(&r->controls);
	INIT_LIST_HEAD(&r->mon_domains);
	INIT_LIST_HEAD(&resource->components);
	resource->num_closid = U32_MAX;

	reserved->scope = RESCTRL_L3_NODE;
	reserved->type = RESCTRL_CTRL_SCALAR;
	reserved->name = RESCTRL_CTRL_NAME_DEF;
	INIT_LIST_HEAD(&reserved->domains);
	INIT_LIST_HEAD(&reserved->emulated_by);
	reserved->scalar.min = 0;
	reserved->scalar.max = MAX_MBA_BW;
	reserved->scalar.gran = 1;
	reserved->scalar.resolution = MAX_MBA_BW;
	reserved->scalar.scale = 1;
	reserved->scalar.unit = RESCTRL_CTRL_UNIT_ALL;
	reserved->scalar.reset_val = 0;
	__set_bit(RESCTRL_SCALAR_FLAG_LINEAR, reserved->scalar.flags);
	resource->reserved_ctrl.kind = CBQRI_CTRL_BANDWIDTH_RESERVED;

	weight->scope = RESCTRL_L3_NODE;
	weight->type = RESCTRL_CTRL_SCALAR;
	weight->name = RESCTRL_CTRL_NAME_WEIGHT;
	INIT_LIST_HEAD(&weight->domains);
	INIT_LIST_HEAD(&weight->emulated_by);
	weight->scalar.min = 0;
	weight->scalar.max = U8_MAX;
	weight->scalar.gran = 1;
	weight->scalar.resolution = 1;
	weight->scalar.scale = 1;
	weight->scalar.unit = RESCTRL_CTRL_UNIT_WEIGHT;
	weight->scalar.reset_val = 0;
	__set_bit(RESCTRL_SCALAR_FLAG_LINEAR, weight->scalar.flags);
	resource->weight_ctrl.kind = CBQRI_CTRL_BANDWIDTH_WEIGHT;
}

static int cbqri_add_memory_component(struct cbqri_controller *ctrl,
				      struct cbqri_resource_info *info)
{
	struct cbqri_resctrl_resource *resource;
	struct cbqri_component *component;

	resource = &cbqri_resources[RDT_RESOURCE_MBA];
	component = cbqri_find_component(resource, info->mem.prox_dom);
	if (!component) {
		component = kzalloc_obj(*component, GFP_KERNEL);
		if (!component)
			return -ENOMEM;

		component->id = info->mem.prox_dom;
		cpumask_copy(&component->cpu_mask,
			     cpumask_of_node(info->mem.prox_dom));
		INIT_LIST_HEAD(&component->controllers);
		list_add_tail(&component->list, &resource->components);
	}

	resource->num_closid = min(resource->num_closid, ctrl->info->rcid_count);
	return cbqri_add_component_controller(component, ctrl);
}

static int cbqri_setup_bandwidth_domain(struct cbqri_resctrl_resource *resource,
					struct cbqri_component *component,
					struct cbqri_resctrl_ctrl *owner)
{
	struct cbqri_ctrl_domain *domain;
	u32 default_value = owner->r_ctrl.scalar.reset_val;
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
	domain->ctrl = owner;
	domain->domain.hdr.id = component->id;
	domain->domain.hdr.rid = RDT_RESOURCE_MBA;
	domain->domain.hdr.type = RESCTRL_CTRL_DOMAIN;
	cpumask_copy(&domain->domain.hdr.cpu_mask, &component->cpu_mask);
	for (i = 0; i < resource->num_closid; i++)
		domain->ctrl_val[i] = default_value;

	ret = resctrl_online_ctrl_domain(&resource->res, &owner->r_ctrl,
					 &domain->domain);
	if (ret) {
		kfree(domain->ctrl_val);
		kfree(domain);
		return ret;
	}

	list_add_tail_rcu(&domain->domain.hdr.list, &owner->r_ctrl.domains);
	return 0;
}

int cbqri_init_bandwidth_controls(void)
{
	struct cbqri_resctrl_resource *resource;
	struct cbqri_resource_info *info;
	struct cbqri_component *component;
	struct cbqri_controller *ctrl;
	int ret;

	resource = &cbqri_resources[RDT_RESOURCE_MBA];
	cbqri_init_bandwidth_resource(resource);

	list_for_each_entry(ctrl, &cbqri_hw_controllers, list) {
		if (ctrl->info->type != CBQRI_CONTROLLER_TYPE_BANDWIDTH ||
		    !ctrl->info->rcid_count)
			continue;

		list_for_each_entry(info, &ctrl->info->resources, list) {
			if (info->id_type != CBQRI_RESOURCE_ID_MEMORY_RANGE)
				continue;
			ret = cbqri_add_memory_component(ctrl, info);
			if (ret)
				goto out_remove;
		}
	}

	if (list_empty(&resource->components))
		return 0;
	if (!resource->num_closid || resource->num_closid == U32_MAX) {
		ret = -EINVAL;
		goto out_remove;
	}

	list_add_tail(&resource->reserved_ctrl.r_ctrl.entry,
		      &resource->res.controls);
	list_add_tail(&resource->weight_ctrl.r_ctrl.entry,
		      &resource->res.controls);
	resource->res.alloc_capable = true;

	list_for_each_entry(component, &resource->components, list) {
		ret = cbqri_setup_bandwidth_domain(resource, component,
						   &resource->reserved_ctrl);
		if (ret)
			goto out_remove;
		ret = cbqri_setup_bandwidth_domain(resource, component,
						   &resource->weight_ctrl);
		if (ret)
			goto out_remove;
	}

	return 0;

out_remove:
	cbqri_remove_bandwidth_controls();
	return ret;
}

static int cbqri_bandwidth_op(struct cbqri_controller *ctrl, u32 op, u32 rcid)
{
	u64 value;
	u32 status;

	value = op | ((u64)rcid << CBQRI_CTL_RCID_SHIFT);
	cbqri_writeq(ctrl, CBQRI_BC_ALLOC_CTL_OFF, value);
	if (cbqri_wait_busy(ctrl, CBQRI_BC_ALLOC_CTL_OFF))
		return -ETIMEDOUT;

	value = cbqri_readq(ctrl, CBQRI_BC_ALLOC_CTL_OFF);
	status = (value >> CBQRI_CTL_STATUS_SHIFT) & CBQRI_CTL_STATUS_MASK;
	return status == CBQRI_BC_ALLOC_CTL_STATUS_SUCCESS ? 0 : -EIO;
}

static u64 cbqri_bandwidth_value(struct cbqri_controller *ctrl,
				 struct cbqri_resctrl_ctrl *control, u32 value)
{
	u64 reg = cbqri_readq(ctrl, CBQRI_BC_BW_ALLOC_OFF);
	u64 mask;
	u64 raw;

	if (control->kind == CBQRI_CTRL_BANDWIDTH_RESERVED) {
		raw = DIV_ROUND_UP((u64)value * ctrl->bc.nbwblks, MAX_MBA_BW);
		mask = (u64)CBQRI_BC_BW_ALLOC_RBWB_MASK <<
			CBQRI_BC_BW_ALLOC_RBWB_SHIFT;
		reg &= ~mask;
		reg |= raw << CBQRI_BC_BW_ALLOC_RBWB_SHIFT;
	} else {
		mask = (u64)CBQRI_BC_BW_ALLOC_MWEIGHT_MASK <<
			CBQRI_BC_BW_ALLOC_MWEIGHT_SHIFT;
		reg &= ~mask;
		reg |= (u64)value << CBQRI_BC_BW_ALLOC_MWEIGHT_SHIFT;
	}

	return reg;
}

int cbqri_bandwidth_set(struct cbqri_ctrl_domain *domain,
			struct cbqri_resctrl_ctrl *control, u32 closid, u32 value)
{
	struct cbqri_component_ctrl *entry;
	u32 op = CBQRI_BC_ALLOC_CTL_OP_CONFIG_LIMIT;
	int ret;

	list_for_each_entry(entry, &domain->component->controllers, list) {
		cbqri_writeq(entry->ctrl, CBQRI_BC_BW_ALLOC_OFF,
			     cbqri_bandwidth_value(entry->ctrl, control, value));
		ret = cbqri_bandwidth_op(entry->ctrl, op, closid);
		if (ret)
			return ret;
	}

	domain->ctrl_val[closid] = value;
	return 0;
}

static u32 cbqri_decode_bandwidth_value(struct cbqri_controller *ctrl,
					struct cbqri_resctrl_ctrl *control)
{
	u64 reg = cbqri_readq(ctrl, CBQRI_BC_BW_ALLOC_OFF);
	u32 raw;

	if (control->kind == CBQRI_CTRL_BANDWIDTH_RESERVED) {
		raw = (reg >> CBQRI_BC_BW_ALLOC_RBWB_SHIFT) &
		      CBQRI_BC_BW_ALLOC_RBWB_MASK;
		return DIV_ROUND_UP((u64)raw * MAX_MBA_BW, ctrl->bc.nbwblks);
	}

	return (reg >> CBQRI_BC_BW_ALLOC_MWEIGHT_SHIFT) &
	       CBQRI_BC_BW_ALLOC_MWEIGHT_MASK;
}

int cbqri_bandwidth_get(struct cbqri_ctrl_domain *domain,
			struct cbqri_resctrl_ctrl *control, u32 closid, u32 *value)
{
	struct cbqri_component_ctrl *entry;
	u32 op = CBQRI_BC_ALLOC_CTL_OP_READ_LIMIT;
	u32 first = 0;
	u32 current_value;
	bool have_value = false;
	int ret;

	list_for_each_entry(entry, &domain->component->controllers, list) {
		ret = cbqri_bandwidth_op(entry->ctrl, op, closid);
		if (ret)
			return ret;
		current_value = cbqri_decode_bandwidth_value(entry->ctrl, control);
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

void cbqri_remove_bandwidth_controls(void)
{
	struct cbqri_resctrl_resource *resource;
	struct cbqri_component_ctrl *entry, *entry_tmp;
	struct cbqri_component *component, *component_tmp;
	struct cbqri_ctrl_domain *domain, *domain_tmp;
	struct cbqri_resctrl_ctrl *controls[2];
	struct resctrl_ctrl *ctrl;
	int i;

	resource = &cbqri_resources[RDT_RESOURCE_MBA];
	controls[0] = &resource->reserved_ctrl;
	controls[1] = &resource->weight_ctrl;

	for (i = 0; i < ARRAY_SIZE(controls); i++) {
		ctrl = &controls[i]->r_ctrl;
		list_for_each_entry_safe(domain, domain_tmp, &ctrl->domains,
					 domain.hdr.list) {
			list_del(&domain->domain.hdr.list);
			resctrl_offline_ctrl_domain(&resource->res, ctrl,
						    &domain->domain);
			kfree(domain->ctrl_val);
			kfree(domain);
		}
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

struct rdt_resource *resctrl_arch_get_resource(enum resctrl_res_level rid)
{
	if (rid < 0 || rid >= RDT_NUM_RESOURCES)
		return NULL;
	return &cbqri_resources[rid].res;
}

u32 resctrl_arch_get_num_closid(struct rdt_resource *r)
{
	struct cbqri_resctrl_resource *resource;

	resource = container_of(r, struct cbqri_resctrl_resource, res);
	return resource->num_closid;
}

bool resctrl_arch_alloc_capable(void)
{
	int rid;

	for (rid = 0; rid < RDT_NUM_RESOURCES; rid++) {
		if (cbqri_resources[rid].res.alloc_capable)
			return true;
	}

	return false;
}

void resctrl_arch_enable_alloc(void)
{
}

void resctrl_arch_disable_alloc(void)
{
}

bool resctrl_arch_mon_capable(void)
{
	return false;
}

void resctrl_arch_enable_mon(void)
{
}

void resctrl_arch_disable_mon(void)
{
}

unsigned int resctrl_arch_round_mon_val(unsigned int value)
{
	return value;
}

void resctrl_arch_set_cpu_default_closid_rmid(int cpu, u32 closid, u32 rmid)
{
	u32 srmcfg;

	WARN_ON_ONCE(closid > FIELD_MAX(SRMCFG_RCID_MASK));
	WARN_ON_ONCE(rmid > FIELD_MAX(SRMCFG_MCID_MASK));

	srmcfg = FIELD_PREP(SRMCFG_RCID_MASK, closid) |
		 FIELD_PREP(SRMCFG_MCID_MASK, rmid);
	WRITE_ONCE(per_cpu(cpu_srmcfg_default, cpu), srmcfg);
}

void resctrl_arch_set_closid_rmid(struct task_struct *task, u32 closid,
				  u32 rmid)
{
	u32 srmcfg;

	WARN_ON_ONCE(closid > FIELD_MAX(SRMCFG_RCID_MASK));
	WARN_ON_ONCE(rmid > FIELD_MAX(SRMCFG_MCID_MASK));

	srmcfg = FIELD_PREP(SRMCFG_RCID_MASK, closid) |
		 FIELD_PREP(SRMCFG_MCID_MASK, rmid);
	WRITE_ONCE(task->thread.srmcfg, srmcfg);
}

bool resctrl_arch_match_closid(struct task_struct *task, u32 closid)
{
	return FIELD_GET(SRMCFG_RCID_MASK,
			 READ_ONCE(task->thread.srmcfg)) == closid;
}

bool resctrl_arch_match_rmid(struct task_struct *task, u32 closid, u32 rmid)
{
	return FIELD_GET(SRMCFG_MCID_MASK,
			 READ_ONCE(task->thread.srmcfg)) == rmid;
}

void resctrl_arch_sched_in(struct task_struct *task)
{
	lockdep_assert_preemption_disabled();
	__switch_to_srmcfg(task);
}

void resctrl_arch_sync_cpu_closid_rmid(void *info)
{
	struct resctrl_cpu_defaults *defaults = info;

	lockdep_assert_preemption_disabled();
	if (defaults)
		resctrl_arch_set_cpu_default_closid_rmid(smp_processor_id(),
							 defaults->closid,
							 defaults->rmid);

	resctrl_arch_sched_in(current);
}

u32 resctrl_arch_system_num_rmid_idx(void)
{
	return 1;
}

u32 resctrl_arch_rmid_idx_encode(u32 closid, u32 rmid)
{
	return rmid;
}

void resctrl_arch_rmid_idx_decode(u32 idx, u32 *closid, u32 *rmid)
{
	*closid = U32_MAX;
	*rmid = idx;
}

void resctrl_arch_pre_mount(void)
{
}

bool resctrl_arch_get_cdp_enabled(struct rdt_resource *r)
{
	return false;
}

int resctrl_arch_set_cdp_enabled(struct rdt_resource *r,
				 struct resctrl_ctrl *ctrl, bool enable)
{
	return enable ? -EOPNOTSUPP : 0;
}

int resctrl_arch_control_mode_set(struct rdt_resource *r,
				  enum resctrl_ctrl_mode newmode)
{
	return 0;
}

bool resctrl_arch_get_io_alloc_enabled(struct rdt_resource *r)
{
	return false;
}

int resctrl_arch_io_alloc_enable(struct rdt_resource *r,
				 struct resctrl_ctrl *ctrl, bool enable)
{
	return enable ? -EOPNOTSUPP : 0;
}

bool resctrl_arch_is_evt_configurable(enum resctrl_event_id eventid)
{
	return false;
}

void resctrl_arch_mon_event_config_read(void *info)
{
}

void resctrl_arch_mon_event_config_write(void *info)
{
}

bool resctrl_arch_mbm_cntr_assign_enabled(struct rdt_resource *r)
{
	return false;
}

int resctrl_arch_mbm_cntr_assign_set(struct rdt_resource *r, bool enable)
{
	return enable ? -EOPNOTSUPP : 0;
}

int resctrl_arch_rmid_read(struct rdt_resource *r, struct rdt_domain_hdr *hdr,
			   u32 closid, u32 rmid,
			   enum resctrl_event_id eventid, void *arch_priv,
			   u64 *value, void *arch_mon_ctx)
{
	*value = 0;
	return -EOPNOTSUPP;
}

void resctrl_arch_reset_rmid(struct rdt_resource *r,
			     struct rdt_l3_mon_domain *domain, u32 closid,
			     u32 rmid, enum resctrl_event_id eventid)
{
}

void resctrl_arch_reset_rmid_all(struct rdt_resource *r,
				 struct rdt_l3_mon_domain *domain)
{
}

void resctrl_arch_config_cntr(struct rdt_resource *r,
			      struct rdt_l3_mon_domain *domain,
			      enum resctrl_event_id eventid, u32 rmid,
			      u32 closid, u32 counter_id, bool assign)
{
}

int resctrl_arch_cntr_read(struct rdt_resource *r,
			   struct rdt_l3_mon_domain *domain, u32 closid,
			   u32 rmid, int counter_id,
			   enum resctrl_event_id eventid, u64 *value)
{
	*value = 0;
	return -EOPNOTSUPP;
}

void resctrl_arch_reset_cntr(struct rdt_resource *r,
			     struct rdt_l3_mon_domain *domain, u32 closid,
			     u32 rmid, int counter_id,
			     enum resctrl_event_id eventid)
{
}

int resctrl_arch_update_one(struct rdt_resource *r, struct resctrl_ctrl *ctrl,
			    struct rdt_ctrl_domain *d, u32 closid,
			    enum resctrl_conf_type type, u32 value)
{
	struct cbqri_resctrl_resource *resource;
	struct cbqri_resctrl_ctrl *control;
	struct cbqri_ctrl_domain *domain;

	resource = container_of(r, struct cbqri_resctrl_resource, res);
	control = container_of(ctrl, struct cbqri_resctrl_ctrl, r_ctrl);
	domain = container_of(d, struct cbqri_ctrl_domain, domain);
	if (closid >= resource->num_closid)
		return -EINVAL;

	switch (control->kind) {
	case CBQRI_CTRL_CAPACITY:
		return cbqri_capacity_set(domain, closid, type, value);
	case CBQRI_CTRL_BANDWIDTH_RESERVED:
	case CBQRI_CTRL_BANDWIDTH_WEIGHT:
		return cbqri_bandwidth_set(domain, control, closid, value);
	}

	return -EINVAL;
}

void resctrl_arch_reset_all_ctrls(struct rdt_resource *r)
{
	struct rdt_ctrl_domain *domain;
	struct resctrl_ctrl *ctrl;
	u32 closid;
	int ret;

	lockdep_assert_cpus_held();

	for_each_resource_ctrl(ctrl, r) {
		list_for_each_entry(domain, &ctrl->domains, hdr.list) {
			for (closid = 0;
			     closid < resctrl_arch_get_num_closid(r); closid++) {
				ret = resctrl_arch_update_one(r, ctrl, domain,
							      closid, CDP_NONE,
							      resctrl_get_default_ctrlval(ctrl));
				if (ret) {
					pr_warn("failed to reset control type %u ID %u in domain %d: %d\n",
						ctrl->name, closid, domain->hdr.id, ret);
					return;
				}
			}
		}
	}
}

static int cbqri_update_control(struct rdt_resource *r,
				struct resctrl_ctrl *ctrl, u32 closid)
{
	struct rdt_ctrl_domain *domain;
	enum resctrl_conf_type type;
	int ret;

	list_for_each_entry(domain, &ctrl->domains, hdr.list) {
		for (type = 0; type < CDP_NUM_TYPES; type++) {
			struct resctrl_staged_config *cfg;

			cfg = &domain->staged_config[type];
			if (!cfg->have_new_ctrl)
				continue;
			ret = resctrl_arch_update_one(r, ctrl, domain, closid,
						      type, cfg->new_ctrl);
			if (ret)
				return ret;
		}
	}

	return 0;
}

int resctrl_arch_update_domains(struct rdt_resource *r, u32 closid,
				enum resctrl_update_reason reason)
{
	struct resctrl_ctrl *ctrl;
	int ret;

	for_each_enabled_ctrl(ctrl, r) {
		ret = cbqri_update_control(r, ctrl, closid);
		if (ret)
			return ret;
	}

	return 0;
}

u32 resctrl_arch_get_config(struct rdt_resource *r, struct resctrl_ctrl *ctrl,
			    struct rdt_ctrl_domain *d, u32 closid,
			    enum resctrl_conf_type type)
{
	struct cbqri_resctrl_resource *resource;
	struct cbqri_resctrl_ctrl *control;
	struct cbqri_ctrl_domain *domain;
	u32 value;
	int ret;

	resource = container_of(r, struct cbqri_resctrl_resource, res);
	control = container_of(ctrl, struct cbqri_resctrl_ctrl, r_ctrl);
	domain = container_of(d, struct cbqri_ctrl_domain, domain);
	if (closid >= resource->num_closid)
		return resctrl_get_default_ctrlval(ctrl);

	switch (control->kind) {
	case CBQRI_CTRL_CAPACITY:
		ret = cbqri_capacity_get(domain, closid, type, &value);
		break;
	case CBQRI_CTRL_BANDWIDTH_RESERVED:
	case CBQRI_CTRL_BANDWIDTH_WEIGHT:
		ret = cbqri_bandwidth_get(domain, control, closid, &value);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	return ret ? domain->ctrl_val[closid] : value;
}

int resctrl_arch_prepare_ctrl_update(u32 closid,
				     enum resctrl_update_reason reason)
{
	return 0;
}

int resctrl_arch_commit_ctrl_update(u32 closid,
				    enum resctrl_update_reason reason)
{
	return 0;
}

void resctrl_arch_abort_ctrl_update(u32 closid,
				    enum resctrl_update_reason reason)
{
}

int resctrl_arch_release_ctrl(u32 closid)
{
	return 0;
}

u32 resctrl_arch_preconvert_bw(const struct rdt_resource *r,
			       struct resctrl_ctrl *ctrl, u32 value)
{
	return value;
}

static void cbqri_init_resources(void)
{
	int rid;

	for (rid = 0; rid < RDT_NUM_RESOURCES; rid++) {
		cbqri_resources[rid].res.rid = rid;
		INIT_LIST_HEAD(&cbqri_resources[rid].res.controls);
		INIT_LIST_HEAD(&cbqri_resources[rid].res.mon_domains);
	}
}

int qos_resctrl_setup(void)
{
	int ret;

	cbqri_init_resources();

	ret = cbqri_probe_controllers();
	if (ret)
		return ret;

	cpus_read_lock();
	ret = cbqri_init_capacity_controls();
	if (ret)
		goto out_unlock;
	ret = cbqri_init_bandwidth_controls();
	if (ret)
		goto out_remove_capacity;
	ret = resctrl_init();
	if (ret)
		goto out_remove_bandwidth;
	cpus_read_unlock();

	return 0;

out_remove_bandwidth:
	cbqri_remove_bandwidth_controls();
out_remove_capacity:
	cbqri_remove_capacity_controls();
out_unlock:
	cpus_read_unlock();
	cbqri_remove_controllers();
	return ret;
}

void qos_resctrl_exit(void)
{
	cpus_read_lock();
	resctrl_exit();
	cbqri_remove_bandwidth_controls();
	cbqri_remove_capacity_controls();
	cpus_read_unlock();
	cbqri_remove_controllers();
}

int qos_resctrl_online_cpu(unsigned int cpu)
{
	resctrl_online_cpu(cpu);
	return 0;
}

int qos_resctrl_offline_cpu(unsigned int cpu)
{
	resctrl_offline_cpu(cpu);
	return 0;
}
