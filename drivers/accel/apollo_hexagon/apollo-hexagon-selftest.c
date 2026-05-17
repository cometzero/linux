// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apollo Hexagon DRM accel bring-up selftests.
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/gfp.h>
#include <linux/io.h>
#include <linux/ioport.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/sizes.h>

#include "apollo-hexagon.h"

#define APOLLO_HEXAGON_TEST_SIZE		SZ_4K
#define APOLLO_HEXAGON_TEST_PATTERN		0xa510beef
#define APOLLO_HEXAGON_DMA_TIMEOUT_MS		2000
#define APOLLO_HEXAGON_DMA_POLL_MS		10
#define APOLLO_TBU_REG_FEATURES			0x24
#define APOLLO_TBU_REG_ATS_STATUS		0x28
#define APOLLO_TBU_REG_PRI_STATUS		0x2c
#define APOLLO_TBU_REG_FAULT_STATUS		0x30
#define APOLLO_TBU_REG_FAULT_IOVA_LO		0x34
#define APOLLO_TBU_REG_FAULT_IOVA_HI		0x38
#define APOLLO_TBU_REG_FAULT_CTRL		0x3c
#define APOLLO_TBU_REG_ARCH_TTBR_LO		0x40
#define APOLLO_TBU_REG_ARCH_TTBR_HI		0x44
#define APOLLO_TBU_REG_ARCH_IOVA_LO		0x48
#define APOLLO_TBU_REG_ARCH_IOVA_HI		0x4c
#define APOLLO_TBU_REG_ARCH_CTRL		0x50
#define APOLLO_TBU_REG_ARCH_STATUS		0x54
#define APOLLO_TBU_REG_ARCH_DESC_LO		0x58
#define APOLLO_TBU_REG_ARCH_DESC_HI		0x5c
#define APOLLO_TBU_REG_ARCH_PA_LO		0x60
#define APOLLO_TBU_REG_ARCH_PA_HI		0x64
#define APOLLO_TBU_REG_ARCH_LEVELS		0x68
#define APOLLO_TBU_REG_ARCH_STE_BASE_LO		0x6c
#define APOLLO_TBU_REG_ARCH_STE_BASE_HI		0x70
#define APOLLO_TBU_REG_ARCH_STE_LO		0x74
#define APOLLO_TBU_REG_ARCH_STE_HI		0x78
#define APOLLO_TBU_REG_ARCH_CD_LO		0x7c
#define APOLLO_TBU_REG_ARCH_CD_HI		0x80
#define APOLLO_TBU_REG_ARCH_FAULT_REASON	0x84
#define APOLLO_TBU_REG_ARCH_FAULT_REPLAY	0x88
#define APOLLO_TBU_REG_ARCH_PROTOCOL_STATUS	0x8c
#define APOLLO_TBU_REG_ARCH_CMD_STATUS		0xb4
#define APOLLO_TBU_REG_ARCH_CMD_DETAIL		0xb8
#define APOLLO_TBU_REG_SMMUV3_BASE		0x1000
#define APOLLO_SMMUV3_IDR0			0x000
#define APOLLO_SMMUV3_IDR1			0x004
#define APOLLO_SMMUV3_IDR3			0x00c
#define APOLLO_SMMUV3_AIDR			0x01c
#define APOLLO_SMMUV3_CR0			0x020
#define APOLLO_SMMUV3_CR0ACK			0x024
#define APOLLO_SMMUV3_CR2			0x02c
#define APOLLO_SMMUV3_STATUSR			0x040
#define APOLLO_SMMUV3_IRQ_CTRL			0x050
#define APOLLO_SMMUV3_IRQ_CTRLACK		0x054
#define APOLLO_SMMUV3_GERROR			0x060
#define APOLLO_SMMUV3_GERRORN			0x064
#define APOLLO_SMMUV3_CMDQ_BASE_LO		0x090
#define APOLLO_SMMUV3_CMDQ_BASE_HI		0x094
#define APOLLO_SMMUV3_CMDQ_PROD			0x098
#define APOLLO_SMMUV3_CMDQ_CONS			0x09c
#define APOLLO_SMMUV3_CMDQ_CONS_RD_MASK		GENMASK(19, 0)
#define APOLLO_SMMUV3_CMDQ_CONS_ERR_SHIFT	24
#define APOLLO_SMMUV3_CMDQ_CONS_ERR_MASK	GENMASK(30, 24)
#define APOLLO_SMMUV3_EVENTQ_BASE_LO		0x0a0
#define APOLLO_SMMUV3_EVENTQ_BASE_HI		0x0a4
#define APOLLO_SMMUV3_EVENTQ_PROD		0x0a8
#define APOLLO_SMMUV3_EVENTQ_CONS		0x0ac
#define APOLLO_SMMUV3_PRIQ_BASE_LO		0x0c0
#define APOLLO_SMMUV3_PRIQ_BASE_HI		0x0c4
#define APOLLO_SMMUV3_PRIQ_PROD			0x0c8
#define APOLLO_SMMUV3_PRIQ_CONS			0x0cc
#define APOLLO_SMMUV3_STATUS			0x0e0
#define APOLLO_TBU_ARCH_CTRL_PROBE		1
#define APOLLO_TBU_ARCH_CTRL_NEGATIVE_REPLAY	2
#define APOLLO_TBU_ARCH_CTRL_ATS_TREQ		4
#define APOLLO_TBU_ARCH_STATUS_OK		1
#define APOLLO_TBU_ARCH_STATUS_ERROR		2
#define APOLLO_TBU_FEATURE_PAGE_TABLE_WALKER	BIT(0)
#define APOLLO_TBU_FEATURE_ATS_CACHE		BIT(1)
#define APOLLO_TBU_FEATURE_PRI_QUEUE		BIT(2)
#define APOLLO_TBU_FEATURE_FAULT_QUEUE		BIT(3)
#define APOLLO_TBU_FEATURE_ARCH_DESCRIPTOR_WALK	BIT(4)
#define APOLLO_TBU_FEATURE_ARCH_4_LEVEL_WALK	BIT(5)
#define APOLLO_TBU_FEATURE_STREAM_TABLE_WALK	BIT(6)
#define APOLLO_TBU_FEATURE_CONTEXT_DESCRIPTOR_WALK \
	BIT(7)
