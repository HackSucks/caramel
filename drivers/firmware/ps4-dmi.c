// SPDX-License-Identifier: GPL-2.0-only

#include <linux/dmi.h>
#include <linux/init.h>
#include <linux/printk.h>

#include "ps4-dmi.h"

static const char * const ps4_strings[DMI_STRING_MAX] = {
	[DMI_BIOS_VENDOR]           = "Sony Interactive Entertainment Inc.",
	[DMI_BIOS_VERSION]          = "1.00",
	[DMI_BIOS_DATE]             = "11/15/2013",

	[DMI_SYS_VENDOR]            = "Sony Interactive Entertainment Inc.",
	[DMI_PRODUCT_NAME]          = "PlayStation 4",
	[DMI_PRODUCT_FAMILY]        = "PlayStation",

	[DMI_BOARD_VENDOR]          = "Sony Interactive Entertainment Inc.",
	[DMI_BOARD_NAME]            = "PlayStation 4",

	[DMI_CHASSIS_VENDOR]        = "Sony Interactive Entertainment Inc.",
	[DMI_CHASSIS_TYPE]          = "3",
};

void __init ps4_dmi_populate(const char *ident[DMI_STRING_MAX])
{
	int i;

	pr_info("ps4_dmi: filling missing DMI fields with PS4 defaults\n");

	for (i = 0; i < DMI_STRING_MAX; i++) {
		if (ps4_strings[i] && !ident[i])
			ident[i] = ps4_strings[i];
	}
}
