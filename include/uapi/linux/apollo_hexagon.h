/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_APOLLO_HEXAGON_H
#define _UAPI_LINUX_APOLLO_HEXAGON_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define APOLLO_HEXAGON_CNN_INPUT_WORDS 16
#define APOLLO_HEXAGON_CNN_OUTPUT_WORDS 4
#define APOLLO_HEXAGON_VADD_WORDS 4
#define APOLLO_HEXAGON_VADD_INPUT_WORDS (APOLLO_HEXAGON_VADD_WORDS * 2)
#define APOLLO_HEXAGON_VADD_OUTPUT_WORDS APOLLO_HEXAGON_VADD_WORDS
#define APOLLO_HEXAGON_DMA_STRESS_BYTES 131072
#define APOLLO_HEXAGON_DMA_STRESS_SEGMENTS 8

struct apollo_hexagon_cnn_job {
	__u32 input[APOLLO_HEXAGON_CNN_INPUT_WORDS];
	__u32 output[APOLLO_HEXAGON_CNN_OUTPUT_WORDS];
	__u32 status;
	__u32 flags;
	__u32 queue_id;
	__u32 fence_seq;
};

struct apollo_hexagon_vadd_job {
	__u32 lhs[APOLLO_HEXAGON_VADD_WORDS];
	__u32 rhs[APOLLO_HEXAGON_VADD_WORDS];
	__u32 output[APOLLO_HEXAGON_VADD_OUTPUT_WORDS];
	__u32 status;
	__u32 flags;
	__u32 queue_id;
	__u32 fence_seq;
};

struct apollo_hexagon_dma_stress_job {
	__u32 bytes;
	__u32 segment_bytes;
	__u32 seed;
	__u32 checksum;
	__u32 status;
	__u32 flags;
	__u32 queue_id;
	__u32 fence_seq;
};

#define APOLLO_HEXAGON_IOC_MAGIC 'H'
#define APOLLO_HEXAGON_IOC_SUBMIT_CNN \
	_IOWR(APOLLO_HEXAGON_IOC_MAGIC, 0x01, struct apollo_hexagon_cnn_job)
#define APOLLO_HEXAGON_IOC_DMA_STRESS \
	_IOWR(APOLLO_HEXAGON_IOC_MAGIC, 0x02, \
	      struct apollo_hexagon_dma_stress_job)
#define APOLLO_HEXAGON_IOC_SUBMIT_VADD \
	_IOWR(APOLLO_HEXAGON_IOC_MAGIC, 0x03, struct apollo_hexagon_vadd_job)

#endif /* _UAPI_LINUX_APOLLO_HEXAGON_H */
