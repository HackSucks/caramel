#include <linux/delay.h>
#include <linux/firmware.h>

#include "amdgpu.h"
#include "amdgpu_vce.h"
#include "cikd.h"
#include "vce_gladius.h"
#include <linux/iopoll.h>
#include "oss/oss_2_0_d.h"

static u32 vce_gladius_read(struct amdgpu_device *adev, u32 index)
{
	unsigned long flags;
	u32 previous, value;

	spin_lock_irqsave(&adev->mmio_idx_lock, flags);
	previous = readl(adev->rmmio);
	writel(index * 4, adev->rmmio);
	value = readl(adev->rmmio + 4);
	writel(previous, adev->rmmio);
	spin_unlock_irqrestore(&adev->mmio_idx_lock, flags);
	return value;
}

static void vce_gladius_write(struct amdgpu_device *adev, u32 index, u32 value)
{
	unsigned long flags;
	u32 previous;

	spin_lock_irqsave(&adev->mmio_idx_lock, flags);
	previous = readl(adev->rmmio);
	writel(index * 4, adev->rmmio);
	writel(value, adev->rmmio + 4);
	readl(adev->rmmio + 4);
	writel(previous, adev->rmmio);
	spin_unlock_irqrestore(&adev->mmio_idx_lock, flags);
}

static int vce_gladius_stop(struct amdgpu_device *adev)
{
	u32 value;
	int r;

	if (!adev->vce.native_running)
		return 0;
	vce_gladius_write(adev, 0x8540, vce_gladius_read(adev, 0x8540) & ~8u);
	r = read_poll_timeout(vce_gladius_read, value, (value & 0x3fff) == 0x337f,
		10, 200000, false, adev, 0x85a7);
	if (r)
		goto timeout;
	r = read_poll_timeout(vce_gladius_read, value, !(value & 0x3c),
		10, 200000, false, adev, 0x8001);
	if (r)
		goto timeout;
	vce_gladius_write(adev, 0x8005, vce_gladius_read(adev, 0x8005) & ~1u);
	vce_gladius_write(adev, 0x8048, 0xfffff);
	vce_gladius_write(adev, 0x8048, 1);
	vce_gladius_write(adev, 0x8001, 0);
	if ((vce_gladius_read(adev, 0x8005) & 1) ||
	    !(vce_gladius_read(adev, 0x8048) & 1) ||
	    (vce_gladius_read(adev, 0x85a7) & 0x3fff) != 0x337f) {
		r = -EIO;
		goto timeout;
	}
	adev->vce.native_running = false;
	adev->vce.native_stop_failed = false;
	return 0;

timeout:
	adev->vce.native_stop_failed = true;
	dev_err(adev->dev, "Gladius VCE did not drain, retaining allocations (%d)\n", r);
	return r;
}

