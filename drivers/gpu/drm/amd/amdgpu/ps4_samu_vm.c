// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <asm/ps4.h>

#include "amdgpu.h"
#include "amdgpu_vm.h"
#include "ps4_samu_vm.h"

#include "bif/bif_4_1_d.h"
#include "gmc/gmc_7_1_d.h"

#define PS4_SAMU_VMID			15
#define PS4_SAMU_PTE_FLAGS		(AMDGPU_PTE_VALID | AMDGPU_PTE_SYSTEM | \
					 AMDGPU_PTE_SNOOPED | AMDGPU_PTE_READABLE | \
					 AMDGPU_PTE_WRITEABLE)

#define SAM_INDEX			(0x22000 / 4)
#define SAM_DATA			(0x22004 / 4)
#define SAM_DOORBELL			0x32
#define SAM_MAILBOX_HIGH		0x33
#define SAM_MAILBOX_LOW			0x34
#define SAM_ACK				0x37
#define SAM_STATUS			0x4a
#define SAM_STATUS_BUSY			BIT(0)

#define SHARED_SIZE			SZ_1M
#define SHARED_PAGES			(SHARED_SIZE / AMDGPU_GPU_PAGE_SIZE)
#define MSG_OFFSET			0x1000
#define MSG_SIZE			0xa8
#define DATA_VA				SZ_1M
#define DATA_SIZE			SZ_1M
#define MAP_VA				SZ_2M
#define MAP_PAGES			(SZ_1M / PAGE_SIZE + 1)
#define SEND_TIMEOUT_MS			2000
#define PROBE_TIMEOUT_MS		200
#define SPIN_US				200
#define INVALIDATE_TIMEOUT_US		10000

#define RNG_MSG_TYPE			0x0a
#define RNG_DATA_OFFSET			0x18
#define RNG_FILL			0xa5
#define CCP_MSG_TYPE			8
#define CCP_OP_OFFSET			0x18
#define CCP_OP_SIZE			0x88

struct ps4_samu_vm {
	struct amdgpu_device *adev;
	struct amdgpu_bo *bo;
	u64 gpu_addr;
	u64 *table;
	unsigned int pt_entries;
	void *shared;
	dma_addr_t shared_dma;
	void *data;
	dma_addr_t data_dma;
	dma_addr_t map_dma[MAP_PAGES];
	unsigned int map_count;
	bool map_broken;
	u64 base;
	u32 end;
	bool ready;
	bool probing;
	u32 fault_page;
	u64 tag;
	struct mutex lock;
};

static struct ps4_samu_vm *ps4_samu_vm;

static bool ps4_samu_vm_supported(struct amdgpu_device *adev)
{
	return adev->asic_type == CHIP_LIVERPOOL ||
	       adev->asic_type == CHIP_GLADIUS;
}

static u64 *ps4_samu_vm_pd(struct ps4_samu_vm *vm)
{
	return vm->table;
}

static u64 *ps4_samu_vm_pt(struct ps4_samu_vm *vm, unsigned int index)
{
	return vm->table + (AMDGPU_GPU_PAGE_SIZE / sizeof(u64)) +
	       index * vm->pt_entries;
}

static u64 ps4_samu_vm_pt_addr(struct ps4_samu_vm *vm, unsigned int index)
{
	return vm->gpu_addr + AMDGPU_GPU_PAGE_SIZE +
	       index * vm->pt_entries * sizeof(u64);
}

static int ps4_samu_vm_invalidate(struct ps4_samu_vm *vm)
{
	struct amdgpu_device *adev = vm->adev;
	u32 bit = 1 << PS4_SAMU_VMID;
	ktime_t timeout;

	mb();
	amdgpu_device_flush_hdp(adev, NULL);
	WREG32(mmGARLIC_FLUSH_REQ, 1);
	WREG32(mmVM_INVALIDATE_RESPONSE, RREG32(mmVM_INVALIDATE_RESPONSE));
	WREG32(mmVM_INVALIDATE_REQUEST, bit);
	timeout = ktime_add_us(ktime_get(), INVALIDATE_TIMEOUT_US);
	while (!(RREG32(mmVM_INVALIDATE_RESPONSE) & bit)) {
		if (ktime_after(ktime_get(), timeout)) {
			dev_err_ratelimited(adev->dev,
					    "samu: vmid %d invalidate timed out\n",
					    PS4_SAMU_VMID);
			return -ETIMEDOUT;
		}
		udelay(1);
	}
	WREG32(mmVM_INVALIDATE_RESPONSE, bit);
	return 0;
}

