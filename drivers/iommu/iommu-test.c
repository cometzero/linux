// SPDX-License-Identifier: GPL-2.0-only
/*
 * IOMMU runtime test driver.
 *
 * This is a device-driver-level smoke test for the Linux IOMMU and DMA API
 * surface exposed by the QBox Apollo arm-smmu-v3 path. It binds to a synthetic
 * platform device with an "iommus" property, then verifies the attached IOMMU
 * group/domain, DMA API mappings, and the QBox Apollo SMMU TBU data path at
 * runtime.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/ioport.h>
#include <linux/iommu.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_address.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>
#include <linux/slab.h>
#include <linux/string.h>

#define IOMMU_TEST_PATTERN	0xa7101eed
#define IOMMU_TEST_COPY_WORDS	8
#define IOMMU_TEST_COPY_BYTES	\
	(IOMMU_TEST_COPY_WORDS * sizeof(u32))
#define IOMMU_TEST_RESOURCE_OFFSET	0x001f0000
#define IOMMU_TEST_SRC_OFFSET		0x40
#define IOMMU_TEST_DST_OFFSET		0x80
#define IOMMU_TEST_RESULT_LEN		384

#define IOMMU_TEST_DMA_REG_SRC		0x00
#define IOMMU_TEST_DMA_REG_DST		0x04
#define IOMMU_TEST_DMA_REG_LEN		0x08
#define IOMMU_TEST_DMA_REG_CTRL		0x0c
#define IOMMU_TEST_DMA_REG_STATUS	0x10
#define IOMMU_TEST_DMA_REG_RESULT	0x14
#define IOMMU_TEST_DMA_REG_CAPS		0x1c
#define IOMMU_TEST_DMA_REG_PATH		0x20
#define IOMMU_TEST_DMA_REG_STREAM_ID	0x24
#define IOMMU_TEST_DMA_CTRL_START		1
#define IOMMU_TEST_DMA_STATUS_DONE		1
#define IOMMU_TEST_DMA_STATUS_ERROR		2
#define IOMMU_TEST_DMA_RESULT_OK		0x444d414f
#define IOMMU_TEST_DMA_CAP_COPY_ENGINE	BIT(0)
#define IOMMU_TEST_DMA_CAP_SMMU_TRANSLATED	BIT(2)
#define IOMMU_TEST_DMA_PATH_SMMU_TRANSLATED	2
#define IOMMU_TEST_DMA_TIMEOUT_US		2000000
#define IOMMU_TEST_DMA_POLL_US		1000

#define IOMMU_TEST_TBU_REG_OFFSET		0x1000
#define IOMMU_TEST_TBU_REG_MAP_IOVA_LO		0x00
#define IOMMU_TEST_TBU_REG_MAP_IOVA_HI		0x04
#define IOMMU_TEST_TBU_REG_MAP_PA_LO		0x08
#define IOMMU_TEST_TBU_REG_MAP_PA_HI		0x0c
#define IOMMU_TEST_TBU_REG_MAP_SIZE_LO		0x10
#define IOMMU_TEST_TBU_REG_MAP_SIZE_HI		0x14
#define IOMMU_TEST_TBU_REG_MAP_CTRL		0x18
#define IOMMU_TEST_TBU_REG_MAP_STATUS		0x1c
#define IOMMU_TEST_TBU_MAP_CTRL_ADD		1
#define IOMMU_TEST_TBU_MAP_CTRL_CLEAR		3
#define IOMMU_TEST_TBU_MAP_STATUS_OK		1

struct iommu_test_sample {
	dma_addr_t iova;
	phys_addr_t phys;
};

struct iommu_test {
	struct device *dev;
	void __iomem *regs;
	void __iomem *tbu_regs;
	struct mutex lock;
	char result[IOMMU_TEST_RESULT_LEN];
	u32 stream_id;
	unsigned int pass_count;
	unsigned int fail_count;
	int last_status;
};

static void iommu_test_set_result(struct iommu_test *test, int status,
				  const char *detail)
{
	test->last_status = status;
	if (status)
		test->fail_count++;
	else
		test->pass_count++;

	scnprintf(test->result, sizeof(test->result),
		  "status=%d pass=%u fail=%u %s\n", status, test->pass_count,
		  test->fail_count, detail);
}

static int iommu_test_get_dma_domain(struct device *dev,
				     struct iommu_domain **domain_out)
{
	struct iommu_domain *domain;
	struct iommu_group *group;

	if (!device_iommu_mapped(dev))
		return dev_err_probe(dev, -ENODEV,
				     "IOMMU group is not attached\n");

	group = iommu_group_get(dev);
	if (!group)
		return dev_err_probe(dev, -ENODEV, "IOMMU group missing\n");

	domain = iommu_get_domain_for_dev(dev);
	iommu_group_put(group);
	if (!domain)
		return dev_err_probe(dev, -ENODEV, "IOMMU domain missing\n");

	if (!iommu_is_dma_domain(domain))
		return dev_err_probe(dev, -EINVAL,
				     "IOMMU domain is not DMA type=0x%x\n",
				     domain->type);

	*domain_out = domain;
	return 0;
}

static int iommu_test_coherent(struct device *dev,
			       struct iommu_domain *domain,
			       struct iommu_test_sample *sample)
{
	dma_addr_t iova;
	phys_addr_t phys;
	u32 *cpu;
	int ret = 0;

	cpu = dma_alloc_coherent(dev, PAGE_SIZE, &iova, GFP_KERNEL);
	if (!cpu)
		return dev_err_probe(dev, -ENOMEM,
				     "coherent DMA allocation failed\n");

	cpu[0] = IOMMU_TEST_PATTERN;
	dma_wmb();
	dma_rmb();
	if (cpu[0] != IOMMU_TEST_PATTERN) {
		ret = dev_err_probe(dev, -EIO,
				    "coherent DMA pattern mismatch\n");
		goto out_free;
	}

	phys = iommu_iova_to_phys(domain, iova);
	if (!phys) {
		ret = dev_err_probe(dev, -EIO,
				    "coherent IOVA translation missing\n");
		goto out_free;
	}

	sample->iova = iova;
	sample->phys = phys;

out_free:
	dma_free_coherent(dev, PAGE_SIZE, cpu, iova);
	return ret;
}

static int iommu_test_streaming(struct device *dev,
				struct iommu_domain *domain,
				struct iommu_test_sample *sample)
{
	struct page *page;
	dma_addr_t iova;
	phys_addr_t expected;
	phys_addr_t phys;
	u32 *cpu;
	int ret = 0;

	page = alloc_page(GFP_KERNEL | __GFP_ZERO);
	if (!page)
		return dev_err_probe(dev, -ENOMEM,
				     "streaming DMA page allocation failed\n");

	cpu = page_address(page);
	cpu[0] = IOMMU_TEST_PATTERN;
	expected = page_to_phys(page);

	iova = dma_map_page(dev, page, 0, PAGE_SIZE, DMA_BIDIRECTIONAL);
	if (dma_mapping_error(dev, iova)) {
		ret = dev_err_probe(dev, -EIO,
				    "streaming DMA map failed\n");
		goto out_page;
	}

	phys = iommu_iova_to_phys(domain, iova);
	if (phys != expected) {
		ret = dev_err_probe(dev, -EIO,
				    "streaming translation mismatch\n");
		goto out_unmap;
	}

	sample->iova = iova;
	sample->phys = phys;

out_unmap:
	dma_unmap_page(dev, iova, PAGE_SIZE, DMA_BIDIRECTIONAL);
out_page:
	__free_page(page);
	return ret;
}

static void iommu_test_write64(void __iomem *base, u32 lo, u32 hi, u64 value)
{
	writel(lower_32_bits(value), base + lo);
	writel(upper_32_bits(value), base + hi);
}

static int iommu_test_tbu_ctrl(struct device *dev,
			       struct iommu_test *test, u32 ctrl)
{
	u32 status;

	writel(ctrl, test->tbu_regs + IOMMU_TEST_TBU_REG_MAP_CTRL);
	status = readl(test->tbu_regs + IOMMU_TEST_TBU_REG_MAP_STATUS);
	if (status != IOMMU_TEST_TBU_MAP_STATUS_OK)
		return dev_err_probe(dev, -EIO,
				     "QBox SMMU TBU ctrl=%u failed status=0x%x\n",
				     ctrl, status);

	return 0;
}

static int iommu_test_tbu_map(struct device *dev,
			      struct iommu_test *test, u64 iova,
			      phys_addr_t phys, u64 size)
{
	iommu_test_write64(test->tbu_regs, IOMMU_TEST_TBU_REG_MAP_IOVA_LO,
			   IOMMU_TEST_TBU_REG_MAP_IOVA_HI, iova);
	iommu_test_write64(test->tbu_regs, IOMMU_TEST_TBU_REG_MAP_PA_LO,
			   IOMMU_TEST_TBU_REG_MAP_PA_HI, phys);
	iommu_test_write64(test->tbu_regs, IOMMU_TEST_TBU_REG_MAP_SIZE_LO,
			   IOMMU_TEST_TBU_REG_MAP_SIZE_HI, size);

	return iommu_test_tbu_ctrl(dev, test, IOMMU_TEST_TBU_MAP_CTRL_ADD);
}

static int iommu_test_check_dataplane_abi(struct device *dev,
					  struct iommu_test *test)
{
	u32 expected_stream_id;
	u32 stream_id;
	u32 caps;
	u32 path;
	int ret;

	ret = of_property_read_u32(dev->of_node, "apollo,smmu-stream-id",
				   &expected_stream_id);
	if (ret)
		return dev_err_probe(dev, ret,
				     "apollo,smmu-stream-id is required\n");

	caps = readl(test->regs + IOMMU_TEST_DMA_REG_CAPS);
	path = readl(test->regs + IOMMU_TEST_DMA_REG_PATH);
	stream_id = readl(test->regs + IOMMU_TEST_DMA_REG_STREAM_ID);
	if (stream_id != expected_stream_id)
		return dev_err_probe(dev, -EINVAL,
				     "QBox data-plane stream-id mismatch hw=0x%x dt=0x%x\n",
				     stream_id, expected_stream_id);
	if (path != IOMMU_TEST_DMA_PATH_SMMU_TRANSLATED ||
	    !(caps & IOMMU_TEST_DMA_CAP_COPY_ENGINE) ||
	    !(caps & IOMMU_TEST_DMA_CAP_SMMU_TRANSLATED))
		return dev_err_probe(dev, -ENODEV,
				     "QBox SMMU data-plane path unavailable path=0x%x caps=0x%x\n",
				     path, caps);

	test->stream_id = stream_id;
	return 0;
}

static int iommu_test_dataplane(struct iommu_test *test,
				struct iommu_domain *domain,
				struct iommu_test_sample *sample)
{
	static const u32 payload[IOMMU_TEST_COPY_WORDS] = {
		IOMMU_TEST_PATTERN, 0x11223344, 0x55667788, 0x99aabbcc,
		0x0badc0de, 0x13579bdf, 0x2468ace0, 0xfeedbeef,
	};
	struct device *dev = test->dev;
	struct device_node *mem_np;
	struct resource res;
	phys_addr_t data_phys;
	dma_addr_t iova;
	void __iomem *cpu;
	phys_addr_t translated;
	u32 status;
	u32 result;
	int ret;
	int i;

	mem_np = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!mem_np)
		return dev_err_probe(dev, -ENOENT,
				     "memory-region is required\n");

	ret = of_address_to_resource(mem_np, 0, &res);
	of_node_put(mem_np);
	if (ret)
		return dev_err_probe(dev, ret, "memory-region parse failed\n");

	if (resource_size(&res) <
	    IOMMU_TEST_RESOURCE_OFFSET + PAGE_SIZE)
		return dev_err_probe(dev, -EINVAL,
				     "memory-region is too small for data-plane page\n");

	ret = iommu_test_check_dataplane_abi(dev, test);
	if (ret)
		return ret;

	data_phys = res.start + IOMMU_TEST_RESOURCE_OFFSET;
	cpu = ioremap(data_phys, PAGE_SIZE);
	if (!cpu)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to map data-plane SRAM\n");

	iova = dma_map_resource(dev, data_phys, PAGE_SIZE, DMA_BIDIRECTIONAL,
				DMA_ATTR_SKIP_CPU_SYNC);
	if (dma_mapping_error(dev, iova)) {
		ret = dev_err_probe(dev, -EIO, "resource DMA map failed\n");
		goto out_iounmap;
	}

	translated = iommu_iova_to_phys(domain, iova);
	if (translated != data_phys) {
		ret = dev_err_probe(dev, -EIO,
				    "resource translation mismatch\n");
		goto out_unmap;
	}
	if ((u64)iova + IOMMU_TEST_DST_OFFSET +
	    IOMMU_TEST_COPY_BYTES - 1 > U32_MAX) {
		ret = dev_err_probe(dev, -EOVERFLOW,
				    "QBox DMA data-plane IOVA exceeds 32-bit register ABI iova=%pad\n",
				    &iova);
		goto out_unmap;
	}

	ret = iommu_test_tbu_ctrl(dev, test, IOMMU_TEST_TBU_MAP_CTRL_CLEAR);
	if (ret)
		goto out_unmap;
	ret = iommu_test_tbu_map(dev, test, iova, data_phys, PAGE_SIZE);
	if (ret)
		goto out_unmap;

	for (i = 0; i < ARRAY_SIZE(payload); i++) {
		writel(payload[i], cpu + IOMMU_TEST_SRC_OFFSET +
		       i * sizeof(payload[i]));
		writel(0, cpu + IOMMU_TEST_DST_OFFSET +
		       i * sizeof(payload[i]));
	}
	dma_wmb();

	writel(lower_32_bits(iova + IOMMU_TEST_SRC_OFFSET),
	       test->regs + IOMMU_TEST_DMA_REG_SRC);
	writel(lower_32_bits(iova + IOMMU_TEST_DST_OFFSET),
	       test->regs + IOMMU_TEST_DMA_REG_DST);
	writel(IOMMU_TEST_COPY_BYTES,
	       test->regs + IOMMU_TEST_DMA_REG_LEN);
	writel(IOMMU_TEST_DMA_CTRL_START,
	       test->regs + IOMMU_TEST_DMA_REG_CTRL);

	ret = readl_poll_timeout(test->regs + IOMMU_TEST_DMA_REG_STATUS,
				 status,
				 status == IOMMU_TEST_DMA_STATUS_DONE ||
				 status == IOMMU_TEST_DMA_STATUS_ERROR,
				 IOMMU_TEST_DMA_POLL_US,
				 IOMMU_TEST_DMA_TIMEOUT_US);
	result = readl(test->regs + IOMMU_TEST_DMA_REG_RESULT);
	if (ret || status != IOMMU_TEST_DMA_STATUS_DONE ||
	    result != IOMMU_TEST_DMA_RESULT_OK) {
		ret = dev_err_probe(dev, ret ? ret : -EIO,
				    "QBox SMMU data-plane DMA failed status=0x%x result=0x%x\n",
				    status, result);
		goto out_clear;
	}

	dma_rmb();
	for (i = 0; i < ARRAY_SIZE(payload); i++) {
		u32 got = readl(cpu + IOMMU_TEST_DST_OFFSET +
				i * sizeof(payload[i]));

		if (got != payload[i]) {
			ret = dev_err_probe(dev, -EIO,
					    "QBox SMMU data-plane payload mismatch word=%d got=0x%x expected=0x%x\n",
					    i, got, payload[i]);
			goto out_clear;
		}
	}

	sample->iova = iova;
	sample->phys = data_phys;
	ret = 0;

out_clear:
	if (iommu_test_tbu_ctrl(dev, test, IOMMU_TEST_TBU_MAP_CTRL_CLEAR) &&
	    !ret)
		ret = -EIO;
out_unmap:
	dma_unmap_resource(dev, iova, PAGE_SIZE, DMA_BIDIRECTIONAL,
			   DMA_ATTR_SKIP_CPU_SYNC);
out_iounmap:
	iounmap(cpu);
	return ret;
}

static int iommu_test_run_locked(struct iommu_test *test)
{
	struct iommu_test_sample coherent = {};
	struct iommu_test_sample dataplane = {};
	struct iommu_test_sample streaming = {};
	struct iommu_domain *domain;
	struct device *dev = test->dev;
	char detail[IOMMU_TEST_RESULT_LEN];
	int ret;

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret) {
		ret = dev_err_probe(dev, ret, "failed to set DMA mask\n");
		goto out_result;
	}

	ret = iommu_test_get_dma_domain(dev, &domain);
	if (ret)
		goto out_result;

	ret = iommu_test_coherent(dev, domain, &coherent);
	if (ret)
		goto out_result;

	ret = iommu_test_streaming(dev, domain, &streaming);
	if (ret)
		goto out_result;

	ret = iommu_test_dataplane(test, domain, &dataplane);
	if (ret)
		goto out_result;

	scnprintf(detail, sizeof(detail),
		  "domain-type=0x%x coherent=%pad->%pa streaming=%pad->%pa dataplane=stream-id=0x%x %pad->%pa",
		  domain->type, &coherent.iova, &coherent.phys,
		  &streaming.iova, &streaming.phys, test->stream_id,
		  &dataplane.iova, &dataplane.phys);
	dev_info(dev, "IOMMU runtime selftest ok %s\n", detail);

out_result:
	if (ret)
		scnprintf(detail, sizeof(detail), "error=%d", ret);
	iommu_test_set_result(test, ret, detail);
	return ret;
}

static ssize_t result_show(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct iommu_test *test = dev_get_drvdata(dev);
	ssize_t ret;

	mutex_lock(&test->lock);
	ret = sysfs_emit(buf, "%s", test->result);
	mutex_unlock(&test->lock);

	return ret;
}
static DEVICE_ATTR_RO(result);

static ssize_t run_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct iommu_test *test = dev_get_drvdata(dev);
	int ret;

	if (!sysfs_streq(buf, "1"))
		return -EINVAL;

	mutex_lock(&test->lock);
	ret = iommu_test_run_locked(test);
	mutex_unlock(&test->lock);

	return ret ? ret : count;
}
static DEVICE_ATTR_WO(run);

static struct attribute *iommu_test_attrs[] = {
	&dev_attr_result.attr,
	&dev_attr_run.attr,
	NULL,
};

static const struct attribute_group iommu_test_attr_group = {
	.attrs = iommu_test_attrs,
};

static int iommu_test_probe(struct platform_device *pdev)
{
	struct iommu_test *test;
	struct device *dev = &pdev->dev;
	int ret;

	test = devm_kzalloc(dev, sizeof(*test), GFP_KERNEL);
	if (!test)
		return -ENOMEM;

	test->dev = dev;
	test->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(test->regs))
		return dev_err_probe(dev, PTR_ERR(test->regs),
				     "failed to map QBox data-plane regs\n");
	test->tbu_regs = (u8 __iomem *)test->regs +
			 IOMMU_TEST_TBU_REG_OFFSET;
	mutex_init(&test->lock);
	platform_set_drvdata(pdev, test);
	scnprintf(test->result, sizeof(test->result), "status=%d not-run\n",
		  -EAGAIN);

	mutex_lock(&test->lock);
	ret = iommu_test_run_locked(test);
	mutex_unlock(&test->lock);
	if (ret)
		return ret;

	ret = devm_device_add_group(dev, &iommu_test_attr_group);
	if (ret)
		return dev_err_probe(dev, ret, "failed to add sysfs group\n");

	dev_info(dev, "runtime IOMMU sysfs ready: result, run\n");
	return 0;
}

static const struct of_device_id iommu_test_of_match[] = {
	{ .compatible = "apollo,iommu-runtime-test" },
	{ }
};
MODULE_DEVICE_TABLE(of, iommu_test_of_match);

static struct platform_driver iommu_test_driver = {
	.probe = iommu_test_probe,
	.driver = {
		.name = "iommu-test",
		.of_match_table = iommu_test_of_match,
	},
};
module_platform_driver(iommu_test_driver);

MODULE_DESCRIPTION("IOMMU runtime test driver");
MODULE_LICENSE("GPL");
