/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __LINUX_RISCV_QOS_H
#define __LINUX_RISCV_QOS_H

#include <linux/cpumask.h>
#include <linux/list.h>
#include <linux/types.h>

enum cbqri_controller_type {
	CBQRI_CONTROLLER_TYPE_CAPACITY,
	CBQRI_CONTROLLER_TYPE_BANDWIDTH,
	CBQRI_CONTROLLER_TYPE_UNKNOWN,
};

enum cbqri_resource_type {
	CBQRI_RESOURCE_TYPE_CACHE,
	CBQRI_RESOURCE_TYPE_MEMORY,
};

/**
 * struct cbqri_resource_info - Resource managed by a CBQRI controller
 * @list: Entry in cbqri_controller_info::resources
 * @type: Cache or memory resource
 * @id_type: Type of ACPI locator stored in @id1 and @id2
 * @flags: Resource-specific RQSC flags
 * @id1: First ACPI locator value
 * @id2: Second ACPI locator value
 * @cache: Cache-resource topology
 * @cache.cache_level: Cache level of the resource
 * @cache.cache_id: PPTT cache ID of the resource
 * @cache.cpu_mask: CPUs sharing the cache resource
 * @mem: Memory-resource topology
 * @mem.prox_dom: SRAT proximity domain of the memory resource
 */
struct cbqri_resource_info {
	struct list_head		list;
	u8				type;
	u8				id_type;
	u16				flags;
	u64				id1;
	u32				id2;

	union {
		struct {
			u32		cache_level;
			u32		cache_id;
			cpumask_t	cpu_mask;
		} cache;
		struct {
			u32		prox_dom;
		} mem;
	};
};

/**
 * struct cbqri_controller_info - Firmware description of a CBQRI controller
 * @addr: Physical base address of the controller register interface
 * @size: Size of the controller register interface
 * @type: Capacity or bandwidth controller
 * @rcid_count: Number of physical allocation entries
 * @mcid_count: Number of physical monitoring counters
 * @list: Entry in the firmware-discovered controller list
 * @resources: RQSC resources managed by this controller
 */
struct cbqri_controller_info {
	u64				addr;
	u64				size;
	enum cbqri_controller_type	type;
	u32				rcid_count;
	u32				mcid_count;
	struct list_head		list;
	struct list_head		resources;
};

extern struct list_head cbqri_controllers;

int qos_resctrl_setup(void);
void qos_resctrl_exit(void);
int qos_resctrl_online_cpu(unsigned int cpu);
int qos_resctrl_offline_cpu(unsigned int cpu);

#endif /* __LINUX_RISCV_QOS_H */