#define APOLLO_TBU_FEATURE_ARCH_FAULT_REPLAY	BIT(8)
#define APOLLO_TBU_FEATURE_ARCH_ATS_PRI_PROTOCOL	BIT(9)
#define APOLLO_TBU_FEATURE_ARCH_REG_QUEUE_SURFACE	BIT(10)
#define APOLLO_TBU_FEATURE_ARCH_CMD_INVALIDATION	BIT(12)
#define APOLLO_TBU_FEATURE_ARCH_CR0_QUEUE_GATES	BIT(18)
#define APOLLO_TBU_FEATURE_ARCH_ATSCHK_EATS_GATES	BIT(19)
#define APOLLO_TBU_FEATURE_ARCH_REC_CFG_ATS_GATES	BIT(20)
#define APOLLO_TBU_FEATURE_ARCH_DPTI_UNSUPPORTED	BIT(21)
#define APOLLO_TBU_FEATURE_ARCH_CMDQ_CERROR	BIT(22)
#define APOLLO_TBU_FEATURE_ARCH_PRI_AUTO_RESPONSE	BIT(25)
#define APOLLO_TBU_FEATURE_ARCH_IRQ_MSI_CFG	BIT(26)
#define APOLLO_SMMUV3_CR0_SMMUEN		BIT(0)
#define APOLLO_SMMUV3_CR0_PRIQEN		BIT(1)
#define APOLLO_SMMUV3_CR0_EVENTQEN		BIT(2)
#define APOLLO_SMMUV3_CR0_CMDQEN		BIT(3)
#define APOLLO_SMMUV3_CR0_ATSCHK		BIT(4)
#define APOLLO_SMMUV3_CR0_ENABLE_QUEUES		\
	(APOLLO_SMMUV3_CR0_SMMUEN | APOLLO_SMMUV3_CR0_PRIQEN | \
	 APOLLO_SMMUV3_CR0_EVENTQEN | APOLLO_SMMUV3_CR0_CMDQEN | \
	 APOLLO_SMMUV3_CR0_ATSCHK)
#define APOLLO_SMMUV3_CR2_RECINVSID		BIT(1)
#define APOLLO_SMMUV3_CR2_PTM			BIT(2)
#define APOLLO_SMMUV3_CR2_REC_CFG_ATS		BIT(3)
#define APOLLO_TBU_FAULT_CTRL_CLEAR		1
#define APOLLO_TBU_FAULT_CTRL_INJECT		2
#define APOLLO_TBU_ARCH_INDEX_MASK		0x1ffULL
#define APOLLO_TBU_ARCH_LEVELS			4
#define APOLLO_TBU_ARCH_L0_SHIFT		39
#define APOLLO_TBU_ARCH_LEVEL_BITS		9
#define APOLLO_TBU_ARCH_DESC_TABLE		0x3ULL
#define APOLLO_TBU_ARCH_DESC_PAGE		0x3ULL
#define APOLLO_TBU_ARCH_DESC_AF			BIT_ULL(10)
#define APOLLO_TBU_ARCH_DESC_SH_INNER		(3ULL << 8)
#define APOLLO_TBU_ARCH_DESC_OUTPUT_MASK	GENMASK_ULL(47, 12)
#define APOLLO_TBU_ARCH_STE_EATS_SHIFT		28
#define APOLLO_TBU_ARCH_STE_EATS_DISABLED	0
#define APOLLO_TBU_ARCH_STE_EATS_FULL		1
#define APOLLO_TBU_ARCH_STE_EATS_SPLIT		2
#define APOLLO_TBU_ARCH_STE_EATS(eats)		\
	(((u64)(eats) & 0x3ULL) << APOLLO_TBU_ARCH_STE_EATS_SHIFT)
