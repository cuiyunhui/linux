// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Tenstorrent
 *	Author: Drew Fustini <fustini@kernel.org>
 *
 */

#define pr_fmt(fmt) "ACPI: RQSC: " fmt

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/cpumask.h>
#include <linux/riscv_qos.h>
#include <linux/slab.h>
#include <linux/stddef.h>

#ifdef CONFIG_RISCV_ISA_SSQOSID

#define CBQRI_CTRL_SIZE 0x1000

static struct acpi_table_rqsc *acpi_get_rqsc(void)
{
	static struct acpi_table_header *rqsc;
	acpi_status status;

	/*
	 * RQSC will be used at runtime on every CPU, so we
	 * don't need to call acpi_put_table() to release the table mapping.
	 */
	if (!rqsc) {
		status = acpi_get_table(ACPI_SIG_RQSC, 0, &rqsc);
		if (ACPI_FAILURE(status)) {
			pr_warn_once("No RQSC table found\n");
			return NULL;
		}
	}

	return (struct acpi_table_rqsc *)rqsc;
}

static void rqsc_fill_cache_cpumask(struct cbqri_controller_info *ctrl_info)
{
	int err;

	/*
	 * For CBQRI, any cpu (technically a hart in RISC-V terms) can access the
	 * memory-mapped registers of any CBQRI controller in the system.
	 *
	 * Prefer deriving the mask from PPTT cache topology. If the cache cannot
	 * be found, fall back to allowing all possible CPUs.
	 */
	err = acpi_pptt_get_cpumask_from_cache_id(ctrl_info->cache.cache_id,
						  &ctrl_info->cache.cpu_mask);
	if (err)
		cpumask_copy(&ctrl_info->cache.cpu_mask, cpu_possible_mask);
}

static int rqsc_gas_get_addr(const u32 reg[3], u8 *space_id, u64 *addr)
{
	u32 reg0;

	/* reg[3] encodes a Generic Address Structure (GAS). */
	reg0 = reg[0];
	*space_id = reg0 & 0xff;
	*addr = ((u64)reg[2] << 32) | reg[1];

	if (*addr > ULONG_MAX)
		return -EOVERFLOW;

	return 0;
}

static int rqsc_init_ctrl_from_entry(struct cbqri_controller_info *ctrl_info,
				     const struct acpi_table_rqsc_fields *f,
				     phys_addr_t *phys)
{
	u64 base;
	u8 space_id;

	ctrl_info->type = f->type;
	ctrl_info->size = CBQRI_CTRL_SIZE;
	ctrl_info->rcid_count = (u32)f->rcid;
	ctrl_info->mcid_count = (u32)f->mcid;

	if (!ctrl_info->rcid_count && !ctrl_info->mcid_count)
		return -EINVAL;

	if (rqsc_gas_get_addr(f->reg, &space_id, &base))
		return -EOVERFLOW;

	*phys = (phys_addr_t)base;
	if (space_id != ACPI_ADR_SPACE_SYSTEM_MEMORY)
		return -EOPNOTSUPP;

	ctrl_info->addr = (unsigned long)base;
	return 0;
}

static int rqsc_controller_alloc_and_init(const struct acpi_table_rqsc_fields *f,
					  struct cbqri_controller_info **ctrl_info,
					 phys_addr_t *phys,
					 const char *caller)
{
	struct cbqri_controller_info *ci;
	int err;

	/*
	 * Return 0 even for skippable entries. Caller checks ctrl_info == NULL
	 * to distinguish skip from success.
	 */
	*ctrl_info = NULL;

	ci = kzalloc_obj(*ci, GFP_KERNEL);
	if (!ci)
		return -ENOMEM;

	err = rqsc_init_ctrl_from_entry(ci, f, phys);
	if (err == -EINVAL) {
		pr_warn("%s(): invalid controller entry: rcid=0 and mcid=0, skipping\n",
			caller);
		kfree(ci);
		return 0;
	}
	if (err == -EOPNOTSUPP) {
		pr_warn("%s(): unsupported GAS space_id=%u for ctrl base %pa\n",
			caller, f->reg[0] & 0xff, phys);
		kfree(ci);
		return 0;
	}
	if (err) {
		pr_warn("%s(): failed to parse controller entry (%d)\n", caller, err);
		kfree(ci);
		return err;
	}

	*ctrl_info = ci;
	return 0;
}

static int rqsc_parse_capacity(struct cbqri_controller_info *ctrl_info,
			       const struct acpi_table_rqsc_fields *f)
{
	u64 id1;
	struct acpi_pptt_cache *cache;

	id1 = f->res.id1;
	if (id1 > U32_MAX) {
		pr_warn("%s(): cache id1 too large: 0x%llx\n", __func__, id1);
		return -EINVAL;
	}

	ctrl_info->cache.cache_id = (u32)id1;
	ctrl_info->cache.cache_level =
		find_acpi_cache_level_from_id(ctrl_info->cache.cache_id);

	cache = find_acpi_cache_from_id(ctrl_info->cache.cache_id);
	if (cache) {
		ctrl_info->cache.cache_size = cache->size;
	} else {
		pr_warn("%s(): failed to determine size for cache id 0x%x\n",
			__func__, ctrl_info->cache.cache_id);
		ctrl_info->cache.cache_size = 0;
	}

	pr_debug("Cache controller has ID 0x%x level %u size %u\n",
		 ctrl_info->cache.cache_id, ctrl_info->cache.cache_level,
		 ctrl_info->cache.cache_size);
	rqsc_fill_cache_cpumask(ctrl_info);

	return 0;
}

