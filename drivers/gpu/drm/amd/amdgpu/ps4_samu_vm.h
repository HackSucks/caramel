/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __PS4_SAMU_VM_H__
#define __PS4_SAMU_VM_H__

struct amdgpu_device;

#ifdef CONFIG_X86_PS4
int ps4_samu_vm_init(struct amdgpu_device *adev);
void ps4_samu_vm_fini(struct amdgpu_device *adev);
void ps4_samu_vm_enable(struct amdgpu_device *adev);
void ps4_samu_vm_fault(struct amdgpu_device *adev, u32 vmid, u32 page);
#else
static inline int ps4_samu_vm_init(struct amdgpu_device *adev)
{
	return 0;
}
static inline void ps4_samu_vm_fini(struct amdgpu_device *adev)
{
}
static inline void ps4_samu_vm_enable(struct amdgpu_device *adev)
{
}
static inline void ps4_samu_vm_fault(struct amdgpu_device *adev, u32 vmid,
				     u32 page)
{
}
#endif

#endif
