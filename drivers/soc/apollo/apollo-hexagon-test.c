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
#define APOLLO_HEXAGON_REG_JOB_QUEUE	0x5c
#define APOLLO_HEXAGON_REG_JOB_FENCE	0x60
#define APOLLO_HEXAGON_REG_IRQ_STATUS	0x64
#define APOLLO_HEXAGON_REG_IRQ_ACK	0x68
#define APOLLO_HEXAGON_REG_QUEUE_CAPS	0x6c
#define APOLLO_HEXAGON_CAP_COPY_ENGINE	BIT(0)
#define APOLLO_HEXAGON_CAP_DIRECT_TLM	BIT(1)
#define APOLLO_HEXAGON_CAP_SMMU_TRANSLATED	BIT(2)
#define APOLLO_HEXAGON_CAP_LARGE_TENSOR	BIT(3)
#define APOLLO_HEXAGON_CAP_MULTI_QUEUE	BIT(4)
#define APOLLO_HEXAGON_CAP_ASYNC_FENCE	BIT(5)
#define APOLLO_HEXAGON_PATH_DIRECT_TLM		1
#define APOLLO_HEXAGON_PATH_SMMU_TRANSLATED	2
#define APOLLO_HEXAGON_QUEUE_DMA	0
#define APOLLO_HEXAGON_QUEUE_CNN	1
#define APOLLO_HEXAGON_JOB_CTRL_START	1
#define APOLLO_HEXAGON_JOB_STATUS_DONE	1
#define APOLLO_HEXAGON_JOB_STATUS_ERROR	2
#define APOLLO_HEXAGON_JOB_RESULT_OK	0x434e4e4f
#define APOLLO_HEXAGON_JOB_RESULT_SG_OK	0x53474f4b
#define APOLLO_HEXAGON_INPUT_OFFSET	0x10000
#define APOLLO_HEXAGON_OUTPUT_OFFSET	0x11000
#define APOLLO_HEXAGON_STRESS_INPUT_BASE	0x20000
#define APOLLO_HEXAGON_STRESS_OUTPUT_BASE	0x80000
#define APOLLO_HEXAGON_STRESS_SEGMENT_STRIDE	0x8000
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
#define APOLLO_TBU_REG_FEATURES		0x24
#define APOLLO_TBU_REG_ATS_STATUS	0x28
#define APOLLO_TBU_REG_PRI_STATUS	0x2c
#define APOLLO_TBU_REG_FAULT_STATUS	0x30
#define APOLLO_TBU_REG_FAULT_IOVA_LO	0x34
#define APOLLO_TBU_REG_FAULT_IOVA_HI	0x38
#define APOLLO_TBU_REG_FAULT_CTRL	0x3c
#define APOLLO_TBU_REG_ARCH_TTBR_LO	0x40
#define APOLLO_TBU_REG_ARCH_TTBR_HI	0x44
#define APOLLO_TBU_REG_ARCH_IOVA_LO	0x48
#define APOLLO_TBU_REG_ARCH_IOVA_HI	0x4c
#define APOLLO_TBU_REG_ARCH_CTRL	0x50
#define APOLLO_TBU_REG_ARCH_STATUS	0x54
#define APOLLO_TBU_REG_ARCH_DESC_LO	0x58
#define APOLLO_TBU_REG_ARCH_DESC_HI	0x5c
#define APOLLO_TBU_REG_ARCH_PA_LO	0x60
#define APOLLO_TBU_REG_ARCH_PA_HI	0x64
#define APOLLO_TBU_REG_ARCH_LEVELS	0x68
#define APOLLO_TBU_REG_ARCH_STE_BASE_LO	0x6c
#define APOLLO_TBU_REG_ARCH_STE_BASE_HI	0x70
#define APOLLO_TBU_REG_ARCH_STE_LO	0x74
#define APOLLO_TBU_REG_ARCH_STE_HI	0x78
#define APOLLO_TBU_REG_ARCH_CD_LO	0x7c
#define APOLLO_TBU_REG_ARCH_CD_HI	0x80
#define APOLLO_TBU_REG_ARCH_FAULT_REASON	0x84
#define APOLLO_TBU_REG_ARCH_FAULT_REPLAY	0x88
#define APOLLO_TBU_REG_ARCH_PROTOCOL_STATUS	0x8c
#define APOLLO_TBU_MAP_CTRL_ADD		1
#define APOLLO_TBU_MAP_CTRL_REMOVE	2
#define APOLLO_TBU_MAP_CTRL_CLEAR	3
#define APOLLO_TBU_MAP_STATUS_OK	1
#define APOLLO_TBU_ARCH_CTRL_PROBE	1
#define APOLLO_TBU_ARCH_CTRL_NEGATIVE_REPLAY	2
#define APOLLO_TBU_ARCH_STATUS_OK	1
#define APOLLO_TBU_ARCH_STATUS_ERROR	2
#define APOLLO_TBU_FEATURE_PAGE_TABLE_WALKER	BIT(0)
#define APOLLO_TBU_FEATURE_ATS_CACHE	BIT(1)
#define APOLLO_TBU_FEATURE_PRI_QUEUE	BIT(2)
#define APOLLO_TBU_FEATURE_FAULT_QUEUE	BIT(3)
#define APOLLO_TBU_FEATURE_ARCH_DESCRIPTOR_WALK	BIT(4)
#define APOLLO_TBU_FEATURE_ARCH_4_LEVEL_WALK	BIT(5)
#define APOLLO_TBU_FEATURE_STREAM_TABLE_WALK	BIT(6)
#define APOLLO_TBU_FEATURE_CONTEXT_DESCRIPTOR_WALK	BIT(7)
#define APOLLO_TBU_FEATURE_ARCH_FAULT_REPLAY	BIT(8)
#define APOLLO_TBU_FEATURE_ARCH_ATS_PRI_PROTOCOL	BIT(9)
#define APOLLO_TBU_FAULT_CTRL_CLEAR	1
#define APOLLO_TBU_FAULT_CTRL_INJECT	2
#define APOLLO_TBU_ARCH_INDEX_MASK	0x1ffULL
#define APOLLO_TBU_ARCH_LEVELS		4
#define APOLLO_TBU_ARCH_L0_SHIFT	39
#define APOLLO_TBU_ARCH_LEVEL_BITS	9
#define APOLLO_TBU_ARCH_DESC_TABLE	0x3ULL
#define APOLLO_TBU_ARCH_DESC_PAGE	0x3ULL
#define APOLLO_TBU_ARCH_DESC_AF		BIT_ULL(10)
#define APOLLO_TBU_ARCH_DESC_SH_INNER	(3ULL << 8)
#define APOLLO_TBU_ARCH_DESC_OUTPUT_MASK	GENMASK_ULL(47, 12)
#define APOLLO_TBU_ARCH_STE_SIZE	64
#define APOLLO_TBU_ARCH_CD_SIZE		64
#define APOLLO_TBU_ARCH_STE_VALID	BIT_ULL(0)
#define APOLLO_TBU_ARCH_STE_S1_ENABLED	BIT_ULL(1)
#define APOLLO_TBU_ARCH_CD_VALID	BIT_ULL(0)
#define APOLLO_TBU_ARCH_FAULT_STE_INVALID	2
#define APOLLO_HEXAGON_STRESS_SEED	0x13579bdf
#define APOLLO_HEXAGON_PTW_OFFSET	0x18000
#define APOLLO_HEXAGON_PTW_PAGES	APOLLO_TBU_ARCH_LEVELS
#define APOLLO_HEXAGON_STE_OFFSET	\
	(APOLLO_HEXAGON_PTW_OFFSET + SZ_4K * APOLLO_HEXAGON_PTW_PAGES)
