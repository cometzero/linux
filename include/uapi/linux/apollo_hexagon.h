/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_APOLLO_HEXAGON_H
#define _UAPI_LINUX_APOLLO_HEXAGON_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define APOLLO_HEXAGON_CNN_INPUT_WORDS 16
#define APOLLO_HEXAGON_CNN_OUTPUT_WORDS 4

struct apollo_hexagon_cnn_job {
	__u32 input[APOLLO_HEXAGON_CNN_INPUT_WORDS];
	__u32 output[APOLLO_HEXAGON_CNN_OUTPUT_WORDS];
	__u32 status;
	__u32 flags;
};

#define APOLLO_HEXAGON_IOC_MAGIC 'H'
#define APOLLO_HEXAGON_IOC_SUBMIT_CNN \
	_IOWR(APOLLO_HEXAGON_IOC_MAGIC, 0x01, struct apollo_hexagon_cnn_job)

#endif /* _UAPI_LINUX_APOLLO_HEXAGON_H */