static void sam_write(struct amdgpu_device *adev, u32 index, u32 value)
{
	WREG32(SAM_INDEX, index);
	WREG32(SAM_DATA, value);
}

static u32 sam_read(struct amdgpu_device *adev, u32 index)
{
	WREG32(SAM_INDEX, index);
	return RREG32(SAM_DATA);
}

static int sam_send(struct ps4_samu_vm *vm, u64 word)
{
	struct amdgpu_device *adev = vm->adev;
	unsigned long timeout = jiffies + msecs_to_jiffies(SEND_TIMEOUT_MS);
	ktime_t spin;

	dma_wmb();
	sam_write(adev, SAM_MAILBOX_LOW, lower_32_bits(word));
	sam_write(adev, SAM_MAILBOX_HIGH, upper_32_bits(word));
	sam_write(adev, SAM_DOORBELL, 1);
	spin = ktime_add_us(ktime_get(), SPIN_US);
	while (sam_read(adev, SAM_STATUS) & SAM_STATUS_BUSY) {
		if (ktime_before(ktime_get(), spin)) {
			udelay(1);
			continue;
		}
		if (time_after(jiffies, timeout))
			return -ETIMEDOUT;
		usleep_range(20, 50);
	}
	dma_rmb();
	return 0;
}

int ps4_samu_vm_init(struct amdgpu_device *adev)
{
	struct ps4_samu_vm *vm;
	size_t size;
	int r;

	if (!ps4_samu_vm_supported(adev) || ps4_samu_vm)
		return 0;

	vm = kzalloc(sizeof(*vm), GFP_KERNEL);
	if (!vm)
		return -ENOMEM;

	vm->adev = adev;
	vm->pt_entries = 1U << adev->vm_manager.block_size;
	mutex_init(&vm->lock);

	vm->shared = dma_alloc_coherent(adev->dev, SHARED_SIZE, &vm->shared_dma,
					GFP_KERNEL);
	if (!vm->shared) {
		r = -ENOMEM;
		goto err_free;
	}

	vm->data = dma_alloc_coherent(adev->dev, DATA_SIZE, &vm->data_dma,
				      GFP_KERNEL);
	if (!vm->data) {
		r = -ENOMEM;
		goto err_shared;
	}

	size = AMDGPU_GPU_PAGE_SIZE + 2 * vm->pt_entries * sizeof(u64);
	r = amdgpu_bo_create_kernel(adev, size, AMDGPU_GPU_PAGE_SIZE,
				    AMDGPU_GEM_DOMAIN_VRAM, &vm->bo,
				    &vm->gpu_addr, (void **)&vm->table);
	if (r)
		goto err_data;

	memset(vm->table, 0, size);
	vm->end = vm->pt_entries - 1;
	ps4_samu_vm = vm;
	return 0;

err_data:
	dma_free_coherent(adev->dev, DATA_SIZE, vm->data, vm->data_dma);
err_shared:
	dma_free_coherent(adev->dev, SHARED_SIZE, vm->shared, vm->shared_dma);
err_free:
	kfree(vm);
	return r;
}

void ps4_samu_vm_fini(struct amdgpu_device *adev)
{
	struct ps4_samu_vm *vm = ps4_samu_vm;

	if (!vm || vm->adev != adev)
		return;

	ps4_samu_vm = NULL;
	amdgpu_bo_free_kernel(&vm->bo, &vm->gpu_addr, (void **)&vm->table);
	dma_free_coherent(adev->dev, DATA_SIZE, vm->data, vm->data_dma);
	dma_free_coherent(adev->dev, SHARED_SIZE, vm->shared, vm->shared_dma);
	kfree(vm);
}

