// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apollo Hexagon executable-handle and generic-submit ioctls.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/iosys-map.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/xarray.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>
#include <drm/drm_gem.h>

#include "apollo-hexagon.h"

#define APOLLO_HEXAGON_MAX_INPUT_WORDS APOLLO_HEXAGON_MNIST_INPUT_WORDS
#define APOLLO_HEXAGON_MAX_OUTPUT_WORDS APOLLO_HEXAGON_MNIST_OUTPUT_WORDS

struct apollo_hexagon_executable {
	u32 handle;
	u32 entry_kind;
	u32 input_bytes;
	u32 output_bytes;
	u32 queue_id;
	u32 expected_result;
};

static void apollo_hexagon_write_guest_words(struct apollo_hexagon *test,
					     const u32 *input, u32 words)
{
	u32 i;

	for (i = 0; i < words; i++)
		writel(input[i],
		       test->shared_base + APOLLO_HEXAGON_INPUT_OFFSET +
		       i * sizeof(u32));
}

static void apollo_hexagon_read_guest_words(struct apollo_hexagon *test,
					    u32 *output, u32 words)
{
	u32 i;

	for (i = 0; i < words; i++)
		output[i] =
			readl(test->shared_base + APOLLO_HEXAGON_OUTPUT_OFFSET +
			      i * sizeof(u32));
}

static int apollo_hexagon_copy_cmd_buffer_from_bo(struct drm_file *file,
							  u32 bo_handle,
							  u64 command_offset,
							  u32 command_size,
							  u32 *command)
{
	struct drm_gem_object *obj;
	struct iosys_map map;
	int ret;

	if (!bo_handle || !command_size ||
	    command_size > APOLLO_HEXAGON_CMDQ_SUBMIT_MAX_BYTES ||
	    command_size % APOLLO_HEXAGON_CMDQ_PACKET_BYTES ||
	    !IS_ALIGNED(command_offset, sizeof(u32)))
		return -EINVAL;

	obj = drm_gem_object_lookup(file, bo_handle);
	if (!obj)
		return -ENOENT;
	if (command_offset > (u64)obj->size ||
	    command_size > (u64)obj->size - command_offset) {
		ret = -EINVAL;
		goto out_put;
	}

	ret = drm_gem_vmap(obj, &map);
	if (ret)
		goto out_put;

	iosys_map_memcpy_from(command, &map, command_offset, command_size);
	drm_gem_vunmap(obj, &map);

out_put:
	drm_gem_object_put(obj);
	return ret;
}

static int apollo_hexagon_copy_submit_input(
	const struct apollo_hexagon_executable *exe,
	const struct drm_apollo_hexagon_submit *job, u32 **input)
{
	if (!job->input_ptr || !job->output_ptr)
		return -EINVAL;
	if (job->input_bytes != exe->input_bytes ||
	    job->output_bytes != exe->output_bytes)
		return -EINVAL;
	if (job->input_bytes % sizeof(u32) ||
	    job->output_bytes % sizeof(u32))
		return -EINVAL;
	if (job->input_bytes > APOLLO_HEXAGON_MAX_INPUT_WORDS * sizeof(u32) ||
	    job->output_bytes > APOLLO_HEXAGON_MAX_OUTPUT_WORDS * sizeof(u32))
		return -EINVAL;

	*input = kmalloc(job->input_bytes, GFP_KERNEL);
	if (!*input)
		return -ENOMEM;

	if (copy_from_user(*input, u64_to_user_ptr(job->input_ptr),
			   job->input_bytes)) {
		kfree(*input);
		*input = NULL;
		return -EFAULT;
	}

	return 0;
}

