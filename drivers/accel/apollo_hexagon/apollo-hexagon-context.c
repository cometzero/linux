// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apollo Hexagon generic context ioctls.
 */

#include <linux/errno.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/xarray.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>

#include "apollo-hexagon.h"

void apollo_hexagon_context_free(struct apollo_hexagon_context *ctx)
{
	struct apollo_hexagon_bo_binding *binding;
	unsigned long handle;

	if (!ctx)
		return;

	xa_for_each(&ctx->bindings, handle, binding) {
		drm_gem_object_put(binding->obj);
		kfree(binding);
	}
	xa_destroy(&ctx->bindings);
	kfree(ctx);
}

int apollo_hexagon_ioctl_context_create(struct drm_device *drm, void *data,
					struct drm_file *file)
{
	struct apollo_hexagon *test = to_apollo_hexagon(drm);
	struct apollo_hexagon_file *afile = file->driver_priv;
	struct drm_apollo_hexagon_context_create *args = data;
	struct apollo_hexagon_context *ctx;
	u32 handle;
	int idx;
	int ret;

	if (!afile)
		return -EINVAL;
	if (args->size != sizeof(*args) || args->flags)
		return -EINVAL;
	if (args->generic_abi_version != APOLLO_HEXAGON_GENERIC_ABI_VERSION)
		return -EINVAL;
	if (args->reserved[0] || args->reserved[1] ||
	    args->reserved[2] || args->reserved[3])
		return -EINVAL;

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ret = -ENOMEM;
		goto out_dev_exit;
	}
	xa_init_flags(&ctx->bindings, XA_FLAGS_ALLOC1);
	ctx->generic_abi_version = args->generic_abi_version;
	ctx->next_iova_offset = APOLLO_HEXAGON_BIND_IOVA_OFFSET;

	mutex_lock(&afile->lock);
	ret = xa_alloc(&afile->contexts, &handle, ctx, XA_LIMIT(1, U32_MAX),
		       GFP_KERNEL);
	if (!ret)
		ctx->handle = handle;
	mutex_unlock(&afile->lock);
	if (ret)
		goto err_free;

	args->handle = handle;
	args->queue_count = readl(test->regs + APOLLO_HEXAGON_REG_QUEUE_CAPS);
	args->fence_model = APOLLO_HEXAGON_FENCE_MODEL_ASYNC_IRQ_POLL;

	drm_dev_exit(idx);
	return 0;

err_free:
	apollo_hexagon_context_free(ctx);
out_dev_exit:
	drm_dev_exit(idx);
	return ret;
}

int apollo_hexagon_ioctl_context_destroy(struct drm_device *drm, void *data,
					 struct drm_file *file)
{
	struct apollo_hexagon_file *afile = file->driver_priv;
	struct drm_apollo_hexagon_context_destroy *args = data;
	struct apollo_hexagon_context *ctx;

	if (!afile)
		return -EINVAL;
	if (!args->handle || args->pad)
		return -EINVAL;

	mutex_lock(&afile->lock);
	ctx = xa_erase(&afile->contexts, args->handle);
	mutex_unlock(&afile->lock);
	if (!ctx)
		return -ENOENT;

	apollo_hexagon_context_free(ctx);
	return 0;
}
