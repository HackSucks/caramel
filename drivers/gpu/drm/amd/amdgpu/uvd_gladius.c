#include <linux/delay.h>
#include <linux/iopoll.h>

#include "amdgpu.h"
#include "amdgpu_uvd.h"
#include "uvd_gladius.h"
#include "uvd/uvd_5_0_d.h"
#include "oss/oss_2_0_d.h"

static void uvd_gladius_map(struct amdgpu_device *adev)
{
	u64 cache = adev->uvd.inst[0].gpu_addr;

	WREG32(0x3d3c, 0);
	WREG32(0x3d62, 0);
	WREG32(0x3d63, 0);
	WREG32_UVD_CTX(0x99, RREG32_UVD_CTX(0x99) & 0xff000000);
	WREG32_UVD_CTX(0x9a, 0);
	WREG32_UVD_CTX(0x162, RREG32_UVD_CTX(0x162) & 0xfff00000);
	WREG32(mmUVD_LMI_RBC_IB_VMID, 0);
	WREG32(mmUVD_LMI_RBC_RB_VMID, 0);
	WREG32(mmUVD_LMI_VCPU_CACHE_64BIT_BAR_LOW, lower_32_bits(cache));
	WREG32(mmUVD_LMI_VCPU_CACHE_64BIT_BAR_HIGH, upper_32_bits(cache));
	WREG32(0x3bec, lower_32_bits(cache));
	WREG32(0x3bed, upper_32_bits(cache));
	WREG32(0x3bf0, lower_32_bits(cache));
	WREG32(0x3bf1, upper_32_bits(cache));
}

int uvd_gladius_stop(struct amdgpu_device *adev)
{
	u32 value;
	int r;

	WREG32(mmUVD_MASTINT_EN, RREG32(mmUVD_MASTINT_EN) & ~6u);
	WREG32(mmUVD_STATUS, RREG32(mmUVD_STATUS) & ~4u);
	r = read_poll_timeout(RREG32, value, (value & 0xf) == 0xf,
		10, 100000, false, mmUVD_LMI_STATUS);
	if (r)
		goto failed;
	WREG32(mmUVD_RBC_RB_CNTL, RREG32(mmUVD_RBC_RB_CNTL) | 0x10010000);
	WREG32(mmUVD_RBC_RB_WPTR, 0);
	WREG32(mmUVD_RBC_RB_RPTR, 0);
	WREG32(mmUVD_LMI_CTRL2, RREG32(mmUVD_LMI_CTRL2) | 0x100);
	r = read_poll_timeout(RREG32, value, (value & 0x240) == 0x240,
		10, 100000, false, mmUVD_LMI_STATUS);
	if (r)
		goto failed;
	WREG32(mmUVD_VCPU_CNTL, RREG32(mmUVD_VCPU_CNTL) & ~0x210u);
	value = RREG32(mmUVD_SOFT_RESET);
	WREG32(mmUVD_SOFT_RESET, value | 0x2000);
	WREG32(mmUVD_SOFT_RESET, value | 0x2004);
	WREG32(mmUVD_SOFT_RESET, value | 0x200c);
	WREG32(mmUVD_STATUS, 0);
	if ((RREG32(mmUVD_VCPU_CNTL) & 0x200) ||
	    (RREG32(mmUVD_SOFT_RESET) & 0x200c) != 0x200c) {
		r = -EIO;
		goto failed;
	}
	r = read_poll_timeout(RREG32, value, (value & 0xf) == 0xf,
		10, 100000, false, mmUVD_LMI_STATUS);
	if (r)
		goto failed;
	adev->uvd.native_stop_failed = false;
	return 0;

failed:
	adev->uvd.native_stop_failed = true;
	dev_err(adev->dev, "Gladius UVD did not drain, retaining allocations (%d)\n", r);
	return r;
}

