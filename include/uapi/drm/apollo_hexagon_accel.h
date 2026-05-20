/* SPDX-License-Identifier: MIT */
#ifndef _UAPI_DRM_APOLLO_HEXAGON_ACCEL_H
#define _UAPI_DRM_APOLLO_HEXAGON_ACCEL_H

#include "drm.h"
#include <linux/types.h>

#define APOLLO_HEXAGON_CNN_INPUT_WORDS 16
#define APOLLO_HEXAGON_CNN_OUTPUT_WORDS 4
#define APOLLO_HEXAGON_VADD_WORDS 4
#define APOLLO_HEXAGON_VADD_INPUT_WORDS (APOLLO_HEXAGON_VADD_WORDS * 2)
#define APOLLO_HEXAGON_VADD_OUTPUT_WORDS APOLLO_HEXAGON_VADD_WORDS
#define APOLLO_HEXAGON_MNIST_INPUT_WORDS (28 * 28)
#define APOLLO_HEXAGON_MNIST_OUTPUT_WORDS 10
#define APOLLO_HEXAGON_DMA_STRESS_BYTES 131072
#define APOLLO_HEXAGON_DMA_STRESS_SEGMENTS 8
#define APOLLO_HEXAGON_APKO_MAGIC 0x4f4b5041
#define APOLLO_HEXAGON_APKO_ABI_VERSION 0
#define APOLLO_HEXAGON_GENERIC_ABI_VERSION 1
#define APOLLO_HEXAGON_EXEC_FORMAT_APKO_V0 1
#define APOLLO_HEXAGON_EXEC_FORMAT_APKO_V0_BIT \
	(1u << APOLLO_HEXAGON_EXEC_FORMAT_APKO_V0)
#define APOLLO_HEXAGON_EXEC_KIND_CNN 1
#define APOLLO_HEXAGON_EXEC_KIND_VADD 2
#define APOLLO_HEXAGON_EXEC_KIND_MNIST 3
#define APOLLO_HEXAGON_FAULT_CODE_JOB_FAILED 1
#define APOLLO_HEXAGON_GET_FAULT_FLAG_CLEAR 1
#define APOLLO_HEXAGON_FENCE_MODEL_ASYNC_IRQ_POLL 1
#define APOLLO_HEXAGON_BO_BIND_USAGE_READ 1
#define APOLLO_HEXAGON_BO_BIND_USAGE_WRITE 2
#define APOLLO_HEXAGON_BO_BIND_USAGE_EXEC 4
#define APOLLO_HEXAGON_BO_BIND_USAGE_MASK \
	(APOLLO_HEXAGON_BO_BIND_USAGE_READ | \
	 APOLLO_HEXAGON_BO_BIND_USAGE_WRITE | \
	 APOLLO_HEXAGON_BO_BIND_USAGE_EXEC)
#define APOLLO_HEXAGON_CMDQ_PACKET_WORDS 8
#define APOLLO_HEXAGON_CMDQ_PACKET_BYTES \
	(APOLLO_HEXAGON_CMDQ_PACKET_WORDS * sizeof(__u32))
#define APOLLO_HEXAGON_CMDQ_SUBMIT_MAX_PACKETS 2
#define APOLLO_HEXAGON_CMDQ_SUBMIT_MAX_BYTES \
	(APOLLO_HEXAGON_CMDQ_SUBMIT_MAX_PACKETS * \
	 APOLLO_HEXAGON_CMDQ_PACKET_BYTES)
#define APOLLO_HEXAGON_CMDQ_OPCODE_NOP 0
#define APOLLO_HEXAGON_CMDQ_OPCODE_COPY 1
#define APOLLO_HEXAGON_CMDQ_OPCODE_BARRIER 2
#define APOLLO_HEXAGON_CMDQ_OPCODE_SIGNAL_FENCE 3
#define APOLLO_HEXAGON_CMDQ_OPCODE_DISPATCH 4
#define APOLLO_HEXAGON_CMDQ_OPCODE_LOAD_EXECUTABLE 5
#define APOLLO_HEXAGON_CMDQ_DISPATCH_EXEC_SLOT_FLAG 0x80000000u
#define APOLLO_HEXAGON_CMDQ_EXEC_SLOT_MAX 15
#define APOLLO_HEXAGON_CMDQ_DISPATCH_KIND_CNN 1
#define APOLLO_HEXAGON_CMDQ_DISPATCH_KIND_VADD 2
#define APOLLO_HEXAGON_CMDQ_DISPATCH_KIND_MNIST 3
#define APOLLO_HEXAGON_CMDQ_STATUS_DONE 1
#define APOLLO_HEXAGON_CMDQ_STATUS_ERROR 2
#define APOLLO_HEXAGON_CMDQ_FAULT_NONE 0
#define APOLLO_HEXAGON_CMDQ_FAULT_EMPTY 1
#define APOLLO_HEXAGON_CMDQ_FAULT_DISABLED 2
#define APOLLO_HEXAGON_CMDQ_FAULT_UNSUPPORTED_PACKET 3
#define APOLLO_HEXAGON_CMDQ_FAULT_MALFORMED_PACKET 4
#define APOLLO_HEXAGON_CMDQ_FAULT_DMA_ERROR 5

