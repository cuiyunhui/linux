// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "riscv-cbqri: " fmt

#include <linux/bitmap.h>
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
static LIST_HEAD(cbqri_mon_domains);

static struct cbqri_resctrl_resource cbqri_resources[RDT_NUM_RESOURCES];
static u32 cbqri_num_rmid = 1;

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

static bool cbqri_supports_rcid_map(struct cbqri_controller *ctrl)
{
	if (ctrl->info->type == CBQRI_CONTROLLER_TYPE_CAPACITY)
		return ctrl->cc.supports_rcid_map;

	return ctrl->bc.supports_rcid_map;
}

static bool cbqri_supports_mcid_map(struct cbqri_controller *ctrl)
{
	if (ctrl->info->type == CBQRI_CONTROLLER_TYPE_CAPACITY)
		return ctrl->cc.supports_mcid_map;

	return ctrl->bc.supports_mcid_map;
}

static bool cbqri_has_rcid_map(struct cbqri_controller *ctrl)
{
	return resctrl_use_logical_ids() && cbqri_supports_rcid_map(ctrl);
}

static bool cbqri_has_mcid_map(struct cbqri_controller *ctrl)
{
	return resctrl_use_logical_ids() && cbqri_supports_mcid_map(ctrl);
}

enum resctrl_id_mode resctrl_arch_id_mode(enum resctrl_id_type type)
{
	struct cbqri_controller *ctrl;

	list_for_each_entry(ctrl, &cbqri_hw_controllers, list) {
		if (type == RESCTRL_CTRL_ID && cbqri_has_rcid_map(ctrl))
			return RESCTRL_LOCAL_MAP;

		if (type == RESCTRL_MON_ID && cbqri_has_mcid_map(ctrl))
			return RESCTRL_LOCAL_MAP;
	}

	return RESCTRL_IDENTITY;
}

static u32 cbqri_logical_rcid_count(struct cbqri_controller *ctrl)
{
	u32 count = cbqri_has_rcid_map(ctrl) ? CBQRI_LOGICAL_ID_COUNT :
		ctrl->info->rcid_count;

	if (ctrl->mon_prefixed && ctrl->info->mcid_count &&
	    !cbqri_supports_mcid_map(ctrl))
		count = min(count, max(1U, ctrl->info->mcid_count >>
					  ctrl->mon_prefix_bits));
	return count;
}

static u32 cbqri_logical_mcid_count(struct cbqri_controller *ctrl)
{
	return cbqri_has_mcid_map(ctrl) ? CBQRI_LOGICAL_ID_COUNT :
		ctrl->info->mcid_count;
}

static int cbqri_effective_int_rcid(struct cbqri_controller *ctrl,
				    u32 req_rcid, u32 *int_rcid)
{
	if (req_rcid >= cbqri_logical_rcid_count(ctrl))
		return -ERANGE;

	if (!cbqri_has_rcid_map(ctrl)) {
		*int_rcid = req_rcid;
		return 0;
	}

	*int_rcid = ctrl->req_to_int[req_rcid];
	return 0;
}

static int cbqri_update_int_rcid(struct cbqri_controller *ctrl, u32 req_rcid,
				 enum resctrl_update_reason reason,
				 u32 *int_rcid)
{
	if (req_rcid >= cbqri_logical_rcid_count(ctrl))
		return -ERANGE;

	if (!cbqri_has_rcid_map(ctrl)) {
		if (cbqri_supports_rcid_map(ctrl) && req_rcid &&
		    ctrl->req_to_int[req_rcid] != req_rcid &&
		    test_bit(req_rcid, ctrl->int_rcid_busy))
			return -EBUSY;
		*int_rcid = req_rcid;
		return 0;
	}
	if (!req_rcid) {
		*int_rcid = 0;
		return 0;
	}
	if (reason == RESCTRL_UPDATE_USER) {
		if (ctrl->pending_valid) {
			if (ctrl->pending_req_rcid != req_rcid)
				return -EINVAL;
			*int_rcid = ctrl->pending_int_rcid;
			return 0;
		}

		*int_rcid = ctrl->req_to_int[req_rcid];
		return *int_rcid ? 0 : -EINVAL;
	}
	if (reason == RESCTRL_UPDATE_GROUP_INIT)
		return -ENOENT;

	*int_rcid = ctrl->req_to_int[req_rcid];
	return *int_rcid ? 0 : -ENOENT;
}

static bool cbqri_component_maps_rcid(struct cbqri_component *component)
{
	struct cbqri_component_ctrl *entry;

	entry = list_first_entry_or_null(&component->controllers,
					 struct cbqri_component_ctrl, list);
	return entry && cbqri_has_rcid_map(entry->ctrl);
}

static u32 cbqri_rcid_map_offset(struct cbqri_controller *ctrl)
{
	u32 slots;

	if (ctrl->info->type == CBQRI_CONTROLLER_TYPE_BANDWIDTH)
		return CBQRI_BC_RCID_MAP_CTL_OFF;

	slots = DIV_ROUND_UP(ctrl->cc.ncblks, 64);
	return CBQRI_CC_BLOCK_MASK_OFF + (slots + 1) * sizeof(u64);
}

static int cbqri_rcid_map_status(u32 status)
{
	switch (status) {
	case CBQRI_RCID_MAP_STATUS_SUCCESS:
		return 0;
	case CBQRI_RCID_MAP_STATUS_INVALID_OP:
		return -EOPNOTSUPP;
	case CBQRI_RCID_MAP_STATUS_INVALID_REQ_RCID:
	case CBQRI_RCID_MAP_STATUS_INVALID_INT_RCID:
		return -ERANGE;
	case CBQRI_RCID_MAP_STATUS_INT_RCID_BUSY:
		return -EBUSY;
	case CBQRI_RCID_MAP_STATUS_CONSTRAINT:
		return -ENOSPC;
	default:
		return -EIO;
	}
}

static int cbqri_rcid_map_op(struct cbqri_controller *ctrl, u32 op,
			     u32 *req_rcid, u32 *int_rcid, bool *valid)
{
	u32 offset = cbqri_rcid_map_offset(ctrl);
	u64 value;
	u32 status;
	int ret;

	if (ctrl->info->size < sizeof(value) ||
	    offset > ctrl->info->size - sizeof(value))
		return -ERANGE;

	if (cbqri_wait_busy(ctrl, offset))
		return -ETIMEDOUT;

	value = op << CBQRI_CTL_OP_SHIFT;
	value |= (u64)*req_rcid << CBQRI_CTL_RCID_SHIFT;
	value |= (u64)*int_rcid << CBQRI_RCID_MAP_INT_RCID_SHIFT;
	cbqri_writeq(ctrl, offset, value);
	if (cbqri_wait_busy(ctrl, offset))
		return -ETIMEDOUT;

	value = cbqri_readq(ctrl, offset);
	status = (value >> CBQRI_CTL_STATUS_SHIFT) & CBQRI_CTL_STATUS_MASK;
	ret = cbqri_rcid_map_status(status);
	if (ret)
		return ret;

	*req_rcid = (value >> CBQRI_CTL_RCID_SHIFT) & CBQRI_CTL_RCID_MASK;
	*int_rcid = (value >> CBQRI_RCID_MAP_INT_RCID_SHIFT) &
		    CBQRI_RCID_MAP_INT_RCID_MASK;
	if (valid)
		*valid = value & BIT_ULL(CBQRI_RCID_MAP_VALID_SHIFT);

	return 0;
}

