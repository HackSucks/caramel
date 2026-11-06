#include <linux/delay.h>
#include <linux/firmware.h>

#include "amdgpu.h"
#include "amdgpu_vce.h"
#include "cikd.h"
#include "vce_liverpool.h"
#include "vce/vce_1_0_d.h"
#include "oss/oss_2_0_d.h"

static int vce_liverpool_stop(struct amdgpu_device *adev)
{
	u32 value, control;
	unsigned int i;

	if (!adev->vce.native_running)
		return 0;
	WREG32(mmVCE_SYS_INT_EN, RREG32(mmVCE_SYS_INT_EN) & ~8u);
	for (i = 0; i < 10000; i++) {
		value = RREG32(mmVCE_LMI_STATUS);
		if ((value & 15) == 15 && !(RREG32(mmVCE_STATUS) & 0x138))
			break;
		udelay(10);
	}
	if (i == 10000)
		goto timeout;
	WREG32(mmVCE_LMI_CTRL2, RREG32(mmVCE_LMI_CTRL2) | 0x100);
	for (i = 0; i < 10000; i++) {
		if ((RREG32(mmVCE_LMI_STATUS) & 0x240) == 0x240)
			break;
		udelay(10);
	}
	if (i == 10000)
		goto timeout;
	WREG32(mmVCE_SOFT_RESET, RREG32(mmVCE_SOFT_RESET) | 1);
	for (i = 0; i < 10000; i++) {
		if ((RREG32(mmVCE_LMI_STATUS) & 0x337f) == 0x337f)
			break;
		udelay(10);
	}
	if (i == 10000)
		goto timeout;
	control = RREG32(mmVCE_VCPU_CNTL) & ~1u;
	WREG32(mmVCE_STATUS, 0);
	WREG32(mmVCE_VCPU_CNTL, control);
	adev->vce.native_running = false;
	adev->vce.native_stop_failed = false;
	return 0;

timeout:
	adev->vce.native_stop_failed = true;
	dev_err(adev->dev, "VCE did not drain, retaining firmware allocation\n");
	return -ETIMEDOUT;
}

static int vce_liverpool_start(struct amdgpu_device *adev)
{
	struct amdgpu_ring *ring = &adev->vce.ring[0];
	u32 code_size = ALIGN(le32_to_cpu(((const struct common_firmware_header *)
		adev->vce.fw->data)->ucode_size_bytes), 256);
	unsigned int i;

	WREG32(mmVCE_SYS_INT_EN, RREG32(mmVCE_SYS_INT_EN) & ~8u);
	WREG32(mmVCE_SOFT_RESET, RREG32(mmVCE_SOFT_RESET) | 1);
	WREG32(mmVCE_STATUS, RREG32(mmVCE_STATUS) | 1);
	WREG32(mmSRBM_SOFT_RESET, RREG32(mmSRBM_SOFT_RESET) & ~0x01000000u);
	WREG32(mmVCE_CLOCK_GATING_A, RREG32(mmVCE_CLOCK_GATING_A) & ~0x10000u);
	WREG32(mmVCE_UENC_CLOCK_GATING,
		(RREG32(mmVCE_UENC_CLOCK_GATING) & 0xc0f81fff) | 0x7e000);
	WREG32(mmVCE_UENC_REG_CLOCK_GATING, RREG32(mmVCE_UENC_REG_CLOCK_GATING) | 0xfe);
	WREG32(mmVCE_CLOCK_GATING_B, RREG32(mmVCE_CLOCK_GATING_B) & ~0x200020u);
	WREG32(mmVCE_LMI_SWAP_CNTL, 0);
	WREG32(mmVCE_LMI_SWAP_CNTL1, 0);
	WREG32(mmVCE_LMI_VM_CTRL, 0);
	WREG32(mmVCE_LMI_CTRL, 0x200000);
	WREG32(mmVCE_LMI_CACHE_CTRL, RREG32(mmVCE_LMI_CACHE_CTRL) & ~1u);
	WREG32(0x83bb, 0);
	WREG32(mmVCE_LMI_VCPU_CACHE_40BIT_BAR, adev->vce.gpu_addr >> 8);
	WREG32(mmVCE_VCPU_CACHE_OFFSET0, 0);
	WREG32(mmVCE_VCPU_CACHE_SIZE0, code_size);
	WREG32(mmVCE_VCPU_CACHE_OFFSET1, code_size);
	WREG32(mmVCE_VCPU_CACHE_SIZE1, 0x40000);
	WREG32(mmVCE_VCPU_CACHE_OFFSET2, code_size + 0x40000);
	WREG32(mmVCE_VCPU_CACHE_SIZE2, 0x80000);
	WREG32(mmVCE_RB_BASE_LO, lower_32_bits(ring->gpu_addr));
	WREG32(mmVCE_RB_BASE_HI, upper_32_bits(ring->gpu_addr));
	WREG32(mmVCE_RB_SIZE, ring->ring_size / 4);
	WREG32(mmVCE_RB_RPTR, 0);
	WREG32(mmVCE_RB_WPTR, 0);
	ring->wptr = 0;
	WREG32(mmVCE_RB_BASE_LO2, lower_32_bits(adev->vce.gpu_addr + 0xcd000));
	WREG32(mmVCE_RB_BASE_HI2, upper_32_bits(adev->vce.gpu_addr + 0xcd000));
	WREG32(mmVCE_RB_SIZE2, 128);
	WREG32(mmVCE_RB_RPTR2, 0);
	WREG32(mmVCE_RB_WPTR2, 0);
	WREG32(mmVCE_LMI_CTRL2, RREG32(mmVCE_LMI_CTRL2) & ~0x100u);
	amdgpu_device_flush_hdp(adev, NULL);
	adev->vce.native_running = true;
	WREG32(mmVCE_VCPU_CNTL, RREG32(mmVCE_VCPU_CNTL) | 1);
	mdelay(10);
	WREG32(mmVCE_SOFT_RESET, RREG32(mmVCE_SOFT_RESET) & ~1u);
	for (i = 0; i < 1000; i++) {
		if (RREG32(mmVCE_STATUS) & 2)
			break;
		usleep_range(1000, 1100);
	}
	if (i == 1000)
		return vce_liverpool_stop(adev) ?: -ETIMEDOUT;
	WREG32(mmVCE_SYS_INT_EN, RREG32(mmVCE_SYS_INT_EN) | 8);
	WREG32(mmVCE_STATUS, RREG32(mmVCE_STATUS) & ~1u);
	return 0;
}

