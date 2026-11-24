// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2023-2024, Ventana Micro Systems Inc
 *	Author: Sunil V L <sunilvl@ventanamicro.com>
 */

#include <linux/acpi.h>
#include <linux/riscv_qos.h>
#include "init.h"

void __init acpi_arch_init(void)
{
	struct acpi_table_header *rqsc;
	acpi_status status;
	int ret;

	riscv_acpi_init_gsi_mapping();
	if (IS_ENABLED(CONFIG_ACPI_RIMT))
		riscv_acpi_rimt_init();

	if (!IS_ENABLED(CONFIG_RISCV_ISA_SSQOSID))
		return;

	status = acpi_get_table(ACPI_SIG_RQSC, 0, &rqsc);
	if (status == AE_NOT_FOUND)
		return;
	if (ACPI_FAILURE(status)) {
		pr_warn("Failed to get RQSC table: %s\n",
			acpi_format_exception(status));
		return;
	}

	ret = acpi_parse_rqsc(rqsc);
	if (ret)
		pr_warn("Failed to parse RQSC table: %d\n", ret);

	acpi_put_table(rqsc);
}
