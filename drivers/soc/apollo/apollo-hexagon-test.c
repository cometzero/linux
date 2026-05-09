// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apollo Hexagon IOMMU probe driver.
 */

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <linux/apollo_hexagon.h>

#define APOLLO_HEXAGON_TEST_SIZE	SZ_4K
#define APOLLO_HEXAGON_TEST_PATTERN	0xa510beef
#define APOLLO_HEXAGON_DMA_TIMEOUT_MS	2000
#define APOLLO_HEXAGON_DMA_POLL_MS	10
#define APOLLO_HEXAGON_ACCEL_TIMEOUT_MS	2000
#define APOLLO_HEXAGON_ACCEL_POLL_MS	10
#define APOLLO_HEXAGON_REG_CAPS		0x1c
#define APOLLO_HEXAGON_REG_PATH		0x20
#define APOLLO_HEXAGON_REG_STREAM_ID	0x24
#define APOLLO_HEXAGON_REG_JOB_INPUT	0x40
#define APOLLO_HEXAGON_REG_JOB_OUTPUT	0x44
#define APOLLO_HEXAGON_REG_JOB_INPUT_BYTES	0x48
#define APOLLO_HEXAGON_REG_JOB_OUTPUT_BYTES	0x4c
#define APOLLO_HEXAGON_REG_JOB_CTRL	0x50
#define APOLLO_HEXAGON_REG_JOB_STATUS	0x54
#define APOLLO_HEXAGON_REG_JOB_RESULT	0x58
#define APOLLO_HEXAGON_CAP_COPY_ENGINE	BIT(0)
#define APOLLO_HEXAGON_CAP_DIRECT_TLM	BIT(1)
#define APOLLO_HEXAGON_CAP_SMMU_TRANSLATED	BIT(2)
#define APOLLO_HEXAGON_PATH_DIRECT_TLM		1
#define APOLLO_HEXAGON_PATH_SMMU_TRANSLATED	2
#define APOLLO_HEXAGON_JOB_CTRL_START	1
#define APOLLO_HEXAGON_JOB_STATUS_DONE	1
#define APOLLO_HEXAGON_JOB_STATUS_ERROR	2
#define APOLLO_HEXAGON_JOB_RESULT_OK	0x434e4e4f
#define APOLLO_HEXAGON_INPUT_OFFSET	0x10000
#define APOLLO_HEXAGON_OUTPUT_OFFSET	0x11000
#define APOLLO_HEXAGON_TBU_REG_OFFSET	0x1000
#define APOLLO_TBU_REG_MAP_IOVA_LO	0x00
#define APOLLO_TBU_REG_MAP_IOVA_HI	0x04
#define APOLLO_TBU_REG_MAP_PA_LO	0x08
#define APOLLO_TBU_REG_MAP_PA_HI	0x0c
#define APOLLO_TBU_REG_MAP_SIZE_LO	0x10
#define APOLLO_TBU_REG_MAP_SIZE_HI	0x14
#define APOLLO_TBU_REG_MAP_CTRL		0x18
#define APOLLO_TBU_REG_MAP_STATUS	0x1c
#define APOLLO_TBU_REG_MAP_COUNT	0x20
#define APOLLO_TBU_MAP_CTRL_ADD		1
#define APOLLO_TBU_MAP_CTRL_REMOVE	2
#define APOLLO_TBU_MAP_STATUS_OK	1

static const u32 apollo_hexagon_dma_expected[] = {
	0x48455831, 0x444d4131, 0x11223344, 0x55667788,
	0xa5a55a5a, 0x5a5aa5a5, 0xfeedc0de, 0x600dbeef,
};

struct apollo_hexagon_test {
	void __iomem *regs;
	void __iomem *tbu_regs;
	void __iomem *shared_base;
	phys_addr_t shared_phys;
	u64 dma_iova_base;
	u64 dma_window_size;
	struct miscdevice miscdev;
	struct mutex lock;
	void *cpu_addr;
	dma_addr_t dma_addr;
	size_t size;
};