#define APOLLO_HEXAGON_CD_OFFSET	(APOLLO_HEXAGON_STE_OFFSET + SZ_4K)
#define APOLLO_HEXAGON_ARCH_BYTES	(APOLLO_HEXAGON_CD_OFFSET + SZ_4K)

static const u32 apollo_hexagon_dma_expected[] = {
	0x48455831, 0x444d4131, 0x11223344, 0x55667788,
	0xa5a55a5a, 0x5a5aa5a5, 0xfeedc0de, 0x600dbeef,
};

struct apollo_hexagon_test {
	void __iomem *regs;
	void __iomem *tbu_regs;
	void __iomem *shared_base;
	phys_addr_t shared_phys;
	resource_size_t shared_size;
	u64 dma_iova_base;
	u64 dma_window_size;
	struct miscdevice miscdev;
	struct mutex lock;
	void *cpu_addr;
	dma_addr_t dma_addr;
	size_t size;
};

static int apollo_hexagon_wait_job(struct device *dev,
				   struct apollo_hexagon_test *test,
				   u32 queue_id, u32 expected_result,
				   u32 *fence_seq);

static int apollo_hexagon_check_dma_abi(struct device *dev,
					struct apollo_hexagon_test *test)
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

static int apollo_hexagon_tbu_map(struct device *dev,
				  struct apollo_hexagon_test *test,
				  u64 iova, phys_addr_t pa, u64 size)
{
	apollo_tbu_write64(test->tbu_regs, APOLLO_TBU_REG_MAP_IOVA_LO,
			   APOLLO_TBU_REG_MAP_IOVA_HI, iova);
	apollo_tbu_write64(test->tbu_regs, APOLLO_TBU_REG_MAP_PA_LO,
			   APOLLO_TBU_REG_MAP_PA_HI, pa);
	apollo_tbu_write64(test->tbu_regs, APOLLO_TBU_REG_MAP_SIZE_LO,
			   APOLLO_TBU_REG_MAP_SIZE_HI, size);

	return apollo_hexagon_tbu_ctrl(dev, test, APOLLO_TBU_MAP_CTRL_ADD);
}