static void ps4_samu_map_pages(u64 *pt, unsigned int first, dma_addr_t addr,
			       unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++)
		WRITE_ONCE(pt[first + i], (addr + (u64)i * AMDGPU_GPU_PAGE_SIZE) |
			   PS4_SAMU_PTE_FLAGS);
}

static void ps4_samu_map_shared_pt(struct ps4_samu_vm *vm)
{
	struct amdgpu_device *adev = vm->adev;
	unsigned int pde = vm->base >> (12 + adev->vm_manager.block_size);

	ps4_samu_map_pages(ps4_samu_vm_pt(vm, 1),
			   (vm->base >> 12) & (vm->pt_entries - 1),
			   vm->shared_dma, SHARED_PAGES);
	ps4_samu_vm_pd(vm)[pde] = ps4_samu_vm_pt_addr(vm, 1) | AMDGPU_PTE_VALID;
}

void ps4_samu_vm_enable(struct amdgpu_device *adev)
{
	struct ps4_samu_vm *vm = ps4_samu_vm;

	if (!vm || vm->adev != adev)
		return;

	ps4_samu_map_pages(ps4_samu_vm_pt(vm, 0), DATA_VA >> 12, vm->data_dma,
			   DATA_SIZE / AMDGPU_GPU_PAGE_SIZE);
	if (!vm->map_count && (MAP_VA >> 12) + MAP_PAGES <= vm->pt_entries)
		memset(ps4_samu_vm_pt(vm, 0) + (MAP_VA >> 12), 0,
		       MAP_PAGES * sizeof(u64));
	ps4_samu_vm_pd(vm)[0] = ps4_samu_vm_pt_addr(vm, 0) | AMDGPU_PTE_VALID;
	if (vm->ready)
		ps4_samu_map_shared_pt(vm);

	WREG32(mmVM_CONTEXT15_PAGE_TABLE_BASE_ADDR, vm->gpu_addr >> 12);
	WREG32(mmVM_CONTEXT8_PAGE_TABLE_START_ADDR + PS4_SAMU_VMID - 8, 0);
	WREG32(mmVM_CONTEXT8_PAGE_TABLE_END_ADDR + PS4_SAMU_VMID - 8, vm->end);
	ps4_samu_vm_invalidate(vm);
}

void ps4_samu_vm_fault(struct amdgpu_device *adev, u32 vmid, u32 page)
{
	struct ps4_samu_vm *vm = ps4_samu_vm;

	if (!vm || vm->adev != adev || vmid != PS4_SAMU_VMID)
		return;
	if (READ_ONCE(vm->probing) && !READ_ONCE(vm->fault_page))
		WRITE_ONCE(vm->fault_page, page);
}

static int ps4_samu_map_shared(struct ps4_samu_vm *vm)
{
	struct amdgpu_device *adev = vm->adev;
	unsigned int pde = vm->base >> (12 + adev->vm_manager.block_size);
	unsigned int first = (vm->base >> 12) & (vm->pt_entries - 1);

	if (!pde || pde >= AMDGPU_GPU_PAGE_SIZE / sizeof(u64) ||
	    first + SHARED_PAGES > vm->pt_entries)
		return -ERANGE;

	ps4_samu_map_shared_pt(vm);
	vm->end = max_t(u32, vm->end, ((vm->base + SHARED_SIZE) >> 12) - 1);
	WREG32(mmVM_CONTEXT8_PAGE_TABLE_END_ADDR + PS4_SAMU_VMID - 8, vm->end);
	ps4_samu_vm_invalidate(vm);
	return 0;
}

