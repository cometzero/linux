// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apollo Hexagon DRM accel driver.
 */

#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/completion.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/string.h>

#include <drm/apollo_hexagon_accel.h>
#include <drm/drm_accel.h>
#include <drm/drm_drv.h>
#include <drm/drm_gem.h>
#include <drm/drm_gem_shmem_helper.h>
#include <drm/drm_ioctl.h>

#include "apollo-hexagon.h"

void apollo_hexagon_prepare_async_fence(struct apollo_hexagon *test,
					u32 queue_id)
{
	atomic_set(&test->async_irq_pending, 0);
	reinit_completion(&test->async_fence);
	writel(BIT(queue_id), test->regs + APOLLO_HEXAGON_REG_IRQ_ACK);
}

static irqreturn_t apollo_hexagon_irq(int irq, void *data)
{
	struct apollo_hexagon *test = data;
	u32 pending = readl(test->regs + APOLLO_HEXAGON_REG_IRQ_STATUS);

	if (!pending)
		return IRQ_NONE;

	atomic_or(pending, &test->async_irq_pending);
	writel(pending, test->regs + APOLLO_HEXAGON_REG_IRQ_ACK);
	complete_all(&test->async_fence);

	return IRQ_HANDLED;
}

static int apollo_hexagon_check_dma_abi(struct device *dev,
					struct apollo_hexagon *test)
{
	u32 expected_stream_id = 1;
	u32 caps;
	u32 path;
	u32 queue_count;
	u32 stream_id;
	const char *expected_path = NULL;
	int ret;

	ret = of_property_read_u32(dev->of_node, "apollo,smmu-stream-id",
				   &expected_stream_id);
	if (ret && ret != -EINVAL)
		return dev_err_probe(dev, ret,
				     "failed to read apollo,smmu-stream-id\n");

	caps = readl(test->regs + APOLLO_HEXAGON_REG_CAPS);
	path = readl(test->regs + APOLLO_HEXAGON_REG_PATH);
	queue_count = readl(test->regs + APOLLO_HEXAGON_REG_QUEUE_CAPS);
	stream_id = readl(test->regs + APOLLO_HEXAGON_REG_STREAM_ID);

	if (!(caps & APOLLO_HEXAGON_CAP_COPY_ENGINE))
		return dev_err_probe(dev, -ENODEV,
				     "missing Hexagon DMA copy capability\n");
	if (!(caps & APOLLO_HEXAGON_CAP_LARGE_TENSOR) ||
	    !(caps & APOLLO_HEXAGON_CAP_MULTI_QUEUE) ||
	    !(caps & APOLLO_HEXAGON_CAP_ASYNC_FENCE) ||
	    queue_count < 2)
		return dev_err_probe(dev, -ENODEV,
				     "missing Hexagon queue/fence/large tensor capability caps=0x%x queues=%u\n",
				     caps, queue_count);

	if (stream_id != expected_stream_id)
		return dev_err_probe(dev, -EINVAL,
				     "stream-id mismatch hw=0x%x dt=0x%x\n",
				     stream_id, expected_stream_id);
	test->stream_id = stream_id;

	ret = of_property_read_string(dev->of_node, "apollo,dma-path",
				      &expected_path);
	if (ret && ret != -EINVAL)
		return dev_err_probe(dev, ret,
				     "failed to read apollo,dma-path\n");

	if (path == APOLLO_HEXAGON_PATH_SMMU_TRANSLATED &&
	    (caps & APOLLO_HEXAGON_CAP_SMMU_TRANSLATED)) {
		if (expected_path && strcmp(expected_path, "smmu-translated"))
			return dev_err_probe(dev, -EINVAL,
					     "DMA path mismatch hw=smmu-translated dt=%s\n",
					     expected_path);

		dev_info(dev, "dma path smmu-translated caps=0x%x stream-id=0x%x smmuv3-translated=yes queues=%u async-fence=yes large-tensor=yes\n",
			 caps, stream_id, queue_count);
		return 0;
	}

	if (path == APOLLO_HEXAGON_PATH_DIRECT_TLM &&
	    (caps & APOLLO_HEXAGON_CAP_DIRECT_TLM)) {
		if (expected_path && strcmp(expected_path, "direct-tlm"))
			return dev_err_probe(dev, -EINVAL,
					     "DMA path mismatch hw=direct-tlm dt=%s\n",
					     expected_path);

		dev_info(dev,
			 "dma path direct-tlm caps=0x%x stream-id=0x%x smmuv3-translated=no\n",
			 caps, stream_id);
		return 0;
	}

