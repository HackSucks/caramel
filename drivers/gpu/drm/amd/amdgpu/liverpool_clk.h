/* SPDX-License-Identifier: MIT */
#ifndef LIVERPOOL_CLK_H
#define LIVERPOOL_CLK_H

struct amdgpu_device;

#ifdef CONFIG_DRM_AMDGPU_CIK
int liverpool_clk_force_max(struct amdgpu_device *adev);
int liverpool_clk_gladius_set_div(struct amdgpu_device *adev, unsigned int div);
#else
static inline int liverpool_clk_force_max(struct amdgpu_device *adev)
{ return 0; }
static inline int liverpool_clk_gladius_set_div(struct amdgpu_device *adev,
						unsigned int div)
{ return -EIO; }
#endif

#endif
