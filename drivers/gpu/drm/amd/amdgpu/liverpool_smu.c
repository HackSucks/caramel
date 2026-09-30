// SPDX-License-Identifier: GPL-2.0-only
/*
 * PlayStation 4 SMU mailbox driver
 *
 * Based on the SMU7 mailbox protocol (smu_7_0_1_d.h).
 *
 * Mailbox commands go through the SAMU GCK proxy (GPU BAR 0x22070,
 * doorbell idx 0x32 / poll idx 0x4a). Raw SMC_IND access to the
 * 0xc2100000 mailbox powers a Baikal Pro off. This file does not
 * export an SMC_IND read/write. liverpool_clk.c uses
 * liverpool_smu_samu_read/write for the Gladius SPLL.
 */

#include <linux/delay.h>
#include <linux/io.h>
#include <linux/mutex.h>

#include "amdgpu.h"
#include "liverpool_smu.h"

#define LIVERPOOL_SMU_MSG_REG		0xc2100000
#define LIVERPOOL_SMU_RESP_REG		0xc2100004
#define LIVERPOOL_SMU_ARG_REG		0xc210003c

#define LIVERPOOL_SMU_RESP_READY	BIT(0)
#define LIVERPOOL_SMU_RESP_DONE		BIT(1)

#define LIVERPOOL_SMU_MSG_MASK		0xfffe0001
#define LIVERPOOL_SMU_MSG_VALID		BIT(0)

#define LIVERPOOL_SMU_ARG_TIMEOUT	(500 << 16)

#define LIVERPOOL_SMU_POLL_US		10
#define LIVERPOOL_SMU_POLL_ITER		100000

static const u8 liverpool_smu_mempstate_msg[] = {
	[0] = 0x58,
	[1] = 0x57,
	[2] = 0x59,
};

/* Orbis GCK helpers: byte offsets from GPU BAR, not SMC_IND. */
#define LIVERPOOL_SAMU_IDX		0x22000
#define LIVERPOOL_SAMU_DAT		0x22004
#define LIVERPOOL_SAMU_CMD		0x22070
#define LIVERPOOL_SAMU_ADDR		0x22074
#define LIVERPOOL_SAMU_DATA		0x22078
#define LIVERPOOL_SAMU_STAT		0x2207c
#define LIVERPOOL_SAMU_DOOR_IDX		0x32
#define LIVERPOOL_SAMU_POLL_IDX		0x4a
#define LIVERPOOL_SAMU_OP_WRITE		0xa505
#define LIVERPOOL_SAMU_OP_READ		0xa404
#define LIVERPOOL_SAMU_POLL_US		10
#define LIVERPOOL_SAMU_POLL_ITER	10000

static DEFINE_MUTEX(liverpool_samu_mutex);

static bool liverpool_samu_mapped(struct amdgpu_device *adev)
{
	return adev->rmmio && adev->rmmio_size >= 0x22080;
}

static void liverpool_samu_idx_w(struct amdgpu_device *adev, u32 idx, u32 val)
{
	writel(idx, adev->rmmio + LIVERPOOL_SAMU_IDX);
	writel(val, adev->rmmio + LIVERPOOL_SAMU_DAT);
}

static u32 liverpool_samu_idx_r(struct amdgpu_device *adev, u32 idx)
{
	writel(idx, adev->rmmio + LIVERPOOL_SAMU_IDX);
	return readl(adev->rmmio + LIVERPOOL_SAMU_DAT);
}

static int liverpool_samu_doorbell(struct amdgpu_device *adev)
{
	u32 i;

	liverpool_samu_idx_w(adev, LIVERPOOL_SAMU_DOOR_IDX, 1);
	for (i = 0; i < LIVERPOOL_SAMU_POLL_ITER; i++) {
		if (!(liverpool_samu_idx_r(adev, LIVERPOOL_SAMU_POLL_IDX) & 1))
			return 0;
		udelay(LIVERPOOL_SAMU_POLL_US);
	}
	return -ETIMEDOUT;
}

int liverpool_smu_samu_read(struct amdgpu_device *adev, u32 reg, u32 *val)
{
	u32 stat;
	int r;

	if (!liverpool_samu_mapped(adev) || !val)
		return -EIO;

	mutex_lock(&liverpool_samu_mutex);
	writel(LIVERPOOL_SAMU_OP_READ, adev->rmmio + LIVERPOOL_SAMU_CMD);
	writel(reg, adev->rmmio + LIVERPOOL_SAMU_ADDR);
	r = liverpool_samu_doorbell(adev);
	if (r) {
		mutex_unlock(&liverpool_samu_mutex);
		return r;
	}
	stat = readl(adev->rmmio + LIVERPOOL_SAMU_STAT);
	if (!stat)
		*val = readl(adev->rmmio + LIVERPOOL_SAMU_DATA);
	else
		*val = 0;
	mutex_unlock(&liverpool_samu_mutex);
	return stat ? -EIO : 0;
}

int liverpool_smu_samu_write(struct amdgpu_device *adev, u32 reg, u32 val)
{
	u32 stat;
	int r;

	if (!liverpool_samu_mapped(adev))
		return -EIO;

	mutex_lock(&liverpool_samu_mutex);
	writel(LIVERPOOL_SAMU_OP_WRITE, adev->rmmio + LIVERPOOL_SAMU_CMD);
	writel(reg, adev->rmmio + LIVERPOOL_SAMU_ADDR);
	writel(val, adev->rmmio + LIVERPOOL_SAMU_DATA);
	r = liverpool_samu_doorbell(adev);
	if (r) {
		mutex_unlock(&liverpool_samu_mutex);
		return r;
	}
	stat = readl(adev->rmmio + LIVERPOOL_SAMU_STAT);
	mutex_unlock(&liverpool_samu_mutex);
	return stat ? -EIO : 0;
}