struct drm_apollo_hexagon_apko_header {
	__u32 magic;
	__u32 header_bytes;
	__u32 abi_version;
	__u32 executable_format;
	__u32 entry_kind;
	__u32 input_bytes;
	__u32 output_bytes;
	__u32 reserved[5];
};

struct drm_apollo_hexagon_query {
	__u32 stream_id;
	__u32 queue_count;
	__u32 capabilities;
	__u32 dma_path;
	__u32 primary_endpoint;
	__u32 pad;
};

struct drm_apollo_hexagon_query_caps {
	__u32 size;
	__u32 flags;
	__u32 generic_abi_version;
	__u32 supported_executable_formats;
	__u32 max_command_bytes;
	__u32 max_bindings_per_dispatch;
	__u32 max_queue_count;
	__u32 max_queue_depth;
	__u32 fence_model;
	__u32 smmu_page_granularity;
	__u32 coherency_flags;
	__u32 fault_record_size;
	__u32 reserved[4];
};

struct drm_apollo_hexagon_context_create {
	__u32 size;
	__u32 flags;
	__u32 generic_abi_version;
	__u32 handle;
	__u32 queue_count;
	__u32 fence_model;
	__u32 reserved[4];
};

struct drm_apollo_hexagon_context_destroy {
	__u32 handle;
	__u32 pad;
};

struct drm_apollo_hexagon_bo_create {
	__u32 size;
	__u32 flags;
	__u64 bo_size;
	__u32 handle;
	__u32 pad;
	__u64 mmap_offset;
	__u32 reserved[4];
};

struct drm_apollo_hexagon_bo_destroy {
	__u32 handle;
	__u32 pad;
};

struct drm_apollo_hexagon_bo_bind {
	__u32 size;
	__u32 flags;
	__u32 context_handle;
	__u32 bo_handle;
	__u64 offset;
	__u64 length;
	__u64 iova;
	__u32 bind_handle;
	__u32 usage;
	__u32 reserved[4];
};

struct drm_apollo_hexagon_bo_unbind {
	__u32 size;
	__u32 flags;
	__u32 context_handle;
	__u32 bind_handle;
	__u32 reserved[2];
};

struct drm_apollo_hexagon_cnn_job {
	__u32 input[APOLLO_HEXAGON_CNN_INPUT_WORDS];
	__u32 output[APOLLO_HEXAGON_CNN_OUTPUT_WORDS];
	__u32 status;
	__u32 flags;
	__u32 queue_id;
	__u32 fence_seq;
};

struct drm_apollo_hexagon_vadd_job {
	__u32 lhs[APOLLO_HEXAGON_VADD_WORDS];
	__u32 rhs[APOLLO_HEXAGON_VADD_WORDS];
	__u32 output[APOLLO_HEXAGON_VADD_OUTPUT_WORDS];
	__u32 status;
	__u32 flags;
	__u32 queue_id;
	__u32 fence_seq;
};

struct drm_apollo_hexagon_dma_stress_job {
	__u32 bytes;
	__u32 segment_bytes;
	__u32 seed;
	__u32 checksum;
	__u32 status;
	__u32 flags;
	__u32 queue_id;
	__u32 fence_seq;
};

struct drm_apollo_hexagon_exec_create {
	__u32 size;
	__u32 flags;
	__u64 data_ptr;
	__u32 data_size;
	__u32 handle;
	__u32 executable_format;
	__u32 abi_version;
	__u32 entry_kind;
	__u32 input_bytes;
	__u32 output_bytes;
	__u32 reserved[4];
};

struct drm_apollo_hexagon_exec_destroy {
	__u32 handle;
	__u32 pad;
};

struct drm_apollo_hexagon_submit {
	__u32 size;
	__u32 flags;
	__u32 executable_handle;
	__u32 queue_id;
	__u64 input_ptr;
	__u64 output_ptr;
	__u32 input_bytes;
	__u32 output_bytes;
	__u32 status;
	__u32 fence_seq;
	__u32 reserved[4];
};

struct drm_apollo_hexagon_cmd_submit {
	__u32 size;
	__u32 flags;
	__u32 context_handle;
	__u32 command_bo_handle;
	__u64 command_offset;
	__u32 command_size;
	__u32 queue_id;
	__u32 status;
	__u32 result;
	__u32 fence_seq;
	__u32 reserved[4];
};

struct drm_apollo_hexagon_fault {
	__u32 size;
	__u32 flags;
	__u32 queue_id;
	__u32 code;
	__u32 status;
	__u32 result;
	__u32 fence_seq;
	__u32 reserved[5];
};

struct drm_apollo_hexagon_wait {
	__u32 size;
	__u32 flags;
	__u32 queue_id;
	__u32 fence_seq;
	__u64 timeout_ns;
	__u32 status;
	__u32 result;
	__u32 current_fence_seq;
	__u32 reserved[3];
};