static int ps4_samu_setup(struct ps4_samu_vm *vm)
{
	struct amdgpu_device *adev = vm->adev;
	unsigned long timeout;
	int r;

	if (vm->ready)
		return 0;

	WRITE_ONCE(vm->fault_page, 0);
	WRITE_ONCE(vm->probing, true);
	r = sam_send(vm, MSG_OFFSET);
	timeout = jiffies + msecs_to_jiffies(PROBE_TIMEOUT_MS);
	while (!r && !READ_ONCE(vm->fault_page) && !time_after(jiffies, timeout))
		usleep_range(500, 1000);
	WRITE_ONCE(vm->probing, false);
	if (r) {
		dev_err(adev->dev, "samu: probe request timed out\n");
		return r;
	}
	if (!vm->fault_page) {
		dev_err(adev->dev, "samu: probe gave no vmid %d fault\n",
			PS4_SAMU_VMID);
		return -ENODEV;
	}

	vm->base = ((u64)vm->fault_page << 12) - MSG_OFFSET;
	if (vm->base % SHARED_SIZE) {
		dev_err(adev->dev, "samu: base 0x%llx is not 1 MiB aligned\n",
			vm->base);
		return -EINVAL;
	}

	memset(vm->shared, 0, SHARED_SIZE);
	r = ps4_samu_map_shared(vm);
	if (r) {
		dev_err(adev->dev, "samu: base 0x%llx out of range\n", vm->base);
		return r;
	}

	vm->ready = true;
	dev_info(adev->dev, "samu: shared buffer at vmid %d va 0x%llx\n",
		 PS4_SAMU_VMID, vm->base);
	return 0;
}

void ps4_samu_lock(void)
{
	if (ps4_samu_vm)
		mutex_lock(&ps4_samu_vm->lock);
}
EXPORT_SYMBOL_GPL(ps4_samu_lock);

void ps4_samu_unlock(void)
{
	if (ps4_samu_vm)
		mutex_unlock(&ps4_samu_vm->lock);
}
EXPORT_SYMBOL_GPL(ps4_samu_unlock);

void *ps4_samu_data(u64 *va, size_t *size)
{
	struct ps4_samu_vm *vm = ps4_samu_vm;

	if (!vm)
		return NULL;
	lockdep_assert_held(&vm->lock);
	if (ps4_samu_setup(vm))
		return NULL;
	*va = DATA_VA;
	*size = DATA_SIZE;
	return vm->data;
}
EXPORT_SYMBOL_GPL(ps4_samu_data);

void ps4_samu_unmap(void)
{
	struct ps4_samu_vm *vm = ps4_samu_vm;
	unsigned int i;
	u64 *pt;

	if (!vm || !vm->map_count)
		return;
	lockdep_assert_held(&vm->lock);
	pt = ps4_samu_vm_pt(vm, 0) + (MAP_VA >> 12);
	for (i = 0; i < vm->map_count; i++)
		WRITE_ONCE(pt[i], 0);
	if (ps4_samu_vm_invalidate(vm))
		vm->map_broken = true;
	for (i = 0; i < vm->map_count; i++)
		dma_unmap_page(vm->adev->dev, vm->map_dma[i], PAGE_SIZE,
			       DMA_BIDIRECTIONAL);
	vm->map_count = 0;
}
EXPORT_SYMBOL_GPL(ps4_samu_unmap);

int ps4_samu_map(struct page **pages, unsigned int count, u64 *va)
{
	struct ps4_samu_vm *vm = ps4_samu_vm;
	struct device *dev;
	dma_addr_t addr;
	unsigned int i;
	u64 *pt;
	int r;

	if (!vm)
		return -ENODEV;
	lockdep_assert_held(&vm->lock);
	if (!count || count > MAP_PAGES || vm->map_count || vm->map_broken ||
	    (MAP_VA >> 12) + MAP_PAGES > vm->pt_entries)
		return -EINVAL;
	r = ps4_samu_setup(vm);
	if (r)
		return r;

	dev = vm->adev->dev;
	pt = ps4_samu_vm_pt(vm, 0) + (MAP_VA >> 12);
	for (i = 0; i < count; i++) {
		addr = dma_map_page(dev, pages[i], 0, PAGE_SIZE,
				    DMA_BIDIRECTIONAL);
		if (dma_mapping_error(dev, addr)) {
			ps4_samu_unmap();
			return -ENOMEM;
		}
		vm->map_dma[i] = addr;
		vm->map_count = i + 1;
		WRITE_ONCE(pt[i], addr | PS4_SAMU_PTE_FLAGS);
	}
	r = ps4_samu_vm_invalidate(vm);
	if (r) {
		ps4_samu_unmap();
		return r;
	}
	*va = MAP_VA;
	return 0;
}
EXPORT_SYMBOL_GPL(ps4_samu_map);

