// SPDX-License-Identifier: GPL-2.0-only

#define pr_fmt(fmt) "ps4-rtc: " fmt

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/uaccess.h>
#include <linux/rtc.h>

#include "ps4-rtc.h"

#define PS4_RTC_MAX_RETRIES         10
#define PS4_RTC_STABILIZE_US        100

static unsigned long long rtc_offset = 1431543102ULL;
module_param(rtc_offset, ullong, 0644);

struct ps4_rtc_dev {
	struct pci_dev *pdev;
	void __iomem *mmio;
	struct miscdevice misc;
	struct rtc_device *rtc_dev;
};

static struct ps4_rtc_dev *g_ps4_rtc;

static u32 ps4_rtc_read_raw_unprotected(void __iomem *mmio)
{
	u32 b0 = ioread32(mmio + PS4_RTC_REG_B0) & 0xff;
	u32 b1 = ioread32(mmio + PS4_RTC_REG_B1) & 0xff;
	u32 b2 = ioread32(mmio + PS4_RTC_REG_B2) & 0xff;
	u32 b3 = ioread32(mmio + PS4_RTC_REG_B3) & 0xff;

	return b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
}

static u32 ps4_rtc_read_stable(void __iomem *mmio)
{
	u32 val1, val2;
	int retry;

	for (retry = 0; retry < PS4_RTC_MAX_RETRIES; retry++) {
		val1 = ps4_rtc_read_raw_unprotected(mmio);
		udelay(PS4_RTC_STABILIZE_US);
		val2 = ps4_rtc_read_raw_unprotected(mmio);

		if (val1 == val2)
			return val1;
	}

	pr_warn_ratelimited("read instability: %08x != %08x\n", val1, val2);
	return val2;
}

u32 ps4_rtc_get_raw_ticks(void)
{
	if (!g_ps4_rtc || !g_ps4_rtc->mmio)
		return 0;

	return ps4_rtc_read_stable(g_ps4_rtc->mmio);
}
EXPORT_SYMBOL_GPL(ps4_rtc_get_raw_ticks);

u64 ps4_rtc_get_time(void)
{
	u32 ticks = ps4_rtc_get_raw_ticks();

	if (!ticks)
		return 0;

	return (u64)((int64_t)ticks - rtc_offset);
}
EXPORT_SYMBOL_GPL(ps4_rtc_get_time);

static int ps4_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct ps4_rtc_dev *rtc = g_ps4_rtc;
	u32 raw;
	time64_t t;

	if (!rtc || !rtc->mmio)
		return -ENODEV;

	raw = ps4_rtc_read_stable(rtc->mmio);
	t = (time64_t)raw - rtc_offset;
	rtc_time64_to_tm(t, tm);
	return 0;
}

static int ps4_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct ps4_rtc_dev *rtc = g_ps4_rtc;
	u32 raw;
	time64_t t;

	if (!rtc || !rtc->mmio)
		return -ENODEV;

	raw = ps4_rtc_read_stable(rtc->mmio);
	t = rtc_tm_to_time64(tm);
	rtc_offset = (unsigned long long)((int64_t)raw - t);
	return 0;
}

static const struct rtc_class_ops ps4_rtc_ops = {
	.read_time = ps4_rtc_read_time,
	.set_time  = ps4_rtc_set_time,
};

static ssize_t ps4_rtc_dev_read(struct file *filp, char __user *ubuf, size_t count, loff_t *ppos)
{
	struct ps4_rtc_dev *rtc = filp->private_data;
	char buf[32];
	u32 ticks;
	int len;

	if (*ppos > 0)
		return 0;

	ticks = ps4_rtc_read_stable(rtc->mmio);
	len = scnprintf(buf, sizeof(buf), "%u\n", ticks);

	if (copy_to_user(ubuf, buf, len))
		return -EFAULT;

	*ppos += len;
	return len;
}

static long ps4_rtc_dev_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct ps4_rtc_dev *rtc = filp->private_data;
	u32 ticks;
	u64 t;

	switch (cmd) {
	case PS4_RTC_IOC_GET_TICKS:
		ticks = ps4_rtc_read_stable(rtc->mmio);
		if (copy_to_user((u32 __user *)arg, &ticks, sizeof(ticks)))
			return -EFAULT;
		return 0;
	case PS4_RTC_IOC_GET_TIME:
		ticks = ps4_rtc_read_stable(rtc->mmio);
		t = (u64)((int64_t)ticks - rtc_offset);
		if (copy_to_user((u64 __user *)arg, &t, sizeof(t)))
			return -EFAULT;
		return 0;
	default:
		return -ENOTTY;
	}
}

