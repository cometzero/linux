// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apollo Hexagon IOMMU probe driver.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/slab.h>

#define APOLLO_HEXAGON_TEST_SIZE	SZ_4K
#define APOLLO_HEXAGON_TEST_PATTERN	0xa510beef

struct apollo_hexagon_test {
	void __iomem *regs;
	void *cpu_addr;
	dma_addr_t dma_addr;
	size_t size;
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

	platform_set_drvdata(pdev, test);
	dev_info(dev, "dma selftest ok dma=%pad size=%zu\n",
		 &test->dma_addr, test->size);
	dev_info(dev, "probe ok\n");

	return 0;
}

static const struct of_device_id apollo_hexagon_of_match[] = {
	{ .compatible = "apollo,hexagon-ip" },
	{ }
};
MODULE_DEVICE_TABLE(of, apollo_hexagon_of_match);

static struct platform_driver apollo_hexagon_driver = {
	.probe = apollo_hexagon_probe,
	.driver = {
		.name = "apollo-hexagon-test",
		.of_match_table = apollo_hexagon_of_match,
	},
};
module_platform_driver(apollo_hexagon_driver);

MODULE_DESCRIPTION("Apollo Hexagon IOMMU probe driver");
MODULE_LICENSE("GPL");
