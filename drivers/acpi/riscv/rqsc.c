// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Tenstorrent
 * Author: Drew Fustini <fustini@kernel.org>
 */

#define pr_fmt(fmt) "ACPI: RQSC: " fmt

#include <linux/acpi.h>
#include <linux/list.h>
#include <linux/riscv_qos.h>
#include <linux/slab.h>

#define CBQRI_CTRL_SIZE			0x1000
#define RQSC_CTRL_HEADER_SIZE		offsetof(struct acpi_table_rqsc_fields, res)
#define RQSC_RESOURCE_HEADER_SIZE	sizeof(struct acpi_table_rqsc_fields_res)

#define RQSC_ID_PROCESSOR_CACHE		0
#define RQSC_ID_MEMORY_RANGE		1

LIST_HEAD(cbqri_controllers);

static void rqsc_free_controller(struct cbqri_controller_info *ctrl)
{
	struct cbqri_resource_info *resource, *tmp;

	list_for_each_entry_safe(resource, tmp, &ctrl->resources, list) {
		list_del(&resource->list);
		kfree(resource);
	}
	kfree(ctrl);
}

static int rqsc_parse_cache_resource(struct cbqri_resource_info *resource)
{
	cpumask_t *cpu_mask = &resource->cache.cpu_mask;
	u64 id = resource->id1;
	u32 cache_id;
	int level;
	int ret;

	if (resource->type != CBQRI_RESOURCE_TYPE_CACHE || id > U32_MAX)
		return -EINVAL;

	cache_id = id;
	resource->cache.cache_id = cache_id;
	level = find_acpi_cache_level_from_id(cache_id);
	if (level < 0)
		return level;
	resource->cache.cache_level = level;

	ret = acpi_pptt_get_cpumask_from_cache_id(cache_id, cpu_mask);
	if (ret)
		return ret;

	return 0;
}

static int rqsc_parse_memory_resource(struct cbqri_resource_info *resource)
{
	u64 prox_dom = resource->id1;

	if (resource->type != CBQRI_RESOURCE_TYPE_MEMORY || prox_dom > U32_MAX)
		return -EINVAL;

	resource->mem.prox_dom = prox_dom;
	return 0;
}

static int rqsc_add_resource(struct cbqri_controller_info *ctrl,
			     const struct acpi_table_rqsc_fields_res *entry)
{
	struct cbqri_resource_info *resource;
	int ret;

	resource = kzalloc_obj(*resource, GFP_KERNEL);
	if (!resource)
		return -ENOMEM;

	resource->type = entry->type;
	resource->id_type = entry->id_type;
	resource->flags = entry->flags;
	resource->id1 = entry->id1;
	resource->id2 = entry->id2;

	switch (resource->id_type) {
	case RQSC_ID_PROCESSOR_CACHE:
		ret = rqsc_parse_cache_resource(resource);
		break;
	case RQSC_ID_MEMORY_RANGE:
		ret = rqsc_parse_memory_resource(resource);
		break;
	default:
		ret = -EOPNOTSUPP;
		break;
	}
	if (ret) {
		kfree(resource);
		return ret;
	}

	INIT_LIST_HEAD(&resource->list);
	list_add_tail(&resource->list, &ctrl->resources);
	return 0;
}

static int rqsc_parse_resources(struct cbqri_controller_info *ctrl,
				const struct acpi_table_rqsc_fields *entry)
{
	const u8 *end = (const u8 *)entry + entry->length;
	const u8 *pos = (const u8 *)&entry->res;
	const struct acpi_table_rqsc_fields_res *resource;
	u16 i;
	int ret;

	for (i = 0; i < entry->nres; i++) {
		if (end - pos < RQSC_RESOURCE_HEADER_SIZE)
			return -EINVAL;

		resource = (const void *)pos;
		if (resource->length < RQSC_RESOURCE_HEADER_SIZE ||
		    resource->length > end - pos)
			return -EINVAL;

		ret = rqsc_add_resource(ctrl, resource);
		if (ret == -EOPNOTSUPP) {
			pr_debug("Ignoring unsupported resource ID type %u\n",
				 resource->id_type);
		} else if (ret) {
			return ret;
		}

		pos += resource->length;
	}

	return list_empty(&ctrl->resources) ? -EOPNOTSUPP : 0;
}

static int rqsc_parse_address(const struct acpi_table_rqsc_fields *entry,
			      u64 *address)
{
	struct acpi_generic_address gas;

	memcpy(&gas, entry->reg, sizeof(gas));
	if (gas.space_id != ACPI_ADR_SPACE_SYSTEM_MEMORY)
		return -EOPNOTSUPP;

	*address = gas.address;
	return 0;
}

static int rqsc_add_controller(const struct acpi_table_rqsc_fields *entry,
			       struct list_head *controllers)
{
	struct cbqri_controller_info *ctrl;
	int ret;

	if (entry->type > CBQRI_CONTROLLER_TYPE_BANDWIDTH)
		return -EOPNOTSUPP;
	if (!entry->rcid && !entry->mcid)
		return -EINVAL;

	ctrl = kzalloc_obj(*ctrl, GFP_KERNEL);
	if (!ctrl)
		return -ENOMEM;

	INIT_LIST_HEAD(&ctrl->resources);
	ctrl->type = entry->type;
	ctrl->size = CBQRI_CTRL_SIZE;
	ctrl->rcid_count = entry->rcid;
	ctrl->mcid_count = entry->mcid;

	ret = rqsc_parse_address(entry, &ctrl->addr);
	if (ret)
		goto out_free;

	ret = rqsc_parse_resources(ctrl, entry);
	if (ret)
		goto out_free;

	INIT_LIST_HEAD(&ctrl->list);
	list_add_tail(&ctrl->list, controllers);
	return 0;

out_free:
	rqsc_free_controller(ctrl);
	return ret;
}

int acpi_parse_rqsc(struct acpi_table_header *table)
{
	struct acpi_table_rqsc *rqsc = (struct acpi_table_rqsc *)table;
	struct cbqri_controller_info *ctrl, *tmp;
	LIST_HEAD(controllers);
	const u8 *end;
	const u8 *pos;
	u32 i;
	int ret;

	if (!table || table->length < offsetof(struct acpi_table_rqsc, f))
		return -EINVAL;

	pos = (const u8 *)&rqsc->f[0];
	end = (const u8 *)table + table->length;

	for (i = 0; i < rqsc->num; i++) {
		const struct acpi_table_rqsc_fields *entry;

		if (end - pos < RQSC_CTRL_HEADER_SIZE) {
			ret = -EINVAL;
			goto out_free;
		}

		entry = (const void *)pos;
		if (entry->length < RQSC_CTRL_HEADER_SIZE ||
		    entry->length > end - pos) {
			ret = -EINVAL;
			goto out_free;
		}

		ret = rqsc_add_controller(entry, &controllers);
		if (ret == -EOPNOTSUPP) {
			pr_debug("Ignoring unsupported controller type %u\n",
				 entry->type);
		} else if (ret) {
			goto out_free;
		}

		pos += entry->length;
	}

	list_splice_tail_init(&controllers, &cbqri_controllers);
	return 0;

out_free:
	list_for_each_entry_safe(ctrl, tmp, &controllers, list) {
		list_del(&ctrl->list);
		rqsc_free_controller(ctrl);
	}
	return ret;
}
