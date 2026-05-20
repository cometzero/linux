/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __APOLLO_HEXAGON_H__
#define __APOLLO_HEXAGON_H__

#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/container_of.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kconfig.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <linux/xarray.h>

#include <drm/apollo_hexagon_accel.h>
#include <drm/drm_drv.h>

struct drm_file;

struct apollo_hexagon_file {
	struct xarray contexts;
	struct xarray executables;
	struct drm_apollo_hexagon_fault last_fault;
	struct mutex lock;
	bool has_fault;
};

struct apollo_hexagon_context {
	struct xarray bindings;
	u32 handle;
	u32 generic_abi_version;
	u64 next_iova_offset;
};

struct apollo_hexagon_bo_binding {
	struct drm_gem_object *obj;
	u64 iova;
	u64 offset;
	u64 length;
	u32 handle;
	u32 bo_handle;
	u32 usage;
};

#define APOLLO_HEXAGON_ACCEL_TIMEOUT_MS		2000
#define APOLLO_HEXAGON_ACCEL_POLL_MS		10
#define APOLLO_HEXAGON_REG_CAPS			0x1c
#define APOLLO_HEXAGON_REG_PATH			0x20
#define APOLLO_HEXAGON_REG_STREAM_ID		0x24
#define APOLLO_HEXAGON_REG_JOB_INPUT		0x40
#define APOLLO_HEXAGON_REG_JOB_OUTPUT		0x44
#define APOLLO_HEXAGON_REG_JOB_INPUT_BYTES	0x48
#define APOLLO_HEXAGON_REG_JOB_OUTPUT_BYTES	0x4c
#define APOLLO_HEXAGON_REG_JOB_CTRL		0x50
#define APOLLO_HEXAGON_REG_JOB_STATUS		0x54
#define APOLLO_HEXAGON_REG_JOB_RESULT		0x58
#define APOLLO_HEXAGON_REG_JOB_QUEUE		0x5c
#define APOLLO_HEXAGON_REG_JOB_FENCE		0x60
#define APOLLO_HEXAGON_REG_IRQ_STATUS		0x64
#define APOLLO_HEXAGON_REG_IRQ_ACK		0x68
#define APOLLO_HEXAGON_REG_QUEUE_CAPS		0x6c
#define APOLLO_HEXAGON_REG_CMDQ_BASE_LO		0x80
#define APOLLO_HEXAGON_REG_CMDQ_BASE_HI		0x84
#define APOLLO_HEXAGON_REG_CMDQ_SIZE		0x88
#define APOLLO_HEXAGON_REG_CMDQ_HEAD		0x8c
#define APOLLO_HEXAGON_REG_CMDQ_TAIL		0x90
#define APOLLO_HEXAGON_REG_CMDQ_DOORBELL	0x94
#define APOLLO_HEXAGON_REG_CMDQ_STATUS		0x98
#define APOLLO_HEXAGON_REG_CMDQ_FENCE_VALUE	0x9c
#define APOLLO_HEXAGON_REG_CMDQ_FAULT_CODE	0xa0
#define APOLLO_HEXAGON_REG_CMDQ_FAULT_ADDR_LO	0xa4
#define APOLLO_HEXAGON_REG_CMDQ_FAULT_ADDR_HI	0xa8
#define APOLLO_HEXAGON_CAP_COPY_ENGINE		BIT(0)
#define APOLLO_HEXAGON_CAP_DIRECT_TLM		BIT(1)
#define APOLLO_HEXAGON_CAP_SMMU_TRANSLATED	BIT(2)
#define APOLLO_HEXAGON_CAP_LARGE_TENSOR		BIT(3)
#define APOLLO_HEXAGON_CAP_MULTI_QUEUE		BIT(4)
#define APOLLO_HEXAGON_CAP_ASYNC_FENCE		BIT(5)
#define APOLLO_HEXAGON_PATH_DIRECT_TLM		1
#define APOLLO_HEXAGON_PATH_SMMU_TRANSLATED	2
#define APOLLO_HEXAGON_QUEUE_DMA		0
#define APOLLO_HEXAGON_QUEUE_CNN		1
#define APOLLO_HEXAGON_QUEUE_VADD		1
#define APOLLO_HEXAGON_JOB_CTRL_START		1
#define APOLLO_HEXAGON_JOB_STATUS_DONE		1
#define APOLLO_HEXAGON_JOB_STATUS_ERROR		2
#define APOLLO_HEXAGON_JOB_RESULT_OK		0x434e4e4f
#define APOLLO_HEXAGON_JOB_RESULT_VADD_OK	0x56414444
#define APOLLO_HEXAGON_JOB_RESULT_MNIST_OK	0x4d4e4953
#define APOLLO_HEXAGON_JOB_RESULT_SG_OK		0x53474f4b
#define APOLLO_HEXAGON_INPUT_OFFSET		0x10000
#define APOLLO_HEXAGON_OUTPUT_OFFSET		0x11000
#define APOLLO_HEXAGON_CMDQ_OFFSET		0x12000
#define APOLLO_HEXAGON_CMDQ_SIZE		0x1000
#define APOLLO_HEXAGON_STRESS_INPUT_BASE	0x20000
#define APOLLO_HEXAGON_STRESS_OUTPUT_BASE	0x80000
#define APOLLO_HEXAGON_STRESS_SEGMENT_STRIDE	0x8000
#define APOLLO_HEXAGON_BIND_IOVA_OFFSET		0x300000
#define APOLLO_HEXAGON_TBU_REG_OFFSET		0x1000
#define APOLLO_TBU_REG_MAP_IOVA_LO		0x00
#define APOLLO_TBU_REG_MAP_IOVA_HI		0x04
#define APOLLO_TBU_REG_MAP_PA_LO		0x08
#define APOLLO_TBU_REG_MAP_PA_HI		0x0c
#define APOLLO_TBU_REG_MAP_SIZE_LO		0x10
#define APOLLO_TBU_REG_MAP_SIZE_HI		0x14
#define APOLLO_TBU_REG_MAP_CTRL			0x18
#define APOLLO_TBU_REG_MAP_STATUS		0x1c
#define APOLLO_TBU_REG_MAP_COUNT		0x20
#define APOLLO_TBU_MAP_CTRL_ADD			1
#define APOLLO_TBU_MAP_CTRL_REMOVE		2
#define APOLLO_TBU_MAP_CTRL_CLEAR		3
#define APOLLO_TBU_MAP_STATUS_OK		1
#define APOLLO_HEXAGON_STRESS_SEED		0x13579bdf