enum drm_apollo_hexagon_ioctl_id {
	DRM_APOLLO_HEXAGON_QUERY = 0,
	DRM_APOLLO_HEXAGON_SUBMIT_CNN,
	DRM_APOLLO_HEXAGON_SUBMIT_VADD,
	DRM_APOLLO_HEXAGON_DMA_STRESS,
	DRM_APOLLO_HEXAGON_EXEC_CREATE,
	DRM_APOLLO_HEXAGON_EXEC_DESTROY,
	DRM_APOLLO_HEXAGON_SUBMIT,
	DRM_APOLLO_HEXAGON_GET_FAULT,
	DRM_APOLLO_HEXAGON_QUERY_CAPS,
	DRM_APOLLO_HEXAGON_CONTEXT_CREATE,
	DRM_APOLLO_HEXAGON_CONTEXT_DESTROY,
	DRM_APOLLO_HEXAGON_BO_CREATE,
	DRM_APOLLO_HEXAGON_BO_DESTROY,
	DRM_APOLLO_HEXAGON_WAIT,
	DRM_APOLLO_HEXAGON_BO_BIND,
	DRM_APOLLO_HEXAGON_BO_UNBIND,
	DRM_APOLLO_HEXAGON_CMD_SUBMIT,
};

#define DRM_IOCTL_APOLLO_HEXAGON_QUERY \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APOLLO_HEXAGON_QUERY, \
		 struct drm_apollo_hexagon_query)
#define DRM_IOCTL_APOLLO_HEXAGON_SUBMIT_CNN \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APOLLO_HEXAGON_SUBMIT_CNN, \
		 struct drm_apollo_hexagon_cnn_job)
#define DRM_IOCTL_APOLLO_HEXAGON_SUBMIT_VADD \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APOLLO_HEXAGON_SUBMIT_VADD, \
		 struct drm_apollo_hexagon_vadd_job)
#define DRM_IOCTL_APOLLO_HEXAGON_DMA_STRESS \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APOLLO_HEXAGON_DMA_STRESS, \
		 struct drm_apollo_hexagon_dma_stress_job)
#define DRM_IOCTL_APOLLO_HEXAGON_EXEC_CREATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APOLLO_HEXAGON_EXEC_CREATE, \
		 struct drm_apollo_hexagon_exec_create)
#define DRM_IOCTL_APOLLO_HEXAGON_EXEC_DESTROY \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APOLLO_HEXAGON_EXEC_DESTROY, \
		 struct drm_apollo_hexagon_exec_destroy)
#define DRM_IOCTL_APOLLO_HEXAGON_SUBMIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APOLLO_HEXAGON_SUBMIT, \
		 struct drm_apollo_hexagon_submit)
#define DRM_IOCTL_APOLLO_HEXAGON_GET_FAULT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APOLLO_HEXAGON_GET_FAULT, \
		 struct drm_apollo_hexagon_fault)
#define DRM_IOCTL_APOLLO_HEXAGON_QUERY_CAPS \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APOLLO_HEXAGON_QUERY_CAPS, \
		 struct drm_apollo_hexagon_query_caps)
#define DRM_IOCTL_APOLLO_HEXAGON_CONTEXT_CREATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APOLLO_HEXAGON_CONTEXT_CREATE, \
		 struct drm_apollo_hexagon_context_create)
#define DRM_IOCTL_APOLLO_HEXAGON_CONTEXT_DESTROY \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APOLLO_HEXAGON_CONTEXT_DESTROY, \
		 struct drm_apollo_hexagon_context_destroy)
#define DRM_IOCTL_APOLLO_HEXAGON_BO_CREATE \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APOLLO_HEXAGON_BO_CREATE, \
		 struct drm_apollo_hexagon_bo_create)
#define DRM_IOCTL_APOLLO_HEXAGON_BO_DESTROY \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APOLLO_HEXAGON_BO_DESTROY, \
		 struct drm_apollo_hexagon_bo_destroy)
#define DRM_IOCTL_APOLLO_HEXAGON_WAIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APOLLO_HEXAGON_WAIT, \
		 struct drm_apollo_hexagon_wait)
#define DRM_IOCTL_APOLLO_HEXAGON_BO_BIND \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APOLLO_HEXAGON_BO_BIND, \
		 struct drm_apollo_hexagon_bo_bind)
#define DRM_IOCTL_APOLLO_HEXAGON_BO_UNBIND \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APOLLO_HEXAGON_BO_UNBIND, \
		 struct drm_apollo_hexagon_bo_unbind)
#define DRM_IOCTL_APOLLO_HEXAGON_CMD_SUBMIT \
	DRM_IOWR(DRM_COMMAND_BASE + DRM_APOLLO_HEXAGON_CMD_SUBMIT, \
		 struct drm_apollo_hexagon_cmd_submit)

#endif /* _UAPI_DRM_APOLLO_HEXAGON_ACCEL_H */
