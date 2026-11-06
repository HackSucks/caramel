// SPDX-License-Identifier: GPL-2.0-only

#ifndef _PS4_RTC_H
#define _PS4_RTC_H

#include <linux/types.h>
#include <linux/ioctl.h>
#include <linux/pci_ids.h>

#define PS4_AEOLIA_VENDOR_ID        PCI_VENDOR_ID_SONY
#define PS4_AEOLIA_PCI_DEV_MISC_0   PCI_DEVICE_ID_SONY_AEOLIA_PCIE
#define PS4_AEOLIA_PCI_DEV_MISC_1   0x9090
#define PS4_AEOLIA_PCI_DEV_MISC_2   PCI_DEVICE_ID_SONY_AEOLIA_SDHCI

#define PS4_BELIZE_VENDOR_ID        PCI_VENDOR_ID_SONY
#define PS4_BELIZE_PCI_DEV_MISC_0   PCI_DEVICE_ID_SONY_BELIZE_PCIE
#define PS4_BELIZE_PCI_DEV_MISC_2   PCI_DEVICE_ID_SONY_BELIZE_SDHCI

#define PS4_RTC_REG_B0              0x130
#define PS4_RTC_REG_B1              0x134
#define PS4_RTC_REG_B2              0x138
#define PS4_RTC_REG_B3              0x13c

#define PS4_AEOLIA_RTC_REG_B0       PS4_RTC_REG_B0
#define PS4_AEOLIA_RTC_REG_B1       PS4_RTC_REG_B1
#define PS4_AEOLIA_RTC_REG_B2       PS4_RTC_REG_B2
#define PS4_AEOLIA_RTC_REG_B3       PS4_RTC_REG_B3

#define PS4_BELIZE_RTC_REG_B0       PS4_RTC_REG_B0
#define PS4_BELIZE_RTC_REG_B1       PS4_RTC_REG_B1
#define PS4_BELIZE_RTC_REG_B2       PS4_RTC_REG_B2
#define PS4_BELIZE_RTC_REG_B3       PS4_RTC_REG_B3

#define PS4_RTC_IOC_MAGIC           'R'
#define PS4_RTC_IOC_GET_TICKS       _IOR(PS4_RTC_IOC_MAGIC, 1, __u32)
#define PS4_RTC_IOC_GET_TIME        _IOR(PS4_RTC_IOC_MAGIC, 2, __u64)

u32 ps4_rtc_get_raw_ticks(void);
u64 ps4_rtc_get_time(void);

#endif