#define APOLLO_TBU_ARCH_STE_SIZE		64
#define APOLLO_TBU_ARCH_STE_VALID		BIT_ULL(0)
#define APOLLO_TBU_ARCH_STE_S1_ENABLED		BIT_ULL(1)
#define APOLLO_TBU_ARCH_CD_VALID		BIT_ULL(0)
#define APOLLO_TBU_ARCH_FAULT_STE_INVALID	2
#define APOLLO_TBU_ARCH_FAULT_BAD_ATS_TREQ	14
#define APOLLO_SMMUV3_ARCH_IDR0			0x098db7cb
#define APOLLO_SMMUV3_ARCH_IDR1			0x0def7d08
#define APOLLO_SMMUV3_ARCH_IDR3			0x00007794
#define APOLLO_SMMUV3_ARCH_IDR3_RIL		BIT(10)
#define APOLLO_SMMUV3_ARCH_AIDR			0x00000003
#define APOLLO_SMMUV3_ARCH_STATUS_READY		BIT(0)
#define APOLLO_SMMUV3_ARCH_STATUS_QUEUE_MODEL	BIT(1)
#define APOLLO_SMMUV3_ARCH_IRQ_EVENTQ		BIT(0)
#define APOLLO_SMMUV3_ARCH_IRQ_PRIQ		BIT(1)
#define APOLLO_SMMUV3_ARCH_IRQ_CMDQ_SYNC	BIT(2)
#define APOLLO_SMMUV3_ARCH_IRQ_GERROR		BIT(3)
#define APOLLO_SMMUV3_ARCH_GERROR_CMDQ_ABORT	BIT(0)
#define APOLLO_SMMUV3_ARCH_GERROR_EVENTQ_ABORT	BIT(2)
#define APOLLO_SMMUV3_ARCH_GERROR_PRIQ_ABORT	BIT(3)
#define APOLLO_SMMUV3_ARCH_GERROR_KNOWN_MASK	\
	(APOLLO_SMMUV3_ARCH_GERROR_CMDQ_ABORT | \
	 APOLLO_SMMUV3_ARCH_GERROR_EVENTQ_ABORT | \
	 APOLLO_SMMUV3_ARCH_GERROR_PRIQ_ABORT)
#define APOLLO_SMMUV3_ARCH_CERROR_ILL		1
#define APOLLO_SMMUV3_ARCH_CMD_SYNC		0x46
#define APOLLO_SMMUV3_ARCH_CMD_PRI_RESP		0x41
#define APOLLO_SMMUV3_ARCH_CMD_TLBI_NH_ALL	0x10
#define APOLLO_SMMUV3_ARCH_CMD_TLBI_NH_VA	0x12
#define APOLLO_SMMUV3_ARCH_CMD_ATC_INV		0x40
#define APOLLO_SMMUV3_ARCH_CMD_DPTI_ALL		0x70
#define APOLLO_SMMUV3_ARCH_CMDQ_ENTRY_BYTES	16
#define APOLLO_SMMUV3_ARCH_QUEUE_LOG2_ENTRIES	3
#define APOLLO_SMMUV3_ARCH_CMDQ_ENTRIES		\
	(1U << APOLLO_SMMUV3_ARCH_QUEUE_LOG2_ENTRIES)
#define APOLLO_SMMUV3_ARCH_CMDQ_INDEX_MASK	\
	(APOLLO_SMMUV3_ARCH_CMDQ_ENTRIES - 1)
#define APOLLO_SMMUV3_ARCH_CMDQ_RANGE_NUM_SHIFT		12
#define APOLLO_SMMUV3_ARCH_CMDQ_RANGE_SCALE_SHIFT	20
#define APOLLO_SMMUV3_ARCH_CMDQ_RANGE_TG_SHIFT		10
#define APOLLO_SMMUV3_ARCH_CMDQ_RANGE_TG_4K		1
#define APOLLO_SMMUV3_ARCH_CMDQ_LEAF		BIT_ULL(0)
#define APOLLO_HEXAGON_PTW_OFFSET		0x18000
#define APOLLO_HEXAGON_PTW_PAGES		APOLLO_TBU_ARCH_LEVELS
#define APOLLO_HEXAGON_STE_OFFSET		\
	(APOLLO_HEXAGON_PTW_OFFSET + SZ_4K * APOLLO_HEXAGON_PTW_PAGES)
#define APOLLO_HEXAGON_CD_OFFSET		(APOLLO_HEXAGON_STE_OFFSET + SZ_4K)
#define APOLLO_HEXAGON_ARCH_BYTES		(APOLLO_HEXAGON_CD_OFFSET + SZ_4K)
#define APOLLO_HEXAGON_SMMUV3_CMDQ_OFFSET	0x100000
#define APOLLO_HEXAGON_SMMUV3_EVENTQ_OFFSET	0x101000
#define APOLLO_HEXAGON_SMMUV3_PRIQ_OFFSET	0x102000
#define APOLLO_HEXAGON_SMMUV3_QUEUE_BYTES	\
	(APOLLO_HEXAGON_SMMUV3_PRIQ_OFFSET + SZ_4K)

static const u32 apollo_hexagon_dma_expected[] = {
	0x48455831, 0x444d4131, 0x11223344, 0x55667788,
	0xa5a55a5a, 0x5a5aa5a5, 0xfeedc0de, 0x600dbeef,
};

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

	base = devm_ioremap(dev, res.start, sizeof(apollo_hexagon_dma_expected));
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

static u64 apollo_tbu_read64(void __iomem *base, u32 lo, u32 hi)
{
	return readl(base + lo) | ((u64)readl(base + hi) << 32);
}

static void apollo_smmuv3_write64(struct apollo_hexagon *test,
				  u32 lo, u32 hi, u64 value)
{
	void __iomem *smmuv3 = test->tbu_regs + APOLLO_TBU_REG_SMMUV3_BASE;

	apollo_tbu_write64(smmuv3, lo, hi, value);
}

static u32 apollo_smmuv3_gerror_active(void __iomem *smmuv3)
{
	return (readl(smmuv3 + APOLLO_SMMUV3_GERROR) ^
		readl(smmuv3 + APOLLO_SMMUV3_GERRORN)) &
	       APOLLO_SMMUV3_ARCH_GERROR_KNOWN_MASK;
}

static u64 apollo_hexagon_shared_read64(struct apollo_hexagon *test, u64 offset)
{
	return readl(test->shared_base + offset) |
	       ((u64)readl(test->shared_base + offset + sizeof(u32)) << 32);
}