/*
 * PS4 (ps4patches): route SMU mailbox commands through the SAMU proxy
 * (2026-09-19, ported from the 7.3-rc2 bfbbe3da worktree). This used to go
 * through RREG32_SMC/WREG32_SMC directly, i.e. mmSMC_IND_INDEX_0/DATA_0
 * unproxied -- confirmed via kernel.elf disassembly (cail_smc_ind_read/
 * write, see Kernel decomiple/SMU.md) that Sony's own kernel never issues
 * a raw SMU mailbox command that way; every access goes through this exact
 * SAMU command/doorbell proxy instead (liverpool_smu_samu_read/write,
 * already used correctly by every real access in liverpool_clk.c).
 * This is a known Baikal PS4 community issue: a direct SMU
 * read/write/command instantly powers off the console, behaving like a
 * tamper/protection circuit, not a power fault. This function is only
 * reachable via the diagnostic mempstate sysfs node (not automatically
 * invoked), but was still a live landmine as written.
 */
int liverpool_smu_cmd(struct amdgpu_device *adev, u32 msg, u32 arg)
{
	u32 resp, tmp;
	int i, r;

	for (i = 0; i < LIVERPOOL_SMU_POLL_ITER; i++) {
		r = liverpool_smu_samu_read(adev, LIVERPOOL_SMU_RESP_REG, &resp);
		if (r)
			return r;
		if (resp & LIVERPOOL_SMU_RESP_DONE)
			break;
		udelay(LIVERPOOL_SMU_POLL_US);
	}
	if (i == LIVERPOOL_SMU_POLL_ITER)
		return -ETIMEDOUT;

	r = liverpool_smu_samu_write(adev, LIVERPOOL_SMU_ARG_REG,
				      LIVERPOOL_SMU_ARG_TIMEOUT | arg);
	if (r)
		return r;

	r = liverpool_smu_samu_read(adev, LIVERPOOL_SMU_MSG_REG, &tmp);
	if (r)
		return r;
	tmp &= LIVERPOOL_SMU_MSG_MASK;
	tmp |= msg << 1;
	tmp ^= LIVERPOOL_SMU_MSG_VALID;
	r = liverpool_smu_samu_write(adev, LIVERPOOL_SMU_MSG_REG, tmp);
	if (r)
		return r;

	for (i = 0; i < LIVERPOOL_SMU_POLL_ITER; i++) {
		r = liverpool_smu_samu_read(adev, LIVERPOOL_SMU_RESP_REG, &resp);
		if (r)
			return r;
		if (resp & LIVERPOOL_SMU_RESP_READY)
			break;
		udelay(LIVERPOOL_SMU_POLL_US);
	}
	if (i == LIVERPOOL_SMU_POLL_ITER)
		return -ETIMEDOUT;

	for (i = 0; i < LIVERPOOL_SMU_POLL_ITER; i++) {
		r = liverpool_smu_samu_read(adev, LIVERPOOL_SMU_RESP_REG, &resp);
		if (r)
			return r;
		if (resp & LIVERPOOL_SMU_RESP_DONE)
			break;
		udelay(LIVERPOOL_SMU_POLL_US);
	}
	if (i == LIVERPOOL_SMU_POLL_ITER)
		return -ETIMEDOUT;

	return 0;
}

int liverpool_smu_set_mempstate(struct amdgpu_device *adev, u32 idx)
{
	int r;

	if (idx >= ARRAY_SIZE(liverpool_smu_mempstate_msg))
		return -EINVAL;

	if (idx != 1) {
		r = liverpool_smu_cmd(adev, liverpool_smu_mempstate_msg[1], 0);
		if (r)
			return r;
	}

	return liverpool_smu_cmd(adev, liverpool_smu_mempstate_msg[idx], 0);
}

static ssize_t liverpool_smu_mempstate_store(struct device *dev,
					     struct device_attribute *attr,
					     const char *buf, size_t count)
{
	struct amdgpu_device *adev =
		drm_to_adev(pci_get_drvdata(to_pci_dev(dev)));
	u32 idx;
	int r;

	r = kstrtou32(buf, 0, &idx);
	if (r)
		return r;

	r = liverpool_smu_set_mempstate(adev, idx);
	if (r)
		return r;

	return count;
}
static DEVICE_ATTR_WO(liverpool_smu_mempstate);

static ssize_t liverpool_smu_cmd_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct amdgpu_device *adev =
		drm_to_adev(pci_get_drvdata(to_pci_dev(dev)));
	u32 msg, arg;
	int r, n;

	n = sscanf(buf, "%u %u", &msg, &arg);
	if (n != 2)
		return -EINVAL;

	r = liverpool_smu_cmd(adev, msg, arg);
	if (r)
		return r;

	return count;
}
static DEVICE_ATTR_WO(liverpool_smu_cmd);

static struct attribute *liverpool_smu_attrs[] = {
	&dev_attr_liverpool_smu_cmd.attr,
	&dev_attr_liverpool_smu_mempstate.attr,
	NULL,
};

static const struct attribute_group liverpool_smu_attr_group = {
	.attrs = liverpool_smu_attrs,
};

int liverpool_smu_sysfs_init(struct amdgpu_device *adev)
{
	return devm_device_add_group(adev->dev, &liverpool_smu_attr_group);
}

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Armandas Kvietkus");
MODULE_DESCRIPTION("PlayStation 4 SMU mailbox driver");