static int apollo_hexagon_check_dma_abi(struct device *dev,
					struct apollo_hexagon_test *test)
{
	u32 expected_stream_id = 1;
	u32 caps;
	u32 path;
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
	stream_id = readl(test->regs + APOLLO_HEXAGON_REG_STREAM_ID);

	if (!(caps & APOLLO_HEXAGON_CAP_COPY_ENGINE))
		return dev_err_probe(dev, -ENODEV,
				     "missing Hexagon DMA copy capability\n");

	if (stream_id != expected_stream_id)
		return dev_err_probe(dev, -EINVAL,
				     "stream-id mismatch hw=0x%x dt=0x%x\n",
				     stream_id, expected_stream_id);

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

		dev_info(dev, "dma path smmu-translated caps=0x%x stream-id=0x%x smmuv3-translated=yes\n",
			 caps, stream_id);
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

static int apollo_hexagon_check_firmware_dma(struct device *dev)
{
	struct device_node *mem_np;
	struct resource res;
	void __iomem *base;
	phys_addr_t start;
	u32 got0 = 0;
	int ret;
	int tries;
	int i;

	mem_np = of_parse_phandle(dev->of_node, "memory-region", 1);
	if (!mem_np)
		return dev_err_probe(dev, -ENOENT,
				     "missing dma-test memory-region\n");

	ret = of_address_to_resource(mem_np, 0, &res);
	of_node_put(mem_np);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to resolve dma-test memory\n");

	if (resource_size(&res) < sizeof(apollo_hexagon_dma_expected))
		return dev_err_probe(dev, -EINVAL,
				     "dma-test memory too small\n");

	base = devm_ioremap(dev, res.start,
			    sizeof(apollo_hexagon_dma_expected));
	if (!base)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to map dma-test memory\n");

	for (tries = 0;
	     tries < APOLLO_HEXAGON_DMA_TIMEOUT_MS / APOLLO_HEXAGON_DMA_POLL_MS;
	     tries++) {
		bool matched = true;

		for (i = 0; i < ARRAY_SIZE(apollo_hexagon_dma_expected); i++) {
			u32 got = readl((u8 __iomem *)base + sizeof(u32) * i);

			if (i == 0)
				got0 = got;
			if (got != apollo_hexagon_dma_expected[i]) {
				matched = false;
				break;
			}
		}
		if (matched) {
			start = res.start;
			dev_info(dev,
				 "firmware dma traffic ok dst=%pa words=%zu first=0x%08x\n",
				 &start, ARRAY_SIZE(apollo_hexagon_dma_expected),
				 got0);
			return 0;
		}
		msleep(APOLLO_HEXAGON_DMA_POLL_MS);
	}

	start = res.start;
	dev_err(dev,
		"firmware dma traffic timeout dst=%pa first=0x%08x expected=0x%08x\n",
		&start, got0, apollo_hexagon_dma_expected[0]);

	return -ETIMEDOUT;
}

static void apollo_tbu_write64(void __iomem *base, u32 lo, u32 hi, u64 value)
{
	writel(lower_32_bits(value), base + lo);
	writel(upper_32_bits(value), base + hi);
}

static int apollo_hexagon_tbu_ctrl(struct device *dev,
				   struct apollo_hexagon_test *test, u32 ctrl)
{
	u32 status;

	writel(ctrl, test->tbu_regs + APOLLO_TBU_REG_MAP_CTRL);
	status = readl(test->tbu_regs + APOLLO_TBU_REG_MAP_STATUS);
	if (status != APOLLO_TBU_MAP_STATUS_OK)
		return dev_err_probe(dev, -EIO,
				     "TBU map ctrl=%u failed status=0x%x count=%u\n",
				     ctrl, status,
				     readl(test->tbu_regs +
					   APOLLO_TBU_REG_MAP_COUNT));

	return 0;
}

static int apollo_hexagon_dynamic_map(struct device *dev,
				      struct apollo_hexagon_test *test)
{
	int ret;

