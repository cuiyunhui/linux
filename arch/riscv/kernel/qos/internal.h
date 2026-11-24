/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _RISCV_QOS_INTERNAL_H
#define _RISCV_QOS_INTERNAL_H

#include <linux/bits.h>
#include <linux/list.h>
#include <linux/resctrl.h>
#include <linux/riscv_qos.h>
#include <linux/types.h>

#define CBQRI_CC_CAPABILITIES_OFF	0x00
#define CBQRI_CC_MON_CTL_OFF		0x08
#define CBQRI_CC_MON_CTR_VAL_OFF	0x10
#define CBQRI_CC_ALLOC_CTL_OFF		0x18
#define CBQRI_CC_BLOCK_MASK_OFF		0x20

#define CBQRI_BC_CAPABILITIES_OFF	0x00
#define CBQRI_BC_MON_CTL_OFF		0x08
#define CBQRI_BC_MON_CTR_VAL_OFF	0x10
#define CBQRI_BC_ALLOC_CTL_OFF		0x18
#define CBQRI_BC_BW_ALLOC_OFF		0x20
#define CBQRI_BC_RCID_MAP_CTL_OFF	0x28

#define CBQRI_CAPABILITIES_VER_MINOR_MASK	GENMASK(3, 0)
#define CBQRI_CAPABILITIES_VER_MAJOR_MASK	GENMASK(7, 4)

#define CBQRI_CC_CAPABILITIES_NCBLKS_SHIFT	8
#define CBQRI_CC_CAPABILITIES_NCBLKS_MASK	GENMASK(15, 0)
#define CBQRI_CC_CAPABILITIES_FRCID_SHIFT	24
#define CBQRI_CC_CAPABILITIES_FRCID_MASK		BIT(0)

#define CBQRI_BC_CAPABILITIES_NBWBLKS_SHIFT	8
#define CBQRI_BC_CAPABILITIES_NBWBLKS_MASK	GENMASK(15, 0)
#define CBQRI_BC_CAPABILITIES_MRBWB_SHIFT	32
#define CBQRI_BC_CAPABILITIES_MRBWB_MASK		GENMASK(15, 0)

#define CBQRI_CAPABILITIES_RCID_MAP_SHIFT	62
#define CBQRI_CAPABILITIES_MCID_MAP_SHIFT	63
#define CBQRI_ID_MAP_VERSION			0x12
#define CBQRI_LOGICAL_ID_COUNT			4096

#define CBQRI_CTL_OP_SHIFT		0
#define CBQRI_CTL_OP_MASK		GENMASK(4, 0)
#define CBQRI_CTL_AT_SHIFT		5
#define CBQRI_CTL_AT_MASK		GENMASK(2, 0)
#define CBQRI_CTL_RCID_SHIFT		8
#define CBQRI_CTL_RCID_MASK		GENMASK(11, 0)
#define CBQRI_CTL_STATUS_SHIFT		32
#define CBQRI_CTL_STATUS_MASK		GENMASK(6, 0)
#define CBQRI_CTL_BUSY_SHIFT		39
#define CBQRI_CTL_BUSY_MASK		BIT(0)

#define CBQRI_RCID_MAP_INT_RCID_SHIFT	20
#define CBQRI_RCID_MAP_INT_RCID_MASK	GENMASK(11, 0)
#define CBQRI_RCID_MAP_VALID_SHIFT	40

#define CBQRI_MON_CTL_EVT_ID_SHIFT	20
#define CBQRI_MON_CTL_EVT_ID_MASK	GENMASK(7, 0)
#define CBQRI_MON_CTL_ATV_SHIFT		28
#define CBQRI_MON_CTL_ATV_MASK		BIT(0)

#define CBQRI_MON_EVT_NONE		0
#define CBQRI_CC_MON_EVT_OCCUPANCY	1
#define CBQRI_BC_MON_EVT_RDWR_COUNT	1

#define CBQRI_AT_DATA			0
#define CBQRI_AT_CODE			1

#define CBQRI_RESOURCE_ID_PROCESSOR_CACHE	0
#define CBQRI_RESOURCE_ID_MEMORY_RANGE		1

#define CBQRI_MON_CTL_OP_CONFIG_EVENT		1
#define CBQRI_MON_CTL_OP_READ_COUNTER		2
#define CBQRI_MON_CTL_STATUS_SUCCESS		1
#define CBQRI_MON_CTL_STATUS_NO_COUNTER		6

#define CBQRI_CC_MON_CTL_OP_CONFIG_EVENT	1
#define CBQRI_CC_MON_CTL_OP_READ_COUNTER	2
#define CBQRI_CC_MON_CTL_STATUS_SUCCESS		1

#define CBQRI_BC_MON_CTL_OP_CONFIG_EVENT	1
#define CBQRI_BC_MON_CTL_OP_READ_COUNTER	2
#define CBQRI_BC_MON_CTL_STATUS_SUCCESS		1

#define CBQRI_CC_ALLOC_CTL_OP_CONFIG_LIMIT	1
#define CBQRI_CC_ALLOC_CTL_OP_READ_LIMIT	2
#define CBQRI_CC_ALLOC_CTL_OP_FLUSH_RCID	3
#define CBQRI_CC_ALLOC_CTL_STATUS_SUCCESS	1

#define CBQRI_BC_ALLOC_CTL_OP_CONFIG_LIMIT	1
#define CBQRI_BC_ALLOC_CTL_OP_READ_LIMIT	2
#define CBQRI_BC_ALLOC_CTL_STATUS_SUCCESS	1

#define CBQRI_RCID_MAP_OP_CONFIG		1
#define CBQRI_RCID_MAP_OP_READ			2
#define CBQRI_RCID_MAP_OP_READ_ENTRY		3