static int ps4_rtc_dev_open(struct inode *inode, struct file *filp)
{
	if (!g_ps4_rtc)
		return -ENODEV;

	filp->private_data = g_ps4_rtc;
	return 0;
}

static const struct file_operations ps4_rtc_fops = {
	.owner          = THIS_MODULE,
	.open           = ps4_rtc_dev_open,
	.read           = ps4_rtc_dev_read,
	.unlocked_ioctl = ps4_rtc_dev_ioctl,
	.llseek         = noop_llseek,
};

static const struct {
	u16 device;
	const char *name;
} ps4_rtc_sb_devices[] = {
	{ PS4_AEOLIA_PCI_DEV_MISC_0, "Aeolia" },
	{ PS4_BELIZE_PCI_DEV_MISC_0, "Belize" },
	{ PS4_AEOLIA_PCI_DEV_MISC_2, "Aeolia" },
	{ PS4_BELIZE_PCI_DEV_MISC_2, "Belize" },
	{ PS4_AEOLIA_PCI_DEV_MISC_1, "Aeolia" },
};

static int __init ps4_rtc_init(void)
{
	struct ps4_rtc_dev *rtc;
	struct pci_dev *pdev = NULL;
	const char *sb_name = "unknown";
	u32 initial_ticks;
	int ret, i;

	for (i = 0; i < ARRAY_SIZE(ps4_rtc_sb_devices); i++) {
		pdev = pci_get_device(PS4_AEOLIA_VENDOR_ID, ps4_rtc_sb_devices[i].device, NULL);
		if (pdev) {
			sb_name = ps4_rtc_sb_devices[i].name;
			break;
		}
	}

	if (!pdev) {
		pr_err("Southbridge device not found\n");
		return -ENODEV;
	}

	rtc = kzalloc(sizeof(*rtc), GFP_KERNEL);
	if (!rtc) {
		pci_dev_put(pdev);
		return -ENOMEM;
	}

	rtc->pdev = pdev;
	rtc->mmio = pci_iomap(pdev, 2, 0x1000);
	if (!rtc->mmio) {
		pr_err("failed to map %s Southbridge BAR2\n", sb_name);
		ret = -ENOMEM;
		goto err_free;
	}

	rtc->misc.minor = MISC_DYNAMIC_MINOR;
	rtc->misc.name  = "ps4rtc";
	rtc->misc.fops  = &ps4_rtc_fops;
	ret = misc_register(&rtc->misc);
	if (ret) {
		pr_err("failed to register misc device\n");
		goto err_unmap;
	}

	g_ps4_rtc = rtc;

	rtc->rtc_dev = devm_rtc_allocate_device(&pdev->dev);
	if (!IS_ERR(rtc->rtc_dev)) {
		rtc->rtc_dev->ops = &ps4_rtc_ops;
		ret = devm_rtc_register_device(rtc->rtc_dev);
		if (ret)
			pr_warn("failed to register with Linux RTC core: %d\n", ret);
	}

	initial_ticks = ps4_rtc_read_stable(rtc->mmio);

	pr_info("%s Southbridge hardware RTC initialized (PCI %04x:%04x)\n",
		sb_name, pdev->vendor, pdev->device);
	pr_info("Current hardware RTC raw ticks: %u (0x%08x)\n",
		initial_ticks, initial_ticks);
	pr_info("Userspace character device registered (/dev/ps4rtc)\n");
	return 0;

err_unmap:
	g_ps4_rtc = NULL;
	pci_iounmap(pdev, rtc->mmio);
err_free:
	pci_dev_put(pdev);
	kfree(rtc);
	return ret;
}

static void __exit ps4_rtc_exit(void)
{
	struct ps4_rtc_dev *rtc = g_ps4_rtc;

	if (!rtc)
		return;

	misc_deregister(&rtc->misc);
	pci_iounmap(rtc->pdev, rtc->mmio);
	pci_dev_put(rtc->pdev);
	kfree(rtc);
	g_ps4_rtc = NULL;
}

module_init(ps4_rtc_init);
module_exit(ps4_rtc_exit);

MODULE_AUTHOR("Armandas Kvietkus <armandas.kvietkus@proton.me>");
MODULE_DESCRIPTION("PS4 Aeolia/Belize Southbridge Hardware Real-Time Clock Driver");
MODULE_LICENSE("GPL");