static void cbqri_free_identifier_map(struct cbqri_controller *ctrl)
{
	bitmap_free(ctrl->mcid_assigned);
	ctrl->mcid_assigned = NULL;
	bitmap_free(ctrl->int_rcid_quarantined);
	bitmap_free(ctrl->int_rcid_busy);
	kfree(ctrl->req_to_int);
	ctrl->int_rcid_quarantined = NULL;
	ctrl->int_rcid_busy = NULL;
	ctrl->req_to_int = NULL;
}

static void cbqri_quarantine_int_rcid(struct cbqri_controller *ctrl,
				      u32 int_rcid)
{
	__set_bit(int_rcid, ctrl->int_rcid_busy);
	__set_bit(int_rcid, ctrl->int_rcid_quarantined);
}

static int cbqri_init_identifier_map(struct cbqri_controller *ctrl)
{
	u32 req_rcid, int_rcid, map_int_rcid;
	bool valid;
	int ret;

	if (!cbqri_supports_rcid_map(ctrl))
		return 0;
	if (!ctrl->info->rcid_count ||
	    ctrl->info->rcid_count > CBQRI_LOGICAL_ID_COUNT)
		return -EINVAL;

	ctrl->req_to_int = kcalloc(CBQRI_LOGICAL_ID_COUNT,
				   sizeof(*ctrl->req_to_int), GFP_KERNEL);
	ctrl->int_rcid_busy = bitmap_zalloc(ctrl->info->rcid_count,
					    GFP_KERNEL);
	ctrl->int_rcid_quarantined = bitmap_zalloc(ctrl->info->rcid_count,
						   GFP_KERNEL);
	if (!ctrl->req_to_int || !ctrl->int_rcid_busy ||
	    !ctrl->int_rcid_quarantined) {
		cbqri_free_identifier_map(ctrl);
		return -ENOMEM;
	}

	__set_bit(0, ctrl->int_rcid_busy);
	for (int_rcid = 1; int_rcid < ctrl->info->rcid_count; int_rcid++) {
		req_rcid = 0;
		map_int_rcid = int_rcid;
		valid = false;
		ret = cbqri_rcid_map_op(ctrl, CBQRI_RCID_MAP_OP_READ_ENTRY,
					&req_rcid, &map_int_rcid, &valid);
		if (ret) {
			cbqri_quarantine_int_rcid(ctrl, int_rcid);
			pr_warn("failed to recover internal RCID %u: %d\n",
				int_rcid, ret);
			continue;
		}
		if (map_int_rcid != int_rcid) {
			cbqri_quarantine_int_rcid(ctrl, int_rcid);
			continue;
		}
		if (!valid)
			continue;

		__set_bit(int_rcid, ctrl->int_rcid_busy);
		if (!req_rcid || req_rcid >= CBQRI_LOGICAL_ID_COUNT ||
		    ctrl->req_to_int[req_rcid]) {
			__set_bit(int_rcid, ctrl->int_rcid_quarantined);
			continue;
		}

		ctrl->req_to_int[req_rcid] = int_rcid;
		if (!cbqri_has_rcid_map(ctrl) && req_rcid == int_rcid)
			continue;
		map_int_rcid = 0;
		ret = cbqri_rcid_map_op(ctrl, CBQRI_RCID_MAP_OP_CONFIG,
					&req_rcid, &map_int_rcid, NULL);
		if (ret) {
			__set_bit(int_rcid, ctrl->int_rcid_quarantined);
			pr_warn("failed to release recovered RCID %u mapping: %d\n",
				req_rcid, ret);
			continue;
		}

		ctrl->req_to_int[req_rcid] = 0;
		__clear_bit(int_rcid, ctrl->int_rcid_busy);
	}

	return 0;
}

static u32 cbqri_mon_ctl_offset(struct cbqri_controller *ctrl)
{
	return ctrl->info->type == CBQRI_CONTROLLER_TYPE_CAPACITY ?
		CBQRI_CC_MON_CTL_OFF : CBQRI_BC_MON_CTL_OFF;
}

static int cbqri_mon_op(struct cbqri_controller *ctrl, u32 op, u32 mcid,
			u32 event_id, u32 *status)
{
	u32 offset = cbqri_mon_ctl_offset(ctrl);
	u64 value;

	if (cbqri_wait_busy(ctrl, offset))
		return -ETIMEDOUT;

	value = op << CBQRI_CTL_OP_SHIFT;
	value |= (u64)mcid << CBQRI_CTL_RCID_SHIFT;
	value |= (u64)event_id << CBQRI_MON_CTL_EVT_ID_SHIFT;
	cbqri_writeq(ctrl, offset, value);
	if (cbqri_wait_busy(ctrl, offset))
		return -ETIMEDOUT;

	value = cbqri_readq(ctrl, offset);
	*status = (value >> CBQRI_CTL_STATUS_SHIFT) & CBQRI_CTL_STATUS_MASK;
	return 0;
}

static int cbqri_config_event(struct cbqri_controller *ctrl, u32 mcid,
			      u32 event_id)
{
	u32 status;
	int ret;

	ret = cbqri_mon_op(ctrl, CBQRI_MON_CTL_OP_CONFIG_EVENT, mcid,
			   event_id, &status);
	if (ret) {
		if (event_id != CBQRI_MON_EVT_NONE &&
		    ctrl->mcid_assigned && mcid < CBQRI_LOGICAL_ID_COUNT)
			__set_bit(mcid, ctrl->mcid_assigned);
		return ret;
	}
	if (status == CBQRI_MON_CTL_STATUS_NO_COUNTER)
		return -ENOSPC;
	if (status != CBQRI_MON_CTL_STATUS_SUCCESS)
		return -EIO;

	if (ctrl->mcid_assigned) {
		if (event_id == CBQRI_MON_EVT_NONE)
			__clear_bit(mcid, ctrl->mcid_assigned);
		else
			__set_bit(mcid, ctrl->mcid_assigned);
	}
	return 0;
}

static int cbqri_event_id(struct cbqri_controller *ctrl,
			  enum resctrl_event_id event_id)
{
	if (ctrl->info->type == CBQRI_CONTROLLER_TYPE_CAPACITY &&
	    event_id == QOS_L3_OCCUP_EVENT_ID)
		return CBQRI_CC_MON_EVT_OCCUPANCY;
	if (ctrl->info->type == CBQRI_CONTROLLER_TYPE_BANDWIDTH &&
	    event_id == QOS_L3_MBM_TOTAL_EVENT_ID)
		return CBQRI_BC_MON_EVT_RDWR_COUNT;