static int apollo_hexagon_submit_executable(
	struct device *dev, struct apollo_hexagon *test,
	const struct apollo_hexagon_executable *exe,
	struct drm_apollo_hexagon_submit *job, u32 *job_status,
	u32 *job_result)
{
	const u64 input_iova = test->dma_iova_base + APOLLO_HEXAGON_INPUT_OFFSET;
	const u64 output_iova = test->dma_iova_base + APOLLO_HEXAGON_OUTPUT_OFFSET;
	const u64 cmdq_iova = test->dma_iova_base + APOLLO_HEXAGON_CMDQ_OFFSET;
	u32 *input;
	u32 *output;
	bool use_cmdq_dispatch = exe->entry_kind == APOLLO_HEXAGON_EXEC_KIND_VADD ||
				 exe->entry_kind == APOLLO_HEXAGON_EXEC_KIND_MNIST;
	u32 input_words;
	u32 output_words;
	u32 cmdq_status;
	u32 cmdq_fault;
	u32 cmdq_head;
	u32 queue_id;
	u32 fence_seq = 0;
	u32 final_status = 0;
	u32 final_result = 0;
	int ret;

	ret = apollo_hexagon_copy_submit_input(exe, job, &input);
	if (ret)
		return ret;

	queue_id = job->queue_id ? job->queue_id : exe->queue_id;
	if (queue_id != exe->queue_id) {
		ret = -EINVAL;
		goto out_free_input;
	}

	input_words = exe->input_bytes / sizeof(u32);
	output_words = exe->output_bytes / sizeof(u32);
	output = kcalloc(output_words, sizeof(*output), GFP_KERNEL);
	if (!output) {
		ret = -ENOMEM;
		goto out_free_input;
	}

	mutex_lock(&test->lock);

	ret = apollo_hexagon_dynamic_map(dev, test);
	if (ret)
		goto out_unlock;

	apollo_hexagon_write_guest_words(test, input, input_words);
	if (use_cmdq_dispatch) {
		dev_info(dev,
			 "APKO CMDQ dispatch start handle=%u kind=%u queue=%u input=%u output=%u\n",
			 exe->handle, exe->entry_kind, queue_id,
			 exe->input_bytes, exe->output_bytes);
		apollo_hexagon_cmdq_write_dispatch(
			test, exe->entry_kind, input_iova, output_iova,
			exe->input_bytes, exe->output_bytes);
		dma_wmb();
		writel(lower_32_bits(cmdq_iova),
		       test->regs + APOLLO_HEXAGON_REG_CMDQ_BASE_LO);
		writel(upper_32_bits(cmdq_iova),
		       test->regs + APOLLO_HEXAGON_REG_CMDQ_BASE_HI);
		writel(APOLLO_HEXAGON_CMDQ_SIZE,
		       test->regs + APOLLO_HEXAGON_REG_CMDQ_SIZE);
		writel(0, test->regs + APOLLO_HEXAGON_REG_CMDQ_HEAD);
		writel(APOLLO_HEXAGON_CMDQ_PACKET_BYTES,
		       test->regs + APOLLO_HEXAGON_REG_CMDQ_TAIL);
		writel(queue_id, test->regs + APOLLO_HEXAGON_REG_JOB_QUEUE);
		writel(0, test->regs + APOLLO_HEXAGON_REG_JOB_STATUS);
		writel(0, test->regs + APOLLO_HEXAGON_REG_JOB_RESULT);
		apollo_hexagon_prepare_async_fence(test, queue_id);
		writel(APOLLO_HEXAGON_JOB_CTRL_START,
		       test->regs + APOLLO_HEXAGON_REG_CMDQ_DOORBELL);
	} else {
		dev_info(dev,
			 "APKO dispatch start handle=%u kind=%u queue=%u input=%u output=%u\n",
			 exe->handle, exe->entry_kind, queue_id,
			 exe->input_bytes, exe->output_bytes);
		dma_wmb();
		writel(lower_32_bits(input_iova),
		       test->regs + APOLLO_HEXAGON_REG_JOB_INPUT);
		writel(lower_32_bits(output_iova),
		       test->regs + APOLLO_HEXAGON_REG_JOB_OUTPUT);
		writel(exe->input_bytes,
		       test->regs + APOLLO_HEXAGON_REG_JOB_INPUT_BYTES);
		writel(exe->output_bytes,
		       test->regs + APOLLO_HEXAGON_REG_JOB_OUTPUT_BYTES);
		writel(queue_id, test->regs + APOLLO_HEXAGON_REG_JOB_QUEUE);
		writel(0, test->regs + APOLLO_HEXAGON_REG_JOB_STATUS);
		writel(0, test->regs + APOLLO_HEXAGON_REG_JOB_RESULT);
		apollo_hexagon_prepare_async_fence(test, queue_id);
		writel(APOLLO_HEXAGON_JOB_CTRL_START,
		       test->regs + APOLLO_HEXAGON_REG_JOB_CTRL);
	}

	ret = apollo_hexagon_wait_job(dev, test, queue_id,
				      exe->expected_result, &fence_seq,
				      &final_status, &final_result);
	if (job_status)
		*job_status = final_status;
	if (job_result)
		*job_result = final_result;
	if (ret) {
		job->status = final_result;
		job->fence_seq = fence_seq;
		goto out_unlock;
	}

	if (use_cmdq_dispatch) {
		cmdq_status = readl(test->regs + APOLLO_HEXAGON_REG_CMDQ_STATUS);
		cmdq_fault = readl(test->regs +
				   APOLLO_HEXAGON_REG_CMDQ_FAULT_CODE);
		cmdq_head = readl(test->regs + APOLLO_HEXAGON_REG_CMDQ_HEAD);
		if (cmdq_status != APOLLO_HEXAGON_CMDQ_STATUS_DONE ||
		    cmdq_fault != APOLLO_HEXAGON_CMDQ_FAULT_NONE ||
		    cmdq_head != APOLLO_HEXAGON_CMDQ_PACKET_BYTES) {
			final_status = cmdq_status;
			final_result = cmdq_fault;
			if (job_status)
				*job_status = final_status;
			if (job_result)
				*job_result = final_result;
			job->status = cmdq_fault;
			job->fence_seq = fence_seq;
			dev_err(dev,
				"APKO CMDQ dispatch failed handle=%u status=0x%x fault=0x%x head=0x%x\n",
				exe->handle, cmdq_status, cmdq_fault, cmdq_head);
			ret = -EIO;
			goto out_unlock;
		}
		dev_info(dev,
			 "APKO CMDQ dispatch complete handle=%u kind=%u head=%u fence=%u\n",
			 exe->handle, exe->entry_kind, cmdq_head, fence_seq);
	}

	dma_rmb();
	apollo_hexagon_read_guest_words(test, output, output_words);
	job->status = exe->expected_result;
	job->queue_id = queue_id;
	job->fence_seq = fence_seq;
	dev_info(dev,
		 "generic submit ok handle=%u queue=%u fence=%u status=0x%x\n",
		 exe->handle, queue_id, fence_seq, job->status);
	dev_info(dev, "APKO dispatch complete handle=%u kind=%u\n",
		 exe->handle, exe->entry_kind);
	ret = 0;

out_unlock:
	mutex_unlock(&test->lock);
	if (ret)
		goto out_free_output;

	if (copy_to_user(u64_to_user_ptr(job->output_ptr), output,
			 job->output_bytes)) {
		ret = -EFAULT;
		goto out_free_output;
	}

	ret = 0;

out_free_output:
	kfree(output);
out_free_input:
	kfree(input);
	return ret;
}