	apollo_tbu_write64(test->tbu_regs, APOLLO_TBU_REG_MAP_IOVA_LO,
			   APOLLO_TBU_REG_MAP_IOVA_HI, test->dma_iova_base);
	apollo_tbu_write64(test->tbu_regs, APOLLO_TBU_REG_MAP_PA_LO,
			   APOLLO_TBU_REG_MAP_PA_HI, test->shared_phys);
	apollo_tbu_write64(test->tbu_regs, APOLLO_TBU_REG_MAP_SIZE_LO,
			   APOLLO_TBU_REG_MAP_SIZE_HI, test->dma_window_size);

	ret = apollo_hexagon_tbu_ctrl(dev, test, APOLLO_TBU_MAP_CTRL_ADD);
	if (ret)
		return ret;
	ret = apollo_hexagon_tbu_ctrl(dev, test, APOLLO_TBU_MAP_CTRL_REMOVE);
	if (ret)
		return ret;
	ret = apollo_hexagon_tbu_ctrl(dev, test, APOLLO_TBU_MAP_CTRL_ADD);
	if (ret)
		return ret;

	dev_info(dev, "dynamic SMMU map refreshed iova=0x%llx pa=%pa size=0x%llx count=%u\n",
		 test->dma_iova_base, &test->shared_phys,
		 test->dma_window_size,
		 readl(test->tbu_regs + APOLLO_TBU_REG_MAP_COUNT));

	return 0;
}

static int apollo_hexagon_map_shared(struct device *dev,
				     struct apollo_hexagon_test *test)
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

	test->shared_base = devm_ioremap(dev, res.start, resource_size(&res));
	if (!test->shared_base)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to map shared SRAM\n");

	test->shared_phys = res.start;

	return 0;
}

static void apollo_hexagon_write_guest_input(struct apollo_hexagon_test *test,
					    const struct apollo_hexagon_cnn_job *job)
{
	int i;

	for (i = 0; i < APOLLO_HEXAGON_CNN_INPUT_WORDS; i++)
		writel(job->input[i],
		       test->shared_base + APOLLO_HEXAGON_INPUT_OFFSET +
		       i * sizeof(u32));
}

static void apollo_hexagon_read_guest_output(struct apollo_hexagon_test *test,
					     struct apollo_hexagon_cnn_job *job)
{
	int i;

	for (i = 0; i < APOLLO_HEXAGON_CNN_OUTPUT_WORDS; i++)
		job->output[i] =
			readl(test->shared_base + APOLLO_HEXAGON_OUTPUT_OFFSET +
			      i * sizeof(u32));
}

static int apollo_hexagon_submit_cnn(struct device *dev,
				     struct apollo_hexagon_test *test,
				     struct apollo_hexagon_cnn_job *job)
{
	const u32 input_iova = lower_32_bits(test->dma_iova_base +
					     APOLLO_HEXAGON_INPUT_OFFSET);
	const u32 output_iova = lower_32_bits(test->dma_iova_base +
					      APOLLO_HEXAGON_OUTPUT_OFFSET);
	u32 status;
	u32 result;
	int ret;
	int tries;

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
	writel(0, test->regs + APOLLO_HEXAGON_REG_JOB_STATUS);
	writel(0, test->regs + APOLLO_HEXAGON_REG_JOB_RESULT);
	writel(APOLLO_HEXAGON_JOB_CTRL_START,
	       test->regs + APOLLO_HEXAGON_REG_JOB_CTRL);

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
	if (status != APOLLO_HEXAGON_JOB_STATUS_DONE ||
	    result != APOLLO_HEXAGON_JOB_RESULT_OK) {
		ret = -EIO;
		dev_err(dev,
			"accelerator job failed status=0x%x result=0x%x\n",
			status, result);
		goto out_unlock;
	}

	dma_rmb();
	apollo_hexagon_read_guest_output(test, job);
	job->status = result;
	dev_info(dev, "accelerator tiny cnn ok status=0x%x out=%08x,%08x,%08x,%08x\n",
		 result, job->output[0], job->output[1], job->output[2],
		 job->output[3]);
	ret = 0;

out_unlock:
	mutex_unlock(&test->lock);
	return ret;
}

