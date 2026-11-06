#include <linux/dma-fence.h>
#include <linux/delay.h>
#include <linux/kref.h>
#include <linux/moduleparam.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>

#include "amdgpu.h"
#include "amdgpu_cs.h"
#include "amdgpu_vce.h"
#include "vce_liverpool.h"
#include "vce_liverpool_vectors.h"
#include "vce_liverpool_sps.h"

#define VCE_NATIVE_POOL_SIZE 0x6a4000u

struct vce_liverpool_session {
	struct kref ref;
	struct amdgpu_device *adev;
	struct drm_file *filp;
	u32 handle, native_handle;
	u32 width, padded_height, height, pitch;
	u32 profile, level, fps_num, fps_den, qp_i, qp_p;
	u32 crop[4];
	u32 record;
	u32 metadata, bitstream, header;
	struct amdgpu_bo *bo;
	struct dma_fence *tail;
	u64 gpu;
	void *cpu;
	bool started, failed;
};

struct vce_liverpool_job {
	struct dma_fence fence;
	spinlock_t lock;
	struct dma_fence_cb callback;
	struct work_struct work, release;
	struct dma_fence *hardware;
	struct vce_liverpool_session *session;
	struct amdgpu_device *adev;
	struct amdgpu_bo *output_bo, *feedback_bo, *input_bo;
	void *output, *feedback;
	u32 output_size, record, fps_num, fps_den;
	bool encode, armed, input_pinned, retain, control_only;
};

static atomic_t vce_liverpool_next_handle = ATOMIC_INIT(0);
static bool vce_liverpool_trace;
module_param_named(vce_native_trace, vce_liverpool_trace, bool, 0600);
MODULE_PARM_DESC(vce_native_trace, "Trace native VCE command submissions");

static void vce_liverpool_session_release(struct kref *ref)
{
	struct vce_liverpool_session *session =
		container_of(ref, struct vce_liverpool_session, ref);

	if (session->adev->vce.native_stop_failed)
		return;
	if (session->bo)
		amdgpu_bo_free_kernel(&session->bo, &session->gpu, &session->cpu);
	dma_fence_put(session->tail);
	kfree(session);
}

static const char *vce_liverpool_fence_name(struct dma_fence *fence)
{
	return "vce-native-output";
}

static void vce_liverpool_release_job(struct work_struct *work)
{
	struct vce_liverpool_job *job =
		container_of(work, struct vce_liverpool_job, release);

	dma_fence_put(job->hardware);
	if (job->input_pinned) {
		if (amdgpu_bo_reserve(job->input_bo, false))
			return;
		amdgpu_bo_unpin(job->input_bo);
		amdgpu_bo_unreserve(job->input_bo);
	}
	amdgpu_bo_unref(&job->input_bo);
	amdgpu_bo_unref(&job->output_bo);
	amdgpu_bo_unref(&job->feedback_bo);
	if (job->session)
		kref_put(&job->session->ref, vce_liverpool_session_release);
	kfree(job);
}

static void vce_liverpool_fence_release(struct dma_fence *fence)
{
	struct vce_liverpool_job *job = container_of(fence, struct vce_liverpool_job, fence);

	queue_work(system_unbound_wq, &job->release);
}

static const struct dma_fence_ops vce_liverpool_fence_ops = {
	.get_driver_name = vce_liverpool_fence_name,
	.get_timeline_name = vce_liverpool_fence_name,
	.release = vce_liverpool_fence_release,
};

static u32 vce_liverpool_startcode(const u8 *data, u32 size, u32 offset)
{
	while (offset + 3 < size) {
		if (!data[offset] && !data[offset + 1] &&
		    (data[offset + 2] == 1 || (!data[offset + 2] && data[offset + 3] == 1)))
			return offset;
		offset++;
	}
	return size;
}