static int apollo_hexagon_validate_apko_header(
	const struct drm_apollo_hexagon_apko_header *header,
	struct apollo_hexagon_executable *exe)
{
	if (header->magic != APOLLO_HEXAGON_APKO_MAGIC ||
	    header->header_bytes < sizeof(*header) ||
	    header->abi_version != APOLLO_HEXAGON_APKO_ABI_VERSION ||
	    header->executable_format != APOLLO_HEXAGON_EXEC_FORMAT_APKO_V0)
		return -EINVAL;
	if (header->reserved[0] || header->reserved[1] ||
	    header->reserved[2] || header->reserved[3] ||
	    header->reserved[4])
		return -EINVAL;

	switch (header->entry_kind) {
	case APOLLO_HEXAGON_EXEC_KIND_CNN:
		if (header->input_bytes !=
		    APOLLO_HEXAGON_CNN_INPUT_WORDS * sizeof(u32) ||
		    header->output_bytes !=
		    APOLLO_HEXAGON_CNN_OUTPUT_WORDS * sizeof(u32))
			return -EINVAL;
		exe->queue_id = APOLLO_HEXAGON_QUEUE_CNN;
		exe->expected_result = APOLLO_HEXAGON_JOB_RESULT_OK;
		break;
	case APOLLO_HEXAGON_EXEC_KIND_VADD:
		if (header->input_bytes !=
		    APOLLO_HEXAGON_VADD_INPUT_WORDS * sizeof(u32) ||
		    header->output_bytes !=
		    APOLLO_HEXAGON_VADD_OUTPUT_WORDS * sizeof(u32))
			return -EINVAL;
		exe->queue_id = APOLLO_HEXAGON_QUEUE_VADD;
		exe->expected_result = APOLLO_HEXAGON_JOB_RESULT_VADD_OK;
		break;
	case APOLLO_HEXAGON_EXEC_KIND_MNIST:
		if (header->input_bytes !=
		    APOLLO_HEXAGON_MNIST_INPUT_WORDS * sizeof(u32) ||
		    header->output_bytes !=
		    APOLLO_HEXAGON_MNIST_OUTPUT_WORDS * sizeof(u32))
			return -EINVAL;
		exe->queue_id = APOLLO_HEXAGON_QUEUE_CNN;
		exe->expected_result = APOLLO_HEXAGON_JOB_RESULT_MNIST_OK;
		break;
	default:
		return -EINVAL;
	}