	return -EOPNOTSUPP;
}

static int cbqri_read_counter(struct cbqri_controller *ctrl, u32 mcid,
			      enum resctrl_event_id event_id, u64 *value)
{
	u32 offset, status;
	u64 counter;
	int hw_event;
	int ret;

	if (mcid >= cbqri_logical_mcid_count(ctrl))
		return -ERANGE;
	hw_event = cbqri_event_id(ctrl, event_id);
	if (hw_event < 0)
		return hw_event;

	ret = cbqri_mon_op(ctrl, CBQRI_MON_CTL_OP_READ_COUNTER, mcid,
			   CBQRI_MON_EVT_NONE, &status);
	if (ret)
		return ret;
	if (status == CBQRI_MON_CTL_STATUS_NO_COUNTER)
		return -ENOSPC;
	if (status != CBQRI_MON_CTL_STATUS_SUCCESS)
		return -EIO;

	if (ctrl->info->type == CBQRI_CONTROLLER_TYPE_CAPACITY) {
		offset = CBQRI_CC_MON_CTR_VAL_OFF;
		counter = cbqri_readq(ctrl, offset);
		if (counter & BIT_ULL(CBQRI_CC_MON_CTR_INV_BIT))
			return -EAGAIN;
		*value = counter & GENMASK_ULL(62, 0);
	} else {
		offset = CBQRI_BC_MON_CTR_VAL_OFF;
		counter = cbqri_readq(ctrl, offset);
		if (counter & BIT_ULL(CBQRI_BC_MON_CTR_INV_BIT))
			return -EAGAIN;
		*value = counter & GENMASK_ULL(61, 0);
	}

	return 0;
}

static int cbqri_probe_monitor(struct cbqri_controller *ctrl)
{
	u32 status;
	u32 mcid;
	int ret;

	if (!ctrl->info->mcid_count)
		return 0;

	ret = cbqri_mon_op(ctrl, CBQRI_MON_CTL_OP_READ_COUNTER, 0,
			   CBQRI_MON_EVT_NONE, &status);
	if (ret)
		return ret;
	if (status != CBQRI_MON_CTL_STATUS_SUCCESS &&
	    !(cbqri_supports_mcid_map(ctrl) &&
	      status == CBQRI_MON_CTL_STATUS_NO_COUNTER))
		return 0;

	ctrl->mcid_assigned = bitmap_zalloc(CBQRI_LOGICAL_ID_COUNT,
					    GFP_KERNEL);
	if (!ctrl->mcid_assigned)
		return -ENOMEM;
	ctrl->mon_capable = true;
	for (mcid = 0; mcid < (cbqri_supports_mcid_map(ctrl) ?
			      CBQRI_LOGICAL_ID_COUNT : ctrl->info->mcid_count);
	     mcid++) {
		ret = cbqri_config_event(ctrl, mcid, CBQRI_MON_EVT_NONE);
		if (ret)
			return ret;
	}

	if (ctrl->mon_prefixed) {
		ctrl->mon_capable = false;
		pr_info("RCID-prefixed monitoring is not exposed by this PoC\n");
	}

	return 0;
}