struct apollo_hexagon {
	struct drm_device drm;
	void __iomem *regs;
	void __iomem *tbu_regs;
	void __iomem *shared_base;
	struct device *dev;
	phys_addr_t shared_phys;
	resource_size_t shared_size;
	u64 dma_iova_base;
	u64 requested_dma_iova_base;
	u64 dma_window_size;
	dma_addr_t linux_iommu_iova;
	size_t linux_iommu_size;
	u32 stream_id;
	bool primary_endpoint;
	bool linux_iommu_mapped;
	int doorbell_irq;
	atomic_t async_irq_pending;
	struct completion async_fence;
	struct mutex lock;
};

static inline struct apollo_hexagon *to_apollo_hexagon(struct drm_device *drm)
{
	return container_of(drm, struct apollo_hexagon, drm);
}

void apollo_tbu_write64(void __iomem *base, u32 lo, u32 hi, u64 value);
int apollo_hexagon_tbu_ctrl(struct device *dev, struct apollo_hexagon *test,
			    u32 ctrl);
int apollo_hexagon_tbu_map(struct device *dev, struct apollo_hexagon *test,
			   u64 iova, phys_addr_t pa, u64 size);
void apollo_hexagon_prepare_async_fence(struct apollo_hexagon *test,
					u32 queue_id);
int apollo_hexagon_configure_linux_iommu(struct device *dev,
						struct apollo_hexagon *test);
int apollo_hexagon_dynamic_map(struct device *dev,
			       struct apollo_hexagon *test);
