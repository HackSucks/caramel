#ifndef VCE_LIVERPOOL_H
#define VCE_LIVERPOOL_H

extern const struct amdgpu_ip_block_version vce_liverpool_ip_block;
#ifdef CONFIG_DRM_AMDGPU_CIK
int vce_liverpool_parse_cs(struct amdgpu_cs_parser *p, struct amdgpu_job *job,
		struct amdgpu_ib *ib);
struct dma_fence *vce_liverpool_complete_job(struct amdgpu_job *job,
		struct dma_fence *hardware);
void vce_liverpool_free_job(struct amdgpu_job *job);
bool vce_liverpool_job_timeout(struct amdgpu_job *job);
bool vce_liverpool_cpu_job(struct amdgpu_job *job);
int vce_liverpool_destroy_session(struct amdgpu_device *adev, u32 handle,
		u32 *words, struct amdgpu_job *job);
int vce_liverpool_test_ib(struct amdgpu_ring *ring, long timeout);
#else
static inline int vce_liverpool_parse_cs(struct amdgpu_cs_parser *p,
		struct amdgpu_job *job, struct amdgpu_ib *ib)
{
	return -EOPNOTSUPP;
}
static inline struct dma_fence *vce_liverpool_complete_job(struct amdgpu_job *job,
		struct dma_fence *hardware)
{
	return hardware;
}
static inline void vce_liverpool_free_job(struct amdgpu_job *job)
{
}
static inline bool vce_liverpool_job_timeout(struct amdgpu_job *job)
{
	return false;
}
static inline bool vce_liverpool_cpu_job(struct amdgpu_job *job)
{
	return false;
}
static inline int vce_liverpool_destroy_session(struct amdgpu_device *adev,
		u32 handle, u32 *words, struct amdgpu_job *job)
{
	return -EOPNOTSUPP;
}
static inline int vce_liverpool_test_ib(struct amdgpu_ring *ring, long timeout)
{
	return -EOPNOTSUPP;
}
#endif

#endif