static int cbqri_read_capacity_caps(struct cbqri_controller *ctrl)
{
	u64 value = cbqri_readq(ctrl, CBQRI_CC_CAPABILITIES_OFF);
	u32 version;

	if (!value)
		return -ENODEV;

	version = value & GENMASK(7, 0);
	ctrl->version_minor = value & CBQRI_CAPABILITIES_VER_MINOR_MASK;
	ctrl->version_major = (value & CBQRI_CAPABILITIES_VER_MAJOR_MASK) >> 4;
	if (version == CBQRI_ID_MAP_VERSION) {
		ctrl->cc.supports_rcid_map =
			(value >> CBQRI_CAPABILITIES_RCID_MAP_SHIFT) & 1;
		ctrl->cc.supports_mcid_map =
			(value >> CBQRI_CAPABILITIES_MCID_MAP_SHIFT) & 1;
	}
	ctrl->mon_prefixed = value & BIT_ULL(26);
	ctrl->mon_prefix_bits = (value >> 27) & 0xf;
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
	u32 version;

	if (!value)
		return -ENODEV;

	version = value & GENMASK(7, 0);
	ctrl->version_minor = value & CBQRI_CAPABILITIES_VER_MINOR_MASK;
	ctrl->version_major = (value & CBQRI_CAPABILITIES_VER_MAJOR_MASK) >> 4;
	if (version == CBQRI_ID_MAP_VERSION) {
		ctrl->bc.supports_rcid_map =
			(value >> CBQRI_CAPABILITIES_RCID_MAP_SHIFT) & 1;
		ctrl->bc.supports_mcid_map =
			(value >> CBQRI_CAPABILITIES_MCID_MAP_SHIFT) & 1;
	}
	if (version == CBQRI_INACTIVE_ENTRY_VERSION ||
	    version == CBQRI_ID_MAP_VERSION)
		ctrl->bc.supports_inactive_entry =
			(value >> CBQRI_INACTIVE_ENTRY_SHIFT) & 1;
	ctrl->mon_prefixed = value & BIT_ULL(24);
	ctrl->mon_prefix_bits = (value >> 25) & 0xf;
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
		cbqri_free_identifier_map(ctrl);
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

		ret = cbqri_init_identifier_map(ctrl);
		if (ret) {
			cbqri_free_identifier_map(ctrl);
			iounmap(ctrl->base);
			release_mem_region(ctrl->info->addr, ctrl->info->size);
			kfree(ctrl);
			goto out_remove;
		}

		ret = cbqri_probe_monitor(ctrl);
		if (ret) {
			cbqri_free_identifier_map(ctrl);
			iounmap(ctrl->base);
			release_mem_region(ctrl->info->addr, ctrl->info->size);
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
	struct cbqri_component_ctrl *first;

	list_for_each_entry(entry, &component->controllers, list) {
		if (entry->ctrl == ctrl)
			return 0;
	}
	first = list_first_entry_or_null(&component->controllers,
					 struct cbqri_component_ctrl, list);
	if (first && cbqri_has_rcid_map(first->ctrl) !=
		     cbqri_has_rcid_map(ctrl))
		return -EOPNOTSUPP;

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

	resource->num_closid = min(resource->num_closid,
				   cbqri_logical_rcid_count(ctrl));
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

	if (cbqri_wait_busy(ctrl, CBQRI_CC_ALLOC_CTL_OFF))
		return -ETIMEDOUT;

	cbqri_writeq(ctrl, CBQRI_CC_ALLOC_CTL_OFF, value);
	if (cbqri_wait_busy(ctrl, CBQRI_CC_ALLOC_CTL_OFF))
		return -ETIMEDOUT;

	value = cbqri_readq(ctrl, CBQRI_CC_ALLOC_CTL_OFF);
	status = (value >> CBQRI_CTL_STATUS_SHIFT) & CBQRI_CTL_STATUS_MASK;
	return status == CBQRI_CC_ALLOC_CTL_STATUS_SUCCESS ? 0 : -EIO;
}

int cbqri_capacity_set(struct cbqri_ctrl_domain *domain, u32 closid,
		       enum resctrl_conf_type type, u32 value,
		       enum resctrl_update_reason reason)
{
	struct cbqri_component_ctrl *entry;
	u32 int_rcid;
	int ret;

	list_for_each_entry(entry, &domain->component->controllers, list) {
		ret = cbqri_update_int_rcid(entry->ctrl, closid, reason,
					    &int_rcid);
		if (ret == -ENOENT)
			continue;
		if (ret)
			return ret;
		if (cbqri_wait_busy(entry->ctrl, CBQRI_CC_ALLOC_CTL_OFF))
			return -ETIMEDOUT;
		cbqri_writeq(entry->ctrl, CBQRI_CC_BLOCK_MASK_OFF, value);
		ret = cbqri_capacity_op(entry->ctrl,
					CBQRI_CC_ALLOC_CTL_OP_CONFIG_LIMIT,
					int_rcid, type);
		if (ret)
			return ret;
	}

	if (reason != RESCTRL_UPDATE_USER ||
	    !cbqri_component_maps_rcid(domain->component))
		domain->ctrl_val[closid] = value;
	return 0;
}

int cbqri_capacity_get(struct cbqri_ctrl_domain *domain, u32 closid,
		       enum resctrl_conf_type type, u32 *value)
{
	struct cbqri_component_ctrl *entry;
	u32 op = CBQRI_CC_ALLOC_CTL_OP_READ_LIMIT;
	u32 first = 0;
	u32 current_value, int_rcid;
	bool have_value = false;
	int ret;

	list_for_each_entry(entry, &domain->component->controllers, list) {
		ret = cbqri_effective_int_rcid(entry->ctrl, closid,
					       &int_rcid);
		if (ret)
			return ret;
		ret = cbqri_capacity_op(entry->ctrl, op, int_rcid, type);
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
	reserved->scalar.reset_val = 1;
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
	weight->scalar.reset_val = 1;
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

	resource->num_closid = min(resource->num_closid,
				   cbqri_logical_rcid_count(ctrl));
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

		if (!ctrl->bc.supports_inactive_entry)
			resource->reserved_ctrl.r_ctrl.scalar.min = 1;

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
	if (cbqri_wait_busy(ctrl, CBQRI_BC_ALLOC_CTL_OFF))
		return -ETIMEDOUT;

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
			struct cbqri_resctrl_ctrl *control, u32 closid, u32 value,
			enum resctrl_update_reason reason)
{
	struct cbqri_component_ctrl *entry;
	u32 int_rcid;
	int ret;

	list_for_each_entry(entry, &domain->component->controllers, list) {
		ret = cbqri_update_int_rcid(entry->ctrl, closid, reason,
					    &int_rcid);
		if (ret == -ENOENT)
			continue;
		if (ret)
			return ret;
		ret = cbqri_bandwidth_op(entry->ctrl,
					 CBQRI_BC_ALLOC_CTL_OP_READ_LIMIT,
					 int_rcid);
		if (ret)
			return ret;
		cbqri_writeq(entry->ctrl, CBQRI_BC_BW_ALLOC_OFF,
			     cbqri_bandwidth_value(entry->ctrl, control, value));
		ret = cbqri_bandwidth_op(entry->ctrl,
					 CBQRI_BC_ALLOC_CTL_OP_CONFIG_LIMIT,
					 int_rcid);
		if (ret)
			return ret;
	}

	if (reason != RESCTRL_UPDATE_USER ||
	    !cbqri_component_maps_rcid(domain->component))
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
	u32 current_value, int_rcid;
	bool have_value = false;
	int ret;

	list_for_each_entry(entry, &domain->component->controllers, list) {
		ret = cbqri_effective_int_rcid(entry->ctrl, closid,
					       &int_rcid);
		if (ret)
			return ret;
		ret = cbqri_bandwidth_op(entry->ctrl, op, int_rcid);
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

static struct cbqri_mon_domain *cbqri_find_mon_domain(u32 id)
{
	struct cbqri_mon_domain *domain;

	list_for_each_entry(domain, &cbqri_mon_domains, list) {
		if (domain->domain.hdr.id == id)
			return domain;
	}

	return NULL;
}

static struct cbqri_mon_domain *
cbqri_find_mon_domain_by_mask(const struct cpumask *mask)
{
	struct cbqri_mon_domain *domain;

	list_for_each_entry(domain, &cbqri_mon_domains, list) {
		if (cpumask_equal(&domain->domain.hdr.cpu_mask, mask))
			return domain;
	}

	return NULL;
}

static struct cbqri_mon_domain *
cbqri_new_mon_domain(struct cbqri_component *component)
{
	struct cbqri_mon_domain *domain;

	domain = kzalloc_obj(*domain, GFP_KERNEL);
	if (!domain)
		return NULL;

	domain->domain.hdr.id = component->id;
	domain->domain.hdr.rid = RDT_RESOURCE_L3;
	domain->domain.hdr.type = RESCTRL_MON_DOMAIN;
	cpumask_copy(&domain->domain.hdr.cpu_mask, &component->cpu_mask);
	INIT_LIST_HEAD(&domain->domain.hdr.list);
	INIT_LIST_HEAD(&domain->controllers);
	INIT_LIST_HEAD(&domain->list);
	list_add_tail(&domain->list, &cbqri_mon_domains);

	return domain;
}

static int cbqri_add_mon_controller(struct cbqri_mon_domain *domain,
				    struct cbqri_controller *ctrl)
{
	struct cbqri_mon_ctrl *entry;

	list_for_each_entry(entry, &domain->controllers, list) {
		if (entry->ctrl == ctrl)
			return 0;
	}

	entry = kzalloc_obj(*entry, GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	entry->ctrl = ctrl;
	list_add_tail(&entry->list, &domain->controllers);

	if (ctrl->info->type == CBQRI_CONTROLLER_TYPE_CAPACITY)
		domain->has_occupancy = true;
	else
		domain->has_mbm_total = true;
	cbqri_num_rmid = min(cbqri_num_rmid,
			     cbqri_logical_mcid_count(ctrl));

	return 0;
}

static int cbqri_add_capacity_mon_domains(void)
{
	struct cbqri_resctrl_resource *resource;
	struct cbqri_component_ctrl *entry;
	struct cbqri_component *component;
	struct cbqri_mon_domain *domain;
	int ret;

	resource = &cbqri_resources[RDT_RESOURCE_L3];
	list_for_each_entry(component, &resource->components, list) {
		domain = NULL;
		list_for_each_entry(entry, &component->controllers, list) {
			if (!entry->ctrl->mon_capable)
				continue;
			if (!domain) {
				domain = cbqri_find_mon_domain(component->id);
				if (!domain)
					domain = cbqri_new_mon_domain(component);
				if (!domain)
					return -ENOMEM;
			}
			ret = cbqri_add_mon_controller(domain, entry->ctrl);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int cbqri_add_bandwidth_mon_controllers(void)
{
	struct cbqri_resctrl_resource *resource;
	struct cbqri_component_ctrl *entry;
	struct cbqri_component *component;
	struct cbqri_mon_domain *domain;
	int ret;

	resource = &cbqri_resources[RDT_RESOURCE_MBA];
	list_for_each_entry(component, &resource->components, list) {
		domain = cbqri_find_mon_domain_by_mask(&component->cpu_mask);

		list_for_each_entry(entry, &component->controllers, list) {
			if (!entry->ctrl->mon_capable)
				continue;
			if (!domain) {
				domain = cbqri_new_mon_domain(component);
				if (!domain)
					return -ENOMEM;
			}
			ret = cbqri_add_mon_controller(domain, entry->ctrl);
			if (ret)
				return ret;
		}
	}

	return 0;
}

int cbqri_init_monitoring(void)
{
	struct cbqri_resctrl_resource *resource;
	struct cbqri_mon_domain *domain;
	bool has_mbm_total = false;
	bool has_occupancy = false;
	int ret;

	cbqri_num_rmid = U32_MAX;
	ret = cbqri_add_capacity_mon_domains();
	if (ret)
		goto out_remove;
	ret = cbqri_add_bandwidth_mon_controllers();
	if (ret)
		goto out_remove;
	if (list_empty(&cbqri_mon_domains)) {
		cbqri_num_rmid = 1;
		return 0;
	}

	list_for_each_entry(domain, &cbqri_mon_domains, list) {
		has_occupancy |= domain->has_occupancy;
		has_mbm_total |= domain->has_mbm_total;
	}
	if (has_occupancy &&
	    !resctrl_enable_mon_event(QOS_L3_OCCUP_EVENT_ID, false, 0, NULL)) {
		ret = -EINVAL;
		goto out_remove;
	}
	if (has_mbm_total &&
	    !resctrl_enable_mon_event(QOS_L3_MBM_TOTAL_EVENT_ID, false, 0,
				      NULL)) {
		ret = -EINVAL;
		goto out_remove;
	}

	resource = &cbqri_resources[RDT_RESOURCE_L3];
	resource->res.mon_capable = true;
	resource->res.mon_scope = RESCTRL_L3_CACHE;
	resource->res.mon.num_rmid = cbqri_num_rmid;
	list_for_each_entry(domain, &cbqri_mon_domains, list) {
		ret = resctrl_online_mon_domain(&resource->res,
						&domain->domain.hdr);
		if (ret)
			goto out_remove;
		list_add_tail_rcu(&domain->domain.hdr.list,
				  &resource->res.mon_domains);
		domain->online = true;
	}

	return 0;

out_remove:
	cbqri_remove_monitoring();
	return ret;
}

void cbqri_remove_monitoring(void)
{
	struct cbqri_resctrl_resource *resource;
	struct cbqri_mon_ctrl *entry, *entry_tmp;
	struct cbqri_mon_domain *domain, *domain_tmp;
	bool synchronize = false;

	resource = &cbqri_resources[RDT_RESOURCE_L3];
	list_for_each_entry(domain, &cbqri_mon_domains, list) {
		if (!domain->online)
			continue;
		list_del_rcu(&domain->domain.hdr.list);
		synchronize = true;
	}
	if (synchronize)
		synchronize_rcu();

	list_for_each_entry_safe(domain, domain_tmp, &cbqri_mon_domains, list) {
		if (domain->online)
			resctrl_offline_mon_domain(&resource->res,
						   &domain->domain.hdr);
		list_for_each_entry_safe(entry, entry_tmp, &domain->controllers,
					 list) {
			list_del(&entry->list);
			kfree(entry);
		}
		list_del(&domain->list);
		kfree(domain);
	}
	resource->res.mon_capable = false;
	cbqri_num_rmid = 1;
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
	return cbqri_resources[RDT_RESOURCE_L3].res.mon_capable;
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
	return cbqri_num_rmid;
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
	struct rdt_l3_mon_domain *rdt_domain;
	struct cbqri_mon_domain *domain;
	struct cbqri_mon_ctrl *entry;
	bool found = false;
	u64 counter_value;
	int ret;

	if (r != &cbqri_resources[RDT_RESOURCE_L3].res || !hdr || !value)
		return -EINVAL;
	if (hdr->type != RESCTRL_MON_DOMAIN || hdr->rid != RDT_RESOURCE_L3)
		return -EINVAL;

	rdt_domain = container_of(hdr, struct rdt_l3_mon_domain, hdr);
	domain = container_of(rdt_domain, struct cbqri_mon_domain, domain);
	*value = 0;
	list_for_each_entry(entry, &domain->controllers, list) {
		ret = cbqri_read_counter(entry->ctrl, rmid, eventid,
					 &counter_value);
		if (ret == -EOPNOTSUPP)
			continue;
		if (ret == -ENOSPC)
			return -ENOENT;
		if (ret)
			return ret;
		if (U64_MAX - *value < counter_value)
			return -EOVERFLOW;
		*value += counter_value;
		found = true;
	}

	return found ? 0 : -EOPNOTSUPP;
}

void resctrl_arch_reset_rmid(struct rdt_resource *r,
			     struct rdt_l3_mon_domain *rdt_domain, u32 closid,
			     u32 rmid, enum resctrl_event_id eventid)
{
	struct cbqri_mon_domain *domain;
	struct cbqri_mon_ctrl *entry;
	int event, ret;

	if (r != &cbqri_resources[RDT_RESOURCE_L3].res)
		return;
	domain = container_of(rdt_domain, struct cbqri_mon_domain, domain);

	list_for_each_entry(entry, &domain->controllers, list) {
		struct cbqri_controller *ctrl = entry->ctrl;

		event = cbqri_event_id(ctrl, eventid);
		if (event < 0)
			continue;
		if (cbqri_supports_mcid_map(ctrl) &&
		    !test_bit(rmid, ctrl->mcid_assigned) &&
		    bitmap_weight(ctrl->mcid_assigned, CBQRI_LOGICAL_ID_COUNT) >=
		    ctrl->info->mcid_count)
			return;
	}
	list_for_each_entry(entry, &domain->controllers, list) {
		event = cbqri_event_id(entry->ctrl, eventid);
		if (event < 0)
			continue;
		ret = cbqri_config_event(entry->ctrl, rmid, event);
		if (ret)
			goto abort;
	}
	return;

abort:
	list_for_each_entry(entry, &domain->controllers, list) {
		if (cbqri_event_id(entry->ctrl, eventid) < 0)
			continue;
		if (cbqri_config_event(entry->ctrl, rmid, CBQRI_MON_EVT_NONE))
			pr_warn_ratelimited("failed to undo MCID %u activation\n",
					    rmid);
	}
	if (ret != -ENOSPC)
		pr_warn_ratelimited("failed to activate MCID %u: %d\n",
				    rmid, ret);
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

static int cbqri_update_one(struct rdt_resource *r, struct resctrl_ctrl *ctrl,
			    struct rdt_ctrl_domain *d, u32 closid,
			    enum resctrl_conf_type type, u32 value,
			    enum resctrl_update_reason reason)
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
		return cbqri_capacity_set(domain, closid, type, value, reason);
	case CBQRI_CTRL_BANDWIDTH_RESERVED:
	case CBQRI_CTRL_BANDWIDTH_WEIGHT:
		return cbqri_bandwidth_set(domain, control, closid, value,
					   reason);
	}

	return -EINVAL;
}

int resctrl_arch_update_one(struct rdt_resource *r, struct resctrl_ctrl *ctrl,
			    struct rdt_ctrl_domain *d, u32 closid,
			    enum resctrl_conf_type type, u32 value)
{
	return cbqri_update_one(r, ctrl, d, closid, type, value,
				RESCTRL_UPDATE_INTERNAL);
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
				if (ret == -ENOENT && closid)
					continue;
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
				struct resctrl_ctrl *ctrl, u32 closid,
				enum resctrl_update_reason reason)
{
	struct rdt_ctrl_domain *domain;
	enum resctrl_conf_type type;
	int ret;

	list_for_each_entry(domain, &ctrl->domains, hdr.list) {
		struct cbqri_ctrl_domain *cbqri_domain;

		for (type = 0; type < CDP_NUM_TYPES; type++) {
			struct resctrl_staged_config *cfg;

			cfg = &domain->staged_config[type];
			if (!cfg->have_new_ctrl)
				continue;
			cbqri_domain = container_of(domain,
						    struct cbqri_ctrl_domain, domain);
			ret = cbqri_update_one(r, ctrl, domain, closid, type,
					       cfg->new_ctrl, reason);
			if (ret)
				return ret;
		}
	}

	return 0;
}

static int cbqri_group_binding(struct cbqri_resctrl_resource *resource,
			       u32 closid, bool initialize)
{
	struct cbqri_component *component;

	if (!closid)
		return 0;
	list_for_each_entry(component, &resource->components, list) {
		struct cbqri_component_ctrl *entry;

		list_for_each_entry(entry, &component->controllers, list) {
			struct cbqri_controller *ctrl = entry->ctrl;
			u32 req_rcid = closid, old_rcid, int_rcid;
			int ret;

			if (!cbqri_supports_rcid_map(ctrl))
				continue;
			if (!initialize && cbqri_has_rcid_map(ctrl))
				continue;
			old_rcid = ctrl->req_to_int[closid];
			if (initialize && !old_rcid)
				continue;
			int_rcid = initialize ? 0 : closid;
			if (!initialize && old_rcid == int_rcid)
				continue;
			ret = cbqri_rcid_map_op(ctrl, CBQRI_RCID_MAP_OP_CONFIG,
						&req_rcid, &int_rcid, NULL);
			if (ret)
				return ret;
			ctrl->req_to_int[closid] = int_rcid;
			if (old_rcid && old_rcid != int_rcid) {
				__clear_bit(old_rcid, ctrl->int_rcid_busy);
				__clear_bit(old_rcid, ctrl->int_rcid_quarantined);
			}
			if (int_rcid)
				__set_bit(int_rcid, ctrl->int_rcid_busy);
		}
	}
	return 0;
}

int resctrl_arch_update_domains(struct rdt_resource *r, u32 closid,
				enum resctrl_update_reason reason)
{
	struct cbqri_resctrl_resource *resource;
	struct resctrl_ctrl *ctrl;
	int ret;

	resource = container_of(r, struct cbqri_resctrl_resource, res);
	if (reason == RESCTRL_UPDATE_GROUP_INIT) {
		ret = cbqri_group_binding(resource, closid, true);
		if (ret)
			return ret;
	}

	for_each_enabled_ctrl(ctrl, r) {
		ret = cbqri_update_control(r, ctrl, closid, reason);
		if (ret)
			return ret;
	}

	if (reason == RESCTRL_UPDATE_GROUP_INIT)
		return cbqri_group_binding(resource, closid, false);
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

static bool cbqri_component_has_controller(struct cbqri_component *component,
					   struct cbqri_controller *ctrl)
{
	struct cbqri_component_ctrl *entry;

	list_for_each_entry(entry, &component->controllers, list) {
		if (entry->ctrl == ctrl)
			return true;
	}

	return false;
}

static bool cbqri_controller_needs_update(struct cbqri_controller *ctrl,
					  u32 closid)
{
	struct cbqri_resctrl_resource *resource;
	struct cbqri_ctrl_domain *domain;
	struct rdt_ctrl_domain *rdt_domain;
	struct resctrl_ctrl *control;
	enum resctrl_conf_type type;
	int rid;

	for (rid = 0; rid < RDT_NUM_RESOURCES; rid++) {
		resource = &cbqri_resources[rid];
		if (!resource->res.alloc_capable ||
		    closid >= resource->num_closid)
			continue;

		for_each_enabled_ctrl(control, &resource->res) {
			list_for_each_entry(rdt_domain, &control->domains,
					    hdr.list) {
				domain = container_of(rdt_domain,
						      struct cbqri_ctrl_domain,
						      domain);
				if (!cbqri_component_has_controller(domain->component,
								    ctrl))
					continue;

				for (type = 0; type < CDP_NUM_TYPES; type++) {
					struct resctrl_staged_config *cfg;

					cfg = &rdt_domain->staged_config[type];
					if (cfg->have_new_ctrl)
						return true;
				}
			}
		}
	}

	return false;
}

static int cbqri_materialize_capacity_entry(struct cbqri_controller *ctrl,
					    u32 from, u32 to)
{
	u64 value;
	int ret;

	ret = cbqri_capacity_op(ctrl, CBQRI_CC_ALLOC_CTL_OP_READ_LIMIT,
				from, CDP_NONE);
	if (ret)
		return ret;
	value = cbqri_readq(ctrl, CBQRI_CC_BLOCK_MASK_OFF);
	cbqri_writeq(ctrl, CBQRI_CC_BLOCK_MASK_OFF, value);
	ret = cbqri_capacity_op(ctrl, CBQRI_CC_ALLOC_CTL_OP_CONFIG_LIMIT,
				to, CDP_NONE);
	if (ret)
		return ret;
	ret = cbqri_capacity_op(ctrl, CBQRI_CC_ALLOC_CTL_OP_READ_LIMIT,
				to, CDP_NONE);
	if (ret)
		return ret;

	return cbqri_readq(ctrl, CBQRI_CC_BLOCK_MASK_OFF) == value ? 0 : -EIO;
}

static int cbqri_materialize_bandwidth_entry(struct cbqri_controller *ctrl,
					     u32 from, u32 to)
{
	u64 value, readback;
	int ret;

	ret = cbqri_bandwidth_op(ctrl, CBQRI_BC_ALLOC_CTL_OP_READ_LIMIT, from);
	if (ret)
		return ret;
	value = cbqri_readq(ctrl, CBQRI_BC_BW_ALLOC_OFF);
	cbqri_writeq(ctrl, CBQRI_BC_BW_ALLOC_OFF, value);
	ret = cbqri_bandwidth_op(ctrl, CBQRI_BC_ALLOC_CTL_OP_CONFIG_LIMIT, to);
	if (ret)
		return ret;
	ret = cbqri_bandwidth_op(ctrl, CBQRI_BC_ALLOC_CTL_OP_READ_LIMIT, to);
	if (ret)
		return ret;
	readback = cbqri_readq(ctrl, CBQRI_BC_BW_ALLOC_OFF);

	return readback == value ? 0 : -EIO;
}

static int cbqri_materialize_entry(struct cbqri_controller *ctrl, u32 from,
				   u32 to)
{
	if (ctrl->info->type == CBQRI_CONTROLLER_TYPE_CAPACITY)
		return cbqri_materialize_capacity_entry(ctrl, from, to);

	return cbqri_materialize_bandwidth_entry(ctrl, from, to);
}

static void cbqri_clear_pending(struct cbqri_controller *ctrl,
				bool release_new)
{
	if (release_new)
		__clear_bit(ctrl->pending_int_rcid, ctrl->int_rcid_busy);
	ctrl->pending_req_rcid = 0;
	ctrl->pending_int_rcid = 0;
	ctrl->pending_old_int_rcid = 0;
	ctrl->pending_valid = false;
}

static void cbqri_discard_pending(u32 closid)
{
	struct cbqri_controller *ctrl;

	list_for_each_entry(ctrl, &cbqri_hw_controllers, list) {
		if (!ctrl->pending_valid || ctrl->pending_req_rcid != closid)
			continue;
		cbqri_clear_pending(ctrl, true);
	}
}

int resctrl_arch_prepare_ctrl_update(u32 closid,
				     enum resctrl_update_reason reason)
{
	struct cbqri_controller *ctrl;
	u32 int_rcid;
	int ret;

	if (reason != RESCTRL_UPDATE_USER || !closid)
		return 0;

	list_for_each_entry(ctrl, &cbqri_hw_controllers, list) {
		if (!cbqri_has_rcid_map(ctrl) ||
		    !cbqri_controller_needs_update(ctrl, closid))
			continue;
		if (ctrl->pending_valid)
			return -EBUSY;
		if (ctrl->req_to_int[closid])
			continue;
		int_rcid = find_next_zero_bit(ctrl->int_rcid_busy,
					      ctrl->info->rcid_count, 1);
		if (int_rcid >= ctrl->info->rcid_count)
			return -ENOSPC;
	}

	list_for_each_entry(ctrl, &cbqri_hw_controllers, list) {
		if (!cbqri_has_rcid_map(ctrl) ||
		    !cbqri_controller_needs_update(ctrl, closid) ||
		    ctrl->req_to_int[closid])
			continue;

		int_rcid = find_next_zero_bit(ctrl->int_rcid_busy,
					      ctrl->info->rcid_count, 1);
		__set_bit(int_rcid, ctrl->int_rcid_busy);
		ctrl->pending_req_rcid = closid;
		ctrl->pending_int_rcid = int_rcid;
		ctrl->pending_old_int_rcid = ctrl->req_to_int[closid];
		ctrl->pending_valid = true;
	}

	list_for_each_entry(ctrl, &cbqri_hw_controllers, list) {
		if (!ctrl->pending_valid || ctrl->pending_req_rcid != closid)
			continue;
		ret = cbqri_materialize_entry(ctrl, ctrl->pending_old_int_rcid,
					      ctrl->pending_int_rcid);
		if (ret) {
			cbqri_discard_pending(closid);
			return ret;
		}
	}

	return 0;
}

static bool cbqri_component_is_unbound(struct cbqri_component *component,
				       u32 closid)
{
	struct cbqri_component_ctrl *entry;

	list_for_each_entry(entry, &component->controllers, list) {
		if (!cbqri_has_rcid_map(entry->ctrl) ||
		    entry->ctrl->req_to_int[closid])
			return false;
	}

	return true;
}

static void
cbqri_commit_cached_value(struct cbqri_resctrl_resource *resource,
			  struct cbqri_ctrl_domain *domain, u32 closid,
			  u32 value)
{
	u32 id;

	domain->ctrl_val[closid] = value;
	if (closid)
		return;

	for (id = 1; id < resource->num_closid; id++) {
		if (cbqri_component_is_unbound(domain->component, id))
			domain->ctrl_val[id] = value;
	}
}

static void
cbqri_commit_staged_domain(struct cbqri_resctrl_resource *resource,
			   struct rdt_ctrl_domain *rdt_domain, u32 closid)
{
	struct cbqri_ctrl_domain *domain;
	enum resctrl_conf_type type;

	domain = container_of(rdt_domain, struct cbqri_ctrl_domain, domain);
	for (type = 0; type < CDP_NUM_TYPES; type++) {
		struct resctrl_staged_config *cfg;

		cfg = &rdt_domain->staged_config[type];
		if (!cfg->have_new_ctrl)
			continue;
		cbqri_commit_cached_value(resource, domain, closid,
					  cfg->new_ctrl);
	}
}

static void cbqri_commit_cached_values(u32 closid)
{
	struct cbqri_resctrl_resource *resource;
	struct rdt_ctrl_domain *rdt_domain;
	struct resctrl_ctrl *control;
	int rid;

	for (rid = 0; rid < RDT_NUM_RESOURCES; rid++) {
		resource = &cbqri_resources[rid];
		if (!resource->res.alloc_capable ||
		    closid >= resource->num_closid)
			continue;

		for_each_enabled_ctrl(control, &resource->res) {
			list_for_each_entry(rdt_domain, &control->domains,
					    hdr.list) {
				cbqri_commit_staged_domain(resource, rdt_domain,
							   closid);
			}
		}
	}
}

static void cbqri_recover_failed_rollback(struct cbqri_controller *ctrl,
					  u32 closid)
{
	u32 req_rcid = closid;
	u32 int_rcid = 0;
	int ret;

	cbqri_quarantine_int_rcid(ctrl, ctrl->pending_int_rcid);
	if (ctrl->pending_old_int_rcid)
		cbqri_quarantine_int_rcid(ctrl, ctrl->pending_old_int_rcid);
	ret = cbqri_rcid_map_op(ctrl, CBQRI_RCID_MAP_OP_READ, &req_rcid,
				&int_rcid, NULL);
	if (!ret) {
		ctrl->req_to_int[closid] = int_rcid;
		if (int_rcid)
			cbqri_quarantine_int_rcid(ctrl, int_rcid);
	}
}

int resctrl_arch_commit_ctrl_update(u32 closid,
				    enum resctrl_update_reason reason)
{
	struct cbqri_controller *ctrl;
	u32 req_rcid, int_rcid;
	int rollback_ret;
	int ret;

	if (reason != RESCTRL_UPDATE_USER)
		return 0;

	list_for_each_entry(ctrl, &cbqri_hw_controllers, list) {
		if (!ctrl->pending_valid || ctrl->pending_req_rcid != closid)
			continue;
		req_rcid = closid;
		int_rcid = ctrl->pending_int_rcid;
		ret = cbqri_rcid_map_op(ctrl, CBQRI_RCID_MAP_OP_CONFIG,
					&req_rcid, &int_rcid, NULL);
		if (ret)
			goto rollback;
	}

	list_for_each_entry(ctrl, &cbqri_hw_controllers, list) {
		if (!ctrl->pending_valid || ctrl->pending_req_rcid != closid)
			continue;
		ctrl->req_to_int[closid] = ctrl->pending_int_rcid;
		if (ctrl->pending_old_int_rcid)
			__clear_bit(ctrl->pending_old_int_rcid,
				    ctrl->int_rcid_busy);
		cbqri_clear_pending(ctrl, false);
	}
	cbqri_commit_cached_values(closid);

	return 0;

rollback:
	list_for_each_entry(ctrl, &cbqri_hw_controllers, list) {
		if (!ctrl->pending_valid || ctrl->pending_req_rcid != closid)
			continue;
		req_rcid = closid;
		int_rcid = ctrl->pending_old_int_rcid;
		rollback_ret = cbqri_rcid_map_op(ctrl,
						 CBQRI_RCID_MAP_OP_CONFIG,
						 &req_rcid, &int_rcid, NULL);
		if (rollback_ret)
			cbqri_recover_failed_rollback(ctrl, closid);
		else
			__clear_bit(ctrl->pending_int_rcid,
				    ctrl->int_rcid_busy);
		cbqri_clear_pending(ctrl, false);
	}

	return ret;
}

void resctrl_arch_abort_ctrl_update(u32 closid,
				    enum resctrl_update_reason reason)
{
	if (reason == RESCTRL_UPDATE_USER)
		cbqri_discard_pending(closid);
}

int resctrl_arch_release_ctrl(u32 closid)
{
	struct cbqri_controller *ctrl;
	u32 req_rcid, int_rcid;
	int ret;

	if (!closid || closid >= CBQRI_LOGICAL_ID_COUNT)
		return closid ? -ERANGE : 0;

	list_for_each_entry(ctrl, &cbqri_hw_controllers, list) {
		if (!cbqri_supports_rcid_map(ctrl)) {
			u64 value;

			if (ctrl->info->type != CBQRI_CONTROLLER_TYPE_BANDWIDTH ||
			    closid >= ctrl->info->rcid_count)
				continue;
			value = ctrl->bc.supports_inactive_entry ? 0 : 1;
			ret = cbqri_bandwidth_op(ctrl,
						 CBQRI_BC_ALLOC_CTL_OP_READ_LIMIT,
						 closid);
			if (ret)
				return ret;
			cbqri_writeq(ctrl, CBQRI_BC_BW_ALLOC_OFF, value);
			ret = cbqri_bandwidth_op(ctrl,
						 CBQRI_BC_ALLOC_CTL_OP_CONFIG_LIMIT,
						 closid);
			if (ret)
				return ret;
			continue;
		}
		int_rcid = ctrl->req_to_int[closid];
		if (!int_rcid)
			continue;
		req_rcid = closid;
		int_rcid = 0;
		ret = cbqri_rcid_map_op(ctrl, CBQRI_RCID_MAP_OP_CONFIG,
					&req_rcid, &int_rcid, NULL);
		if (ret)
			return ret;
		int_rcid = ctrl->req_to_int[closid];
		ctrl->req_to_int[closid] = 0;
		__clear_bit(int_rcid, ctrl->int_rcid_busy);
		__clear_bit(int_rcid, ctrl->int_rcid_quarantined);
	}

	return 0;
}

int resctrl_arch_release_rmid(u32 closid, u32 rmid)
{
	struct cbqri_controller *ctrl;
	int ret;

	list_for_each_entry(ctrl, &cbqri_hw_controllers, list) {
		if (!ctrl->mon_capable)
			continue;
		if (!cbqri_supports_mcid_map(ctrl) &&
		    ctrl->info->type == CBQRI_CONTROLLER_TYPE_CAPACITY) {
			u64 occupancy;

			if (!test_bit(rmid, ctrl->mcid_assigned)) {
				ret = cbqri_config_event(ctrl, rmid,
							 CBQRI_CC_MON_EVT_OCCUPANCY);
				if (ret)
					return ret;
			}
			ret = cbqri_read_counter(ctrl, rmid,
						 QOS_L3_OCCUP_EVENT_ID, &occupancy);
			if (ret || occupancy)
				return -EAGAIN;
		}
		ret = cbqri_config_event(ctrl, rmid, CBQRI_MON_EVT_NONE);
		if (ret)
			return ret;
	}

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

static int cbqri_init_default_policies(void)
{
	struct rdt_ctrl_domain *domain;
	struct resctrl_ctrl *ctrl;
	struct rdt_resource *r;
	int rid, ret;

	for (rid = 0; rid < RDT_NUM_RESOURCES; rid++) {
		r = &cbqri_resources[rid].res;
		if (!r->alloc_capable)
			continue;
		for_each_enabled_ctrl(ctrl, r) {
			list_for_each_entry(domain, &ctrl->domains, hdr.list) {
				ret = resctrl_arch_update_one(r, ctrl, domain,
							      RESCTRL_RESERVED_CLOSID,
							      CDP_NONE,
							      resctrl_get_default_ctrlval(ctrl));
				if (ret)
					return ret;
			}
		}
	}

	return 0;
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
	ret = cbqri_init_default_policies();
	if (ret)
		goto out_remove_bandwidth;
	ret = cbqri_init_monitoring();
	if (ret)
		goto out_remove_bandwidth;
	ret = resctrl_init();
	if (ret)
		goto out_remove_monitoring;
	cpus_read_unlock();

	return 0;

out_remove_monitoring:
	cbqri_remove_monitoring();
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
	cbqri_remove_monitoring();
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
