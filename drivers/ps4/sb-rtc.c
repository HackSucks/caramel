// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/pci.h>
#include <linux/rtc.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/unaligned.h>

#include "ps4sb.h"

#define RTC_READ_RETRIES	21
#define RTC_RETRY_DELAY_US	100
#define RTC_STATUS_BATTERY_OK	BIT(8)
#define RTC_STATUS_CLOCK_OK	BIT(9)

#define RTC_NVS_REPLY_SIZE	0x302
#define RTC_NVS_KEY_A		(2 + 0x6c)
#define RTC_NVS_KEY_B		(2 + 0x1a8)
#define RTC_VALID_AFTER		1577836800LL

#define RTC_IOC_GET_TICKS	_IOR('R', 1, __u32)
#define RTC_IOC_GET_TIME	_IOR('R', 2, __u64)

struct ps4sb_rtc {
	struct ps4sb_dev *sc;
	void __iomem *regs;
	time64_t offset;
	bool valid;
};

static struct ps4sb_rtc ps4sb_rtc;

static u32 rtc_read_raw(struct ps4sb_rtc *rtc)
{
	u32 value = 0;
	int i;

	if (!rtc->sc->family->rtc_byte_regs)
		return ioread32(rtc->regs);

	for (i = 0; i < 4; i++)
		value |= (ioread32(rtc->regs + i * 4) & 0xff) << (i * 8);
	return value;
}

static int rtc_read_ticks(struct ps4sb_rtc *rtc, u32 *ticks)
{
	u32 previous = rtc_read_raw(rtc);
	u32 current_value;
	int retry;

	for (retry = 0; retry < RTC_READ_RETRIES; retry++) {
		udelay(RTC_RETRY_DELAY_US);
		current_value = rtc_read_raw(rtc);
		if (current_value == previous) {
			*ticks = current_value;
			return 0;
		}
		previous = current_value;
	}
	return -EIO;
}

static time64_t rtc_ticks_to_time(struct ps4sb_rtc *rtc, u32 ticks)
{
	return rtc->sc->family->rtc_epoch + ticks + rtc->offset;
}

static int ps4sb_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	u32 ticks;
	int ret;

	if (!ps4sb_rtc.valid)
		return -EINVAL;

	ret = rtc_read_ticks(&ps4sb_rtc, &ticks);
	if (ret)
		return ret;

	rtc_time64_to_tm(rtc_ticks_to_time(&ps4sb_rtc, ticks), tm);
	return 0;
}

static int ps4sb_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	u32 ticks;
	int ret;

	ret = rtc_read_ticks(&ps4sb_rtc, &ticks);
	if (ret)
		return ret;

	ps4sb_rtc.offset = rtc_tm_to_time64(tm) -
			   (ps4sb_rtc.sc->family->rtc_epoch + ticks);
	ps4sb_rtc.valid = true;
	return 0;
}

static const struct rtc_class_ops ps4sb_rtc_ops = {
	.read_time = ps4sb_rtc_read_time,
	.set_time = ps4sb_rtc_set_time,
};

static ssize_t rtc_dev_read(struct file *file, char __user *ubuf, size_t count,
			    loff_t *ppos)
{
	char buf[32];
	u32 ticks;
	int ret;

	ret = rtc_read_ticks(&ps4sb_rtc, &ticks);
	if (ret)
		return ret;

	ret = scnprintf(buf, sizeof(buf), "%u\n", ticks);
	return simple_read_from_buffer(ubuf, count, ppos, buf, ret);
}

static long rtc_dev_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	u32 ticks;
	u64 time;
	int ret;

	ret = rtc_read_ticks(&ps4sb_rtc, &ticks);
	if (ret)
		return ret;

	switch (cmd) {
	case RTC_IOC_GET_TICKS:
		return put_user(ticks, (u32 __user *)arg);
	case RTC_IOC_GET_TIME:
		time = rtc_ticks_to_time(&ps4sb_rtc, ticks);
		return put_user(time, (u64 __user *)arg);
	default:
		return -ENOTTY;
	}
}

static const struct file_operations rtc_dev_fops = {
	.owner		= THIS_MODULE,
	.read		= rtc_dev_read,
	.unlocked_ioctl	= rtc_dev_ioctl,
	.llseek		= noop_llseek,
};

static struct miscdevice rtc_miscdev = {
	.minor	= MISC_DYNAMIC_MINOR,
	.name	= "ps4rtc",
	.fops	= &rtc_dev_fops,
};

static s64 rtc_nvs_value(const u8 *reply, unsigned int pos)
{
	s64 value = get_unaligned_le64(reply + pos);

	return value == S64_MAX ? 0 : value;
}

static void rtc_load_nvs_offset(struct ps4sb_rtc *rtc, bool hw_ok)
{
	static const u8 request[] = { 0x00, 0x04, 0x00, 0x10, 0x00, 0x03 };
	u32 ticks;
	u8 *reply;
	int ret;

	reply = kzalloc(RTC_NVS_REPLY_SIZE, GFP_KERNEL);
	if (!reply)
		return;

	ret = ps4sb_icc_cmd(3, 1, request, sizeof(request), reply,
			    RTC_NVS_REPLY_SIZE);
	if (ret >= RTC_NVS_KEY_B + 8 && !rtc_read_ticks(rtc, &ticks)) {
		rtc->offset = rtc_nvs_value(reply, RTC_NVS_KEY_A) +
			      rtc_nvs_value(reply, RTC_NVS_KEY_B);
		rtc->valid = hw_ok &&
			     rtc_ticks_to_time(rtc, ticks) >= RTC_VALID_AFTER;
		dev_info(&rtc->sc->pdev->dev, "rtc: nvs offset %lld, %s\n",
			 rtc->offset, rtc->valid ? "valid" : "not valid");
	}
	kfree(reply);
}

int ps4sb_rtc_init(struct ps4sb_dev *sc)
{
	const struct ps4sb_family *f = sc->family;
	struct device *dev = &sc->pdev->dev;
	struct rtc_device *rtc_dev;
	bool hw_ok = true;
	int ret;

	if (ps4sb_rtc.sc)
		return -EBUSY;

	if (f->rtc_status) {
		u32 status = ioread32(ps4sb_bar(sc, f->rtc_bar) + f->rtc_status);

		if (!(status & RTC_STATUS_BATTERY_OK)) {
			dev_warn(dev, "rtc: battery failure indication\n");
			hw_ok = false;
		}
		if (!(status & RTC_STATUS_CLOCK_OK)) {
			dev_warn(dev, "rtc: clock failure indication\n");
			hw_ok = false;
		}
	}

	rtc_dev = devm_rtc_allocate_device(dev);
	if (IS_ERR(rtc_dev))
		return PTR_ERR(rtc_dev);

	ps4sb_rtc.sc = sc;
	ps4sb_rtc.regs = ps4sb_bar(sc, f->rtc_bar) + f->rtc_reg;
	rtc_load_nvs_offset(&ps4sb_rtc, hw_ok);
	rtc_dev->ops = &ps4sb_rtc_ops;

	ret = devm_rtc_register_device(rtc_dev);
	if (ret)
		return ret;

	return misc_register(&rtc_miscdev);
}