static u64 apollo_tbu_read64(void __iomem *base, u32 lo, u32 hi)
{
	return readl(base + lo) | ((u64)readl(base + hi) << 32);
}

static u64 apollo_tbu_arch_index(u64 iova, u32 level)
{
	return (iova >> (APOLLO_TBU_ARCH_L0_SHIFT -
			 level * APOLLO_TBU_ARCH_LEVEL_BITS)) &
	       APOLLO_TBU_ARCH_INDEX_MASK;
}

static void apollo_hexagon_ptw_write_desc(struct apollo_hexagon_test *test,
					  u64 offset, u64 desc)
{
	writel(lower_32_bits(desc), test->shared_base + offset);
	writel(upper_32_bits(desc), test->shared_base + offset + sizeof(u32));
}

static int apollo_hexagon_check_arch_ptw(struct device *dev,
					 struct apollo_hexagon_test *test)
{
	const u64 iova = test->dma_iova_base;
	const u64 ttbr = test->shared_phys + APOLLO_HEXAGON_PTW_OFFSET;
	const u64 ste_base = test->shared_phys + APOLLO_HEXAGON_STE_OFFSET;
	const u64 cd_base = test->shared_phys + APOLLO_HEXAGON_CD_OFFSET;
	const u64 arch_bytes = APOLLO_HEXAGON_ARCH_BYTES -
			       APOLLO_HEXAGON_PTW_OFFSET;
	const u32 stream_id = readl(test->regs + APOLLO_HEXAGON_REG_STREAM_ID);
	const u64 l0_index = apollo_tbu_arch_index(iova, 0);
	const u64 l1_index = apollo_tbu_arch_index(iova, 1);
	const u64 l2_index = apollo_tbu_arch_index(iova, 2);
	const u64 l3_index = apollo_tbu_arch_index(iova, 3);
	const u64 l1_table = ttbr + SZ_4K;
	const u64 l2_table = ttbr + 2 * SZ_4K;
	const u64 l3_table = ttbr + 3 * SZ_4K;
	const u64 l0_desc = (l1_table & APOLLO_TBU_ARCH_DESC_OUTPUT_MASK) |
			    APOLLO_TBU_ARCH_DESC_TABLE;
	const u64 l1_desc = (l2_table & APOLLO_TBU_ARCH_DESC_OUTPUT_MASK) |
			    APOLLO_TBU_ARCH_DESC_TABLE;
	const u64 l2_desc = (l3_table & APOLLO_TBU_ARCH_DESC_OUTPUT_MASK) |
			    APOLLO_TBU_ARCH_DESC_TABLE;
	const u64 l3_desc = (test->shared_phys &
			     APOLLO_TBU_ARCH_DESC_OUTPUT_MASK) |
			    APOLLO_TBU_ARCH_DESC_AF |
			    APOLLO_TBU_ARCH_DESC_SH_INNER |
			    APOLLO_TBU_ARCH_DESC_PAGE;
	const u64 l0_offset = APOLLO_HEXAGON_PTW_OFFSET +
			      l0_index * sizeof(l0_desc);
	const u64 l1_offset = APOLLO_HEXAGON_PTW_OFFSET + SZ_4K +
			      l1_index * sizeof(l1_desc);
	const u64 l2_offset = APOLLO_HEXAGON_PTW_OFFSET + 2 * SZ_4K +
			      l2_index * sizeof(l2_desc);
	const u64 l3_offset = APOLLO_HEXAGON_PTW_OFFSET + 3 * SZ_4K +
			      l3_index * sizeof(l3_desc);
	const u64 ste0 = APOLLO_TBU_ARCH_STE_VALID |
			 APOLLO_TBU_ARCH_STE_S1_ENABLED;
	const u64 ste1 = cd_base & APOLLO_TBU_ARCH_DESC_OUTPUT_MASK;
	const u64 cd0 = APOLLO_TBU_ARCH_CD_VALID;
	const u64 cd1 = ttbr & APOLLO_TBU_ARCH_DESC_OUTPUT_MASK;
	const u64 ste_offset = APOLLO_HEXAGON_STE_OFFSET +
			       stream_id * APOLLO_TBU_ARCH_STE_SIZE;
	const u64 cd_offset = APOLLO_HEXAGON_CD_OFFSET;
	u32 status;
	u32 levels;
	u32 fault_count;
	u32 protocol;
	u32 reason;
	u32 replay;
	u64 got_desc;
	u64 got_pa;
	u64 got_ste;
	u64 got_cd;