static int vce_liverpool_copy_output(struct vce_liverpool_job *job, u32 start, u32 size)
{
	struct vce_liverpool_session *session = job->session;
	u8 *source, sps[512];
	u32 first, position, next, nal, copied = 0;
	int length;

	if (!size || size > VCE_NATIVE_POOL_SIZE || start >= VCE_NATIVE_POOL_SIZE)
		return -EINVAL;
	source = kvmalloc(size, GFP_KERNEL);
	if (!source)
		return -ENOMEM;
	first = min(size, VCE_NATIVE_POOL_SIZE - start);
	memcpy_fromio(source, session->cpu + session->bitstream + start, first);
	if (first < size)
		memcpy_fromio(source + first, session->cpu + session->bitstream, size - first);
	position = vce_liverpool_startcode(source, size, 0);
	if (position != 0) {
		length = -EINVAL;
		goto out;
	}
	while (position < size) {
		nal = position + (source[position + 2] == 1 ? 3 : 4);
		if (nal >= size) {
			length = -EINVAL;
			goto out;
		}
		next = vce_liverpool_startcode(source, size, nal + 1);
		if ((source[nal] & 31) == 7) {
			length = vce_liverpool_fix_sps(source + nal, next - nal, sps, sizeof(sps),
				session->width, session->height, job->fps_num, job->fps_den, session->profile);
			if (length < 0)
				goto out;
			if (copied + nal - position + length > job->output_size) {
				length = -ENOSPC;
				goto out;
			}
			memcpy_toio(job->output + copied, source + position, nal - position);
			copied += nal - position;
			memcpy_toio(job->output + copied, sps, length);
			copied += length;
		} else {
			if (next - position > job->output_size - copied) {
				length = -ENOSPC;
				goto out;
			}
			memcpy_toio(job->output + copied, source + position, next - position);
			copied += next - position;
		}
		position = next;
	}
	length = copied;
out:
	kvfree(source);
	return length;
}

static void vce_liverpool_finish(struct work_struct *work)
{
	struct vce_liverpool_job *job = container_of(work, struct vce_liverpool_job, work);
	struct vce_liverpool_session *session = job->session;
	u32 producer, start, bytes, padding, i;
	int error = dma_fence_get_status(job->hardware), length = 0;

	if (READ_ONCE(job->retain))
		return;
	if (error > 0)
		error = 0;
	else
		job->retain = true;
	if (!error && job->encode) {
		void *metadata = session->cpu + session->metadata;
		void *record = metadata + 8 + job->record * 512;

		for (i = 0; i < 10000; i++) {
			producer = readl(metadata + 4);
			bytes = readl(record + 0x10);
			padding = readl(record + 0x1c);
			if (producer != job->record && bytes > padding)
				break;
			usleep_range(100, 150);
		}
		if (i == 10000) {
			error = -ETIMEDOUT;
			job->retain = true;
		} else {
			start = readl(record + 0xc);
			length = vce_liverpool_copy_output(job, start, bytes - padding);
			if (length < 0)
				error = length;
		}
		if (i != 10000) {
			writel(0, record + 0x10);
			writel((job->record + 1) % 5, metadata);
		}
	}
	if (error && session) {
		WRITE_ONCE(session->failed, true);
		dev_err(job->adev->dev, "VCE output completion failed (%d)\n", error);
	}
	if (job->retain)
		WRITE_ONCE(job->adev->vce.native_stop_failed, true);
	if (job->feedback) {
		memset_io(job->feedback, 0, 40);
		writel(error ? 0 : length, job->feedback + 16);
		wmb();
		writel(error ? 0 : 1, job->feedback + 4);
	}
	amdgpu_device_flush_hdp(job->adev, NULL);
	if (error)
		dma_fence_set_error(&job->fence, error);
	dma_fence_signal(&job->fence);
	if (!job->retain)
		dma_fence_put(&job->fence);
}

static void vce_liverpool_hardware_done(struct dma_fence *fence, struct dma_fence_cb *callback)
{
	struct vce_liverpool_job *job = container_of(callback, struct vce_liverpool_job, callback);

	queue_work(job->adev->vce.native_workqueue, &job->work);
}

