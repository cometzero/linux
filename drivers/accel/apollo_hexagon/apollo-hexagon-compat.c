// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apollo Hexagon fixed compatibility ioctls.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mutex.h>

#include <drm/drm_drv.h>
#include <drm/drm_file.h>

#include "apollo-hexagon.h"

static int apollo_hexagon_dynamic_sg_map(struct device *dev,
					 struct apollo_hexagon *test)
{
	const u64 input_iova = test->dma_iova_base +
			       APOLLO_HEXAGON_STRESS_INPUT_BASE;
	const u64 output_iova = test->dma_iova_base +
				APOLLO_HEXAGON_STRESS_OUTPUT_BASE;
	const u32 segment_bytes =
		APOLLO_HEXAGON_DMA_STRESS_BYTES /
		APOLLO_HEXAGON_DMA_STRESS_SEGMENTS;
	int ret;
	u32 i;

	ret = apollo_hexagon_tbu_ctrl(dev, test, APOLLO_TBU_MAP_CTRL_CLEAR);
	if (ret)
		return ret;

	for (i = 0; i < APOLLO_HEXAGON_DMA_STRESS_SEGMENTS; i++) {
		ret = apollo_hexagon_tbu_map(dev, test,
					     input_iova + i * segment_bytes,
					     test->shared_phys +
					     APOLLO_HEXAGON_STRESS_INPUT_BASE +
					     i * APOLLO_HEXAGON_STRESS_SEGMENT_STRIDE,
					     segment_bytes);
		if (ret)
			return ret;
	}
	for (i = 0; i < APOLLO_HEXAGON_DMA_STRESS_SEGMENTS; i++) {
		ret = apollo_hexagon_tbu_map(dev, test,
					     output_iova + i * segment_bytes,
					     test->shared_phys +
					     APOLLO_HEXAGON_STRESS_OUTPUT_BASE +
					     i * APOLLO_HEXAGON_STRESS_SEGMENT_STRIDE,
					     segment_bytes);
		if (ret)
			return ret;
	}

	dev_info(dev,
		 "dynamic SMMU SG map refreshed bytes=%u segments=%u segment-bytes=%u count=%u\n",
		 APOLLO_HEXAGON_DMA_STRESS_BYTES,
		 APOLLO_HEXAGON_DMA_STRESS_SEGMENTS, segment_bytes,
		 readl(test->tbu_regs + APOLLO_TBU_REG_MAP_COUNT));

	return 0;
}

static void apollo_hexagon_write_guest_input(
	struct apollo_hexagon *test,
	const struct drm_apollo_hexagon_cnn_job *job)
{
	int i;

	for (i = 0; i < APOLLO_HEXAGON_CNN_INPUT_WORDS; i++)
		writel(job->input[i],
		       test->shared_base + APOLLO_HEXAGON_INPUT_OFFSET +
		       i * sizeof(u32));
}

static void apollo_hexagon_read_guest_output(
	struct apollo_hexagon *test,
	struct drm_apollo_hexagon_cnn_job *job)
{
	int i;

	for (i = 0; i < APOLLO_HEXAGON_CNN_OUTPUT_WORDS; i++)
		job->output[i] =
			readl(test->shared_base + APOLLO_HEXAGON_OUTPUT_OFFSET +
			      i * sizeof(u32));
}

static void apollo_hexagon_write_guest_vadd_input(
	struct apollo_hexagon *test,
	const struct drm_apollo_hexagon_vadd_job *job)
{
	int i;

	for (i = 0; i < APOLLO_HEXAGON_VADD_WORDS; i++) {
		writel(job->lhs[i],
		       test->shared_base + APOLLO_HEXAGON_INPUT_OFFSET +
		       i * sizeof(u32));
		writel(job->rhs[i],
		       test->shared_base + APOLLO_HEXAGON_INPUT_OFFSET +
		       (APOLLO_HEXAGON_VADD_WORDS + i) * sizeof(u32));
	}
}

static void apollo_hexagon_read_guest_vadd_output(
	struct apollo_hexagon *test,
	struct drm_apollo_hexagon_vadd_job *job)
{
	int i;

	for (i = 0; i < APOLLO_HEXAGON_VADD_OUTPUT_WORDS; i++)
		job->output[i] =
			readl(test->shared_base + APOLLO_HEXAGON_OUTPUT_OFFSET +
			      i * sizeof(u32));
}