	exe->entry_kind = header->entry_kind;
	exe->input_bytes = header->input_bytes;
	exe->output_bytes = header->output_bytes;

	return 0;
}

int apollo_hexagon_file_open(struct drm_device *dev, struct drm_file *file)
{
	struct apollo_hexagon_file *afile;

	afile = kzalloc(sizeof(*afile), GFP_KERNEL);
	if (!afile)
		return -ENOMEM;

	xa_init_flags(&afile->executables, XA_FLAGS_ALLOC1);
	xa_init_flags(&afile->contexts, XA_FLAGS_ALLOC1);
	mutex_init(&afile->lock);
	file->driver_priv = afile;

	return 0;
}

void apollo_hexagon_file_postclose(struct drm_device *dev, struct drm_file *file)
{
	struct apollo_hexagon_file *afile = file->driver_priv;
	struct apollo_hexagon_executable *exe;
	struct apollo_hexagon_context *ctx;
	unsigned long handle;

	if (!afile)
		return;

	xa_for_each(&afile->executables, handle, exe)
		kfree(exe);
	xa_destroy(&afile->executables);
	xa_for_each(&afile->contexts, handle, ctx)
		apollo_hexagon_context_free(ctx);
	xa_destroy(&afile->contexts);
	mutex_destroy(&afile->lock);
	kfree(afile);
	file->driver_priv = NULL;
}

int apollo_hexagon_ioctl_exec_create(struct drm_device *drm, void *data,
				     struct drm_file *file)
{
	struct apollo_hexagon_file *afile = file->driver_priv;
	struct drm_apollo_hexagon_exec_create *args = data;
	struct drm_apollo_hexagon_apko_header header;
	struct apollo_hexagon_executable *exe;
	u32 handle;
	int ret;

	if (!afile)
		return -EINVAL;
	if (args->size != sizeof(*args) || args->flags || !args->data_ptr ||
	    args->data_size < sizeof(header))
		return -EINVAL;
	if (args->reserved[0] || args->reserved[1] ||
	    args->reserved[2] || args->reserved[3])
		return -EINVAL;

	if (copy_from_user(&header, u64_to_user_ptr(args->data_ptr),
			   sizeof(header)))
		return -EFAULT;
	if (header.header_bytes > args->data_size)
		return -EINVAL;

	exe = kzalloc(sizeof(*exe), GFP_KERNEL);
	if (!exe)
		return -ENOMEM;

	ret = apollo_hexagon_validate_apko_header(&header, exe);
	if (ret)
		goto err_free;

	mutex_lock(&afile->lock);
	ret = xa_alloc(&afile->executables, &handle, exe,
		       XA_LIMIT(1, U32_MAX), GFP_KERNEL);
	if (!ret)
		exe->handle = handle;
	mutex_unlock(&afile->lock);
	if (ret)
		goto err_free;

	args->handle = handle;
	args->executable_format = header.executable_format;
	args->abi_version = header.abi_version;
	args->entry_kind = header.entry_kind;
	args->input_bytes = header.input_bytes;
	args->output_bytes = header.output_bytes;