struct dma_fence *vce_liverpool_complete_job(struct amdgpu_job *job, struct dma_fence *hardware)
{
	struct vce_liverpool_job *native = job->vce_native;

	if (native->armed) {
		dma_fence_put(hardware);
		return dma_fence_get(&native->fence);
	}
	native->armed = true;
	native->hardware = hardware;
	dma_fence_get(&native->fence);
	if (dma_fence_add_callback(hardware, &native->callback, vce_liverpool_hardware_done))
		queue_work(native->adev->vce.native_workqueue, &native->work);
	return dma_fence_get(&native->fence);
}

void vce_liverpool_free_job(struct amdgpu_job *job)
{
	if (job->vce_native) {
		if (!job->vce_native->armed) {
			dma_fence_set_error(&job->vce_native->fence, -ECANCELED);
			dma_fence_signal(&job->vce_native->fence);
			if (job->vce_native->session)
				WRITE_ONCE(job->vce_native->session->failed, true);
		}
		dma_fence_put(&job->vce_native->fence);
		job->vce_native = NULL;
	}
}

bool vce_liverpool_cpu_job(struct amdgpu_job *job)
{
	return job->vce_native && job->vce_native->control_only;
}

bool vce_liverpool_job_timeout(struct amdgpu_job *job)
{
	struct vce_liverpool_job *native = job->vce_native;

	if (!native)
		return false;
	WRITE_ONCE(native->retain, true);
	WRITE_ONCE(native->adev->vce.native_stop_failed, true);
	dma_fence_set_error(&native->fence, -ETIMEDOUT);
	dma_fence_signal(&native->fence);
	dev_err(native->adev->dev, "VCE disabled after timeout, retaining DMA buffers\n");
	return true;
}

static int vce_liverpool_find_buffer(struct amdgpu_cs_parser *p, u64 address,
		u64 size, struct amdgpu_bo **bo, u64 *offset)
{
	struct amdgpu_bo_va_mapping *mapping;
	u64 start, bo_size;
	int r;

	if (!address || !size || address + size < address)
		return -EINVAL;
	r = amdgpu_cs_find_mapping(p, address, bo, &mapping);
	if (r)
		return r;
	if (address + size > (mapping->last + 1) * AMDGPU_GPU_PAGE_SIZE)
		return -EINVAL;
	start = address - mapping->start * AMDGPU_GPU_PAGE_SIZE;
	bo_size = amdgpu_bo_size(*bo);
	if (mapping->offset > bo_size || start > bo_size - mapping->offset)
		return -EINVAL;
	*offset = mapping->offset + start;
	if (size > bo_size - *offset)
		return -EINVAL;
	return 0;
}

static int vce_liverpool_map(struct amdgpu_cs_parser *p, u64 address, u32 size,
		struct amdgpu_bo **reference, void **cpu)
{
	struct amdgpu_bo *bo;
	u64 offset;
	void *base;
	int r;

	r = vce_liverpool_find_buffer(p, address, size, &bo, &offset);
	if (r)
		return r;
	r = amdgpu_bo_kmap(bo, &base);
	if (r)
		return r;
	*reference = amdgpu_bo_ref(bo);
	*cpu = base + offset;
	return 0;
}

static u64 vce_liverpool_address(const u32 *words)
{
	return ((u64)words[0] << 32) | words[1];
}

static void vce_liverpool_set_address(u32 *words, u64 address)
{
	words[0] = upper_32_bits(address);
	words[1] = lower_32_bits(address);
}

static bool vce_liverpool_packet(u32 command, u32 length)
{
	switch (command) {
	case 1: return length == 12;
	case 2: return length == 32;
	case 0x01000001: return length == 48;
	case 0x02000001: return length == 8;
	case 0x03000001: return length == 352;
	case 0x04000001: return length == 12;
	case 0x04000002: return length == 116;
	case 0x04000005: return length == 104 || length == 112;
	case 0x04000007: return length == 104;
	case 0x04000008: return length == 76;
	case 0x04000009: return length >= 8 && length <= 256;
	case 0x05000001: return length == 16;
	case 0x05000002: return length == 72;
	case 0x05000004: return length == 20;
	case 0x05000005: return length == 20;
	default: return false;
	}
}

