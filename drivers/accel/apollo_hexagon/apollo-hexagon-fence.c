// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apollo Hexagon generic fence/wait ioctls.
 */

#include <linux/delay.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/time64.h>
#include <linux/types.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>

#include "apollo-hexagon.h"

static bool apollo_hexagon_fence_reached(u32 current_seq, u32 target)
{
	return (s32)(current_seq - target) >= 0;
}

static void apollo_hexagon_read_wait_state(struct apollo_hexagon *test,
					   struct drm_apollo_hexagon_wait *args)
{
	args->status = readl(test->regs + APOLLO_HEXAGON_REG_JOB_STATUS);
	args->result = readl(test->regs + APOLLO_HEXAGON_REG_JOB_RESULT);
	args->current_fence_seq =
		readl(test->regs + APOLLO_HEXAGON_REG_JOB_FENCE);
}

int apollo_hexagon_ioctl_wait(struct drm_device *drm, void *data,
			      struct drm_file *file)
{
	struct apollo_hexagon *test = to_apollo_hexagon(drm);
	struct drm_apollo_hexagon_wait *args = data;
	unsigned long deadline = 0;
	u64 timeout_ms;
	u32 queue_count;
	int idx;
	int ret = -ETIMEDOUT;

	if (args->size != sizeof(*args) || args->flags || !args->fence_seq)
		return -EINVAL;
	if (args->status || args->result || args->current_fence_seq)
		return -EINVAL;
	if (args->reserved[0] || args->reserved[1] || args->reserved[2])
		return -EINVAL;

	timeout_ms = DIV_ROUND_UP_ULL(args->timeout_ns, NSEC_PER_MSEC);
	if (timeout_ms > APOLLO_HEXAGON_ACCEL_TIMEOUT_MS)
		return -EINVAL;
	if (timeout_ms)
		deadline = jiffies + msecs_to_jiffies(timeout_ms);

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;

	queue_count = readl(test->regs + APOLLO_HEXAGON_REG_QUEUE_CAPS);
	if (args->queue_id >= queue_count) {
		ret = -EINVAL;
		goto out_dev_exit;
	}

	for (;;) {
		apollo_hexagon_read_wait_state(test, args);
		if (apollo_hexagon_fence_reached(args->current_fence_seq,
						 args->fence_seq)) {
			ret = 0;
			break;
		}
		if (!timeout_ms || time_after_eq(jiffies, deadline))
			break;

		msleep(APOLLO_HEXAGON_ACCEL_POLL_MS);
	}

out_dev_exit:
	drm_dev_exit(idx);
	return ret;
}
