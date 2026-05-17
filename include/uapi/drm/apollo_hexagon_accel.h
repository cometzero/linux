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
#define APOLLO_HEXAGON_DMA_STRESS_BYTES 131072
#define APOLLO_HEXAGON_DMA_STRESS_SEGMENTS 8

struct drm_apollo_hexagon_query {
	__u32 stream_id;
	__u32 queue_count;
	__u32 capabilities;
	__u32 dma_path;
	__u32 primary_endpoint;
	__u32 pad;
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

enum drm_apollo_hexagon_ioctl_id {
	DRM_APOLLO_HEXAGON_QUERY = 0,
	DRM_APOLLO_HEXAGON_SUBMIT_CNN,
	DRM_APOLLO_HEXAGON_SUBMIT_VADD,
	DRM_APOLLO_HEXAGON_DMA_STRESS,
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

#endif /* _UAPI_DRM_APOLLO_HEXAGON_ACCEL_H */
