#ifndef VCE_GLADIUS_H
#define VCE_GLADIUS_H

extern const struct amdgpu_ip_block_version vce_gladius_ip_block;

#define VCE_GLADIUS_POOL_SIZE 0x400000u
#define VCE_GLADIUS_RECORD_SIZE 0x300u

struct vce_gladius_config {
	u32 handle, profile, level, width, height;
	u32 fps_num, fps_den, qp_i, qp_p;
	u32 max_bitstream;
	u64 cpb, descriptor, output, output1, feedback, bitstream;
};

unsigned int vce_gladius_emit_create(u32 *words, const struct vce_gladius_config *config);
unsigned int vce_gladius_emit_encode(u32 *words, const struct vce_gladius_config *config,
		u64 luma, u64 chroma, u32 pitch, u32 type, u32 frame_num, u32 poc,
		bool initial, bool rate_changed);
unsigned int vce_gladius_emit_destroy(u32 *words, u32 handle);
#ifdef CONFIG_DRM_AMDGPU_CIK
int vce_gladius_test_ib(struct amdgpu_ring *ring, long timeout);
#else
static inline int vce_gladius_test_ib(struct amdgpu_ring *ring, long timeout)
{
	return -EOPNOTSUPP;
}
#endif

#endif