static int apollo_hexagon_open(struct inode *inode, struct file *file)
{
	struct miscdevice *miscdev = file->private_data;
	struct apollo_hexagon_test *test =
		container_of(miscdev, struct apollo_hexagon_test, miscdev);

	file->private_data = test;
	return 0;
}

static long apollo_hexagon_ioctl(struct file *file, unsigned int cmd,
				 unsigned long arg)
{
	struct apollo_hexagon_test *test = file->private_data;
	struct apollo_hexagon_cnn_job job;
	int ret;

	if (cmd != APOLLO_HEXAGON_IOC_SUBMIT_CNN)
		return -ENOTTY;

	if (copy_from_user(&job, (void __user *)arg, sizeof(job)))
		return -EFAULT;

	ret = apollo_hexagon_submit_cnn(test->miscdev.parent, test, &job);
	if (ret)
		return ret;

	if (copy_to_user((void __user *)arg, &job, sizeof(job)))
		return -EFAULT;

	return 0;
}

static const struct file_operations apollo_hexagon_fops = {
	.owner = THIS_MODULE,
	.open = apollo_hexagon_open,
	.unlocked_ioctl = apollo_hexagon_ioctl,
	.compat_ioctl = compat_ptr_ioctl,
	.llseek = noop_llseek,
};

static int apollo_hexagon_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct apollo_hexagon_test *test;
	struct iommu_group *group;
	u32 *word;
	int ret;

	test = devm_kzalloc(dev, sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;

	test->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(test->regs))
		return dev_err_probe(dev, PTR_ERR(test->regs),
				     "failed to map control window\n");
	test->tbu_regs = test->regs + APOLLO_HEXAGON_TBU_REG_OFFSET;
	mutex_init(&test->lock);

	ret = of_property_read_u64(dev->of_node, "apollo,dma-iova-base",
				   &test->dma_iova_base);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to read apollo,dma-iova-base\n");
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

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(48));
	if (ret)
		ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return dev_err_probe(dev, ret, "failed to set DMA mask\n");

	group = iommu_group_get(dev);
	if (group) {
		dev_info(dev, "iommu group attached\n");
		iommu_group_put(group);
	} else {
		dev_warn(dev, "iommu group unavailable\n");
	}

	test->size = APOLLO_HEXAGON_TEST_SIZE;
	test->cpu_addr = dmam_alloc_coherent(dev, test->size, &test->dma_addr,
					     GFP_KERNEL);
	if (!test->cpu_addr)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to allocate coherent DMA buffer\n");

	word = test->cpu_addr;
	word[0] = APOLLO_HEXAGON_TEST_PATTERN;
	dma_wmb();
	dma_rmb();
	if (word[0] != APOLLO_HEXAGON_TEST_PATTERN)
		return dev_err_probe(dev, -EIO,
				     "coherent DMA pattern mismatch\n");

	ret = apollo_hexagon_check_firmware_dma(dev);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, test);
	test->miscdev.minor = MISC_DYNAMIC_MINOR;
	test->miscdev.name = "apollo-hexagon";
	test->miscdev.fops = &apollo_hexagon_fops;
	test->miscdev.parent = dev;
	ret = misc_register(&test->miscdev);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register /dev/apollo-hexagon\n");

	dev_info(dev, "dma selftest ok dma=%pad size=%zu\n",
		 &test->dma_addr, test->size);
	dev_info(dev, "userspace submit ABI ready at /dev/%s\n",
		 test->miscdev.name);
	dev_info(dev, "probe ok\n");

	return 0;
}

static void apollo_hexagon_remove(struct platform_device *pdev)
{
	struct apollo_hexagon_test *test = platform_get_drvdata(pdev);

	if (test)
		misc_deregister(&test->miscdev);
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
		.name = "apollo-hexagon-test",
		.of_match_table = apollo_hexagon_of_match,
	},
};
module_platform_driver(apollo_hexagon_driver);

MODULE_DESCRIPTION("Apollo Hexagon IOMMU probe driver");
MODULE_LICENSE("GPL");
