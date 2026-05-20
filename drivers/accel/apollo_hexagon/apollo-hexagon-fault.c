// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apollo Hexagon generic fault recording and retrieval ioctls.
 */

#include <linux/errno.h>
#include <linux/string.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>

#include "apollo-hexagon.h"

void apollo_hexagon_record_fault(struct apollo_hexagon_file *afile,
				 u32 queue_id, u32 status, u32 result,
				 u32 fence_seq)
{
	struct drm_apollo_hexagon_fault fault = { 0 };

	fault.size = sizeof(fault);
	fault.queue_id = queue_id;
	fault.code = APOLLO_HEXAGON_FAULT_CODE_JOB_FAILED;
	fault.status = status;
	fault.result = result;
	fault.fence_seq = fence_seq;

	mutex_lock(&afile->lock);
	afile->last_fault = fault;
	afile->has_fault = true;
	mutex_unlock(&afile->lock);
}

int apollo_hexagon_ioctl_get_fault(struct drm_device *drm, void *data,
				   struct drm_file *file)
{
	struct apollo_hexagon_file *afile = file->driver_priv;
	struct drm_apollo_hexagon_fault *args = data;
	struct drm_apollo_hexagon_fault fault;
	bool clear;
	int ret = 0;

	if (!afile)
		return -EINVAL;
	if (args->size != sizeof(*args) ||
	    (args->flags & ~APOLLO_HEXAGON_GET_FAULT_FLAG_CLEAR))
		return -EINVAL;
	if (args->reserved[0] || args->reserved[1] ||
	    args->reserved[2] || args->reserved[3] || args->reserved[4])
		return -EINVAL;

	clear = args->flags & APOLLO_HEXAGON_GET_FAULT_FLAG_CLEAR;

	mutex_lock(&afile->lock);
	if (!afile->has_fault) {
		ret = -ENODATA;
		goto out_unlock;
	}

	fault = afile->last_fault;
	if (clear) {
		memset(&afile->last_fault, 0, sizeof(afile->last_fault));
		afile->has_fault = false;
	}
	*args = fault;

out_unlock:
	mutex_unlock(&afile->lock);
	return ret;
}
