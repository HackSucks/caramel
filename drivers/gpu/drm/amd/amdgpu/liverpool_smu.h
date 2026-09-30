// SPDX-License-Identifier: GPL-2.0-only
#ifndef LIVERPOOL_SMU_H
#define LIVERPOOL_SMU_H

struct amdgpu_device;

/* SMU7 mailbox message IDs (smu_7_0_1_d.h). Sent via liverpool_smu_cmd(). */
#define PPSMC_MSG_ForceHigh			0x42

#ifdef CONFIG_DRM_AMDGPU_CIK
int liverpool_smu_samu_read(struct amdgpu_device *adev, u32 reg, u32 *val);
int liverpool_smu_samu_write(struct amdgpu_device *adev, u32 reg, u32 val);
int liverpool_smu_cmd(struct amdgpu_device *adev, u32 msg, u32 arg);
int liverpool_smu_set_mempstate(struct amdgpu_device *adev, u32 idx);
int liverpool_smu_sysfs_init(struct amdgpu_device *adev);
#else
static inline int liverpool_smu_samu_read(struct amdgpu_device *adev, u32 reg,
					  u32 *val)
{ return -EIO; }
static inline int liverpool_smu_samu_write(struct amdgpu_device *adev, u32 reg,
					   u32 val)
{ return -EIO; }
static inline int liverpool_smu_cmd(struct amdgpu_device *adev, u32 msg,
				    u32 arg)
{ return -EIO; }
static inline int liverpool_smu_set_mempstate(struct amdgpu_device *adev,
					      u32 idx)
{ return -EINVAL; }
static inline int liverpool_smu_sysfs_init(struct amdgpu_device *adev)
{ return 0; }
#endif

#endif