static int apollo_hexagon_submit_cnn(
	struct device *dev, struct apollo_hexagon *test,
	struct drm_apollo_hexagon_cnn_job *job)
{
	const u32 input_iova = lower_32_bits(test->dma_iova_base +
					     APOLLO_HEXAGON_INPUT_OFFSET);
	const u32 output_iova = lower_32_bits(test->dma_iova_base +
					      APOLLO_HEXAGON_OUTPUT_OFFSET);
	u32 queue_id = job->queue_id ? job->queue_id : APOLLO_HEXAGON_QUEUE_CNN;
	u32 fence_seq = 0;
	int ret;

	if (queue_id != APOLLO_HEXAGON_QUEUE_CNN)
		return -EINVAL;

	mutex_lock(&test->lock);

	ret = apollo_hexagon_dynamic_map(dev, test);
	if (ret)
		goto out_unlock;

	apollo_hexagon_write_guest_input(test, job);
	dma_wmb();

	writel(input_iova, test->regs + APOLLO_HEXAGON_REG_JOB_INPUT);
	writel(output_iova, test->regs + APOLLO_HEXAGON_REG_JOB_OUTPUT);
	writel(APOLLO_HEXAGON_CNN_INPUT_WORDS * sizeof(u32),
	       test->regs + APOLLO_HEXAGON_REG_JOB_INPUT_BYTES);
	writel(APOLLO_HEXAGON_CNN_OUTPUT_WORDS * sizeof(u32),
	       test->regs + APOLLO_HEXAGON_REG_JOB_OUTPUT_BYTES);
	writel(queue_id, test->regs + APOLLO_HEXAGON_REG_JOB_QUEUE);
	writel(0, test->regs + APOLLO_HEXAGON_REG_JOB_STATUS);
	writel(0, test->regs + APOLLO_HEXAGON_REG_JOB_RESULT);
	apollo_hexagon_prepare_async_fence(test, queue_id);
	writel(APOLLO_HEXAGON_JOB_CTRL_START,
	       test->regs + APOLLO_HEXAGON_REG_JOB_CTRL);

	ret = apollo_hexagon_wait_job(dev, test, queue_id,
				      APOLLO_HEXAGON_JOB_RESULT_OK,
				      &fence_seq, NULL, NULL);
	if (ret)
		goto out_unlock;

	dma_rmb();
	apollo_hexagon_read_guest_output(test, job);
	job->status = APOLLO_HEXAGON_JOB_RESULT_OK;
	job->queue_id = queue_id;
	job->fence_seq = fence_seq;
	dev_info(dev, "accelerator tiny cnn ok queue=%u fence=%u status=0x%x out=%08x,%08x,%08x,%08x\n",
		 queue_id, fence_seq, job->status, job->output[0],
		 job->output[1], job->output[2], job->output[3]);
	ret = 0;

out_unlock:
	mutex_unlock(&test->lock);
	return ret;
}

static int apollo_hexagon_submit_vadd(
	struct device *dev, struct apollo_hexagon *test,
	struct drm_apollo_hexagon_vadd_job *job)
{
	const u32 input_iova = lower_32_bits(test->dma_iova_base +
					     APOLLO_HEXAGON_INPUT_OFFSET);
	const u32 output_iova = lower_32_bits(test->dma_iova_base +
					      APOLLO_HEXAGON_OUTPUT_OFFSET);
	u32 queue_id = job->queue_id ? job->queue_id : APOLLO_HEXAGON_QUEUE_VADD;
	u32 fence_seq = 0;
	int ret;

	if (queue_id != APOLLO_HEXAGON_QUEUE_VADD)
		return -EINVAL;

	mutex_lock(&test->lock);

	ret = apollo_hexagon_dynamic_map(dev, test);
	if (ret)
		goto out_unlock;

	apollo_hexagon_write_guest_vadd_input(test, job);
	dma_wmb();

	writel(input_iova, test->regs + APOLLO_HEXAGON_REG_JOB_INPUT);
	writel(output_iova, test->regs + APOLLO_HEXAGON_REG_JOB_OUTPUT);
	writel(APOLLO_HEXAGON_VADD_INPUT_WORDS * sizeof(u32),
	       test->regs + APOLLO_HEXAGON_REG_JOB_INPUT_BYTES);
	writel(APOLLO_HEXAGON_VADD_OUTPUT_WORDS * sizeof(u32),
	       test->regs + APOLLO_HEXAGON_REG_JOB_OUTPUT_BYTES);
	writel(queue_id, test->regs + APOLLO_HEXAGON_REG_JOB_QUEUE);
	writel(0, test->regs + APOLLO_HEXAGON_REG_JOB_STATUS);
	writel(0, test->regs + APOLLO_HEXAGON_REG_JOB_RESULT);
	apollo_hexagon_prepare_async_fence(test, queue_id);
	writel(APOLLO_HEXAGON_JOB_CTRL_START,
	       test->regs + APOLLO_HEXAGON_REG_JOB_CTRL);