static uint64_t vce_liverpool_get_rptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	if (!adev->vce.native_running)
		return ring->wptr;
	return RREG32(mmVCE_RB_RPTR);
}

static uint64_t vce_liverpool_get_wptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	if (!adev->vce.native_running)
		return ring->wptr;
	return RREG32(mmVCE_RB_WPTR);
}

static void vce_liverpool_set_wptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	if (!adev->vce.native_running)
		return;
	amdgpu_device_flush_hdp(adev, NULL);
	WREG32(mmVCE_RB_WPTR, lower_32_bits(ring->wptr));
}

static const struct amdgpu_ring_funcs vce_liverpool_ring_funcs = {
	.type = AMDGPU_RING_TYPE_VCE,
	.align_mask = 0xf,
	.nop = VCE_CMD_NO_OP,
	.support_64bit_ptrs = false,
	.no_user_fence = true,
	.get_rptr = vce_liverpool_get_rptr,
	.get_wptr = vce_liverpool_get_wptr,
	.set_wptr = vce_liverpool_set_wptr,
	.parse_cs = amdgpu_vce_ring_parse_cs,
	.emit_frame_size = 6,
	.emit_ib_size = 4,
	.emit_ib = amdgpu_vce_ring_emit_ib,
	.emit_fence = amdgpu_vce_ring_emit_fence,
	.test_ring = amdgpu_vce_ring_test_ring,
	.test_ib = amdgpu_vce_ring_test_ib,
	.insert_nop = amdgpu_ring_insert_nop,
	.pad_ib = amdgpu_ring_generic_pad_ib,
};

static int vce_liverpool_set_irq(struct amdgpu_device *adev,
		struct amdgpu_irq_src *source, unsigned int type,
		enum amdgpu_interrupt_state state)
{
	if (!adev->vce.native_running)
		return 0;
	WREG32(mmVCE_SYS_INT_EN, (RREG32(mmVCE_SYS_INT_EN) & ~8u) |
		(state == AMDGPU_IRQ_STATE_ENABLE ? 8 : 0));
	return 0;
}

static int vce_liverpool_irq(struct amdgpu_device *adev,
		struct amdgpu_irq_src *source, struct amdgpu_iv_entry *entry)
{
	amdgpu_fence_process(&adev->vce.ring[0]);
	return 0;
}

static const struct amdgpu_irq_src_funcs vce_liverpool_irq_funcs = {
	.set = vce_liverpool_set_irq,
	.process = vce_liverpool_irq,
};