static struct vce_liverpool_session *vce_liverpool_find_session(struct amdgpu_device *adev,
		u32 handle, struct drm_file *filp, unsigned int *slot)
{
	unsigned int i;

	for (i = 0; i < AMDGPU_MAX_VCE_HANDLES; i++) {
		struct vce_liverpool_session *session = adev->vce.native_sessions[i];

		if (session && session->handle == handle && session->filp == filp) {
			*slot = i;
			return session;
		}
	}
	return NULL;
}

static int vce_liverpool_allocate_session(struct vce_liverpool_session *session)
{
	u32 frame, allocation;
	int r;

	session->pitch = ALIGN(session->width, 64);
	frame = session->pitch * ALIGN(session->height, 32) * 3 / 2;
	session->metadata = ALIGN(frame * 2, 256);
	session->bitstream = session->metadata + 0xb00;
	session->header = session->bitstream + VCE_NATIVE_POOL_SIZE;
	allocation = PAGE_ALIGN(session->header + 4096);
	r = amdgpu_bo_create_kernel(session->adev, allocation, PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM,
		&session->bo, &session->gpu, &session->cpu);
	if (r)
		return r;
	if (session->gpu >= (1ULL << 40) || allocation > (1ULL << 40) - session->gpu) {
		amdgpu_bo_free_kernel(&session->bo, &session->gpu, &session->cpu);
		return -ERANGE;
	}
	memset_io(session->cpu, 0, allocation);
	return 0;
}

int vce_liverpool_parse_cs(struct amdgpu_cs_parser *p, struct amdgpu_job *job,
		struct amdgpu_ib *ib)
{
	struct amdgpu_device *adev = p->adev;
	struct vce_liverpool_session *session = NULL;
	struct vce_liverpool_job *native;
	struct amdgpu_ib translated = {};
	u32 *original, *words, index, handle, slot = 0, count = 0;
	u32 create = 0, encode = 0, destroy = 0, bitstream = 0, feedback = 0;
	u32 picture = 0, rate = 0;
	bool allocated = false, legacy_parsed = false;
	int r = -EINVAL;