static u64 apollo_tbu_arch_index(u64 iova, u32 level)
{
	return (iova >> (APOLLO_TBU_ARCH_L0_SHIFT -
			 level * APOLLO_TBU_ARCH_LEVEL_BITS)) &
	       APOLLO_TBU_ARCH_INDEX_MASK;
}

static void apollo_hexagon_ptw_write_desc(struct apollo_hexagon *test,
					  u64 offset, u64 desc)
{
	writel(lower_32_bits(desc), test->shared_base + offset);
	writel(upper_32_bits(desc), test->shared_base + offset + sizeof(u32));
}

static void apollo_hexagon_smmuv3_write_cmd(struct apollo_hexagon *test,
					    u32 index, u64 word0, u64 word1)
{
	const u64 offset = APOLLO_HEXAGON_SMMUV3_CMDQ_OFFSET +
			   index * APOLLO_SMMUV3_ARCH_CMDQ_ENTRY_BYTES;

	apollo_hexagon_ptw_write_desc(test, offset, word0);
	apollo_hexagon_ptw_write_desc(test, offset + sizeof(word0), word1);
}

static u32 apollo_smmuv3_cmdq_advance(u32 value)
{
	u32 index = (value & APOLLO_SMMUV3_ARCH_CMDQ_INDEX_MASK) + 1;
	u32 wrap = value & APOLLO_SMMUV3_ARCH_CMDQ_ENTRIES;

	if (index == APOLLO_SMMUV3_ARCH_CMDQ_ENTRIES) {
		index = 0;
		wrap ^= APOLLO_SMMUV3_ARCH_CMDQ_ENTRIES;
	}

	return wrap | index;
}

static int apollo_hexagon_issue_ril_tlbi(struct device *dev,
					 struct apollo_hexagon *test,
					 u64 iova, u32 bytes)
{
	void __iomem *smmuv3 = test->tbu_regs + APOLLO_TBU_REG_SMMUV3_BASE;
	const u32 pages = DIV_ROUND_UP(bytes, SZ_4K);
	const u32 prod = readl(smmuv3 + APOLLO_SMMUV3_CMDQ_PROD);
	const u32 index = prod & APOLLO_SMMUV3_ARCH_CMDQ_INDEX_MASK;
	const u32 next_prod = apollo_smmuv3_cmdq_advance(prod);
	u32 cmdq_cons;
	u32 cmd_status;
	u32 cmd_detail;
	u32 invalidated;
	u32 last_opcode;
	u64 word0;
	u64 word1;

	if (!(readl(smmuv3 + APOLLO_SMMUV3_IDR3) &
	      APOLLO_SMMUV3_ARCH_IDR3_RIL))
		return dev_err_probe(dev, -ENODEV,
				     "SMMUv3 RIL bit not advertised idr3=0x%x\n",
				     readl(smmuv3 + APOLLO_SMMUV3_IDR3));
	if (pages < 2 || pages > 32)
		return dev_err_probe(dev, -EINVAL,
				     "SMMUv3 RIL stress pages out of modeled range pages=%u bytes=%u\n",
				     pages, bytes);

	word0 = APOLLO_SMMUV3_ARCH_CMD_TLBI_NH_VA |
		((u64)(pages - 1) <<
		 APOLLO_SMMUV3_ARCH_CMDQ_RANGE_NUM_SHIFT);
	word1 = (iova & PAGE_MASK) |
		((u64)APOLLO_SMMUV3_ARCH_CMDQ_RANGE_TG_4K <<
		 APOLLO_SMMUV3_ARCH_CMDQ_RANGE_TG_SHIFT) |
		APOLLO_SMMUV3_ARCH_CMDQ_LEAF;

	apollo_hexagon_smmuv3_write_cmd(test, index, word0, word1);
	writel(next_prod, smmuv3 + APOLLO_SMMUV3_CMDQ_PROD);

	cmdq_cons = readl(smmuv3 + APOLLO_SMMUV3_CMDQ_CONS) &
		    APOLLO_SMMUV3_CMDQ_CONS_RD_MASK;
	cmd_status = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_CMD_STATUS);
	cmd_detail = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_CMD_DETAIL);
	invalidated = cmd_detail & 0xff;
	last_opcode = (cmd_status >> 24) & 0xff;
	if (cmdq_cons != next_prod ||
	    last_opcode != APOLLO_SMMUV3_ARCH_CMD_TLBI_NH_VA ||
	    !invalidated)
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 RIL TLBI_NH_VA range mismatch prod=0x%x next=0x%x cons=0x%x status=0x%x detail=0x%x invalidated=%u\n",
				     prod, next_prod, cmdq_cons, cmd_status,
				     cmd_detail, invalidated);

	dev_info(dev,
		 "SMMUv3 RIL TLBI_NH_VA range selftest ok iova=0x%llx bytes=%u pages=%u prod=0x%x cons=0x%x cmd-status=0x%x cmd-detail=0x%x invalidated=%u\n",
		 iova & PAGE_MASK, bytes, pages, prod, cmdq_cons,
		 cmd_status, cmd_detail, invalidated);

	return 0;
}

