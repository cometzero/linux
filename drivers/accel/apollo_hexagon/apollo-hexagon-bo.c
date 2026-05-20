// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apollo Hexagon GEM buffer-object ioctls.
 */

#include <linux/err.h>
#include <linux/errno.h>
#include <linux/dma-mapping.h>
#include <linux/kernel.h>
#include <linux/limits.h>
#include <linux/mm.h>
#include <linux/overflow.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/sizes.h>
#include <linux/types.h>
#include <linux/xarray.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_vma_manager.h>

#include "apollo-hexagon.h"

#define APOLLO_HEXAGON_TBU_MAX_DYNAMIC_MAPS 64

int apollo_hexagon_ioctl_bo_create(struct drm_device *drm, void *data,
				   struct drm_file *file)
{
	struct apollo_hexagon *test = to_apollo_hexagon(drm);
	struct drm_apollo_hexagon_bo_create *args = data;
	struct drm_gem_shmem_object *shmem;
	struct drm_gem_object *obj;
	size_t aligned_size;
	int idx;
	int ret;

	if (args->size != sizeof(*args) || args->flags || args->handle ||
	    args->pad || args->mmap_offset)
		return -EINVAL;
	if (args->reserved[0] || args->reserved[1] ||
	    args->reserved[2] || args->reserved[3])
		return -EINVAL;
	if (!args->bo_size || args->bo_size > SIZE_MAX - PAGE_SIZE)
		return -EINVAL;

	aligned_size = PAGE_ALIGN(args->bo_size);
	if (!aligned_size || aligned_size > test->dma_window_size)
		return -EINVAL;

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;

	shmem = drm_gem_shmem_create(drm, aligned_size);
	if (IS_ERR(shmem)) {
		ret = PTR_ERR(shmem);
		goto out_dev_exit;
	}
	shmem->map_wc = false;
	obj = &shmem->base;

	ret = drm_gem_handle_create(file, obj, &args->handle);
	if (ret)
		goto err_put_object;

	args->bo_size = aligned_size;
	args->mmap_offset = drm_vma_node_offset_addr(&obj->vma_node);
	drm_gem_object_put(obj);
	drm_dev_exit(idx);

	return 0;

err_put_object:
	drm_gem_object_put(obj);
out_dev_exit:
	drm_dev_exit(idx);
	return ret;
}

int apollo_hexagon_ioctl_bo_destroy(struct drm_device *drm, void *data,
				    struct drm_file *file)
{
	struct drm_apollo_hexagon_bo_destroy *args = data;

	if (!args->handle || args->pad)
		return -EINVAL;

	return drm_gem_handle_delete(file, args->handle);
}

static bool apollo_hexagon_bo_bind_usage_valid(u32 usage)
{
	return usage && !(usage & ~APOLLO_HEXAGON_BO_BIND_USAGE_MASK);
}

static struct apollo_hexagon_bo_binding *
apollo_hexagon_context_find_iova_binding(struct apollo_hexagon_context *ctx,
					 u64 iova, u32 bytes,
					 u32 required_usage, u64 *binding_offset)
{
	struct apollo_hexagon_bo_binding *binding;
	unsigned long handle;
	u64 offset;

	if (!ctx || !bytes || !required_usage)
		return NULL;

	xa_for_each(&ctx->bindings, handle, binding) {
		if ((binding->usage & required_usage) != required_usage)
			continue;
		if (iova < binding->iova)
			continue;
		offset = iova - binding->iova;
		if (offset > binding->length ||
		    bytes > binding->length - offset)
			continue;
		if (binding_offset)
			*binding_offset = offset;
		return binding;
	}

	return NULL;
}

bool apollo_hexagon_context_iova_bound(struct apollo_hexagon_context *ctx,
				       u64 iova, u32 bytes,
				       u32 required_usage)
{
	return apollo_hexagon_context_find_iova_binding(ctx, iova, bytes,
						       required_usage, NULL);
}

int apollo_hexagon_context_get_iova_bo(struct apollo_hexagon_context *ctx,
				       u64 iova, u32 bytes,
				       u32 required_usage,
				       struct drm_gem_object **obj,
				       u64 *obj_offset)
{
	struct apollo_hexagon_bo_binding *binding;
	u64 binding_offset = 0;
	u64 map_offset;

	if (!obj || !obj_offset)
		return -EINVAL;
	*obj = NULL;
	*obj_offset = 0;

	binding = apollo_hexagon_context_find_iova_binding(
		ctx, iova, bytes, required_usage, &binding_offset);
	if (!binding)
		return -ENOENT;

	if (check_add_overflow(binding->offset, binding_offset, &map_offset))
		return -EINVAL;
	if (map_offset > (u64)binding->obj->size ||
	    bytes > (u64)binding->obj->size - map_offset)
		return -EINVAL;

	drm_gem_object_get(binding->obj);
	*obj = binding->obj;
	*obj_offset = map_offset;
	return 0;
}