static int vce_gladius_start(struct amdgpu_device *adev)
{
	struct amdgpu_ring *ring = &adev->vce.ring[0];
	u64 base = adev->vce.gpu_addr;
	u32 value;
	int r;

	if (adev->vce.native_stop_failed)
		return -EBUSY;
	if (base >= (1ULL << 40) || 0xe5000 > (1ULL << 40) - base ||
	    ring->gpu_addr >= (1ULL << 40) ||
	    ring->ring_size > (1ULL << 40) - ring->gpu_addr)
		return -ERANGE;
	adev->vce.native_running = true;
	vce_gladius_write(adev, 0xf802, vce_gladius_read(adev, 0xf802) | 5);
	vce_gladius_write(adev, 0x398, vce_gladius_read(adev, 0x398) | 0x1000000);
	usleep_range(5000, 5500);
	vce_gladius_write(adev, 0x398, vce_gladius_read(adev, 0x398) & ~0x1000000u);
	vce_gladius_write(adev, 0x8048, vce_gladius_read(adev, 0x8048) | 1);
	usleep_range(5000, 5500);
	vce_gladius_write(adev, 0x8540, vce_gladius_read(adev, 0x8540) & ~8u);
	vce_gladius_write(adev, 0x85a8, 0);
	vce_gladius_write(adev, 0x85bb, 0x1111);
	vce_gladius_write(adev, 0x8597, base >> 8);
	vce_gladius_write(adev, 0x8009, 0);
	vce_gladius_write(adev, 0x800a, 0x24b00);
	vce_gladius_write(adev, 0x800b, 0x24b00);
	vce_gladius_write(adev, 0x800c, 0x40000);
	vce_gladius_write(adev, 0x800d, 0x64b00);
	vce_gladius_write(adev, 0x800e, 0x80000);
	vce_gladius_write(adev, 0x8060, lower_32_bits(ring->gpu_addr));
	vce_gladius_write(adev, 0x8061, upper_32_bits(ring->gpu_addr));
	vce_gladius_write(adev, 0x8062, ring->ring_size / 4);
	vce_gladius_write(adev, 0x8064, 0);
	vce_gladius_write(adev, 0x8063, 0);
	vce_gladius_write(adev, 0x805b, lower_32_bits(base + 0xe4d00));
	vce_gladius_write(adev, 0x805c, upper_32_bits(base + 0xe4d00));
	vce_gladius_write(adev, 0x805d, 128);
	vce_gladius_write(adev, 0x805f, 0);
	vce_gladius_write(adev, 0x805e, 0);
	ring->wptr = 0;
	vce_gladius_write(adev, 0x809f, vce_gladius_read(adev, 0x809f) | 0x10000);
	vce_gladius_write(adev, 0x8001, vce_gladius_read(adev, 0x8001) | 1);
	vce_gladius_write(adev, 0x8392, 0);
	vce_gladius_write(adev, 0x8393, 0);
	vce_gladius_write(adev, 0x83a5, 0);
	vce_gladius_write(adev, 0x83a6, 0);
	vce_gladius_write(adev, 0x85ad, 0);
	vce_gladius_write(adev, 0x85ae, 0);
	vce_gladius_write(adev, 0x85a8, 0);
	vce_gladius_write(adev, 0x85a6, 0x200000);
	vce_gladius_write(adev, 0x85b5, 0xfa);
	vce_gladius_write(adev, 0x85bd, vce_gladius_read(adev, 0x85bd) & ~1u);
	vce_gladius_write(adev, 0x8005, vce_gladius_read(adev, 0x8005) | 1);
	vce_gladius_write(adev, 0x85bd, vce_gladius_read(adev, 0x85bd) & ~1u);
	vce_gladius_write(adev, 0x859d, vce_gladius_read(adev, 0x859d) & ~0x100u);
	vce_gladius_write(adev, 0x85bb, 0);
	vce_gladius_write(adev, 0x1520, 1);
	vce_gladius_write(adev, 0x501, 3);
	vce_gladius_write(adev, 0x8048, vce_gladius_read(adev, 0x8048) & ~1u);
	usleep_range(16000, 17000);
	r = read_poll_timeout(vce_gladius_read, value, value & 2,
		1000, 1000000, false, adev, 0x8001);
	if (r) {
		vce_gladius_stop(adev);
		return r;
	}
	vce_gladius_write(adev, 0x8001, vce_gladius_read(adev, 0x8001) & ~1u);
	vce_gladius_write(adev, 0x8540, 8);
	return 0;
}

static uint64_t vce_gladius_get_rptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	if (!adev->vce.native_running)
		return ring->wptr;
	return vce_gladius_read(adev, 0x8063);
}

static uint64_t vce_gladius_get_wptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	if (!adev->vce.native_running)
		return ring->wptr;
	return vce_gladius_read(adev, 0x8064);
}

static void vce_gladius_set_wptr(struct amdgpu_ring *ring)
{
	struct amdgpu_device *adev = ring->adev;

	if (!adev->vce.native_running)
		return;
	vce_gladius_write(adev, 0x1520, 1);
	vce_gladius_write(adev, 0x8064, lower_32_bits(ring->wptr));
}