	return 0;

err_free:
	kfree(exe);
	return ret;
}

int apollo_hexagon_ioctl_exec_destroy(struct drm_device *drm, void *data,
				      struct drm_file *file)
{
	struct apollo_hexagon_file *afile = file->driver_priv;
	struct drm_apollo_hexagon_exec_destroy *args = data;
	struct apollo_hexagon_executable *exe;

	if (!afile)
		return -EINVAL;
	if (!args->handle || args->pad)
		return -EINVAL;

	mutex_lock(&afile->lock);
	exe = xa_erase(&afile->executables, args->handle);
	mutex_unlock(&afile->lock);
	if (!exe)
		return -ENOENT;

	kfree(exe);
	return 0;
}

int apollo_hexagon_ioctl_cmd_submit(struct drm_device *drm, void *data,
				    struct drm_file *file)
{
	struct apollo_hexagon *test = to_apollo_hexagon(drm);
	struct apollo_hexagon_file *afile = file->driver_priv;
	struct drm_apollo_hexagon_cmd_submit *args = data;
	struct apollo_hexagon_context *ctx;
	struct apollo_hexagon_bound_dispatch bound_dispatch;
	u32 command[APOLLO_HEXAGON_CMDQ_SUBMIT_MAX_BYTES / sizeof(u32)] = { 0 };
	const u64 cmdq_iova = test->dma_iova_base + APOLLO_HEXAGON_CMDQ_OFFSET;
	u32 queue_count;
	u32 cmdq_status = 0;
	u32 cmdq_fault = 0;
	u32 cmdq_head = 0;
	u32 fence_seq = 0;
	int idx;
	int ret;

	if (!afile)
		return -EINVAL;
	if (args->size != sizeof(*args) || args->flags ||
	    !args->context_handle || !args->command_bo_handle ||
	    args->status || args->result || args->fence_seq)
		return -EINVAL;
	if (args->reserved[0] || args->reserved[1] ||
	    args->reserved[2] || args->reserved[3])
		return -EINVAL;

	ret = apollo_hexagon_copy_cmd_buffer_from_bo(
		file, args->command_bo_handle, args->command_offset,
		args->command_size, command);
	if (ret)
		return ret;

	memset(&bound_dispatch, 0, sizeof(bound_dispatch));
	mutex_lock(&afile->lock);
	ctx = xa_load(&afile->contexts, args->context_handle);
	if (!ctx) {
		ret = -ENOENT;
	} else {
		ret = apollo_hexagon_cmdq_prepare_bound_dispatch(
			test->dev, ctx, command, args->command_size,
			&bound_dispatch);
	}
	mutex_unlock(&afile->lock);
	if (ret)
		goto out_bound_dispatch;

	if (!drm_dev_enter(drm, &idx)) {
		ret = -ENODEV;
		goto out_bound_dispatch;
	}

	queue_count = readl(test->regs + APOLLO_HEXAGON_REG_QUEUE_CAPS);
	if (args->queue_id >= queue_count) {
		ret = -EINVAL;
		goto out_dev_exit;
	}

	mutex_lock(&test->lock);

	ret = apollo_hexagon_dynamic_map(test->dev, test);
	if (ret)
		goto out_unlock;

	ret = apollo_hexagon_cmdq_map_bound_dispatch(test->dev, test,
						     &bound_dispatch);
	if (ret)
		goto out_unlock;