static int apollo_hexagon_bo_tbu_map_count(struct sg_table *sgt,
					   u64 obj_offset, u64 bytes,
					   u32 *count)
{
	struct scatterlist *sg;
	u64 skip = obj_offset;
	u64 remaining = bytes;
	unsigned int i;
	u32 segments = 0;

	if (!sgt || !bytes || !count)
		return -EINVAL;

	for_each_sgtable_sg(sgt, sg, i) {
		u64 sg_len = sg->length;
		u64 len;

		if (skip >= sg_len) {
			skip -= sg_len;
			continue;
		}

		len = min_t(u64, sg_len - skip, remaining);
		if (!len)
			break;

		if (++segments > APOLLO_HEXAGON_TBU_MAX_DYNAMIC_MAPS)
			return -ENOSPC;

		remaining -= len;
		skip = 0;
		if (!remaining)
			break;
	}
	if (remaining)
		return -EINVAL;

	*count = segments;
	return 0;
}

static void apollo_hexagon_bo_unmap_tbu_entries(
	struct device *dev, struct apollo_hexagon *test,
	struct apollo_hexagon_tbu_mapping *maps, u32 map_count)
{
	u32 i;

	for (i = map_count; i > 0; i--) {
		struct apollo_hexagon_tbu_mapping *map = &maps[i - 1];
		int ret;

		ret = apollo_hexagon_tbu_unmap(dev, test, map->iova,
					       map->pa, map->size);
		if (ret)
			dev_warn(dev,
				 "failed to unmap BO TBU entry iova=0x%llx pa=%pa size=0x%llx ret=%d\n",
				 map->iova, &map->pa, map->size, ret);
	}
}

int apollo_hexagon_bo_map_tbu(struct device *dev,
			      struct apollo_hexagon *test,
			      struct drm_gem_object *obj, u64 obj_offset,
			      u64 iova, u32 bytes,
			      enum dma_data_direction dir,
			      struct apollo_hexagon_bo_tbu_map *map)
{
	struct drm_gem_shmem_object *shmem;
	struct apollo_hexagon_tbu_mapping *maps;
	struct scatterlist *sg;
	struct sg_table *sgt;
	u64 map_offset;
	u64 map_iova;
	u64 map_bytes;
	u64 range_bias;
	u64 skip;
	u64 mapped = 0;
	u64 remaining;
	unsigned int i;
	u32 map_count = 0;
	u32 installed = 0;
	int ret;

	if (!dev || !test || !obj || !bytes || !map)
		return -EINVAL;
	if (obj_offset > (u64)obj->size || bytes > (u64)obj->size - obj_offset)
		return -EINVAL;

	map_offset = round_down(obj_offset, (u64)PAGE_SIZE);
	map_iova = round_down(iova, (u64)PAGE_SIZE);
	range_bias = obj_offset - map_offset;
	if (iova - map_iova != range_bias)
		return -EINVAL;
	if (check_add_overflow(range_bias, (u64)bytes, &map_bytes))
		return -EOVERFLOW;
	map_bytes = ALIGN(map_bytes, (u64)PAGE_SIZE);
	if (map_offset > (u64)obj->size ||
	    map_bytes > (u64)obj->size - map_offset)
		return -EINVAL;

	shmem = to_drm_gem_shmem_obj(obj);
	sgt = drm_gem_shmem_get_pages_sgt(shmem);
	if (IS_ERR(sgt))
		return PTR_ERR(sgt);

	ret = apollo_hexagon_bo_tbu_map_count(sgt, map_offset, map_bytes,
					      &map_count);
	if (ret)
		return ret;

	maps = kcalloc(map_count, sizeof(*maps), GFP_KERNEL);
	if (!maps)
		return -ENOMEM;

	dma_sync_sgtable_for_device(dev, sgt, dir);

	skip = map_offset;
	remaining = map_bytes;
	for_each_sgtable_sg(sgt, sg, i) {
		u64 sg_len = sg->length;
		u64 phys = sg_phys(sg);
		u64 len;

		if (skip >= sg_len) {
			skip -= sg_len;
			continue;
		}

		if (check_add_overflow(phys, skip, &phys)) {
			ret = -EOVERFLOW;
			goto err_unmap;
		}

		len = min_t(u64, sg_len - skip, remaining);
		ret = apollo_hexagon_tbu_map(dev, test, map_iova + mapped,
					     (phys_addr_t)phys, len);
		if (ret)
			goto err_unmap;

		maps[installed].iova = map_iova + mapped;
		maps[installed].pa = (phys_addr_t)phys;
		maps[installed].size = len;
		installed++;
		mapped += len;
		remaining -= len;
		skip = 0;
		if (!remaining)
			break;
	}
	if (remaining) {
		ret = -EINVAL;
		goto err_unmap;
	}

	map->sgt = sgt;
	map->maps = maps;
	map->map_count = installed;
	map->dir = dir;
	dev_info(dev,
		 "command BO TBU map iova=0x%llx object-offset=0x%llx bytes=%u mapped-bytes=0x%llx segments=%u\n",
		 iova, obj_offset, bytes, map_bytes, installed);