static void vce_gladius_emit_ib(struct amdgpu_ring *ring, struct amdgpu_job *job,
		struct amdgpu_ib *ib, u32 flags)
{
	struct amdgpu_ib task = *ib;
	u32 count;

	if (ib->ptr && ib->length_dw >= 8 && ib->ptr[0] == 12 && ib->ptr[1] == 1 &&
	    ib->ptr[3] == 20 && ib->ptr[4] == 2) {
		count = ib->ptr[6];
		if (count >= 8 && count <= ib->length_dw - 8 &&
		    ib->ptr[count] == 12 && ib->ptr[count + 1] == 1 &&
		    ib->ptr[count + 3] == 20 && ib->ptr[count + 4] == 2 &&
		    ib->ptr[count + 6] == ib->length_dw - count) {
			task.length_dw = count;
			amdgpu_vce_ring_emit_ib(ring, job, &task, flags);
			task.gpu_addr += count * sizeof(u32);
			task.length_dw = ib->length_dw - count;
		}
	}
	amdgpu_vce_ring_emit_ib(ring, job, &task, flags);
}

static const struct amdgpu_ring_funcs vce_gladius_ring_funcs = {
	.type = AMDGPU_RING_TYPE_VCE,
	.align_mask = 0xf,
	.nop = VCE_CMD_NO_OP,
	.support_64bit_ptrs = false,
	.no_user_fence = true,
	.get_rptr = vce_gladius_get_rptr,
	.get_wptr = vce_gladius_get_wptr,
	.set_wptr = vce_gladius_set_wptr,
	.parse_cs = amdgpu_vce_ring_parse_cs,
	.emit_frame_size = 6,
	.emit_ib_size = 8,
	.emit_ib = vce_gladius_emit_ib,
	.emit_fence = amdgpu_vce_ring_emit_fence,
	.test_ring = amdgpu_vce_ring_test_ring,
	.test_ib = amdgpu_vce_ring_test_ib,
	.insert_nop = amdgpu_ring_insert_nop,
	.pad_ib = amdgpu_ring_generic_pad_ib,
};

static int vce_gladius_set_irq(struct amdgpu_device *adev,
		struct amdgpu_irq_src *source, unsigned int type,
		enum amdgpu_interrupt_state state)
{
	if (!adev->vce.native_running)
		return 0;
	vce_gladius_write(adev, 0x8540, (vce_gladius_read(adev, 0x8540) & ~8u) |
		(state == AMDGPU_IRQ_STATE_ENABLE ? 8 : 0));
	return 0;
}

static int vce_gladius_irq(struct amdgpu_device *adev,
		struct amdgpu_irq_src *source, struct amdgpu_iv_entry *entry)
{
	amdgpu_fence_process(&adev->vce.ring[0]);
	return 0;
}

static const struct amdgpu_irq_src_funcs vce_gladius_irq_funcs = {
	.set = vce_gladius_set_irq,
	.process = vce_gladius_irq,
};

static int vce_gladius_early_init(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	int r;

	if (!amdgpu_vce_is_gladius(adev))
		return -ENODEV;
	r = amdgpu_vce_early_init(adev);
	if (r)
		return r;
	adev->vce.num_rings = 1;
	adev->vce.ring[0].funcs = &vce_gladius_ring_funcs;
	adev->vce.ring[0].me = 0;
	adev->vce.irq.num_types = 1;
	adev->vce.irq.funcs = &vce_gladius_irq_funcs;
	return 0;
}