static int apollo_hexagon_check_arch_ptw(struct device *dev,
					 struct apollo_hexagon *test)
{
	void __iomem *smmuv3 = test->tbu_regs + APOLLO_TBU_REG_SMMUV3_BASE;
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
	const u64 ste1_eats_full =
		(cd_base & APOLLO_TBU_ARCH_DESC_OUTPUT_MASK) |
		APOLLO_TBU_ARCH_STE_EATS(APOLLO_TBU_ARCH_STE_EATS_FULL);
	const u64 ste1_eats_disabled =
		cd_base & APOLLO_TBU_ARCH_DESC_OUTPUT_MASK;
	const u64 ste1_eats_split =
		(cd_base & APOLLO_TBU_ARCH_DESC_OUTPUT_MASK) |
		APOLLO_TBU_ARCH_STE_EATS(APOLLO_TBU_ARCH_STE_EATS_SPLIT);
	const u64 ste1 = ste1_eats_full;
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
	protocol = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_PROTOCOL_STATUS);
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

	writel(APOLLO_SMMUV3_CR0_EVENTQEN, smmuv3 + APOLLO_SMMUV3_CR0);
	writel(0, smmuv3 + APOLLO_SMMUV3_CR2);
	writel(APOLLO_TBU_FAULT_CTRL_CLEAR,
	       test->tbu_regs + APOLLO_TBU_REG_FAULT_CTRL);
	writel(APOLLO_TBU_ARCH_CTRL_ATS_TREQ,
	       test->tbu_regs + APOLLO_TBU_REG_ARCH_CTRL);
	status = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_STATUS);
	reason = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_FAULT_REASON);
	fault_count = readl(test->tbu_regs + APOLLO_TBU_REG_FAULT_STATUS);
	if (status != APOLLO_TBU_ARCH_STATUS_ERROR ||
	    reason != APOLLO_TBU_ARCH_FAULT_BAD_ATS_TREQ || fault_count)
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 ATS REC_CFG_ATS disabled mismatch status=0x%x reason=%u faultq=%u\n",
				     status, reason, fault_count);

	writel(APOLLO_SMMUV3_CR2_REC_CFG_ATS, smmuv3 + APOLLO_SMMUV3_CR2);
	writel(APOLLO_TBU_FAULT_CTRL_CLEAR,
	       test->tbu_regs + APOLLO_TBU_REG_FAULT_CTRL);
	writel(APOLLO_TBU_ARCH_CTRL_ATS_TREQ,
	       test->tbu_regs + APOLLO_TBU_REG_ARCH_CTRL);
	status = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_STATUS);
	reason = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_FAULT_REASON);
	fault_count = readl(test->tbu_regs + APOLLO_TBU_REG_FAULT_STATUS);
	if (status != APOLLO_TBU_ARCH_STATUS_ERROR ||
	    reason != APOLLO_TBU_ARCH_FAULT_BAD_ATS_TREQ || !fault_count)
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 ATS REC_CFG_ATS enabled mismatch status=0x%x reason=%u faultq=%u\n",
				     status, reason, fault_count);

	dev_info(dev,
		 "SMMUv3 REC_CFG_ATS translation request selftest ok stream-id=0x%x faultq=%u\n",
		 stream_id, fault_count);

	apollo_hexagon_ptw_write_desc(test, ste_offset, ste0);
	writel(APOLLO_TBU_FAULT_CTRL_CLEAR,
	       test->tbu_regs + APOLLO_TBU_REG_FAULT_CTRL);

	writel(APOLLO_SMMUV3_CR2_REC_CFG_ATS | APOLLO_SMMUV3_CR2_RECINVSID |
	       APOLLO_SMMUV3_CR2_PTM,
	       smmuv3 + APOLLO_SMMUV3_CR2);
	writel(APOLLO_SMMUV3_CR0_SMMUEN |
	       APOLLO_SMMUV3_CR0_PRIQEN |
	       APOLLO_SMMUV3_CR0_EVENTQEN |
	       APOLLO_SMMUV3_CR0_CMDQEN,
	       smmuv3 + APOLLO_SMMUV3_CR0);
	apollo_hexagon_ptw_write_desc(test, ste_offset + sizeof(ste0),
				      ste1_eats_split);
	writel(APOLLO_TBU_ARCH_CTRL_ATS_TREQ,
	       test->tbu_regs + APOLLO_TBU_REG_ARCH_CTRL);
	status = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_STATUS);
	reason = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_FAULT_REASON);
	protocol = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_PROTOCOL_STATUS);
	if (status != APOLLO_TBU_ARCH_STATUS_ERROR ||
	    reason != APOLLO_TBU_ARCH_FAULT_BAD_ATS_TREQ || !protocol)
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 ATSCHK/EATS split-without-ATSCHK mismatch status=0x%x reason=%u protocol=0x%x\n",
				     status, reason, protocol);

	writel(APOLLO_SMMUV3_CR0_ENABLE_QUEUES, smmuv3 + APOLLO_SMMUV3_CR0);
	apollo_hexagon_ptw_write_desc(test, ste_offset + sizeof(ste0),
				      ste1_eats_disabled);
	writel(APOLLO_TBU_FAULT_CTRL_CLEAR,
	       test->tbu_regs + APOLLO_TBU_REG_FAULT_CTRL);
	writel(APOLLO_TBU_ARCH_CTRL_ATS_TREQ,
	       test->tbu_regs + APOLLO_TBU_REG_ARCH_CTRL);
	status = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_STATUS);
	reason = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_FAULT_REASON);
	if (status != APOLLO_TBU_ARCH_STATUS_ERROR ||
	    reason != APOLLO_TBU_ARCH_FAULT_BAD_ATS_TREQ)
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 ATS EATS-disabled mismatch status=0x%x reason=%u\n",
				     status, reason);

	apollo_hexagon_ptw_write_desc(test, ste_offset + sizeof(ste0),
				      ste1_eats_full);
	writel(APOLLO_TBU_FAULT_CTRL_CLEAR,
	       test->tbu_regs + APOLLO_TBU_REG_FAULT_CTRL);
	writel(APOLLO_TBU_ARCH_CTRL_ATS_TREQ,
	       test->tbu_regs + APOLLO_TBU_REG_ARCH_CTRL);
	status = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_STATUS);
	protocol = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_PROTOCOL_STATUS);
	if (status != APOLLO_TBU_ARCH_STATUS_OK || !protocol)
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 ATS EATS-full mismatch status=0x%x protocol=0x%x\n",
				     status, protocol);

	dev_info(dev,
		 "SMMUv3 ATSCHK/EATS translation request selftest ok stream-id=0x%x reason=%u protocol=0x%x\n",
		 stream_id, APOLLO_TBU_ARCH_FAULT_BAD_ATS_TREQ, protocol);

	return 0;
}