static int vce_liverpool_early_init(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	int r;

	if (!amdgpu_vce_is_native(adev))
		return -ENODEV;
	r = amdgpu_vce_early_init(adev);
	if (r)
		return r;
	adev->vce.num_rings = 1;
	adev->vce.ring[0].funcs = &vce_liverpool_ring_funcs;
	adev->vce.ring[0].me = 0;
	adev->vce.irq.num_types = 1;
	adev->vce.irq.funcs = &vce_liverpool_irq_funcs;
	return 0;
}

static int vce_liverpool_sw_init(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	struct amdgpu_ring *ring = &adev->vce.ring[0];
	int r;

	r = amdgpu_irq_add_id(adev, AMDGPU_IRQ_CLIENTID_LEGACY, 167, &adev->vce.irq);
	if (r)
		return r;
	r = amdgpu_vce_sw_init(adev, 0xd0000);
	if (r)
		return r;
	mutex_init(&adev->vce.native_mutex);
	adev->vce.native_workqueue = alloc_ordered_workqueue("vce-native", WQ_MEM_RECLAIM);
	if (!adev->vce.native_workqueue)
		return -ENOMEM;
	r = amdgpu_vce_resume(adev);
	if (r)
		return r;
	snprintf(ring->name, sizeof(ring->name), "vce0");
	return amdgpu_ring_init(adev, ring, 512, &adev->vce.irq, 0,
		AMDGPU_RING_PRIO_0, NULL);
}

static int vce_liverpool_hw_init(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	int r;

	r = vce_liverpool_start(adev);
	if (r)
		return r;
	r = amdgpu_ring_test_helper(&adev->vce.ring[0]);
	if (r) {
		vce_liverpool_stop(adev);
		return r;
	}
	dev_info(adev->dev, "VCE native firmware initialized\n");
	return 0;
}

static int vce_liverpool_hw_fini(struct amdgpu_ip_block *ip_block)
{
	int r;

	cancel_delayed_work_sync(&ip_block->adev->vce.idle_work);
	r = amdgpu_fence_wait_empty(&ip_block->adev->vce.ring[0]);
	if (r)
		return r;
	flush_workqueue(ip_block->adev->vce.native_workqueue);
	return vce_liverpool_stop(ip_block->adev);
}

static int vce_liverpool_sw_fini(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	int r;

	if (adev->vce.native_stop_failed)
		return -EBUSY;
	r = amdgpu_vce_suspend(adev);
	if (r)
		return r;
	destroy_workqueue(adev->vce.native_workqueue);
	mutex_destroy(&adev->vce.native_mutex);
	return amdgpu_vce_sw_fini(adev);
}

static int vce_liverpool_suspend(struct amdgpu_ip_block *ip_block)
{
	int r = amdgpu_vce_suspend(ip_block->adev);

	return r ?: vce_liverpool_hw_fini(ip_block);
}

static int vce_liverpool_resume(struct amdgpu_ip_block *ip_block)
{
	int r = amdgpu_vce_resume(ip_block->adev);

	return r ?: vce_liverpool_hw_init(ip_block);
}

static bool vce_liverpool_is_idle(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;

	if (!adev->vce.native_running)
		return true;
	return !(RREG32(mmVCE_STATUS) & 0x138) &&
		(RREG32(mmVCE_LMI_STATUS) & 15) == 15;
}

static int vce_liverpool_wait_idle(struct amdgpu_ip_block *ip_block)
{
	unsigned int i;

	for (i = 0; i < 10000; i++) {
		if (vce_liverpool_is_idle(ip_block))
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

static int vce_liverpool_clockgating(struct amdgpu_ip_block *ip_block,
		enum amd_clockgating_state state)
{
	return 0;
}

static int vce_liverpool_powergating(struct amdgpu_ip_block *ip_block,
		enum amd_powergating_state state)
{
	return 0;
}

static const struct amd_ip_funcs vce_liverpool_ip_funcs = {
	.name = "vce_liverpool",
	.early_init = vce_liverpool_early_init,
	.sw_init = vce_liverpool_sw_init,
	.sw_fini = vce_liverpool_sw_fini,
	.hw_init = vce_liverpool_hw_init,
	.hw_fini = vce_liverpool_hw_fini,
	.suspend = vce_liverpool_suspend,
	.resume = vce_liverpool_resume,
	.is_idle = vce_liverpool_is_idle,
	.wait_for_idle = vce_liverpool_wait_idle,
	.set_clockgating_state = vce_liverpool_clockgating,
	.set_powergating_state = vce_liverpool_powergating,
};

const struct amdgpu_ip_block_version vce_liverpool_ip_block = {
	.type = AMD_IP_BLOCK_TYPE_VCE,
	.major = 1,
	.minor = 0,
	.rev = 0,
	.funcs = &vce_liverpool_ip_funcs,
};