	return dev_err_probe(dev, -EINVAL,
			     "unsupported DMA path path=0x%x caps=0x%x\n",
			     path, caps);
}

static int apollo_hexagon_map_shared(struct device *dev,
				     struct apollo_hexagon *test)
{
	struct device_node *mem_np;
	struct resource res;
	int ret;

	mem_np = of_parse_phandle(dev->of_node, "memory-region", 1);
	if (!mem_np)
		return dev_err_probe(dev, -ENOENT,
				     "missing shared SRAM memory-region\n");

	ret = of_address_to_resource(mem_np, 0, &res);
	of_node_put(mem_np);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to resolve shared SRAM\n");

	if (resource_size(&res) < APOLLO_HEXAGON_OUTPUT_OFFSET +
	    APOLLO_HEXAGON_CNN_OUTPUT_WORDS * sizeof(u32))
		return dev_err_probe(dev, -EINVAL,
				     "shared SRAM too small for accelerator ABI\n");
	if (resource_size(&res) < APOLLO_HEXAGON_STRESS_OUTPUT_BASE +
	    (APOLLO_HEXAGON_DMA_STRESS_SEGMENTS - 1) *
	    APOLLO_HEXAGON_STRESS_SEGMENT_STRIDE +
	    APOLLO_HEXAGON_DMA_STRESS_BYTES /
	    APOLLO_HEXAGON_DMA_STRESS_SEGMENTS)
		return dev_err_probe(dev, -EINVAL,
				     "shared SRAM too small for >64KB SG DMA stress\n");

	test->shared_base = devm_ioremap(dev, res.start, resource_size(&res));
	if (!test->shared_base)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to map shared SRAM\n");

	test->shared_phys = res.start;
	test->shared_size = resource_size(&res);

	return 0;
}

int apollo_hexagon_wait_job(struct device *dev, struct apollo_hexagon *test,
			    u32 queue_id, u32 expected_result,
			    u32 *fence_seq, u32 *final_status,
			    u32 *final_result)
{
	u32 status = APOLLO_HEXAGON_JOB_STATUS_ERROR;
	u32 result;
	u32 irq;
	unsigned long timeout;
	int tries;

	status = readl(test->regs + APOLLO_HEXAGON_REG_JOB_STATUS);
	irq = readl(test->regs + APOLLO_HEXAGON_REG_IRQ_STATUS) |
	      atomic_read(&test->async_irq_pending);
	if (test->doorbell_irq >= 0 &&
	    status != APOLLO_HEXAGON_JOB_STATUS_DONE &&
	    status != APOLLO_HEXAGON_JOB_STATUS_ERROR &&
	    !(irq & BIT(queue_id))) {
		timeout = wait_for_completion_timeout(
			&test->async_fence,
			msecs_to_jiffies(APOLLO_HEXAGON_ACCEL_TIMEOUT_MS));
		if (timeout) {
			dev_info(dev, "async fence irq wait signaled queue=%u irq=%d\n",
				 queue_id, test->doorbell_irq);
		} else {
			dev_warn(dev, "async fence irq wait timeout queue=%u irq=%d; polling fallback\n",
				 queue_id, test->doorbell_irq);
		}
	}

	for (tries = 0;
	     tries < APOLLO_HEXAGON_ACCEL_TIMEOUT_MS /
		     APOLLO_HEXAGON_ACCEL_POLL_MS;
	     tries++) {
		status = readl(test->regs + APOLLO_HEXAGON_REG_JOB_STATUS);
		if (status == APOLLO_HEXAGON_JOB_STATUS_DONE)
			break;
		if (status == APOLLO_HEXAGON_JOB_STATUS_ERROR)
			break;
		msleep(APOLLO_HEXAGON_ACCEL_POLL_MS);
	}

	result = readl(test->regs + APOLLO_HEXAGON_REG_JOB_RESULT);
	if (final_status)
		*final_status = status;
	if (final_result)
		*final_result = result;
	if (status != APOLLO_HEXAGON_JOB_STATUS_DONE ||
	    result != expected_result) {
		dev_err(dev, "accelerator job failed queue=%u status=0x%x result=0x%x expected=0x%x\n",
			queue_id, status, result, expected_result);
		return -EIO;
	}

	irq = readl(test->regs + APOLLO_HEXAGON_REG_IRQ_STATUS) |
	      atomic_read(&test->async_irq_pending);
	if (!(irq & BIT(queue_id))) {
		dev_err(dev, "async irq missing queue=%u irq=0x%x\n", queue_id,
			irq);
		return -EIO;
	}

	*fence_seq = readl(test->regs + APOLLO_HEXAGON_REG_JOB_FENCE);
	atomic_andnot(BIT(queue_id), &test->async_irq_pending);
	writel(BIT(queue_id), test->regs + APOLLO_HEXAGON_REG_IRQ_ACK);
	dev_info(dev, "async fence signaled queue=%u fence=%u irq=0x%x\n",
		 queue_id, *fence_seq, irq);

	return 0;
}