static int apollo_hexagon_check_arch_queues(struct device *dev,
					    struct apollo_hexagon *test)
{
	void __iomem *smmuv3 = test->tbu_regs + APOLLO_TBU_REG_SMMUV3_BASE;
	const u64 cmdq = test->shared_phys + APOLLO_HEXAGON_SMMUV3_CMDQ_OFFSET;
	const u64 eventq = test->shared_phys + APOLLO_HEXAGON_SMMUV3_EVENTQ_OFFSET;
	const u64 priq = test->shared_phys + APOLLO_HEXAGON_SMMUV3_PRIQ_OFFSET;
	const u32 stream_id = readl(test->regs + APOLLO_HEXAGON_REG_STREAM_ID);
	u32 cmdq_cons;
	u32 cmd_status;
	u32 cmd_detail;
	u32 cmd_invalidations;
	u32 invalidation_cmd_status;
	u32 dpti_cons;
	u32 dpti_cons_rd;
	u32 dpti_cerror;
	u32 dpti_gerror;
	u32 dpti_gerrorn;
	u32 dpti_gerror_active;
	u32 eventq_prod;
	u32 priq_prod;
	u32 statusr;
	u32 status;
	u64 event0;
	u64 pri0;
	int ret;

	if (APOLLO_HEXAGON_SMMUV3_QUEUE_BYTES > test->shared_size)
		return dev_err_probe(dev, -EINVAL,
				     "shared SRAM too small for SMMUv3 queue probe\n");

	if (readl(smmuv3 + APOLLO_SMMUV3_IDR0) != APOLLO_SMMUV3_ARCH_IDR0 ||
	    readl(smmuv3 + APOLLO_SMMUV3_IDR1) != APOLLO_SMMUV3_ARCH_IDR1 ||
	    readl(smmuv3 + APOLLO_SMMUV3_IDR3) != APOLLO_SMMUV3_ARCH_IDR3 ||
	    readl(smmuv3 + APOLLO_SMMUV3_AIDR) != APOLLO_SMMUV3_ARCH_AIDR ||
	    readl(smmuv3 + APOLLO_SMMUV3_STATUS) !=
	    (APOLLO_SMMUV3_ARCH_STATUS_READY |
	     APOLLO_SMMUV3_ARCH_STATUS_QUEUE_MODEL))
		return dev_err_probe(dev, -ENODEV,
				     "SMMUv3 architected register ID/status probe failed idr0=0x%x idr1=0x%x idr3=0x%x aidr=0x%x status=0x%x\n",
				     readl(smmuv3 + APOLLO_SMMUV3_IDR0),
				     readl(smmuv3 + APOLLO_SMMUV3_IDR1),
				     readl(smmuv3 + APOLLO_SMMUV3_IDR3),
				     readl(smmuv3 + APOLLO_SMMUV3_AIDR),
				     readl(smmuv3 + APOLLO_SMMUV3_STATUS));

