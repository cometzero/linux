// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apollo Hexagon IOMMU probe driver.
 */

#include <linux/bits.h>
#include <linux/device.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>

#define APOLLO_HEXAGON_TEST_SIZE	SZ_4K
#define APOLLO_HEXAGON_TEST_PATTERN	0xa510beef
#define APOLLO_HEXAGON_DMA_TIMEOUT_MS	2000
#define APOLLO_HEXAGON_DMA_POLL_MS	10
#define APOLLO_HEXAGON_REG_CAPS		0x1c
#define APOLLO_HEXAGON_REG_PATH		0x20
#define APOLLO_HEXAGON_REG_STREAM_ID	0x24
#define APOLLO_HEXAGON_CAP_COPY_ENGINE	BIT(0)
#define APOLLO_HEXAGON_CAP_DIRECT_TLM	BIT(1)
#define APOLLO_HEXAGON_CAP_SMMU_TRANSLATED	BIT(2)
#define APOLLO_HEXAGON_PATH_DIRECT_TLM		1
#define APOLLO_HEXAGON_PATH_SMMU_TRANSLATED	2

static const u32 apollo_hexagon_dma_expected[] = {
	0x48455831, 0x444d4131, 0x11223344, 0x55667788,
	0xa5a55a5a, 0x5a5aa5a5, 0xfeedc0de, 0x600dbeef,
};

struct apollo_hexagon_test {
	void __iomem *regs;
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
