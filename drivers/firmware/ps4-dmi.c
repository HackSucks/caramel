// SPDX-License-Identifier: GPL-2.0-only

#include <linux/ctype.h>
#include <linux/dmi.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/string.h>

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

static bool __init ps4_dmi_date_ok(const char *date)
{
	int i;

	if (strlen(date) != 10)
		return false;
	for (i = 0; i < 10; i++) {
		if (i == 2 || i == 5) {
			if (date[i] != '/')
				return false;
		} else if (!isdigit(date[i])) {
			return false;
		}
	}
	return true;
}

void __init ps4_dmi_populate(const char *ident[DMI_STRING_MAX])
{
	int i;

	if (ident[DMI_BIOS_DATE] && !ps4_dmi_date_ok(ident[DMI_BIOS_DATE])) {
		pr_info("ps4_dmi: replacing the bad loader bios version and date\n");
		ident[DMI_BIOS_VERSION] = ps4_strings[DMI_BIOS_VERSION];
		ident[DMI_BIOS_DATE] = ps4_strings[DMI_BIOS_DATE];
	}

	pr_info("ps4_dmi: filling missing DMI fields with PS4 defaults\n");

	for (i = 0; i < DMI_STRING_MAX; i++) {
		if (ps4_strings[i] && !ident[i])
			ident[i] = ps4_strings[i];
	}
}