	writel(APOLLO_SMMUV3_ARCH_IRQ_EVENTQ |
	       APOLLO_SMMUV3_ARCH_IRQ_PRIQ |
	       APOLLO_SMMUV3_ARCH_IRQ_CMDQ_SYNC |
	       APOLLO_SMMUV3_ARCH_IRQ_GERROR,
	       smmuv3 + APOLLO_SMMUV3_IRQ_CTRL);
	if (readl(smmuv3 + APOLLO_SMMUV3_IRQ_CTRLACK) !=
	    (APOLLO_SMMUV3_ARCH_IRQ_EVENTQ |
	     APOLLO_SMMUV3_ARCH_IRQ_PRIQ |
	     APOLLO_SMMUV3_ARCH_IRQ_CMDQ_SYNC |
	     APOLLO_SMMUV3_ARCH_IRQ_GERROR))
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 IRQ_CTRLACK mismatch ack=0x%x\n",
				     readl(smmuv3 + APOLLO_SMMUV3_IRQ_CTRLACK));

	writel(APOLLO_SMMUV3_CR0_ENABLE_QUEUES, smmuv3 + APOLLO_SMMUV3_CR0);
	if (readl(smmuv3 + APOLLO_SMMUV3_CR0ACK) !=
	    APOLLO_SMMUV3_CR0_ENABLE_QUEUES)
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 CR0ACK mismatch ack=0x%x\n",
				     readl(smmuv3 + APOLLO_SMMUV3_CR0ACK));

	memset_io(test->shared_base + APOLLO_HEXAGON_SMMUV3_CMDQ_OFFSET, 0,
		  APOLLO_HEXAGON_SMMUV3_QUEUE_BYTES -
		  APOLLO_HEXAGON_SMMUV3_CMDQ_OFFSET);
	apollo_hexagon_smmuv3_write_cmd(test, 0,
					APOLLO_SMMUV3_ARCH_CMD_SYNC, 0);
	apollo_hexagon_smmuv3_write_cmd(test, 1,
					APOLLO_SMMUV3_ARCH_CMD_PRI_RESP, 0);
	apollo_hexagon_smmuv3_write_cmd(test, 2,
					((u64)stream_id << 32) |
					APOLLO_SMMUV3_ARCH_CMD_ATC_INV,
					test->dma_iova_base);
	apollo_hexagon_smmuv3_write_cmd(test, 3,
					APOLLO_SMMUV3_ARCH_CMD_TLBI_NH_ALL,
					0);

	apollo_smmuv3_write64(test, APOLLO_SMMUV3_CMDQ_BASE_LO,
			      APOLLO_SMMUV3_CMDQ_BASE_HI,
			      cmdq | APOLLO_SMMUV3_ARCH_QUEUE_LOG2_ENTRIES);
	apollo_smmuv3_write64(test, APOLLO_SMMUV3_EVENTQ_BASE_LO,
			      APOLLO_SMMUV3_EVENTQ_BASE_HI,
			      eventq | APOLLO_SMMUV3_ARCH_QUEUE_LOG2_ENTRIES);
	apollo_smmuv3_write64(test, APOLLO_SMMUV3_PRIQ_BASE_LO,
			      APOLLO_SMMUV3_PRIQ_BASE_HI,
			      priq | APOLLO_SMMUV3_ARCH_QUEUE_LOG2_ENTRIES);

	writel(4, smmuv3 + APOLLO_SMMUV3_CMDQ_PROD);
	cmdq_cons = readl(smmuv3 + APOLLO_SMMUV3_CMDQ_CONS);
	statusr = readl(smmuv3 + APOLLO_SMMUV3_STATUSR);
	cmd_status = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_CMD_STATUS);
	cmd_detail = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_CMD_DETAIL);
	invalidation_cmd_status = cmd_status;
	cmd_invalidations = (cmd_status >> 8) & 0xff;
	if (cmdq_cons != 4 ||
	    !(statusr & APOLLO_SMMUV3_ARCH_IRQ_CMDQ_SYNC))
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 CMDQ consume mismatch cons=%u statusr=0x%x cmd-status=0x%x\n",
				     cmdq_cons, statusr, cmd_status);
	if (!cmd_invalidations)
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 CMDQ invalidation did not clear ATS entries cmd-status=0x%x cmd-detail=0x%x\n",
				     cmd_status, cmd_detail);

	apollo_hexagon_smmuv3_write_cmd(test, 4,
					APOLLO_SMMUV3_ARCH_CMD_DPTI_ALL,
					test->shared_phys);
	writel(5, smmuv3 + APOLLO_SMMUV3_CMDQ_PROD);
	dpti_cons = readl(smmuv3 + APOLLO_SMMUV3_CMDQ_CONS);
	dpti_cons_rd = dpti_cons & APOLLO_SMMUV3_CMDQ_CONS_RD_MASK;
	dpti_cerror = (dpti_cons & APOLLO_SMMUV3_CMDQ_CONS_ERR_MASK) >>
		      APOLLO_SMMUV3_CMDQ_CONS_ERR_SHIFT;
	statusr = readl(smmuv3 + APOLLO_SMMUV3_STATUSR);
	dpti_gerror = readl(smmuv3 + APOLLO_SMMUV3_GERROR);
	dpti_gerrorn = readl(smmuv3 + APOLLO_SMMUV3_GERRORN);
	dpti_gerror_active = (dpti_gerror ^ dpti_gerrorn) &
		APOLLO_SMMUV3_ARCH_GERROR_KNOWN_MASK;
	cmd_status = readl(test->tbu_regs + APOLLO_TBU_REG_ARCH_CMD_STATUS);
	if (dpti_cons_rd != 4 ||
	    dpti_cerror != APOLLO_SMMUV3_ARCH_CERROR_ILL ||
	    ((cmd_status >> 24) & 0xff) != APOLLO_SMMUV3_ARCH_CMD_DPTI_ALL ||
	    !(statusr & APOLLO_SMMUV3_ARCH_IRQ_GERROR) ||
	    !(dpti_gerror_active & APOLLO_SMMUV3_ARCH_GERROR_CMDQ_ABORT))
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 DPTI unsupported command mismatch cons=0x%x rd=%u cerror=%u statusr=0x%x gerror=0x%x gerrorn=0x%x active=0x%x cmd-status=0x%x\n",
				     dpti_cons, dpti_cons_rd, dpti_cerror,
				     statusr, dpti_gerror, dpti_gerrorn,
				     dpti_gerror_active, cmd_status);
	writel(APOLLO_SMMUV3_ARCH_GERROR_CMDQ_ABORT,
	       smmuv3 + APOLLO_SMMUV3_GERRORN);
	if (apollo_smmuv3_gerror_active(smmuv3) &
	    APOLLO_SMMUV3_ARCH_GERROR_CMDQ_ABORT)
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 DPTI unsupported GERROR ack failed gerror=0x%x gerrorn=0x%x active=0x%x\n",
				     readl(smmuv3 + APOLLO_SMMUV3_GERROR),
				     readl(smmuv3 + APOLLO_SMMUV3_GERRORN),
				     apollo_smmuv3_gerror_active(smmuv3));
	writel(5, smmuv3 + APOLLO_SMMUV3_CMDQ_CONS);

	ret = apollo_hexagon_tbu_map(dev, test, test->dma_iova_base,
				     test->shared_phys, test->dma_window_size);
	if (ret)
		return ret;
	priq_prod = readl(smmuv3 + APOLLO_SMMUV3_PRIQ_PROD);
	pri0 = apollo_hexagon_shared_read64(test,
					     APOLLO_HEXAGON_SMMUV3_PRIQ_OFFSET);
	if (!priq_prod || (pri0 & 0xffffffffULL) != 0x2)
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 PRIQ record mismatch prod=%u word0=0x%llx\n",
				     priq_prod, pri0);

	writel(APOLLO_TBU_FAULT_CTRL_INJECT,
	       test->tbu_regs + APOLLO_TBU_REG_FAULT_CTRL);
	eventq_prod = readl(smmuv3 + APOLLO_SMMUV3_EVENTQ_PROD);
	event0 = apollo_hexagon_shared_read64(test,
					      APOLLO_HEXAGON_SMMUV3_EVENTQ_OFFSET);
	if (!eventq_prod || (event0 & 0xffffffffULL) != 0x1)
		return dev_err_probe(dev, -EIO,
				     "SMMUv3 EVENTQ record mismatch prod=%u word0=0x%llx\n",
				     eventq_prod, event0);

	writel(eventq_prod, smmuv3 + APOLLO_SMMUV3_EVENTQ_CONS);
	writel(priq_prod, smmuv3 + APOLLO_SMMUV3_PRIQ_CONS);
	writel(APOLLO_TBU_FAULT_CTRL_CLEAR,
	       test->tbu_regs + APOLLO_TBU_REG_FAULT_CTRL);

	status = readl(smmuv3 + APOLLO_SMMUV3_STATUSR);
	dev_info(dev,
		 "SMMUv3 architected queue register selftest ok idr0=0x%x cmdq-cons=%u eventq-prod=%u priq-prod=%u statusr=0x%x event0=0x%llx pri0=0x%llx\n",
		 readl(smmuv3 + APOLLO_SMMUV3_IDR0), cmdq_cons, eventq_prod,
		 priq_prod, status, event0, pri0);
	dev_info(dev,
		 "SMMUv3 command invalidation selftest ok stream-id=0x%x cmdq-cons=%u cmd-status=0x%x cmd-detail=0x%x invalidations=%u ops=ATC_INV,TLBI_NH_ALL\n",
		 stream_id, cmdq_cons, invalidation_cmd_status, cmd_detail,
		 cmd_invalidations);
	dev_info(dev,
		 "SMMUv3 DPTI unsupported command selftest ok idr3=0x%x cmdq-cons=0x%x rd=%u cerror=%u gerror=0x%x gerrorn=0x%x active=0x%x opcode=0x%x\n",
		 readl(smmuv3 + APOLLO_SMMUV3_IDR3), dpti_cons,
		 dpti_cons_rd, dpti_cerror, dpti_gerror, dpti_gerrorn,
		 dpti_gerror_active, APOLLO_SMMUV3_ARCH_CMD_DPTI_ALL);

	return 0;
}

