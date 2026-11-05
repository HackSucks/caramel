// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/pci.h>
#include <linux/rtc.h>

#include "baikal.h"

#define BPCIE_RTC_STATUS		0x50cc
#define BPCIE_RTC_SECONDS		0x50d0
#define BPCIE_RTC_STATUS_BATTERY_OK	BIT(8)
#define BPCIE_RTC_STATUS_CLOCK_OK	BIT(9)
#define BPCIE_RTC_EPOCH			1325376000LL
#define BPCIE_RTC_READ_RETRIES		21
#define BPCIE_RTC_RETRY_DELAY_US	100

struct bpcie_rtc {
	struct bpcie_dev *sc;
	time64_t offset;
};

static struct bpcie_rtc bpcie_rtc;

static int bpcie_rtc_read_seconds(struct bpcie_dev *sc, u32 *seconds)
{
	u32 previous, current_value;
	int retry;

	previous = ioread32(sc->bar4 + BPCIE_RTC_SECONDS);
	for (retry = 0; retry < BPCIE_RTC_READ_RETRIES; retry++) {
		udelay(BPCIE_RTC_RETRY_DELAY_US);
		current_value = ioread32(sc->bar4 + BPCIE_RTC_SECONDS);
		if (current_value == previous) {
			*seconds = current_value;
			return 0;
		}
		previous = current_value;
	}
	return -EIO;
}

static int bpcie_rtc_read_time(struct device *dev, struct rtc_time *tm)
{
	struct bpcie_rtc *rtc = &bpcie_rtc;
	u32 seconds;
	int ret;

	ret = bpcie_rtc_read_seconds(rtc->sc, &seconds);
	if (ret)
		return ret;

	rtc_time64_to_tm(BPCIE_RTC_EPOCH + seconds + rtc->offset, tm);
	return 0;
}

static int bpcie_rtc_set_time(struct device *dev, struct rtc_time *tm)
{
	struct bpcie_rtc *rtc = &bpcie_rtc;
	u32 seconds;
	int ret;

	ret = bpcie_rtc_read_seconds(rtc->sc, &seconds);
	if (ret)
		return ret;

	rtc->offset = rtc_tm_to_time64(tm) - (BPCIE_RTC_EPOCH + seconds);
	return 0;
}

static const struct rtc_class_ops bpcie_rtc_ops = {
	.read_time = bpcie_rtc_read_time,
	.set_time = bpcie_rtc_set_time,
};

int bpcie_rtc_init(struct bpcie_dev *sc)
{
	struct device *dev = &sc->pdev->dev;
	struct rtc_device *rtc_dev;
	u32 status;

	status = ioread32(sc->bar4 + BPCIE_RTC_STATUS);
	if (!(status & BPCIE_RTC_STATUS_BATTERY_OK))
		dev_warn(dev, "rtc: battery failure indication\n");
	if (!(status & BPCIE_RTC_STATUS_CLOCK_OK))
		dev_warn(dev, "rtc: clock failure indication\n");

	if (bpcie_rtc.sc)
		return -EBUSY;

	rtc_dev = devm_rtc_allocate_device(dev);
	if (IS_ERR(rtc_dev))
		return PTR_ERR(rtc_dev);

	bpcie_rtc.sc = sc;
	bpcie_rtc.offset = 0;
	rtc_dev->ops = &bpcie_rtc_ops;

	return devm_rtc_register_device(rtc_dev);
}