int apollo_hexagon_wait_job(struct device *dev, struct apollo_hexagon *test,
			    u32 queue_id, u32 expected_result,
			    u32 *fence_seq, u32 *final_status,
			    u32 *final_result);
int apollo_hexagon_file_open(struct drm_device *dev, struct drm_file *file);
void apollo_hexagon_file_postclose(struct drm_device *dev,
				   struct drm_file *file);
void apollo_hexagon_context_free(struct apollo_hexagon_context *ctx);
bool apollo_hexagon_context_iova_bound(struct apollo_hexagon_context *ctx,
				       u64 iova, u32 bytes,
				       u32 required_usage);
int apollo_hexagon_context_copy_from_iova(struct apollo_hexagon_context *ctx,
					  u64 iova, u32 bytes,
					  u32 required_usage, void *dst);
int apollo_hexagon_context_copy_to_iova(struct apollo_hexagon_context *ctx,
					u64 iova, u32 bytes,
					u32 required_usage, const void *src);
int apollo_hexagon_context_get_iova_bo(struct apollo_hexagon_context *ctx,
				       u64 iova, u32 bytes,
				       u32 required_usage,
				       struct drm_gem_object **obj,
				       u64 *obj_offset);
int apollo_hexagon_bo_copy_to(struct drm_gem_object *obj, u64 offset,
			      u32 bytes, const void *src);
int apollo_hexagon_ioctl_submit_cnn(struct drm_device *drm, void *data,
				    struct drm_file *file);
int apollo_hexagon_ioctl_submit_vadd(struct drm_device *drm, void *data,
				     struct drm_file *file);
int apollo_hexagon_ioctl_dma_stress(struct drm_device *drm, void *data,
				    struct drm_file *file);
int apollo_hexagon_ioctl_exec_create(struct drm_device *drm, void *data,
				     struct drm_file *file);
int apollo_hexagon_ioctl_exec_destroy(struct drm_device *drm, void *data,
				      struct drm_file *file);
int apollo_hexagon_ioctl_context_create(struct drm_device *drm, void *data,
					struct drm_file *file);
int apollo_hexagon_ioctl_context_destroy(struct drm_device *drm, void *data,
					 struct drm_file *file);
int apollo_hexagon_ioctl_bo_create(struct drm_device *drm, void *data,
				   struct drm_file *file);
int apollo_hexagon_ioctl_bo_destroy(struct drm_device *drm, void *data,
				    struct drm_file *file);
int apollo_hexagon_ioctl_bo_bind(struct drm_device *drm, void *data,
				 struct drm_file *file);
int apollo_hexagon_ioctl_bo_unbind(struct drm_device *drm, void *data,
				   struct drm_file *file);
int apollo_hexagon_ioctl_submit(struct drm_device *drm, void *data,
				struct drm_file *file);
int apollo_hexagon_ioctl_cmd_submit(struct drm_device *drm, void *data,
				    struct drm_file *file);
void apollo_hexagon_record_fault(struct apollo_hexagon_file *afile,
				 u32 queue_id, u32 status, u32 result,
				 u32 fence_seq);
int apollo_hexagon_ioctl_get_fault(struct drm_device *drm, void *data,
				   struct drm_file *file);
int apollo_hexagon_ioctl_wait(struct drm_device *drm, void *data,
			      struct drm_file *file);

#if IS_ENABLED(CONFIG_DRM_ACCEL_APOLLO_HEXAGON_SELFTEST)
int apollo_hexagon_run_selftests(struct device *dev,
				 struct apollo_hexagon *test);
int apollo_hexagon_run_dma_stress_selftests(struct device *dev,
					    struct apollo_hexagon *test);
#else
static inline int apollo_hexagon_run_selftests(struct device *dev,
					       struct apollo_hexagon *test)
{
	return 0;
}

static inline int apollo_hexagon_run_dma_stress_selftests(
	struct device *dev, struct apollo_hexagon *test)
{
	return 0;
}
#endif

#endif /* __APOLLO_HEXAGON_H__ */