int uvd_gladius_start(struct amdgpu_device *adev)
{
	struct amdgpu_ring *ring = &adev->uvd.inst[0].ring;
	static const u32 config[] = {0x3bd4, 0x3bd5, 0x3bd3, 0x3992, 0x39c5, 0x3993};
	u32 value, control = 0x11010800 | order_base_2(ring->ring_size);
	unsigned int i;
	int r;

	if (adev->uvd.native_stop_failed)
		return -EBUSY;
	WREG32(mmSRBM_SOFT_RESET, RREG32(mmSRBM_SOFT_RESET) | 0x40000);
	usleep_range(5000, 5500);
	WREG32(mmSRBM_SOFT_RESET, RREG32(mmSRBM_SOFT_RESET) & ~0x40000u);
	WREG32(mmUVD_SOFT_RESET, RREG32(mmUVD_SOFT_RESET) | 0x200d);
	usleep_range(5000, 5500);
	WREG32(mmUVD_MASTINT_EN, RREG32(mmUVD_MASTINT_EN) & ~6u);
	WREG32(0x3d2c, RREG32(0x3d2c) & ~1u);
	WREG32(mmUVD_RBC_RB_CNTL, RREG32(mmUVD_RBC_RB_CNTL) | 0x10000);
	WREG32(mmUVD_LMI_CTRL2, RREG32(mmUVD_LMI_CTRL2) | 0x100);
	WREG32(mmUVD_STATUS, RREG32(mmUVD_STATUS) | 4);
	for (i = 0; i < ARRAY_SIZE(config); i++)
		WREG32(config[i], 0x02011002);
	WREG32(mmUVD_CGC_GATE, RREG32(mmUVD_CGC_GATE) & 0xfff00008);
	WREG32(mmUVD_SUVD_CGC_GATE, RREG32(mmUVD_SUVD_CGC_GATE) & 0xffffe000);
	WREG32(mmSRBM_SOFT_RESET, RREG32(mmSRBM_SOFT_RESET) & ~0x40000u);
	uvd_gladius_map(adev);
	WREG32(mmUVD_VCPU_CNTL, (RREG32(mmUVD_VCPU_CNTL) | 0x200) & ~0x40000u);
	WREG32(mmUVD_LMI_SWAP_CNTL, 0);
	WREG32(mmUVD_MP_SWAP_CNTL, 0);
	WREG32(0x3d68, 0);
	WREG32(mmUVD_LMI_CTRL, 0x203108);
	WREG32(mmUVD_MPC_CNTL, 0x10);
	WREG32(mmUVD_MPC_SET_MUXA0, 0x040c2040);
	WREG32(mmUVD_MPC_SET_MUXA1, 0);
	WREG32(mmUVD_MPC_SET_MUXB0, 0x040c2040);
	WREG32(mmUVD_MPC_SET_MUXB1, 0);
	WREG32(mmUVD_MPC_SET_ALU, 0);
	WREG32(mmUVD_MPC_SET_MUX, 0x88);
	WREG32(0x3d68, 0x3dff);
	uvd_gladius_map(adev);
	WREG32(mmUVD_VCPU_CACHE_OFFSET0, 0);
	WREG32(mmUVD_VCPU_CACHE_SIZE0, 0x7d000);
	WREG32(mmUVD_VCPU_CACHE_OFFSET1, 0x20fa00);
	WREG32(mmUVD_VCPU_CACHE_SIZE1, 0x40000);
	WREG32(mmUVD_VCPU_CACHE_OFFSET2, 0x417a00);
	WREG32(mmUVD_VCPU_CACHE_SIZE2, 0x120800);
	WREG32(mmUVD_RBC_RB_CNTL, control);
	WREG32(0x3dab, 1);
	WREG32(0x1401, RREG32(0x1401) | 8);
	WREG32(mmUVD_LMI_RBC_RB_64BIT_BAR_LOW, lower_32_bits(ring->gpu_addr));
	WREG32(mmUVD_LMI_RBC_RB_64BIT_BAR_HIGH, upper_32_bits(ring->gpu_addr));
	WREG32(mmUVD_RBC_RB_RPTR, 0);
	WREG32(mmUVD_RBC_RB_WPTR, 0);
	WREG32(mmUVD_RBC_RB_WPTR_CNTL, 0);
	WREG32(mmUVD_RBC_RB_RPTR_ADDR, upper_32_bits(ring->gpu_addr) >> 2);
	ring->wptr = 0;
	WREG32(mmUVD_RBC_RB_CNTL, control & 0xeffeffff);
	amdgpu_hdp_flush(adev, NULL);
	WREG32(0x501, 3);
	WREG32(mmUVD_SOFT_RESET, RREG32(mmUVD_SOFT_RESET) & ~1u);
	WREG32_UVD_CTX(0x9b, RREG32_UVD_CTX(0x9b) & ~0x10u);
	WREG32(0x3dac, 0x10);
	WREG32(mmUVD_LMI_CTRL2, RREG32(mmUVD_LMI_CTRL2) & ~0x100u);
	WREG32(mmUVD_SOFT_RESET, RREG32(mmUVD_SOFT_RESET) & ~4u);
	WREG32(mmUVD_SOFT_RESET, RREG32(mmUVD_SOFT_RESET) & ~8u);
	WREG32(mmUVD_SOFT_RESET, RREG32(mmUVD_SOFT_RESET) & ~0x2000u);
	usleep_range(16000, 17000);
	r = read_poll_timeout(RREG32, value, value & 2, 1000, 1000000, false, mmUVD_STATUS);
	if (r) {
		uvd_gladius_stop(adev);
		return r;
	}
	WREG32(0x501, 3);
	WREG32(mmUVD_MASTINT_EN, RREG32(mmUVD_MASTINT_EN) | 6);
	WREG32(mmUVD_STATUS, RREG32(mmUVD_STATUS) & ~4u);
	return 0;
}