	return 0;

err_unmap:
	apollo_hexagon_bo_unmap_tbu_entries(dev, test, maps, installed);
	kfree(maps);
	return ret;
}

void apollo_hexagon_bo_sync_tbu_for_cpu(
	struct device *dev, struct apollo_hexagon_bo_tbu_map *map)
{
	if (!map || !map->sgt)
		return;

	dma_sync_sgtable_for_cpu(dev, map->sgt, map->dir);
}

void apollo_hexagon_bo_unmap_tbu(struct device *dev,
				 struct apollo_hexagon *test,
				 struct apollo_hexagon_bo_tbu_map *map)
{
	if (!map || !map->maps)
		return;

	apollo_hexagon_bo_unmap_tbu_entries(dev, test, map->maps,
					    map->map_count);
	kfree(map->maps);
	memset(map, 0, sizeof(*map));
}

int apollo_hexagon_ioctl_bo_bind(struct drm_device *drm, void *data,
				 struct drm_file *file)
{
	struct apollo_hexagon *test = to_apollo_hexagon(drm);
	struct apollo_hexagon_file *afile = file->driver_priv;
	struct drm_apollo_hexagon_bo_bind *args = data;
	struct apollo_hexagon_bo_binding *binding;
	struct apollo_hexagon_context *ctx;
	struct drm_gem_object *obj;
	u64 next_iova_offset;
	u32 handle;
	int ret;

	if (!afile)
		return -EINVAL;
	if (args->size != sizeof(*args) || args->flags ||
	    !args->context_handle || !args->bo_handle || args->bind_handle ||
	    args->iova)
		return -EINVAL;
	if (!apollo_hexagon_bo_bind_usage_valid(args->usage))
		return -EINVAL;
	if (args->reserved[0] || args->reserved[1] ||
	    args->reserved[2] || args->reserved[3])
		return -EINVAL;
	if (!args->length || !IS_ALIGNED(args->offset, SZ_4K) ||
	    !IS_ALIGNED(args->length, SZ_4K))
		return -EINVAL;

	obj = drm_gem_object_lookup(file, args->bo_handle);
	if (!obj)
		return -ENOENT;
	if (args->offset > (u64)obj->size ||
	    args->length > (u64)obj->size - args->offset) {
		ret = -EINVAL;
		goto err_put_object;
	}

	binding = kzalloc(sizeof(*binding), GFP_KERNEL);
	if (!binding) {
		ret = -ENOMEM;
		goto err_put_object;
	}
	binding->obj = obj;
	binding->bo_handle = args->bo_handle;
	binding->offset = args->offset;
	binding->length = args->length;
	binding->usage = args->usage;

	mutex_lock(&afile->lock);
	ctx = xa_load(&afile->contexts, args->context_handle);
	if (!ctx) {
		ret = -ENOENT;
		goto out_unlock;
	}

	if (ctx->next_iova_offset > test->dma_window_size ||
	    args->length > test->dma_window_size - ctx->next_iova_offset) {
		ret = -ENOSPC;
		goto out_unlock;
	}

	if (check_add_overflow(test->dma_iova_base, ctx->next_iova_offset,
			       &binding->iova)) {
		ret = -ENOSPC;
		goto out_unlock;
	}
	next_iova_offset = ctx->next_iova_offset + args->length;
	ret = xa_alloc(&ctx->bindings, &handle, binding, XA_LIMIT(1, U32_MAX),
		       GFP_KERNEL);
	if (ret)
		goto out_unlock;

	binding->handle = handle;
	ctx->next_iova_offset = next_iova_offset;
	args->bind_handle = handle;
	args->iova = binding->iova;

	mutex_unlock(&afile->lock);
	return 0;

out_unlock:
	mutex_unlock(&afile->lock);
	kfree(binding);
err_put_object:
	drm_gem_object_put(obj);
	return ret;
}

int apollo_hexagon_ioctl_bo_unbind(struct drm_device *drm, void *data,
				   struct drm_file *file)
{
	struct apollo_hexagon_file *afile = file->driver_priv;
	struct drm_apollo_hexagon_bo_unbind *args = data;
	struct apollo_hexagon_bo_binding *binding;
	struct apollo_hexagon_context *ctx;

	if (!afile)
		return -EINVAL;
	if (args->size != sizeof(*args) || args->flags ||
	    !args->context_handle || !args->bind_handle ||
	    args->reserved[0] || args->reserved[1])
		return -EINVAL;

	mutex_lock(&afile->lock);
	ctx = xa_load(&afile->contexts, args->context_handle);
	if (!ctx) {
		mutex_unlock(&afile->lock);
		return -ENOENT;
	}

	binding = xa_erase(&ctx->bindings, args->bind_handle);
	mutex_unlock(&afile->lock);
	if (!binding)
		return -ENOENT;

	drm_gem_object_put(binding->obj);
	kfree(binding);
	return 0;
}