static int rqsc_parse_bandwidth(struct cbqri_controller_info *ctrl_info,
				const struct acpi_table_rqsc_fields *f)
{
	u64 id1;

	id1 = f->res.id1;
	if (id1 > U32_MAX) {
		pr_warn("%s(): prox dom id1 too large: 0x%llx\n", __func__, id1);
		return -EINVAL;
	}

	ctrl_info->mem.prox_dom = (u32)id1;
	pr_debug("Memory controller with proximity domain %u\n",
		 ctrl_info->mem.prox_dom);

	return 0;
}

static int rqsc_controller_parse_type(struct cbqri_controller_info *ctrl_info,
				      const struct acpi_table_rqsc_fields *f,
				      bool *skip,
				      const char *caller)
{
	int err;

	*skip = false;

	switch (ctrl_info->type) {
	case CBQRI_CONTROLLER_TYPE_CAPACITY:
		err = rqsc_parse_capacity(ctrl_info, f);
		break;
	case CBQRI_CONTROLLER_TYPE_BANDWIDTH:
		err = rqsc_parse_bandwidth(ctrl_info, f);
		break;
	default:
		pr_warn("%s(): unknown controller type %u, skipping\n",
			caller, ctrl_info->type);
		*skip = true;
		return 0;
	}

	if (err)
		return err;

	return 0;
}

int acpi_parse_rqsc(struct acpi_table_header *table)
{
	struct acpi_table_rqsc *rqsc;
	LIST_HEAD(new_ctrls);
	struct cbqri_controller_info *ctrl_info, *tmp;
	phys_addr_t phys;
	u32 num;
	u32 i;
	int err;
	bool skip;
	int ret;
	unsigned int max_entries;
	u32 total_ctrl, cache_ctrl, bw_ctrl;

	if (WARN_ON_ONCE(acpi_disabled))
		return -ENODEV;

	ret = -EINVAL;
	if (!table) {
		rqsc = acpi_get_rqsc();
		if (!rqsc)
			return -ENOENT;
	} else {
		rqsc = (struct acpi_table_rqsc *)table;
	}

	if (rqsc->header.length < offsetof(struct acpi_table_rqsc, f)) {
		pr_warn("%s(): invalid RQSC table length: %u\n",
			__func__, rqsc->header.length);
		return -EINVAL;
	}

	max_entries = (rqsc->header.length - offsetof(struct acpi_table_rqsc, f)) /
		     sizeof(struct acpi_table_rqsc_fields);
	if (!max_entries) {
		pr_warn("%s(): invalid RQSC table length: %u\n",
			__func__, rqsc->header.length);
		return -EINVAL;
	}

	num = rqsc->num;
	if (num > max_entries) {
		pr_warn("RQSC num=%u exceeds table capacity %u, truncating\n",
			num, max_entries);
		num = max_entries;
	}

	total_ctrl = 0;
	cache_ctrl = 0;
	bw_ctrl = 0;

	for (i = 0; i < num; i++) {
		err = rqsc_controller_alloc_and_init(&rqsc->f[i], &ctrl_info, &phys,
						     __func__);
		if (err) {
			ret = err;
			goto out_free_new;
		}
		if (!ctrl_info)
			continue;

		pr_debug("Found controller type %u base %pa size 0x%lx rcid %u mcid %u\n",
			 ctrl_info->type, &phys, ctrl_info->size,
			ctrl_info->rcid_count, ctrl_info->mcid_count);

		err = rqsc_controller_parse_type(ctrl_info, &rqsc->f[i], &skip,
						 __func__);
		if (skip) {
			kfree(ctrl_info);
			continue;
		}
		if (err) {
			kfree(ctrl_info);
			ret = err;
			goto out_free_new;
		}

		total_ctrl++;
		if (ctrl_info->type == CBQRI_CONTROLLER_TYPE_CAPACITY)
			cache_ctrl++;
		else if (ctrl_info->type == CBQRI_CONTROLLER_TYPE_BANDWIDTH)
			bw_ctrl++;

		/* Fill the list shared with RISC-V QoS resctrl */
		INIT_LIST_HEAD(&ctrl_info->list);
		list_add_tail(&ctrl_info->list, &new_ctrls);
	}

	pr_info("RQSC controllers: total=%u cache=%u bw=%u\n",
		total_ctrl, cache_ctrl, bw_ctrl);

	list_splice_tail_init(&new_ctrls, &cbqri_controllers);
	return 0;

out_free_new:
	list_for_each_entry_safe(ctrl_info, tmp, &new_ctrls, list) {
		list_del(&ctrl_info->list);
		kfree(ctrl_info);
	}
	return ret;
}

#endif /* CONFIG_RISCV_ISA_SSQOSID */