int ps4_samu_rng(void *buf, size_t len)
{
	struct ps4_samu_vm *vm = ps4_samu_vm;
	u8 *msg;
	int r;

	if (!vm)
		return -EPROBE_DEFER;
	if (!len || len > 16)
		return -EINVAL;

	mutex_lock(&vm->lock);
	r = ps4_samu_setup(vm);
	if (r)
		goto out;

	msg = vm->shared + MSG_OFFSET;
	memset(msg, 0, MSG_SIZE);
	memset(msg + RNG_DATA_OFFSET, RNG_FILL, 16);
	*(u32 *)msg = RNG_MSG_TYPE;

	r = sam_send(vm, vm->base + MSG_OFFSET);
	if (r)
		goto out;
	if (*(u32 *)(msg + 4) || !memchr_inv(msg + RNG_DATA_OFFSET, RNG_FILL, 16)) {
		r = -EIO;
		goto out;
	}
	memcpy(buf, msg + RNG_DATA_OFFSET, len);
out:
	mutex_unlock(&vm->lock);
	return r;
}
EXPORT_SYMBOL_GPL(ps4_samu_rng);

int ps4_samu_ccp(void *op)
{
	struct ps4_samu_vm *vm = ps4_samu_vm;
	struct amdgpu_device *adev;
	unsigned long timeout;
	u8 *msg, *reply;
	u32 cmd, status;
	ktime_t spin;
	int r;

	if (!vm)
		return -ENODEV;
	lockdep_assert_held(&vm->lock);
	adev = vm->adev;
	msg = vm->shared + MSG_OFFSET;
	reply = vm->shared;
	r = ps4_samu_setup(vm);
	if (r)
		return r;

	memset(reply, 0, MSG_SIZE);
	memset(msg, 0, MSG_SIZE);
	*(u32 *)msg = CCP_MSG_TYPE;
	*(u64 *)(msg + 8) = ++vm->tag << 16;
	memcpy(msg + CCP_OP_OFFSET, op, CCP_OP_SIZE);

	r = sam_send(vm, vm->base + MSG_OFFSET);
	if (r)
		return r;
	status = *(u32 *)(msg + 4);
	if (status)
		return (s32)status < 0 && (s32)status >= -MAX_ERRNO ? (s32)status : -EIO;

	timeout = jiffies + msecs_to_jiffies(SEND_TIMEOUT_MS);
	spin = ktime_add_us(ktime_get(), SPIN_US);
	for (;;) {
		cmd = READ_ONCE(*(u32 *)reply);
		if (cmd == CCP_MSG_TYPE)
			break;
		if (cmd) {
			dev_warn_ratelimited(adev->dev,
					     "samu: host message cmd %u ignored\n",
					     cmd);
			WRITE_ONCE(*(u32 *)reply, 0);
			sam_write(adev, SAM_ACK, 1);
		}
		if (ktime_before(ktime_get(), spin)) {
			cpu_relax();
			continue;
		}
		if (time_after(jiffies, timeout))
			return -ETIMEDOUT;
		usleep_range(10, 30);
	}
	dma_rmb();
	status = *(u32 *)(reply + 4);
	memcpy(op, reply + CCP_OP_OFFSET, CCP_OP_SIZE);
	*(u32 *)reply = 0;
	*(u64 *)(reply + 8) = 0;
	dma_wmb();
	sam_write(adev, SAM_ACK, 1);
	return status ? -EIO : 0;
}
EXPORT_SYMBOL_GPL(ps4_samu_ccp);
