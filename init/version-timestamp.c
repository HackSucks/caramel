// SPDX-License-Identifier: GPL-2.0-only

#include <generated/compile.h>
#include <generated/utsrelease.h>
#include <linux/proc_ns.h>
#include <linux/refcount.h>
#include <linux/uts.h>
#include <linux/utsname.h>

struct uts_namespace init_uts_ns = {
	.ns = NS_COMMON_INIT(init_uts_ns),
	.name = {
		.sysname	= UTS_SYSNAME,
		.nodename	= UTS_NODENAME,
		.release	= UTS_RELEASE,
		.version	= UTS_VERSION,
		.machine	= UTS_MACHINE,
		.domainname	= UTS_DOMAINNAME,
	},
	.user_ns = &init_user_ns,
};

const char linux_banner[] =
	"Linux version " UTS_RELEASE " (" LINUX_COMPILE_BY "@"
	LINUX_COMPILE_HOST ") (" LINUX_COMPILER ") " UTS_VERSION "\n"
	"\n"
	"_________                                    .__\n"
	"\\_   ___ \\_____ ____________    _____   ____ |  |\n"
	"/    \\  \\/\\__  \\\\_  __ \\__  \\  /     \\_/ __ \\|  |\n"
	"\\     \\____/ __ \\|  | \\// __ \\|  Y Y  \\  ___/|  |__\n"
	" \\______  (____  /__|  (____  /__|_|  /\\___  >____/\n"
	"        \\/     \\/           \\/      \\/     \\/\n"
	"\n"
	"  Caramel PS4 Linux (based on Strawberry by rmux)\n"
	"  Platform  : Sony PlayStation 4\n"
	"  Release   : " UTS_RELEASE "\n"
	"  Builder   : " LINUX_COMPILE_BY "@" LINUX_COMPILE_HOST "\n"
	"  Toolchain : " LINUX_COMPILER "\n"
	"  Build     : " UTS_VERSION "\n"
	"\n"
	"  strawberry and caramel was here\n"
	"  also strawberry kernel team was here - rmux\n"
	"  i call it caramel because i like caramel - NVMe\n"
	"\n"
	"  Devs      : ArabPixel, feeRnt, bjhabibie\n"
	"  Coord     : dreams-of-formaldehyde, TheVorkMan, vanillitefroyo\n"
	"  GPU, BT   : manya4090, Comfy702\n"
	"  Baikal    : blyadimir., dewaardt, ss6530, isitnull., shyxuo, zarbok.,\n"
	"              vanixvanixvanix, jhuhblya, mechanical, kingabut, izanhower,\n"
	"              sgtxkitkat, rodrigo, sudofrontman, fluffysheep\n"
	"  Belize    : DanielG18, GattoDev-debug, angheloisfish, wondermine,\n"
	"              nadaman, razzle\n"
	"  Aeolia    : rmux, diema, lupusetflos\n"
	"  Logs      : erkki, Bbang, gryoza, fleur, TheGreekOne, felix_suicide,\n"
	"              tteons, Scrooge\n"
	"  Thanks    : syrianclippy (emotional support)\n"
	"  Updating and Performance Patches: NVMe\n"
	"  Maintainer: rmux (Strawberry), HackSucks/NVMe (Caramel)\n"
	"\n";