	if (job->num_ibs != 1 || job->vce_native || ib->length_dw < 3 ||
	    ib->length_dw > 4096 || adev->vce.native_stop_failed)
		return -EINVAL;
	original = kmalloc_array(ib->length_dw, sizeof(*original), GFP_KERNEL);
	if (!original)
		return -ENOMEM;
	for (index = 0; index < ib->length_dw; index++)
		original[index] = amdgpu_ib_get_value(ib, index);
	if (original[0] != 12 || original[1] != 1 || !original[2])
		goto free_original;
	handle = original[2];
	for (index = 0; index < ib->length_dw;) {
		u32 length, command;

		if (ib->length_dw - index < 2)
			goto free_original;
		length = original[index];
		command = original[index + 1];
		if (length < 8 || (length & 3) || length / 4 > ib->length_dw - index ||
		    !vce_liverpool_packet(command, length)) {
			dev_err_ratelimited(adev->dev, "VCE unsupported command %08x length %u\n",
				command, length);
			goto free_original;
		}
		if (vce_liverpool_trace)
			dev_info(adev->dev, "VCE command %08x length %u data %*ph\n",
				command, length, min_t(u32, length, 64), original + index);
		switch (command) {
		case 1:
			if (index || original[index + 2] != handle)
				goto free_original;
			break;
		case 2:
			if (original[index + 6] > 0 && original[index + 6] != ~0u)
				goto free_original;
			if (original[index + 7])
				goto free_original;
			break;
		case 0x01000001: if (create) goto free_original; create = index; break;
		case 0x02000001: if (destroy) goto free_original; destroy = index; break;
		case 0x03000001: if (encode) goto free_original; encode = index; break;
		case 0x04000002: picture = index; break;
		case 0x04000005: rate = index; break;
		case 0x05000004: bitstream = index; break;
		case 0x05000005: feedback = index; break;
		}
		index += length / 4;
	}
	if (encode && (!bitstream || !feedback || destroy))
		goto free_original;
	r = amdgpu_vce_ring_parse_cs_legacy(p, job, ib);
	if (r)
		goto free_original;
	legacy_parsed = true;
	native = kzalloc(sizeof(*native), GFP_KERNEL);
	if (!native) {
		r = -ENOMEM;
		goto free_original;
	}
	spin_lock_init(&native->lock);
	dma_fence_init(&native->fence, &vce_liverpool_fence_ops, &native->lock,
		dma_fence_context_alloc(1), 1);
	INIT_WORK(&native->work, vce_liverpool_finish);
	INIT_WORK(&native->release, vce_liverpool_release_job);
	native->adev = adev;
	job->vce_native = native;
	mutex_lock(&adev->vce.native_mutex);
	session = vce_liverpool_find_session(adev, handle, p->filp, &slot);
	if (create) {
		if (session) {
			r = -EEXIST;
			goto unlock;
		}
		for (slot = 0; slot < AMDGPU_MAX_VCE_HANDLES; slot++)
			if (!adev->vce.native_sessions[slot])
				break;
		if (slot == AMDGPU_MAX_VCE_HANDLES) {
			r = -ENOSPC;
			goto unlock;
		}
		session = kzalloc(sizeof(*session), GFP_KERNEL);
		if (!session) {
			r = -ENOMEM;
			goto unlock;
		}
		kref_init(&session->ref);
		session->adev = adev;
		session->filp = p->filp;
		session->handle = handle;
		session->native_handle = atomic_inc_return(&vce_liverpool_next_handle);
		session->profile = original[create + 3];
		session->level = original[create + 4];
		session->width = original[create + 6];
		session->padded_height = original[create + 7];
		session->fps_num = 30;
		session->fps_den = 1;
		session->qp_i = session->qp_p = 20;
		session->record = 0;
		adev->vce.native_sessions[slot] = session;
		allocated = true;
	}
	if (!session || (session->failed && !destroy)) {
		r = -EIO;
		goto unlock;
	}
	if (picture) {
		if (session->started && memcmp(session->crop, original + picture + 8,
				sizeof(session->crop))) {
			r = -EOPNOTSUPP;
			goto unlock;
		}
		for (index = 0; index < 4; index++)
			session->crop[index] = original[picture + 8 + index];
		if (original[picture + 21] || original[picture + 24] > 1) {
			r = -EOPNOTSUPP;
			goto unlock;
		}
	}
	if (rate) {
		if (original[rate + 2]) {
			r = -EOPNOTSUPP;
			goto unlock;
		}
		session->fps_num = original[rate + 5] ?: 30;
		session->fps_den = original[rate + 11] ?: 1;
		session->qp_i = original[rate + 7];
		session->qp_p = original[rate + 8];
	}
	if (session->profile != 66 && session->profile != 77 && session->profile != 100) {
		r = -EOPNOTSUPP;
		goto unlock;
	}
	if (session->crop[0] || session->crop[1] || session->crop[2] ||
	    session->crop[3] > 7 || session->padded_height < session->crop[3] * 2) {
		r = -EINVAL;
		goto unlock;
	}
	session->height = session->padded_height - session->crop[3] * 2;
	if (session->width < 64 || session->width > 1920 || (session->width & 15) ||
	    session->height < 64 || session->height > 1080 || (session->height & 1) ||
	    session->qp_i > 51 || session->qp_p > 51 || !session->fps_num ||
	    session->fps_num > 1000000 || !session->fps_den || session->fps_den > 1000000) {
		r = -EINVAL;
		goto unlock;
	}
	native->session = session;
	kref_get(&session->ref);
	native->fps_num = session->fps_num;
	native->fps_den = session->fps_den;
	if (session->tail) {
		struct dma_fence *dependency = dma_fence_get(session->tail);

		r = drm_sched_job_add_dependency(&job->base, dependency);
		if (r)
			goto unlock;
	}
	if (feedback) {
		r = vce_liverpool_map(p, vce_liverpool_address(original + feedback + 2),
			40, &native->feedback_bo, &native->feedback);
		if (r)
			goto unlock;
	}
	r = amdgpu_ib_get(adev, NULL, 4096, AMDGPU_IB_POOL_DELAYED, &translated);
	if (r)
		goto unlock;
	words = translated.ptr;
	if (encode) {
		u32 type = original[encode + 18];
		u32 qp_i = session->qp_i ?: session->qp_p;
		u32 qp_p = session->qp_p ?: session->qp_i;
		u32 frame_rate = session->fps_num / session->fps_den;
		u32 *commands;
		u64 input_y, input_uv, luma_size, chroma_size;
		struct amdgpu_bo *input_bo, *chroma_bo;

		if ((type != 0 && type != 2 && type != 3) || original[encode + 3] ||
		    original[encode + 16] || original[encode + 17] ||
		    original[encode + 13] < ALIGN(session->height, 16) ||
		    original[encode + 14] < session->width ||
		    original[encode + 14] > 16384 ||
		    original[encode + 14] != original[encode + 15]) {
			r = -EOPNOTSUPP;
			goto free_translated;
		}
		if (!frame_rate) {
			r = -ERANGE;
			goto free_translated;
		}
		if ((!type && !session->qp_p) || (type && !session->qp_i) ||
		    (!session->started && !type)) {
			r = -EOPNOTSUPP;
			goto free_translated;
		}
		if (!session->bo) {
			r = vce_liverpool_allocate_session(session);
			if (r)
				goto free_translated;
		}
		luma_size = (u64)original[encode + 14] * ALIGN(session->height, 16);
		chroma_size = luma_size / 2;
		r = vce_liverpool_find_buffer(p, vce_liverpool_address(original + encode + 9),
			luma_size, &input_bo, &input_y);
		if (r)
			goto free_translated;
		r = vce_liverpool_find_buffer(p, vce_liverpool_address(original + encode + 11),
			chroma_size, &chroma_bo, &input_uv);
		if (r || chroma_bo != input_bo) {
			r = r ?: -EOPNOTSUPP;
			goto free_translated;
		}
		r = amdgpu_bo_pin(input_bo,
			amdgpu_mem_type_to_domain(input_bo->tbo.resource->mem_type));
		if (r)
			goto free_translated;
		native->input_bo = amdgpu_bo_ref(input_bo);
		native->input_pinned = true;
		input_y += amdgpu_bo_gpu_offset(input_bo);
		input_uv += amdgpu_bo_gpu_offset(input_bo);
		if (input_y >= (1ULL << 40) || luma_size > (1ULL << 40) - input_y ||
		    input_uv >= (1ULL << 40) || chroma_size > (1ULL << 40) - input_uv) {
			r = -ERANGE;
			goto free_translated;
		}
		native->output_size = original[bitstream + 4];
		r = vce_liverpool_map(p, vce_liverpool_address(original + bitstream + 2),
			native->output_size, &native->output_bo, &native->output);
		if (r)
			goto free_translated;
		if (!session->started) {
			memcpy(words, vce_liverpool_create, sizeof(vce_liverpool_create));
			words[2] = session->native_handle;
			words[6] = session->profile == 100 ? 77 : session->profile;
			words[7] = session->level;
			words[11] = session->width;
			words[12] = session->height;
			words[13] = words[14] = session->pitch;
			words[15] = ALIGN(session->height, 32) / 8;
			vce_liverpool_set_address(words + 17, session->gpu + session->bitstream);
			vce_liverpool_set_address(words + 20, session->gpu + session->metadata);
			vce_liverpool_set_address(words + 48, session->gpu + session->header);
			vce_liverpool_set_address(words + 53, session->gpu + session->header);
			count = ARRAY_SIZE(vce_liverpool_create);
			session->started = true;
		}
		commands = words + count;
		memcpy(commands, vce_liverpool_encode, sizeof(vce_liverpool_encode));
		commands[2] = session->native_handle;
		commands[6] = session->profile != 66;
		commands[8] = 0;
		commands[14] = commands[15] = commands[16] = commands[17] = 0;
		commands[18] = DIV_ROUND_UP(session->width, 16) * DIV_ROUND_UP(session->height, 16);
		commands[30] = commands[18];
		commands[59] = session->fps_num;
		commands[61] = qp_i;
		commands[62] = qp_p;
		commands[65] = session->fps_den;
		commands[70] = commands[57] / frame_rate;
		commands[74] = qp_i;
		commands[76] = qp_p;
		commands[266] = fls(max(session->width, session->height) - 1) + 3;
		commands[267] = commands[266];
		commands[295] = type;
		commands[299] = original[encode + 81];
		commands[300] = original[encode + 82];
		vce_liverpool_set_address(commands + 301, input_y);
		vce_liverpool_set_address(commands + 303, input_uv);
		vce_liverpool_set_address(commands + 305, session->gpu);
		commands[307] = session->height;
		commands[308] = original[encode + 14];
		commands[309] = original[encode + 15];
		vce_liverpool_set_address(commands + 312, session->gpu + session->bitstream);
		vce_liverpool_set_address(commands + 314, session->gpu + session->metadata);
		vce_liverpool_set_address(commands + 316, session->gpu + session->header);
		vce_liverpool_set_address(commands + 318, session->gpu + session->header);
		count += ARRAY_SIZE(vce_liverpool_encode);
		native->encode = true;
		native->record = session->record;
		session->record = (session->record + 1) % 5;
	} else if (destroy && session->started) {
		u32 destroy_words[] = {12, 1, session->native_handle, 8, 5};

		memcpy(words, destroy_words, sizeof(destroy_words));
		count = ARRAY_SIZE(destroy_words);
	} else {
		words[0] = 8;
		words[1] = 0;
		count = 2;
		native->control_only = true;
	}
	translated.length_dw = count;
	amdgpu_ib_free(ib, NULL);
	*ib = translated;
	dma_fence_put(session->tail);
	session->tail = destroy ? NULL : dma_fence_get(&native->fence);
	if (destroy) {
		adev->vce.native_sessions[slot] = NULL;
		kref_put(&session->ref, vce_liverpool_session_release);
	}
	r = 0;
	goto unlock;

free_translated:
	amdgpu_ib_free(&translated, NULL);
unlock:
	if (r && allocated) {
		adev->vce.native_sessions[slot] = NULL;
		kref_put(&session->ref, vce_liverpool_session_release);
	}
	mutex_unlock(&adev->vce.native_mutex);
	if (r)
		vce_liverpool_free_job(job);
free_original:
	if (r)
		dev_err_ratelimited(adev->dev, "VCE command translation rejected (%d)\n", r);
	if (r && create && legacy_parsed) {
		for (index = 0; index < AMDGPU_MAX_VCE_HANDLES; index++) {
			if (atomic_read(&adev->vce.handles[index]) == handle &&
			    adev->vce.filp[index] == p->filp)
				atomic_set(&adev->vce.handles[index], 0);
		}
	}
	kfree(original);
	return r;
}