	dev_info(test->dev,
		 "command BO submit start ctx=%u bo=%u queue=%u offset=%llu size=%u opcode=%u\n",
		 args->context_handle, args->command_bo_handle, args->queue_id,
		 args->command_offset, args->command_size, command[0]);
	apollo_hexagon_cmdq_write_buffer(test, command, args->command_size);
	dma_wmb();
	writel(lower_32_bits(cmdq_iova),
	       test->regs + APOLLO_HEXAGON_REG_CMDQ_BASE_LO);
	writel(upper_32_bits(cmdq_iova),
	       test->regs + APOLLO_HEXAGON_REG_CMDQ_BASE_HI);
	writel(APOLLO_HEXAGON_CMDQ_SIZE,
	       test->regs + APOLLO_HEXAGON_REG_CMDQ_SIZE);
	writel(0, test->regs + APOLLO_HEXAGON_REG_CMDQ_HEAD);
	writel(args->command_size, test->regs + APOLLO_HEXAGON_REG_CMDQ_TAIL);
	writel(args->queue_id, test->regs + APOLLO_HEXAGON_REG_JOB_QUEUE);
	writel(0, test->regs + APOLLO_HEXAGON_REG_CMDQ_STATUS);
	writel(0, test->regs + APOLLO_HEXAGON_REG_CMDQ_FAULT_CODE);
	writel(0, test->regs + APOLLO_HEXAGON_REG_JOB_STATUS);
	writel(0, test->regs + APOLLO_HEXAGON_REG_JOB_RESULT);
	apollo_hexagon_prepare_async_fence(test, args->queue_id);
	writel(APOLLO_HEXAGON_JOB_CTRL_START,
	       test->regs + APOLLO_HEXAGON_REG_CMDQ_DOORBELL);

	ret = apollo_hexagon_cmdq_wait(test->dev, test, args->queue_id,
				       args->command_size, &fence_seq,
				       &cmdq_status, &cmdq_fault, &cmdq_head);
	args->status = cmdq_status;
	args->result = cmdq_fault;
	args->fence_seq = fence_seq;
	if (ret)
		goto out_unlock;

	if (bound_dispatch.active) {
		dma_rmb();
		apollo_hexagon_cmdq_sync_bound_dispatch_for_cpu(
			test->dev, &bound_dispatch);
	}

	dev_info(test->dev,
		 "command BO submit complete ctx=%u bo=%u queue=%u head=%u fence=%u\n",
		 args->context_handle, args->command_bo_handle, args->queue_id,
		 cmdq_head, fence_seq);

out_unlock:
	apollo_hexagon_cmdq_unmap_bound_dispatch(test->dev, test,
						 &bound_dispatch);
	mutex_unlock(&test->lock);
	if (!ret && bound_dispatch.active) {
		dev_info(test->dev,
			 "command BO bound %s output ready ctx=%u output=0x%llx bytes=%u direct-tbu=yes\n",
			 apollo_hexagon_exec_kind_name(bound_dispatch.entry_kind),
			 args->context_handle, bound_dispatch.output_iova,
			 bound_dispatch.output_bytes);
	}
out_dev_exit:
	drm_dev_exit(idx);
out_bound_dispatch:
	apollo_hexagon_cmdq_bound_dispatch_put(&bound_dispatch);
	if (ret == -EIO)
		apollo_hexagon_record_fault(afile, args->queue_id, cmdq_status,
					   cmdq_fault, args->fence_seq);
	return ret;
}

int apollo_hexagon_ioctl_submit(struct drm_device *drm, void *data,
				struct drm_file *file)
{
	struct apollo_hexagon *test = to_apollo_hexagon(drm);
	struct apollo_hexagon_file *afile = file->driver_priv;
	struct drm_apollo_hexagon_submit *args = data;
	struct apollo_hexagon_executable exe;
	struct apollo_hexagon_executable *stored;
	u32 job_status = 0;
	u32 job_result = 0;
	int idx;
	int ret;

	if (!afile)
		return -EINVAL;
	if (args->size != sizeof(*args) || args->flags ||
	    !args->executable_handle)
		return -EINVAL;
	if (args->reserved[0] || args->reserved[1] ||
	    args->reserved[2] || args->reserved[3])
		return -EINVAL;

	mutex_lock(&afile->lock);
	stored = xa_load(&afile->executables, args->executable_handle);
	if (stored)
		exe = *stored;
	mutex_unlock(&afile->lock);
	if (!stored)
		return -ENOENT;

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;

	ret = apollo_hexagon_submit_executable(test->dev, test, &exe, args,
					       &job_status, &job_result);
	drm_dev_exit(idx);
	if (ret == -EIO)
		apollo_hexagon_record_fault(afile,
					   args->queue_id ? args->queue_id : exe.queue_id,
					   job_status, job_result, args->fence_seq);

	return ret;
}
