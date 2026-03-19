// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2025 Tenstorrent
 *	Author: Drew Fustini <fustini@kernel.org>
 *
 */

#define pr_fmt(fmt) "ACPI: RQSC: " fmt

#include <linux/acpi.h>
#include <linux/bits.h>
#include <linux/riscv_qos.h>
#include <linux/slab.h>

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

int acpi_parse_rqsc(struct acpi_table_header *table)
{
	struct acpi_table_rqsc *rqsc;
	LIST_HEAD(new_ctrls);
	struct cbqri_controller_info *ctrl_info, *tmp;
	u32 num;
	u32 i;

	if (WARN_ON_ONCE(acpi_disabled))
		return -ENODEV;
	if (!table) {
		rqsc = acpi_get_rqsc();
		if (!rqsc)
			return -ENOENT;
	} else {
		rqsc = (struct acpi_table_rqsc *)table;
	}

	num = rqsc->num;
	if (num > ARRAY_SIZE(rqsc->f)) {
		pr_warn("RQSC num=%u exceeds table capacity %zu, truncating\n",
			num, ARRAY_SIZE(rqsc->f));
		num = ARRAY_SIZE(rqsc->f);
	}

	for (i = 0; i < num; i++) {
		u64 base;
		u32 size;
		u64 id1;
		struct acpi_pptt_cache *cache;

		ctrl_info = kzalloc(sizeof(*ctrl_info), GFP_KERNEL);
		if (!ctrl_info)
			goto out_free_new_nomem;

		ctrl_info->type = rqsc->f[i].type;
		base = ((u64)rqsc->f[i].reg[1] << 32) | rqsc->f[i].reg[0];
		size = rqsc->f[i].reg[2];
		if (!size)
			size = CBQRI_CTRL_SIZE;
		if (base > ULONG_MAX) {
			pr_warn("%s(): ctrl base too large: 0x%llx", __func__, base);
			kfree(ctrl_info);
			goto out_free_new;
		}
		ctrl_info->addr = (unsigned long)base;
		ctrl_info->size = size;
		ctrl_info->rcid_count = rqsc->f[i].rcid;
		ctrl_info->mcid_count = rqsc->f[i].mcid;

		pr_info("Found controller with type %u addr 0x%lx size  %lu rcid  %u mcid  %u",
			ctrl_info->type, ctrl_info->addr, ctrl_info->size,
			ctrl_info->rcid_count, ctrl_info->mcid_count);

		if (ctrl_info->type == CBQRI_CONTROLLER_TYPE_CAPACITY) {
			id1 = rqsc->f[i].res.id1;
			if (id1 > U32_MAX) {
				pr_warn("%s(): cache id1 too large: 0x%llx",
					__func__, id1);
				kfree(ctrl_info);
				goto out_free_new;
			}

			ctrl_info->cache.cache_id = (u32)id1;
			ctrl_info->cache.cache_level =
				find_acpi_cache_level_from_id(ctrl_info->cache.cache_id);

			cache = find_acpi_cache_from_id(ctrl_info->cache.cache_id);
			if (cache) {
				ctrl_info->cache.cache_size = cache->size;
			} else {
				pr_warn("%s(): failed to determine size for cache id 0x%x",
					__func__, ctrl_info->cache.cache_id);
				ctrl_info->cache.cache_size = 0;
			}

			pr_info("Cache controller has ID 0x%x level %u size %u ",
				ctrl_info->cache.cache_id, ctrl_info->cache.cache_level,
				ctrl_info->cache.cache_size);

			/*
			 * For CBQRI, any cpu (technically a hart in RISC-V terms)
			 * can access the memory-mapped registers of any CBQRI
			 * controller in the system.
			 */
			cpumask_copy(&ctrl_info->cache.cpu_mask, cpu_possible_mask);

		} else if (ctrl_info->type == CBQRI_CONTROLLER_TYPE_BANDWIDTH) {
			id1 = rqsc->f[i].res.id1;
			if (id1 > U32_MAX) {
				pr_warn("%s(): prox dom id1 too large: 0x%llx",
					__func__, id1);
				kfree(ctrl_info);
				goto out_free_new;
			}
			ctrl_info->mem.prox_dom = (u32)id1;
			pr_info("Memory controller with proximity domain %u",
				ctrl_info->mem.prox_dom);
		} else {
			pr_warn("%s(): unknown controller type %u, skipping",
				__func__, ctrl_info->type);
			kfree(ctrl_info);
			continue;
		}

		/* Fill the list shared with RISC-V QoS resctrl */
		INIT_LIST_HEAD(&ctrl_info->list);
		list_add_tail(&ctrl_info->list, &new_ctrls);
	}

	list_splice_tail_init(&new_ctrls, &cbqri_controllers);
	return 0;

out_free_new:
	list_for_each_entry_safe(ctrl_info, tmp, &new_ctrls, list) {
		list_del(&ctrl_info->list);
		kfree(ctrl_info);
	}
	return -EINVAL;

out_free_new_nomem:
	list_for_each_entry_safe(ctrl_info, tmp, &new_ctrls, list) {
		list_del(&ctrl_info->list);
		kfree(ctrl_info);
	}
	return -ENOMEM;
}

#endif /* CONFIG_RISCV_ISA_SSQOSID */
