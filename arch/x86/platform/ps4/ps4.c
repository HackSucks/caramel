/*
 * ps4.c: Sony PS4 platform setup code
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; version 2
 * of the License.
 */

#define pr_fmt(fmt) "ps4: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/scatterlist.h>
#include <linux/irq.h>
#include <linux/module.h>
#include <linux/notifier.h>

#include <asm/setup.h>
#include <asm/mpspec_def.h>
#include <asm/hw_irq.h>
#include <asm/apic.h>
#include <asm/io_apic.h>
#include <asm/io.h>
#include <asm/i8259.h>
#include <asm/reboot.h>
#include <asm/msr.h>
#include <asm/pci-direct.h>
#include <asm/ps4.h>

static bool is_ps4;
static bool is_baikal;
bool ps4sb_initialized;

/*
 * The RTC is part of the Aeolia PCI device and will be implemented there as
 * an RTC class device; stub these out.
 */
static void dummy_get_wallclock(struct timespec64 *now)
{
	now->tv_sec = now->tv_nsec = 0;
}
static int dummy_set_wallclock(const struct timespec64 *now)
{
	return -ENODEV;
}

int ps4sb_status(void)
{
	if (!is_ps4)
		return -ENODEV;
	return READ_ONCE(ps4sb_initialized);
}
EXPORT_SYMBOL_GPL(ps4sb_status);

int ps4sb_assign_irqs(struct pci_dev *dev, int nvec)
{
	return is_baikal ? bpcie_assign_irqs(dev, nvec) :
			   apcie_assign_irqs(dev, nvec);
}
EXPORT_SYMBOL_GPL(ps4sb_assign_irqs);

void ps4sb_free_irqs(unsigned int virq, unsigned int nr_irqs)
{
	if (is_baikal)
		bpcie_free_irqs(virq, nr_irqs);
	else
		apcie_free_irqs(virq, nr_irqs);
}
EXPORT_SYMBOL_GPL(ps4sb_free_irqs);

bool ps4_is_baikal(void)
{
	return is_baikal;
}
EXPORT_SYMBOL_GPL(ps4_is_baikal);

static void ps4_emergency_restart(void)
{
	ps4sb_icc_reboot();
}

/*
 * PS4 specific x86_init function overrides and early setup calls.
 */
void __init x86_ps4_early_setup(void)
{
	pr_info("x86_ps4_early_setup: PS4 early setup\n");
	is_ps4 = true;
	is_baikal = read_pci_config_16(0, 0x14, 4, PCI_DEVICE_ID) ==
		    PCI_DEVICE_ID_SONY_BAIKAL_PCIE;
	pr_info("x86_ps4_early_setup: southbridge %s\n",
		is_baikal ? "Baikal" : "Aeolia or Belize");
	x86_platform.calibrate_tsc = ps4_calibrate_tsc;
	x86_platform.get_wallclock = dummy_get_wallclock;
	x86_platform.set_wallclock = dummy_set_wallclock;

	legacy_pic = &null_legacy_pic;
	machine_ops.emergency_restart = ps4_emergency_restart;
}