	if (APOLLO_HEXAGON_ARCH_BYTES > test->shared_size)
		return dev_err_probe(dev, -EINVAL,
				     "shared SRAM too small for SMMUv3 stream/context descriptor probe\n");
	if (ste_offset + APOLLO_TBU_ARCH_STE_SIZE > APOLLO_HEXAGON_CD_OFFSET)
		return dev_err_probe(dev, -EINVAL,
				     "SMMUv3 StreamID 0x%x exceeds staged stream table\n",
				     stream_id);

	memset_io(test->shared_base + APOLLO_HEXAGON_PTW_OFFSET, 0,
		  arch_bytes);
	apollo_hexagon_ptw_write_desc(test, l0_offset, l0_desc);
	apollo_hexagon_ptw_write_desc(test, l1_offset, l1_desc);
	apollo_hexagon_ptw_write_desc(test, l2_offset, l2_desc);
	apollo_hexagon_ptw_write_desc(test, l3_offset, l3_desc);
	apollo_hexagon_ptw_write_desc(test, ste_offset, ste0);
	apollo_hexagon_ptw_write_desc(test, ste_offset + sizeof(ste0), ste1);
	apollo_hexagon_ptw_write_desc(test, cd_offset, cd0);
	apollo_hexagon_ptw_write_desc(test, cd_offset + sizeof(cd0), cd1);

	apollo_tbu_write64(test->tbu_regs, APOLLO_TBU_REG_ARCH_TTBR_LO,
			   APOLLO_TBU_REG_ARCH_TTBR_HI, ttbr);
	apollo_tbu_write64(test->tbu_regs, APOLLO_TBU_REG_ARCH_STE_BASE_LO,
			   APOLLO_TBU_REG_ARCH_STE_BASE_HI, ste_base);
	apollo_tbu_write64(test->tbu_regs, APOLLO_TBU_REG_ARCH_IOVA_LO,
			   APOLLO_TBU_REG_ARCH_IOVA_HI, iova);
	writel(APOLLO_TBU_ARCH_CTRL_PROBE,
	       test->tbu_regs + APOLLO_TBU_REG_ARCH_CTRL);