static int apollo_hexagon_ioctl_query(struct drm_device *drm, void *data,
				      struct drm_file *file)
{
	struct apollo_hexagon *test = to_apollo_hexagon(drm);
	struct drm_apollo_hexagon_query *args = data;
	int idx;

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;

	args->stream_id = test->stream_id;
	args->queue_count = readl(test->regs + APOLLO_HEXAGON_REG_QUEUE_CAPS);
	args->capabilities = readl(test->regs + APOLLO_HEXAGON_REG_CAPS);
	args->dma_path = readl(test->regs + APOLLO_HEXAGON_REG_PATH);
	args->primary_endpoint = test->primary_endpoint;
	args->pad = 0;

	drm_dev_exit(idx);

	return 0;
}

static int apollo_hexagon_ioctl_query_caps(struct drm_device *drm, void *data,
					   struct drm_file *file)
{
	struct apollo_hexagon *test = to_apollo_hexagon(drm);
	struct drm_apollo_hexagon_query_caps *args = data;
	int idx;

	if (args->size != sizeof(*args) || args->flags)
		return -EINVAL;
	if (args->reserved[0] || args->reserved[1] ||
	    args->reserved[2] || args->reserved[3])
		return -EINVAL;

	if (!drm_dev_enter(drm, &idx))
		return -ENODEV;

	args->generic_abi_version = APOLLO_HEXAGON_GENERIC_ABI_VERSION;
	args->supported_executable_formats =
		APOLLO_HEXAGON_EXEC_FORMAT_APKO_V0_BIT;
	args->max_command_bytes = APOLLO_HEXAGON_CMDQ_SUBMIT_MAX_BYTES;
	args->max_bindings_per_dispatch = 2;
	args->max_queue_count =
		readl(test->regs + APOLLO_HEXAGON_REG_QUEUE_CAPS);
	args->max_queue_depth = 1;
	args->fence_model = APOLLO_HEXAGON_FENCE_MODEL_ASYNC_IRQ_POLL;
	args->smmu_page_granularity = SZ_4K;
	args->coherency_flags = 0;
	args->fault_record_size = sizeof(struct drm_apollo_hexagon_fault);

	drm_dev_exit(idx);

	return 0;
}