int vce_liverpool_destroy_session(struct amdgpu_device *adev, u32 handle, u32 *words,
		struct amdgpu_job *job)
{
	struct vce_liverpool_session *session = NULL;
	unsigned int i;
	int count = -ENOENT;

	words[0] = 8;
	words[1] = 0;
	mutex_lock(&adev->vce.native_mutex);
	for (i = 0; i < AMDGPU_MAX_VCE_HANDLES; i++) {
		session = adev->vce.native_sessions[i];
		if (!session || session->handle != handle)
			continue;
		count = 2;
		if (session->started && !adev->vce.native_stop_failed) {
			words[0] = 12;
			words[1] = 1;
			words[2] = session->native_handle;
			words[3] = 8;
			words[4] = 5;
			count = 5;
		}
		job->vce_native = kzalloc(sizeof(*job->vce_native), GFP_KERNEL);
		if (!job->vce_native) {
			count = -ENOMEM;
			break;
		}
		spin_lock_init(&job->vce_native->lock);
		dma_fence_init(&job->vce_native->fence, &vce_liverpool_fence_ops,
			&job->vce_native->lock, dma_fence_context_alloc(1), 1);
		INIT_WORK(&job->vce_native->work, vce_liverpool_finish);
		INIT_WORK(&job->vce_native->release, vce_liverpool_release_job);
		job->vce_native->adev = adev;
		job->vce_native->session = session;
		job->vce_native->control_only = count == 2;
		if (session->tail) {
			struct dma_fence *dependency = dma_fence_get(session->tail);
			int r = drm_sched_job_add_dependency(&job->base, dependency);

			if (r) {
				job->vce_native->session = NULL;
				count = r;
				break;
			}
			dma_fence_put(session->tail);
			session->tail = NULL;
		}
		adev->vce.native_sessions[i] = NULL;
		break;
	}
	mutex_unlock(&adev->vce.native_mutex);
	return count;
}