	status = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_STATUS);
	if (status != APOLLO_TBU_ARCH_STATUS_OK)
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 architectural descriptor probe failed status=0x%x faultq=%u\n",
				     status,
				     readl(test->tbu_regs +
					   APOLLO_TBU_REG_FAULT_STATUS));

	got_desc = apollo_tbu_read64(test->tbu_regs, APOLLO_TBU_REG_ARCH_DESC_LO,
				     APOLLO_TBU_REG_ARCH_DESC_HI);
	got_pa = apollo_tbu_read64(test->tbu_regs, APOLLO_TBU_REG_ARCH_PA_LO,
				   APOLLO_TBU_REG_ARCH_PA_HI);
	levels = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_LEVELS);
	got_ste = apollo_tbu_read64(test->tbu_regs, APOLLO_TBU_REG_ARCH_STE_LO,
				    APOLLO_TBU_REG_ARCH_STE_HI);
	got_cd = apollo_tbu_read64(test->tbu_regs, APOLLO_TBU_REG_ARCH_CD_LO,
				   APOLLO_TBU_REG_ARCH_CD_HI);
	protocol = readl(test->tbu_regs +
			 APOLLO_TBU_REG_ARCH_PROTOCOL_STATUS);
	if (levels != APOLLO_TBU_ARCH_LEVELS ||
	    got_desc != l3_desc ||
	    got_ste != ste0 ||
	    got_cd != cd0 ||
	    !protocol ||
	    (got_pa & APOLLO_TBU_ARCH_DESC_OUTPUT_MASK) !=
	    (test->shared_phys & APOLLO_TBU_ARCH_DESC_OUTPUT_MASK))
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 stream/context descriptor mismatch levels=%u ste=0x%llx/0x%llx cd=0x%llx/0x%llx desc=0x%llx/0x%llx pa=0x%llx expected=%pa protocol=0x%x\n",
				     levels, got_ste, ste0, got_cd, cd0,
				     got_desc, l3_desc, got_pa,
				     &test->shared_phys, protocol);

	dev_info(dev,
		 "SMMUv3 architectural descriptor probe ok 4-level ttbr=0x%llx iova=0x%llx l0=%llu l1=%llu l2=%llu l3=%llu desc=0x%llx pa=0x%llx levels=%u\n",
		 ttbr, iova, l0_index, l1_index, l2_index, l3_index,
		 got_desc, got_pa, levels);
	dev_info(dev,
		 "SMMUv3 stream/context descriptor probe ok stream-id=0x%x ste-base=0x%llx ste=0x%llx cd-base=0x%llx cd=0x%llx protocol=0x%x\n",
		 stream_id, ste_base, got_ste, cd_base, got_cd, protocol);

	writel(APOLLO_TBU_FAULT_CTRL_CLEAR,
	       test->tbu_regs + APOLLO_TBU_REG_FAULT_CTRL);
	apollo_hexagon_ptw_write_desc(test, ste_offset, 0);
	writel(APOLLO_TBU_ARCH_CTRL_NEGATIVE_REPLAY,
	       test->tbu_regs + APOLLO_TBU_REG_ARCH_CTRL);

	status = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_STATUS);
	fault_count = readl(test->tbu_regs + APOLLO_TBU_REG_FAULT_STATUS);
	reason = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_FAULT_REASON);
	replay = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_FAULT_REPLAY);
	if (status != APOLLO_TBU_ARCH_STATUS_ERROR ||
	    reason != APOLLO_TBU_ARCH_FAULT_STE_INVALID ||
	    !fault_count || !replay)
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 negative fault replay mismatch status=0x%x reason=%u faultq=%u replay=%u\n",
				     status, reason, fault_count, replay);

	dev_info(dev,
		 "SMMUv3 negative fault replay ok reason=stream-descriptor-invalid faultq=%u replay=%u\n",
		 fault_count, replay);

	apollo_hexagon_ptw_write_desc(test, ste_offset, ste0);
	writel(APOLLO_TBU_FAULT_CTRL_CLEAR,
	       test->tbu_regs + APOLLO_TBU_REG_FAULT_CTRL);

	return 0;
}

static int apollo_hexagon_check_tbu_features(struct device *dev,
					     struct apollo_hexagon_test *test)
{
	const u32 required = APOLLO_TBU_FEATURE_PAGE_TABLE_WALKER |
			    APOLLO_TBU_FEATURE_ATS_CACHE |
			    APOLLO_TBU_FEATURE_PRI_QUEUE |
			    APOLLO_TBU_FEATURE_FAULT_QUEUE |
			    APOLLO_TBU_FEATURE_ARCH_DESCRIPTOR_WALK |
			    APOLLO_TBU_FEATURE_ARCH_4_LEVEL_WALK |
			    APOLLO_TBU_FEATURE_STREAM_TABLE_WALK |
			    APOLLO_TBU_FEATURE_CONTEXT_DESCRIPTOR_WALK |
			    APOLLO_TBU_FEATURE_ARCH_FAULT_REPLAY |
			    APOLLO_TBU_FEATURE_ARCH_ATS_PRI_PROTOCOL;
	u64 fault_iova = test->dma_iova_base + test->dma_window_size + SZ_4K;
	u32 features = readl(test->tbu_regs + APOLLO_TBU_REG_FEATURES);
	u32 fault_count;
	int ret;

	if ((features & required) != required)
		return dev_err_probe(dev, -ENODEV,
				     "SMMUv3 functional features missing features=0x%x required=0x%x\n",
				     features, required);