#define APOLLO_HEXAGON_IOCTL(n, func) \
	DRM_IOCTL_DEF_DRV(APOLLO_HEXAGON_##n, apollo_hexagon_ioctl_##func, 0)

static const struct drm_ioctl_desc apollo_hexagon_drm_ioctls[] = {
	APOLLO_HEXAGON_IOCTL(QUERY, query),
	APOLLO_HEXAGON_IOCTL(SUBMIT_CNN, submit_cnn),
	APOLLO_HEXAGON_IOCTL(SUBMIT_VADD, submit_vadd),
	APOLLO_HEXAGON_IOCTL(DMA_STRESS, dma_stress),
	APOLLO_HEXAGON_IOCTL(EXEC_CREATE, exec_create),
	APOLLO_HEXAGON_IOCTL(EXEC_DESTROY, exec_destroy),
	APOLLO_HEXAGON_IOCTL(SUBMIT, submit),
	APOLLO_HEXAGON_IOCTL(GET_FAULT, get_fault),
	APOLLO_HEXAGON_IOCTL(QUERY_CAPS, query_caps),
	APOLLO_HEXAGON_IOCTL(CONTEXT_CREATE, context_create),
	APOLLO_HEXAGON_IOCTL(CONTEXT_DESTROY, context_destroy),
	APOLLO_HEXAGON_IOCTL(BO_CREATE, bo_create),
	APOLLO_HEXAGON_IOCTL(BO_DESTROY, bo_destroy),
	APOLLO_HEXAGON_IOCTL(WAIT, wait),
	APOLLO_HEXAGON_IOCTL(BO_BIND, bo_bind),
	APOLLO_HEXAGON_IOCTL(BO_UNBIND, bo_unbind),
	APOLLO_HEXAGON_IOCTL(CMD_SUBMIT, cmd_submit),
};

DEFINE_DRM_ACCEL_FOPS(apollo_hexagon_accel_fops);

static const struct drm_driver apollo_hexagon_drm_driver = {
	.driver_features = DRIVER_COMPUTE_ACCEL | DRIVER_GEM,
	.open = apollo_hexagon_file_open,
	.postclose = apollo_hexagon_file_postclose,
	.ioctls = apollo_hexagon_drm_ioctls,
	.num_ioctls = ARRAY_SIZE(apollo_hexagon_drm_ioctls),
	.fops = &apollo_hexagon_accel_fops,
	.name = "apollo_hexagon",
	.desc = "Apollo Hexagon DRM accel driver",
	.major = 1,
	.minor = 0,
	DRM_GEM_SHMEM_DRIVER_OPS,
};

static int apollo_hexagon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apollo_hexagon *test;
	int ret;

	test = devm_drm_dev_alloc(dev, &apollo_hexagon_drm_driver,
				  struct apollo_hexagon, drm);
	if (IS_ERR(test))
		return PTR_ERR(test);
	test->dev = dev;

	test->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(test->regs))
		return dev_err_probe(dev, PTR_ERR(test->regs),
				     "failed to map control window\n");
	test->tbu_regs = test->regs + APOLLO_HEXAGON_TBU_REG_OFFSET;
	mutex_init(&test->lock);
	init_completion(&test->async_fence);

	test->doorbell_irq = platform_get_irq_byname_optional(pdev,
							      "doorbell");
	if (test->doorbell_irq == -ENXIO) {
		dev_warn(dev,
			 "doorbell irq absent; async fence uses polling fallback\n");
	} else if (test->doorbell_irq < 0) {
		return dev_err_probe(dev, test->doorbell_irq,
				     "failed to resolve doorbell irq\n");
	} else {
		ret = devm_request_irq(dev, test->doorbell_irq,
				       apollo_hexagon_irq, 0, dev_name(dev),
				       test);
		if (ret)
			return dev_err_probe(dev, ret,
					     "failed to request doorbell irq\n");
		dev_info(dev, "async doorbell irq ready irq=%d\n",
			 test->doorbell_irq);
	}

	ret = of_property_read_u64(dev->of_node, "apollo,dma-iova-base",
				   &test->dma_iova_base);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read apollo,dma-iova-base\n");
	test->requested_dma_iova_base = test->dma_iova_base;
	ret = of_property_read_u64(dev->of_node, "apollo,dma-window-size",
				   &test->dma_window_size);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read apollo,dma-window-size\n");

	ret = apollo_hexagon_map_shared(dev, test);
	if (ret)
		return ret;

	ret = apollo_hexagon_check_dma_abi(dev, test);
	if (ret)
		return ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to set 32-bit DMA mask\n");

	ret = apollo_hexagon_configure_linux_iommu(dev, test);
	if (ret)
		return ret;

	ret = apollo_hexagon_run_selftests(dev, test);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, test);
	test->primary_endpoint =
		of_property_read_bool(dev->of_node, "apollo,primary-endpoint");

	ret = drm_dev_register(&test->drm, 0);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register DRM accel device\n");

	dev_info(dev,
		 "userspace submit ABI ready at /dev/accel/accel* stream-id=0x%x primary=%u\n",
		 test->stream_id, test->primary_endpoint);
	dev_info(dev, "probe ok\n");

	return 0;
}

static void apollo_hexagon_remove(struct platform_device *pdev)
{
	struct apollo_hexagon *test = platform_get_drvdata(pdev);

	drm_dev_unplug(&test->drm);
}

static const struct of_device_id apollo_hexagon_of_match[] = {
	{ .compatible = "apollo,hexagon-ip" },
	{ }
};
MODULE_DEVICE_TABLE(of, apollo_hexagon_of_match);

static struct platform_driver apollo_hexagon_driver = {
	.probe = apollo_hexagon_probe,
	.remove = apollo_hexagon_remove,
	.driver = {
		.name = "apollo-hexagon",
		.of_match_table = apollo_hexagon_of_match,
	},
};
module_platform_driver(apollo_hexagon_driver);

MODULE_DESCRIPTION("Apollo Hexagon DRM accel driver");
MODULE_LICENSE("GPL");
