#include "amdgpu.h"
#include "amdgpu_vce.h"
#include "vce_gladius.h"
#include "vce_gladius_vectors.h"

static void vce_gladius_address(u32 *words, u64 address)
{
	words[0] = upper_32_bits(address);
	words[1] = lower_32_bits(address);
}

unsigned int vce_gladius_emit_create(u32 *words, const struct vce_gladius_config *config)
{
	memcpy(words, vce_gladius_create_words, sizeof(vce_gladius_create_words));
	words[2] = config->handle;
	words[25] = config->profile == 100 ? 77 : config->profile;
	words[26] = config->level;
	words[28] = config->width;
	words[29] = config->height;
	vce_gladius_address(words + 10, config->descriptor);
	vce_gladius_address(words + 15, config->cpb);
	vce_gladius_address(words + 19, config->output);
	words[21] = VCE_GLADIUS_POOL_SIZE;
	vce_gladius_address(words + 31, config->output1);
	words[33] = VCE_GLADIUS_POOL_SIZE;
	vce_gladius_address(words + 40, config->feedback);
	return ARRAY_SIZE(vce_gladius_create_words);
}

unsigned int vce_gladius_emit_encode(u32 *words, const struct vce_gladius_config *config,
		u64 luma, u64 chroma, u32 pitch, u32 type, u32 frame_num, u32 poc,
		bool initial, bool rate_changed)
{
	u32 count, offset, i;
	u32 macroblocks = DIV_ROUND_UP(config->width, 16) * DIV_ROUND_UP(config->height, 16);
	u32 frame_rate = config->fps_num / config->fps_den;

	if (initial) {
		memcpy(words, vce_gladius_picture_words, sizeof(vce_gladius_picture_words));
		count = ARRAY_SIZE(vce_gladius_picture_words);
	} else {
		memcpy(words, vce_gladius_p_words, sizeof(vce_gladius_p_words));
		count = ARRAY_SIZE(vce_gladius_p_words);
		if (rate_changed) {
			memmove(words + 38, words + 8, (count - 8) * sizeof(*words));
			memcpy(words + 8, vce_gladius_picture_words + 44, 120);
			count += 30;
		}
	}
	words[2] = config->handle;
	for (offset = 8; offset < count;) {
		u32 *packet = words + offset;
		u32 length = packet[0] / 4;

		if (packet[1] == 0x05000004) {
			count -= length;
			memmove(packet, packet + length, (count - offset) * sizeof(*packet));
			continue;
		}

		switch (packet[1]) {
		case 0x04000002:
			packet[0x3c / 4] = macroblocks;
			packet[0x7c / 4] = macroblocks;
			break;
		case 0x04000004:
			packet[0x08 / 4] = 0;
			packet[0x14 / 4] = config->fps_num;
			packet[0x1c / 4] = config->qp_i;
			packet[0x20 / 4] = config->qp_p;
			packet[0x24 / 4] = config->qp_p;
			packet[0x2c / 4] = config->fps_den;
			packet[0x34 / 4] = config->width * config->height * 2;
			packet[0x3c / 4] = packet[0x0c / 4] / frame_rate;
			break;
		case 0x04000007:
			packet[0x210 / 4] = fls(max(config->width, config->height) - 1) + 3;
			packet[0x214 / 4] = packet[0x210 / 4];
			break;
		case 0x03000001:
			packet[0x10 / 4] = config->max_bitstream;
			packet[0x18 / 4] = frame_num;
			packet[0x1c / 4] = poc;
			packet[0x20 / 4] = type;
			vce_gladius_address(packet + 0x28 / 4, luma);
			vce_gladius_address(packet + 0x30 / 4, chroma);
			for (i = 0; i < 5; i++) {
				u32 stride = i ? ALIGN(config->width, 32) : pitch;

				packet[(0x38 + i * 0x24) / 4] = stride;
				packet[(0x3c + i * 0x24) / 4] = stride;
			}
			packet[0xd8 / 4] = 1;
			packet[0xdc / 4] = 1;
			vce_gladius_address(packet + 0xe4 / 4, config->output1);
			packet[0xec / 4] = config->width;
			packet[0xf0 / 4] = config->height;
			break;
		}
		offset += length;
	}
	words[6] = count;
	return count;
}

unsigned int vce_gladius_emit_destroy(u32 *words, u32 handle)
{
	u32 command[] = {12, 1, handle, 20, 2, 1, 10, 1, 8, 0x02000001};

	memcpy(words, command, sizeof(command));
	return ARRAY_SIZE(command);
}

int vce_gladius_test_ib(struct amdgpu_ring *ring, long timeout)
{
	struct amdgpu_device *adev = ring->adev;
	struct amdgpu_bo *bo = NULL;
	struct amdgpu_job *job;
	struct dma_fence *fence = NULL;
	struct vce_gladius_config config = {
		.handle = 1, .profile = 66, .level = 31, .width = 160, .height = 128,
	};
	void *cpu;
	u64 gpu;
	unsigned int count;
	long waited;
	int r;

	r = amdgpu_bo_create_kernel(adev, 0x21000, PAGE_SIZE, AMDGPU_GEM_DOMAIN_GTT,
		&bo, &gpu, &cpu);
	if (r)
		return r;
	memset_io(cpu, 0, 0x21000);
	config.cpb = gpu;
	config.descriptor = gpu + 0x19000;
	config.output = gpu + 0x1a000;
	config.output1 = gpu + 0x1b000;
	config.feedback = gpu + 0x1c000;
	r = amdgpu_job_alloc_with_ib(adev, &adev->vce.entity, AMDGPU_FENCE_OWNER_UNDEFINED,
		4096, AMDGPU_IB_POOL_DIRECT, AMDGPU_KERNEL_JOB_ID_VCN_RING_TEST, &job);
	if (r)
		goto free_bo;
	count = vce_gladius_emit_create(job->ibs[0].ptr, &config);
	job->ibs[0].ptr[21] = job->ibs[0].ptr[33] = PAGE_SIZE;
	count += vce_gladius_emit_destroy(job->ibs[0].ptr + count, config.handle);
	job->ibs[0].length_dw = count;
	r = amdgpu_job_submit_direct(job, ring, &fence);
	if (r) {
		amdgpu_job_free(job);
		goto free_bo;
	}
	waited = dma_fence_wait_timeout(fence, false, timeout);
	r = waited <= 0 ? waited ?: -ETIMEDOUT : dma_fence_get_status(fence);
	dma_fence_put(fence);
	if (r < 0) {
		adev->vce.native_stop_failed = true;
		return r;
	}
	r = 0;
free_bo:
	amdgpu_bo_free_kernel(&bo, &gpu, &cpu);
	return r;
}