static int vce_gladius_sw_init(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	struct amdgpu_ring *ring = &adev->vce.ring[0];
	int r;

	r = amdgpu_irq_add_id(adev, AMDGPU_IRQ_CLIENTID_LEGACY, 167, &adev->vce.irq);
	if (r)
		return r;
	r = amdgpu_vce_sw_init(adev, 0xe5000);
	if (r)
		return r;
	mutex_init(&adev->vce.native_mutex);
	adev->vce.native_workqueue = alloc_ordered_workqueue("vce-native", WQ_MEM_RECLAIM);
	if (!adev->vce.native_workqueue) {
		r = -ENOMEM;
		goto free_vcpu;
	}
	r = amdgpu_vce_resume(adev);
	if (r)
		goto free_workqueue;
	snprintf(ring->name, sizeof(ring->name), "vce0");
	r = amdgpu_ring_init(adev, ring, 512, &adev->vce.irq, 0,
		AMDGPU_RING_PRIO_0, NULL);
	if (!r)
		return 0;
free_workqueue:
	destroy_workqueue(adev->vce.native_workqueue);
	adev->vce.native_workqueue = NULL;
free_vcpu:
	mutex_destroy(&adev->vce.native_mutex);
	amdgpu_vce_sw_fini(adev);
	return r;
}

static int vce_gladius_hw_init(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;
	int r;

	r = vce_gladius_start(adev);
	if (r)
		return r;
	r = amdgpu_ring_test_helper(&adev->vce.ring[0]);
	if (r) {
		vce_gladius_stop(adev);
		return r;
	}
	dev_info(adev->dev, "VCE native firmware initialized\n");

	return 0;
}

static int vce_gladius_hw_fini(struct amdgpu_ip_block *ip_block)
{
	int r;

	cancel_delayed_work_sync(&ip_block->adev->vce.idle_work);
	r = amdgpu_fence_wait_empty(&ip_block->adev->vce.ring[0]);
	if (r)
		return r;
	flush_workqueue(ip_block->adev->vce.native_workqueue);
	return vce_gladius_stop(ip_block->adev);
}

static int vce_gladius_sw_fini(struct amdgpu_ip_block *ip_block)
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

static int vce_gladius_suspend(struct amdgpu_ip_block *ip_block)
{
	int r = amdgpu_vce_suspend(ip_block->adev);

	return r ?: vce_gladius_hw_fini(ip_block);
}

static int vce_gladius_resume(struct amdgpu_ip_block *ip_block)
{
	int r = amdgpu_vce_resume(ip_block->adev);

	return r ?: vce_gladius_hw_init(ip_block);
}

static bool vce_gladius_is_idle(struct amdgpu_ip_block *ip_block)
{
	struct amdgpu_device *adev = ip_block->adev;

	if (!adev->vce.native_running)
		return true;
	return !(vce_gladius_read(adev, 0x8001) & 0x3c) &&
		(vce_gladius_read(adev, 0x85a7) & 0x3fff) == 0x337f;
}

static int vce_gladius_wait_idle(struct amdgpu_ip_block *ip_block)
{
	unsigned int i;

	for (i = 0; i < 10000; i++) {
		if (vce_gladius_is_idle(ip_block))
			return 0;
		udelay(10);
	}
	return -ETIMEDOUT;
}

static int vce_gladius_clockgating(struct amdgpu_ip_block *ip_block,
		enum amd_clockgating_state state)
{
	return 0;
}

static int vce_gladius_powergating(struct amdgpu_ip_block *ip_block,
		enum amd_powergating_state state)
{
	return 0;
}

static const struct amd_ip_funcs vce_gladius_ip_funcs = {
	.name = "vce_gladius",
	.early_init = vce_gladius_early_init,
	.sw_init = vce_gladius_sw_init,
	.sw_fini = vce_gladius_sw_fini,
	.hw_init = vce_gladius_hw_init,
	.hw_fini = vce_gladius_hw_fini,
	.suspend = vce_gladius_suspend,
	.resume = vce_gladius_resume,
	.wait_for_idle = vce_gladius_wait_idle,
	.set_clockgating_state = vce_gladius_clockgating,
	.set_powergating_state = vce_gladius_powergating,
};

const struct amdgpu_ip_block_version vce_gladius_ip_block = {
	.type = AMD_IP_BLOCK_TYPE_VCE,
	.major = 1,
	.minor = 0,
	.rev = 0,
	.funcs = &vce_gladius_ip_funcs,
};