	ret = apollo_hexagon_check_arch_ptw(dev, test);
	if (ret)
		return ret;

	apollo_tbu_write64(test->tbu_regs, APOLLO_TBU_REG_MAP_IOVA_LO,
			   APOLLO_TBU_REG_MAP_IOVA_HI, fault_iova);
	apollo_tbu_write64(test->tbu_regs, APOLLO_TBU_REG_MAP_SIZE_LO,
			   APOLLO_TBU_REG_MAP_SIZE_HI, SZ_4K);
	writel(APOLLO_TBU_FAULT_CTRL_INJECT,
	       test->tbu_regs + APOLLO_TBU_REG_FAULT_CTRL);
	fault_count = readl(test->tbu_regs + APOLLO_TBU_REG_FAULT_STATUS);
	if (!fault_count)
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 fault queue probe did not enqueue\n");

	dev_info(dev,
		 "SMMUv3 page-table walker/ATS/PRI/fault queue ready features=0x%x ats=0x%x pri=%u faultq=%u fault_iova=0x%llx\n",
		 features, readl(test->tbu_regs + APOLLO_TBU_REG_ATS_STATUS),
		 readl(test->tbu_regs + APOLLO_TBU_REG_PRI_STATUS),
		 fault_count, fault_iova);
	writel(APOLLO_TBU_FAULT_CTRL_CLEAR,
	       test->tbu_regs + APOLLO_TBU_REG_FAULT_CTRL);

	return 0;
}

static int apollo_hexagon_dynamic_map(struct device *dev,
				      struct apollo_hexagon_test *test)
{
	int ret;

	ret = apollo_hexagon_tbu_map(dev, test, test->dma_iova_base,
				     test->shared_phys, test->dma_window_size);
	if (ret)
		return ret;
	ret = apollo_hexagon_tbu_ctrl(dev, test, APOLLO_TBU_MAP_CTRL_REMOVE);
	if (ret)
		return ret;
	ret = apollo_hexagon_tbu_map(dev, test, test->dma_iova_base,
				     test->shared_phys, test->dma_window_size);
	if (ret)
		return ret;

	dev_info(dev, "dynamic SMMU map refreshed iova=0x%llx pa=%pa size=0x%llx count=%u\n",
		 test->dma_iova_base, &test->shared_phys,
		 test->dma_window_size,
		 readl(test->tbu_regs + APOLLO_TBU_REG_MAP_COUNT));

	return 0;
}

static int apollo_hexagon_dynamic_sg_map(struct device *dev,
					 struct apollo_hexagon_test *test)
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
	if (resource_size(&res) < APOLLO_HEXAGON_STRESS_OUTPUT_BASE +
	    (APOLLO_HEXAGON_DMA_STRESS_SEGMENTS - 1) *
	    APOLLO_HEXAGON_STRESS_SEGMENT_STRIDE +
	    APOLLO_HEXAGON_DMA_STRESS_BYTES /
	    APOLLO_HEXAGON_DMA_STRESS_SEGMENTS)
		return dev_err_probe(dev, -EINVAL,
				     "shared SRAM too small for >64KB SG DMA stress\n");
	if (resource_size(&res) <
	    APOLLO_HEXAGON_ARCH_BYTES)
		return dev_err_probe(dev, -EINVAL,
				     "shared SRAM too small for SMMUv3 stream/context descriptor probe\n");

	test->shared_base = devm_ioremap(dev, res.start, resource_size(&res));
	if (!test->shared_base)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to map shared SRAM\n");

	test->shared_phys = res.start;
	test->shared_size = resource_size(&res);

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
	writel(APOLLO_HEXAGON_JOB_CTRL_START,
	       test->regs + APOLLO_HEXAGON_REG_JOB_CTRL);

	ret = apollo_hexagon_wait_job(dev, test, queue_id,
				      APOLLO_HEXAGON_JOB_RESULT_OK,
				      &fence_seq);
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

static u32 apollo_hexagon_stress_word(u32 index, u32 seed)
{
	return seed ^ (0x9e3779b9u * (index + 1));
}

static void apollo_hexagon_stress_write_segment(struct apollo_hexagon_test *test,
						u32 offset, u32 first_word,
						u32 words, u32 seed)
{
	u32 i;

	for (i = 0; i < words; i++)
		writel(apollo_hexagon_stress_word(first_word + i, seed),
		       test->shared_base + offset + i * sizeof(u32));
}

