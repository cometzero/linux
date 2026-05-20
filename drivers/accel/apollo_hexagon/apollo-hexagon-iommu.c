// SPDX-License-Identifier: GPL-2.0-only
/*
 * Apollo Hexagon Linux DMA/IOMMU and QBox TBU mapping helpers.
 */

#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/iommu.h>
#include <linux/kernel.h>
#include <linux/sizes.h>

#include "apollo-hexagon.h"

void apollo_tbu_write64(void __iomem *base, u32 lo, u32 hi, u64 value)
{
	writel(lower_32_bits(value), base + lo);
	writel(upper_32_bits(value), base + hi);
}

int apollo_hexagon_tbu_ctrl(struct device *dev, struct apollo_hexagon *test,
			    u32 ctrl)
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

int apollo_hexagon_tbu_map(struct device *dev, struct apollo_hexagon *test,
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

int apollo_hexagon_tbu_unmap(struct device *dev, struct apollo_hexagon *test,
			     u64 iova, phys_addr_t pa, u64 size)
{
	apollo_tbu_write64(test->tbu_regs, APOLLO_TBU_REG_MAP_IOVA_LO,
			   APOLLO_TBU_REG_MAP_IOVA_HI, iova);
	apollo_tbu_write64(test->tbu_regs, APOLLO_TBU_REG_MAP_PA_LO,
			   APOLLO_TBU_REG_MAP_PA_HI, pa);
	apollo_tbu_write64(test->tbu_regs, APOLLO_TBU_REG_MAP_SIZE_LO,
			   APOLLO_TBU_REG_MAP_SIZE_HI, size);

	return apollo_hexagon_tbu_ctrl(dev, test, APOLLO_TBU_MAP_CTRL_REMOVE);
}

static void apollo_hexagon_release_linux_iommu(void *data)
{
	struct apollo_hexagon *test = data;

	if (!test->linux_iommu_mapped)
		return;

	dma_unmap_resource(test->dev, test->linux_iommu_iova,
			   test->linux_iommu_size, DMA_BIDIRECTIONAL,
			   DMA_ATTR_SKIP_CPU_SYNC);
	test->linux_iommu_mapped = false;
}

int apollo_hexagon_configure_linux_iommu(struct device *dev,
						struct apollo_hexagon *test)
{
	struct iommu_domain *domain;
	struct iommu_group *group;
	phys_addr_t translated;
	dma_addr_t mapped_iova;
	size_t map_size;
	u64 requested_end;
	int ret;

	group = iommu_group_get(dev);
	if (!group)
		return dev_err_probe(dev, -ENODEV,
				     "arm-smmu-v3 IOMMU group unavailable\n");

	domain = iommu_get_domain_for_dev(dev);
	if (!domain) {
		iommu_group_put(group);
		return dev_err_probe(dev, -ENODEV,
				     "arm-smmu-v3 IOMMU domain unavailable\n");
	}

	if (!iommu_is_dma_domain(domain)) {
		iommu_group_put(group);
		return dev_err_probe(dev, -EINVAL,
				     "arm-smmu-v3 domain is not a DMA domain type=0x%x\n",
				     domain->type);
	}

	iommu_group_put(group);

	if (!test->dma_window_size || test->dma_window_size > SIZE_MAX)
		return dev_err_probe(dev, -EINVAL,
				     "invalid DMA/IOMMU window size=0x%llx\n",
				     test->dma_window_size);
	if ((u64)test->requested_dma_iova_base > U32_MAX ||
	    test->dma_window_size - 1 >
	    U32_MAX - test->requested_dma_iova_base)
		return dev_err_probe(dev, -EOVERFLOW,
				     "requested DMA/IOMMU window exceeds 32-bit Hexagon job ABI iova=0x%llx size=0x%llx\n",
				     test->requested_dma_iova_base,
				     test->dma_window_size);

	map_size = (size_t)test->dma_window_size;
	requested_end = test->requested_dma_iova_base +
			test->dma_window_size - 1;
	dev->bus_dma_limit = requested_end;
	mapped_iova = dma_map_resource(dev, test->shared_phys, map_size,
				       DMA_BIDIRECTIONAL,
				       DMA_ATTR_SKIP_CPU_SYNC);
	if (dma_mapping_error(dev, mapped_iova))
		return dev_err_probe(dev, -EIO,
				     "arm-smmu-v3 dma_map_resource failed phys=%pa size=0x%zx\n",
				     &test->shared_phys, map_size);

	if ((u64)mapped_iova > U32_MAX ||
	    test->dma_window_size - 1 > U32_MAX - (u64)mapped_iova) {
		ret = dev_err_probe(dev, -EOVERFLOW,
				    "arm-smmu-v3 DMA IOVA exceeds 32-bit Hexagon job ABI iova=%pad size=0x%zx\n",
				    &mapped_iova, map_size);
		goto err_unmap;
	}
	if (mapped_iova != test->requested_dma_iova_base) {
		ret = dev_err_probe(dev, -ERANGE,
				    "arm-smmu-v3 DMA IOVA did not match Hexagon firmware ABI iova=%pad requested=0x%llx limit=0x%llx\n",
				    &mapped_iova,
				    test->requested_dma_iova_base,
				    requested_end);
		goto err_unmap;
	}

	translated = iommu_iova_to_phys(domain, mapped_iova);
	if (translated != test->shared_phys) {
		ret = dev_err_probe(dev, -EIO,
				    "arm-smmu-v3 IOVA translation mismatch iova=%pad phys=%pa expected=%pa\n",
				    &mapped_iova, &translated,
				    &test->shared_phys);
		goto err_unmap;
	}

	test->linux_iommu_iova = mapped_iova;
	test->linux_iommu_size = map_size;
	test->linux_iommu_mapped = true;
	test->dma_iova_base = mapped_iova;

	ret = devm_add_action_or_reset(dev, apollo_hexagon_release_linux_iommu,
				       test);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to register Linux IOMMU cleanup\n");

	dev_info(dev,
		 "arm-smmu-v3 dma-iommu map installed iova=%pad requested=0x%llx phys=%pa size=0x%zx domain-type=0x%x\n",
		 &test->linux_iommu_iova, test->requested_dma_iova_base,
		 &test->shared_phys, test->linux_iommu_size, domain->type);
	dev_info(dev, "iommu group attached domain-type=0x%x\n", domain->type);

	return 0;

err_unmap:
	dma_unmap_resource(dev, mapped_iova, map_size, DMA_BIDIRECTIONAL,
			   DMA_ATTR_SKIP_CPU_SYNC);
	return ret;
}

int apollo_hexagon_dynamic_map(struct device *dev, struct apollo_hexagon *test)
{
	u64 shared_size = min_t(u64, test->shared_size,
				APOLLO_HEXAGON_BIND_IOVA_OFFSET);
	int ret;

	ret = apollo_hexagon_tbu_map(dev, test, test->dma_iova_base,
				     test->shared_phys, shared_size);
	if (ret)
		return ret;
	ret = apollo_hexagon_tbu_ctrl(dev, test, APOLLO_TBU_MAP_CTRL_REMOVE);
	if (ret)
		return ret;
	ret = apollo_hexagon_tbu_map(dev, test, test->dma_iova_base,
				     test->shared_phys, shared_size);
	if (ret)
		return ret;

	dev_info(dev,
		 "dynamic SMMU map refreshed linux-iommu=yes iova=0x%llx pa=%pa size=0x%llx count=%u\n",
		 test->dma_iova_base, &test->shared_phys,
		 shared_size,
		 readl(test->tbu_regs + APOLLO_TBU_REG_MAP_COUNT));

	return 0;
}