int vce_liverpool_test_ib(struct amdgpu_ring *ring, long timeout)
{
	struct amdgpu_device *adev = ring->adev;
	struct amdgpu_bo *bo = NULL;
	struct amdgpu_job *job;
	struct dma_fence *fence = NULL;
	void *cpu;
	u64 gpu;
	u32 *words;
	int r;
	long waited;

	r = amdgpu_bo_create_kernel(adev, 3 * PAGE_SIZE, PAGE_SIZE, AMDGPU_GEM_DOMAIN_VRAM,
		&bo, &gpu, &cpu);
	if (r)
		return r;
	memset_io(cpu, 0, 3 * PAGE_SIZE);
	r = amdgpu_job_alloc_with_ib(adev, &adev->vce.entity, AMDGPU_FENCE_OWNER_UNDEFINED,
		4096, AMDGPU_IB_POOL_DIRECT, &job, AMDGPU_KERNEL_JOB_ID_VCN_RING_TEST);
	if (r)
		goto free_bo;
	words = job->ibs[0].ptr;
	memcpy(words, vce_liverpool_create, sizeof(vce_liverpool_create));
	words[2] = 1;
	words[6] = 66;
	words[11] = 160;
	words[12] = 128;
	words[13] = words[14] = 192;
	words[15] = 16;
	vce_liverpool_set_address(words + 17, gpu + PAGE_SIZE);
	words[19] = PAGE_SIZE;
	vce_liverpool_set_address(words + 20, gpu);
	vce_liverpool_set_address(words + 48, gpu + 2 * PAGE_SIZE);
	vce_liverpool_set_address(words + 53, gpu + 2 * PAGE_SIZE);
	words += ARRAY_SIZE(vce_liverpool_create);
	words[0] = 12;
	words[1] = 1;
	words[2] = 1;
	words[3] = 8;
	words[4] = 5;
	job->ibs[0].length_dw = ARRAY_SIZE(vce_liverpool_create) + 5;
	r = amdgpu_job_submit_direct(job, ring, &fence);
	if (r) {
		amdgpu_job_free(job);
		goto free_bo;
	}
	waited = dma_fence_wait_timeout(fence, false, timeout);
	if (waited <= 0) {
		adev->vce.native_stop_failed = true;
		r = waited ?: -ETIMEDOUT;
		dma_fence_put(fence);
		return r;
	}
	r = dma_fence_get_status(fence);
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