static int apollo_hexagon_check_tbu_features(struct device *dev,
					     struct apollo_hexagon *test)
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
			    APOLLO_TBU_FEATURE_ARCH_ATS_PRI_PROTOCOL |
			    APOLLO_TBU_FEATURE_ARCH_REG_QUEUE_SURFACE |
			    APOLLO_TBU_FEATURE_ARCH_CMD_INVALIDATION |
			    APOLLO_TBU_FEATURE_ARCH_CR0_QUEUE_GATES |
			    APOLLO_TBU_FEATURE_ARCH_ATSCHK_EATS_GATES |
			    APOLLO_TBU_FEATURE_ARCH_REC_CFG_ATS_GATES |
			    APOLLO_TBU_FEATURE_ARCH_DPTI_UNSUPPORTED |
			    APOLLO_TBU_FEATURE_ARCH_CMDQ_CERROR |
			    APOLLO_TBU_FEATURE_ARCH_PRI_AUTO_RESPONSE |
			    APOLLO_TBU_FEATURE_ARCH_IRQ_MSI_CFG;
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
	ret = apollo_hexagon_check_arch_queues(dev, test);
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

int apollo_hexagon_run_dma_stress_selftests(struct device *dev,
					    struct apollo_hexagon *test)
{
	return apollo_hexagon_issue_ril_tlbi(
		dev, test,
		test->dma_iova_base + APOLLO_HEXAGON_STRESS_OUTPUT_BASE,
		APOLLO_HEXAGON_DMA_STRESS_BYTES);
}

int apollo_hexagon_run_selftests(struct device *dev,
				 struct apollo_hexagon *test)
{
	const size_t size = APOLLO_HEXAGON_TEST_SIZE;
	dma_addr_t dma_addr;
	u32 *word;
	int ret;

	ret = apollo_hexagon_check_tbu_features(dev, test);
	if (ret)
		return ret;

	word = dmam_alloc_coherent(dev, size, &dma_addr, GFP_KERNEL);
	if (!word)
		return dev_err_probe(dev, -ENOMEM,
				     "failed to allocate coherent DMA buffer\n");

	word[0] = APOLLO_HEXAGON_TEST_PATTERN;
	dma_wmb();
	dma_rmb();
	if (word[0] != APOLLO_HEXAGON_TEST_PATTERN)
		return dev_err_probe(dev, -EIO,
				     "coherent DMA pattern mismatch\n");

	ret = apollo_hexagon_check_firmware_dma(dev);
	if (ret)
		return ret;

	dev_info(dev, "dma selftest ok dma=%pad size=%zu\n", &dma_addr, size);

	return 0;
}