	ret = apollo_hexagon_wait_job(dev, test, queue_id,
				      APOLLO_HEXAGON_JOB_RESULT_VADD_OK,
				      &fence_seq, NULL, NULL);
	if (ret)
		goto out_unlock;

	dma_rmb();
	apollo_hexagon_read_guest_vadd_output(test, job);
	job->status = APOLLO_HEXAGON_JOB_RESULT_VADD_OK;
	job->queue_id = queue_id;
	job->fence_seq = fence_seq;
	dev_info(dev,
		 "accelerator vector add ok queue=%u fence=%u status=0x%x out=%08x,%08x,%08x,%08x\n",
		 queue_id, fence_seq, job->status, job->output[0],
		 job->output[1], job->output[2], job->output[3]);
	ret = 0;

out_unlock:
	mutex_unlock(&test->lock);
	return ret;
}

static u32 apollo_hexagon_stress_word(u32 index, u32 seed)
{
	return seed ^ (0x9e3779b9u * (index + 1));
}

static void apollo_hexagon_stress_write_segment(
	struct apollo_hexagon *test, u32 offset, u32 first_word, u32 words,
	u32 seed)
{
	u32 i;

	for (i = 0; i < words; i++)
		writel(apollo_hexagon_stress_word(first_word + i, seed),
		       test->shared_base + offset + i * sizeof(u32));
}

static u32 apollo_hexagon_stress_verify_segment(
	struct apollo_hexagon *test, u32 offset, u32 first_word, u32 words,
	u32 seed, bool *matched)
{
	u32 checksum = 0;
	u32 i;

	for (i = 0; i < words; i++) {
		u32 expected = apollo_hexagon_stress_word(first_word + i, seed);
		u32 got = readl(test->shared_base + offset + i * sizeof(u32));

		checksum ^= got + first_word + i;
		if (got != expected)
			*matched = false;
	}

	return checksum;
}

static void apollo_hexagon_stress_clear_segment(struct apollo_hexagon *test,
						u32 offset, u32 words)
{
	u32 i;

	for (i = 0; i < words; i++)
		writel(0, test->shared_base + offset + i * sizeof(u32));
}

static int apollo_hexagon_submit_dma_stress(
	struct device *dev, struct apollo_hexagon *test,
	struct drm_apollo_hexagon_dma_stress_job *job)
{
	const u32 bytes = job->bytes ? job->bytes :
		APOLLO_HEXAGON_DMA_STRESS_BYTES;
	const u32 segment_bytes = job->segment_bytes ? job->segment_bytes :
		APOLLO_HEXAGON_DMA_STRESS_BYTES /
		APOLLO_HEXAGON_DMA_STRESS_SEGMENTS;
	const u32 words_per_segment = segment_bytes / sizeof(u32);
	const u32 input_iova = lower_32_bits(test->dma_iova_base +
					     APOLLO_HEXAGON_STRESS_INPUT_BASE);
	const u32 output_iova = lower_32_bits(test->dma_iova_base +
					      APOLLO_HEXAGON_STRESS_OUTPUT_BASE);
	u32 queue_id = job->queue_id;
	u32 fence_seq = 0;
	u32 checksum = 0;
	bool matched = true;
	int ret;
	u32 i;

	if (bytes != APOLLO_HEXAGON_DMA_STRESS_BYTES ||
	    segment_bytes != bytes / APOLLO_HEXAGON_DMA_STRESS_SEGMENTS ||
	    segment_bytes % sizeof(u32))
		return -EINVAL;
	if (queue_id != APOLLO_HEXAGON_QUEUE_DMA)
		return -EINVAL;

	mutex_lock(&test->lock);

	ret = apollo_hexagon_dynamic_sg_map(dev, test);
	if (ret)
		goto out_unlock;