static u32 apollo_hexagon_stress_verify_segment(struct apollo_hexagon_test *test,
						u32 offset, u32 first_word,
						u32 words, u32 seed,
						bool *matched)
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

static void apollo_hexagon_stress_clear_segment(struct apollo_hexagon_test *test,
						u32 offset, u32 words)
{
	u32 i;

	for (i = 0; i < words; i++)
		writel(0, test->shared_base + offset + i * sizeof(u32));
}

static int apollo_hexagon_wait_job(struct device *dev,
				   struct apollo_hexagon_test *test,
				   u32 queue_id, u32 expected_result,
				   u32 *fence_seq)
{
	u32 status = APOLLO_HEXAGON_JOB_STATUS_ERROR;
	u32 result;
	u32 irq;
	int tries;

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
	    result != expected_result) {
		dev_err(dev, "accelerator job failed queue=%u status=0x%x result=0x%x expected=0x%x\n",
			queue_id, status, result, expected_result);
		return -EIO;
	}

	irq = readl(test->regs + APOLLO_HEXAGON_REG_IRQ_STATUS);
	if (!(irq & BIT(queue_id))) {
		dev_err(dev, "async irq missing queue=%u irq=0x%x\n", queue_id,
			irq);
		return -EIO;
	}

	*fence_seq = readl(test->regs + APOLLO_HEXAGON_REG_JOB_FENCE);
	writel(BIT(queue_id), test->regs + APOLLO_HEXAGON_REG_IRQ_ACK);
	dev_info(dev, "async fence signaled queue=%u fence=%u irq=0x%x\n",
		 queue_id, *fence_seq, irq);

	return 0;
}

static int apollo_hexagon_submit_dma_stress(struct device *dev,
					    struct apollo_hexagon_test *test,
					    struct apollo_hexagon_dma_stress_job *job)
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
		apollo_hexagon_stress_write_segment(test,
				APOLLO_HEXAGON_STRESS_INPUT_BASE +
				i * APOLLO_HEXAGON_STRESS_SEGMENT_STRIDE,
				i * words_per_segment, words_per_segment,
				job->seed);
		apollo_hexagon_stress_clear_segment(test,
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
	writel(APOLLO_HEXAGON_JOB_CTRL_START,
	       test->regs + APOLLO_HEXAGON_REG_JOB_CTRL);

	ret = apollo_hexagon_wait_job(dev, test, queue_id,
				      APOLLO_HEXAGON_JOB_RESULT_SG_OK,
				      &fence_seq);
	if (ret)
		goto out_unlock;

	dma_rmb();
	for (i = 0; i < APOLLO_HEXAGON_DMA_STRESS_SEGMENTS; i++)
		checksum ^= apollo_hexagon_stress_verify_segment(test,
				APOLLO_HEXAGON_STRESS_OUTPUT_BASE +
				i * APOLLO_HEXAGON_STRESS_SEGMENT_STRIDE,
				i * words_per_segment, words_per_segment,
				job->seed, &matched);
	if (!matched) {
		ret = -EIO;
		dev_err(dev, "SG DMA stress data mismatch checksum=0x%x\n",
			checksum);
		goto out_unlock;
	}

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
	struct apollo_hexagon_dma_stress_job stress;
	int ret;

	switch (cmd) {
	case APOLLO_HEXAGON_IOC_SUBMIT_CNN:
		if (copy_from_user(&job, (void __user *)arg, sizeof(job)))
			return -EFAULT;

		ret = apollo_hexagon_submit_cnn(test->miscdev.parent, test,
						&job);
		if (ret)
			return ret;

		if (copy_to_user((void __user *)arg, &job, sizeof(job)))
			return -EFAULT;

		return 0;
	case APOLLO_HEXAGON_IOC_DMA_STRESS:
		if (copy_from_user(&stress, (void __user *)arg,
				   sizeof(stress)))
			return -EFAULT;
		if (!stress.seed)
			stress.seed = APOLLO_HEXAGON_STRESS_SEED;

		ret = apollo_hexagon_submit_dma_stress(test->miscdev.parent,
						       test, &stress);
		if (ret)
			return ret;

		if (copy_to_user((void __user *)arg, &stress,
				 sizeof(stress)))
			return -EFAULT;

		return 0;
	default:
		return -ENOTTY;
	}
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
	ret = apollo_hexagon_check_tbu_features(dev, test);
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