#define CBQRI_RCID_MAP_STATUS_SUCCESS		1
#define CBQRI_RCID_MAP_STATUS_INVALID_OP		2
#define CBQRI_RCID_MAP_STATUS_INVALID_REQ_RCID	3
#define CBQRI_RCID_MAP_STATUS_INVALID_INT_RCID	4
#define CBQRI_RCID_MAP_STATUS_INT_RCID_BUSY	5
#define CBQRI_RCID_MAP_STATUS_CONSTRAINT		6

#define CBQRI_BC_BW_ALLOC_RBWB_SHIFT	0
#define CBQRI_BC_BW_ALLOC_RBWB_MASK	GENMASK(15, 0)
#define CBQRI_BC_BW_ALLOC_MWEIGHT_SHIFT	20
#define CBQRI_BC_BW_ALLOC_MWEIGHT_MASK	GENMASK(7, 0)
#define CBQRI_BC_BW_ALLOC_SHARED_AT_SHIFT	28
#define CBQRI_BC_BW_ALLOC_SHARED_AT_MASK	GENMASK(2, 0)
#define CBQRI_BC_BW_ALLOC_USE_SHARED_SHIFT	31
#define CBQRI_BC_BW_ALLOC_USE_SHARED_MASK	BIT(0)

#define CBQRI_CC_MON_CTR_INV_BIT	63
#define CBQRI_BC_MON_CTR_INV_BIT	62
#define CBQRI_BC_MON_CTR_OVF_BIT	63

struct riscv_cbqri_capacity_caps {
	u16 ncblks;

	bool supports_alloc_at_data;
	bool supports_alloc_at_code;
	bool supports_alloc_op_config_limit;
	bool supports_alloc_op_read_limit;
	bool supports_alloc_op_flush_rcid;

	bool supports_mon_at_data;
	bool supports_mon_at_code;
	bool supports_mon_op_config_event;
	bool supports_mon_op_read_counter;
	bool supports_mon_evt_id_none;
	bool supports_mon_evt_id_occupancy;
	bool supports_rcid_map;
	bool supports_mcid_map;
};

struct riscv_cbqri_bandwidth_caps {
	u16 nbwblks;
	u16 mrbwb;

	bool supports_alloc_at_data;
	bool supports_alloc_at_code;
	bool supports_alloc_op_config_limit;
	bool supports_alloc_op_read_limit;

	bool supports_mon_at_data;
	bool supports_mon_at_code;
	bool supports_mon_op_config_event;
	bool supports_mon_op_read_counter;
	bool supports_mon_evt_id_none;
	bool supports_mon_evt_id_rdwr_count;
	bool supports_mon_evt_id_rdonly_count;
	bool supports_mon_evt_id_wronly_count;
	bool supports_rcid_map;
	bool supports_mcid_map;
};

struct cbqri_controller {
	struct list_head			list;
	struct cbqri_controller_info	*info;
	void __iomem			*base;
	u8				version_major;
	u8				version_minor;

	union {
		struct riscv_cbqri_capacity_caps	cc;
		struct riscv_cbqri_bandwidth_caps	bc;
	};

	u16				*req_to_int;
	unsigned long			*int_rcid_busy;
	unsigned long			*int_rcid_quarantined;
	u16				pending_req_rcid;
	u16				pending_int_rcid;
	u16				pending_old_int_rcid;
	bool				pending_valid;
	bool				mon_capable;
};

enum cbqri_ctrl_kind {
	CBQRI_CTRL_CAPACITY,
	CBQRI_CTRL_BANDWIDTH_RESERVED,
	CBQRI_CTRL_BANDWIDTH_WEIGHT,
};

struct cbqri_component_ctrl {
	struct list_head		list;
	struct cbqri_controller	*ctrl;
};

struct cbqri_component {
	struct list_head	controllers;
	struct list_head	list;
	cpumask_t		cpu_mask;
	u32			id;
	u32			level;
};

struct cbqri_resctrl_ctrl {
	struct resctrl_ctrl	r_ctrl;
	enum cbqri_ctrl_kind	kind;
};

struct cbqri_resctrl_resource {
	struct rdt_resource		res;
	struct cbqri_resctrl_ctrl	capacity_ctrl;
	struct cbqri_resctrl_ctrl	reserved_ctrl;
	struct cbqri_resctrl_ctrl	weight_ctrl;
	struct list_head		components;
	u32				num_closid;
};

struct cbqri_ctrl_domain {
	struct rdt_ctrl_domain	domain;
	struct cbqri_component	*component;
	struct cbqri_resctrl_ctrl *ctrl;
	u32			*ctrl_val;
};

extern struct list_head cbqri_hw_controllers;

int cbqri_probe_controllers(void);
void cbqri_remove_controllers(void);
int cbqri_init_capacity_controls(void);
void cbqri_remove_capacity_controls(void);
int cbqri_capacity_set(struct cbqri_ctrl_domain *domain, u32 closid,
		       enum resctrl_conf_type type, u32 value,
		       enum resctrl_update_reason reason);
int cbqri_capacity_get(struct cbqri_ctrl_domain *domain, u32 closid,
		       enum resctrl_conf_type type, u32 *value);
int cbqri_init_bandwidth_controls(void);
void cbqri_remove_bandwidth_controls(void);
int cbqri_bandwidth_set(struct cbqri_ctrl_domain *domain,
			struct cbqri_resctrl_ctrl *ctrl, u32 closid, u32 value,
			enum resctrl_update_reason reason);
int cbqri_bandwidth_get(struct cbqri_ctrl_domain *domain,
			struct cbqri_resctrl_ctrl *ctrl, u32 closid, u32 *value);

#endif /* _RISCV_QOS_INTERNAL_H */