	for (i = 0; i < APOLLO_HEXAGON_DMA_STRESS_SEGMENTS; i++) {
		apollo_hexagon_stress_write_segment(
			test,
			APOLLO_HEXAGON_STRESS_INPUT_BASE +
			i * APOLLO_HEXAGON_STRESS_SEGMENT_STRIDE,
			i * words_per_segment, words_per_segment, job->seed);
		apollo_hexagon_stress_clear_segment(
			test,
			APOLLO_HEXAGON_STRESS_OUTPUT_BASE +
			i * APOLLO_HEXAGON_STRESS_SEGMENT_STRIDE,
			words_per_segment);
	}
	dma_wmb();

	writel(input_iova, test->regs + APOLLO_HEXAGON_REG_JOB_INPUT);
	writel(output_iova, test->regs + APOLLO_HEXAGON_REG_JOB_OUTPUT);
	writel(bytes, test->regs + APOLLO_HEXAGON_REG_JOB_INPUT_BYTES);
	writel(bytes, test->regs + APOLLO_HEXAGON_REG_JOB_OUTPUT_BYTES);
	writel(queue_id, test->regs + APOLLO_HEXAGON_REG_JOB_QUEUE);
	writel(0, test->regs + APOLLO_HEXAGON_REG_JOB_STATUS);
	writel(0, test->regs + APOLLO_HEXAGON_REG_JOB_RESULT);
	apollo_hexagon_prepare_async_fence(test, queue_id);
	writel(APOLLO_HEXAGON_JOB_CTRL_START,
	       test->regs + APOLLO_HEXAGON_REG_JOB_CTRL);

	ret = apollo_hexagon_wait_job(dev, test, queue_id,
				      APOLLO_HEXAGON_JOB_RESULT_SG_OK,
				      &fence_seq, NULL, NULL);
	if (ret)
		goto out_unlock;

	dma_rmb();
	for (i = 0; i < APOLLO_HEXAGON_DMA_STRESS_SEGMENTS; i++)
		checksum ^= apollo_hexagon_stress_verify_segment(
			test,
			APOLLO_HEXAGON_STRESS_OUTPUT_BASE +
			i * APOLLO_HEXAGON_STRESS_SEGMENT_STRIDE,
			i * words_per_segment, words_per_segment, job->seed,
			&matched);
	if (!matched) {
		ret = -EIO;
		dev_err(dev, "SG DMA stress data mismatch checksum=0x%x\n",
			checksum);
		goto out_unlock;
	}

	ret = apollo_hexagon_run_dma_stress_selftests(dev, test);
	if (ret)
		goto out_unlock;

	job->bytes = bytes;
	job->segment_bytes = segment_bytes;
	job->checksum = checksum;
	job->status = APOLLO_HEXAGON_JOB_RESULT_SG_OK;
	job->queue_id = queue_id;
	job->fence_seq = fence_seq;
	dev_info(dev,
		 "SG DMA stress ok queue=%u fence=%u bytes=%u segments=%u checksum=0x%08x status=0x%x\n",
		 queue_id, fence_seq, bytes, APOLLO_HEXAGON_DMA_STRESS_SEGMENTS,
		 checksum, job->status);
	ret = 0;

out_unlock:
	mutex_unlock(&test->lock);
	return ret;
}

int apollo_hexagon_ioctl_submit_cnn(struct drm_device *drm, void *data,
				    struct drm_file *file)
{
	struct apollo_hexagon *test = to_apollo_hexagon(drm);
	int idx;
	int ret;

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;

	ret = apollo_hexagon_submit_cnn(test->dev, test, data);
	drm_dev_exit(idx);

	return ret;
}

int apollo_hexagon_ioctl_submit_vadd(struct drm_device *drm, void *data,
				     struct drm_file *file)
{
	struct apollo_hexagon *test = to_apollo_hexagon(drm);
	int idx;
	int ret;

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;

	ret = apollo_hexagon_submit_vadd(test->dev, test, data);
	drm_dev_exit(idx);

	return ret;
}

int apollo_hexagon_ioctl_dma_stress(struct drm_device *drm, void *data,
				    struct drm_file *file)
{
	struct apollo_hexagon *test = to_apollo_hexagon(drm);
	struct drm_apollo_hexagon_dma_stress_job *job = data;
	int idx;
	int ret;

	if (!job->seed)
		job->seed = APOLLO_HEXAGON_STRESS_SEED;

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;

	ret = apollo_hexagon_submit_dma_stress(test->dev, test, job);
	drm_dev_exit(idx);

	return ret;
}
