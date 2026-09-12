// SPDX-License-Identifier: GPL-2.0-only
/* QBox virtual PCIe endpoint controller */

#include <linux/bits.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci-epc.h>
#include <linux/pci.h>
#include <linux/platform_device.h>
#include <linux/sizes.h>

#define QBOX_EPC_ID			0x000
#define QBOX_EPC_ID_VALUE		0x51455043
#define QBOX_EPC_VERSION		0x004
#define QBOX_EPC_VERSION_1_0		0x00010000
#define QBOX_EPC_STATUS			0x008
#define  QBOX_EPC_STATUS_LINK_PRESENT	BIT(0)
#define  QBOX_EPC_STATUS_LINK_STARTED	BIT(1)
#define  QBOX_EPC_STATUS_ERROR		BIT(31)
#define QBOX_EPC_COMMAND		0x00c
#define  QBOX_EPC_COMMAND_START		BIT(0)
#define  QBOX_EPC_COMMAND_STOP		BIT(1)
#define  QBOX_EPC_COMMAND_CLEAR_ERROR	BIT(31)

#define QBOX_EPC_VENDOR_DEVICE		0x020
#define QBOX_EPC_REV_CLASS		0x024
#define QBOX_EPC_SUBSYS_VENDOR_DEVICE	0x028

#define QBOX_EPC_BAR0_PHYS_LO		0x100
#define QBOX_EPC_BAR0_PHYS_HI		0x104
#define QBOX_EPC_BAR0_SIZE		0x108
#define QBOX_EPC_BAR0_CTRL		0x10c
#define  QBOX_EPC_BAR_ENABLE		BIT(0)

#define QBOX_EPC_MSI_REQUEST_COUNT	0x200
#define QBOX_EPC_MSI_ENABLED_COUNT	0x204
#define QBOX_EPC_MSI_RAISE		0x208

#define QBOX_EPC_OB_BASE(n)		(0x300 + (n) * 0x20)
#define QBOX_EPC_OB_LOCAL_OFFSET(n)	(QBOX_EPC_OB_BASE(n) + 0x00)
#define QBOX_EPC_OB_PCI_ADDR_LO(n)	(QBOX_EPC_OB_BASE(n) + 0x04)
#define QBOX_EPC_OB_PCI_ADDR_HI(n)	(QBOX_EPC_OB_BASE(n) + 0x08)
#define QBOX_EPC_OB_SIZE(n)		(QBOX_EPC_OB_BASE(n) + 0x0c)
#define QBOX_EPC_OB_CTRL(n)		(QBOX_EPC_OB_BASE(n) + 0x10)
#define  QBOX_EPC_OB_ENABLE		BIT(0)
#define QBOX_EPC_OB_COUNT		2

#define QBOX_EPC_BAR0_SIZE_VALUE	SZ_64K
#define QBOX_EPC_OB_PAGE_SIZE		SZ_4K

struct qbox_pcie_ep {
	void __iomem *base;
	struct pci_epc *epc;
	resource_size_t mem_start;
	resource_size_t mem_size;
	phys_addr_t ob_phys[QBOX_EPC_OB_COUNT];
	bool ob_used[QBOX_EPC_OB_COUNT];
};

static const struct pci_epc_features qbox_pcie_epc_features = {
	.msi_capable = true,
	.bar[BAR_0] = {
		.type = BAR_FIXED,
		.fixed_size = QBOX_EPC_BAR0_SIZE_VALUE,
		.only_64bit = true,
	},
	.bar[BAR_1] = { .type = BAR_RESERVED },
	.bar[BAR_2] = { .type = BAR_RESERVED },
	.bar[BAR_3] = { .type = BAR_RESERVED },
	.bar[BAR_4] = { .type = BAR_RESERVED },
	.bar[BAR_5] = { .type = BAR_RESERVED },
	.align = QBOX_EPC_OB_PAGE_SIZE,
};

static int qbox_pcie_ep_check_function(u8 func_no, u8 vfunc_no)
{
	return (func_no || vfunc_no) ? -EINVAL : 0;
}

static void qbox_pcie_ep_clear_error(struct qbox_pcie_ep *ep)
{
	writel(QBOX_EPC_COMMAND_CLEAR_ERROR, ep->base + QBOX_EPC_COMMAND);
}

static int qbox_pcie_ep_check_error(struct qbox_pcie_ep *ep)
{
	return readl(ep->base + QBOX_EPC_STATUS) & QBOX_EPC_STATUS_ERROR ?
		-EIO : 0;
}

static int qbox_pcie_ep_write_header(struct pci_epc *epc, u8 func_no,
				     u8 vfunc_no,
				     struct pci_epf_header *hdr)
{
	struct qbox_pcie_ep *ep = epc_get_drvdata(epc);
	u32 val;
	int ret;

	ret = qbox_pcie_ep_check_function(func_no, vfunc_no);
	if (ret)
		return ret;

	qbox_pcie_ep_clear_error(ep);
	val = hdr->vendorid | ((u32)hdr->deviceid << 16);
	writel(val, ep->base + QBOX_EPC_VENDOR_DEVICE);
	val = hdr->revid | ((u32)hdr->progif_code << 8) |
	      ((u32)hdr->subclass_code << 16) |
	      ((u32)hdr->baseclass_code << 24);
	writel(val, ep->base + QBOX_EPC_REV_CLASS);
	val = hdr->subsys_vendor_id | ((u32)hdr->subsys_id << 16);
	writel(val, ep->base + QBOX_EPC_SUBSYS_VENDOR_DEVICE);

	return qbox_pcie_ep_check_error(ep);
}

static int qbox_pcie_ep_set_bar(struct pci_epc *epc, u8 func_no,
				u8 vfunc_no, struct pci_epf_bar *bar)
{
	struct qbox_pcie_ep *ep = epc_get_drvdata(epc);
	int ret;

	ret = qbox_pcie_ep_check_function(func_no, vfunc_no);
	if (ret)
		return ret;
	if (bar->barno != BAR_0 || bar->size != QBOX_EPC_BAR0_SIZE_VALUE)
		return -EINVAL;

	qbox_pcie_ep_clear_error(ep);
	writel(lower_32_bits(bar->phys_addr), ep->base + QBOX_EPC_BAR0_PHYS_LO);
	writel(upper_32_bits(bar->phys_addr), ep->base + QBOX_EPC_BAR0_PHYS_HI);
	writel(bar->size, ep->base + QBOX_EPC_BAR0_SIZE);
	writel(QBOX_EPC_BAR_ENABLE, ep->base + QBOX_EPC_BAR0_CTRL);

	return qbox_pcie_ep_check_error(ep);
}

static void qbox_pcie_ep_clear_bar(struct pci_epc *epc, u8 func_no,
				   u8 vfunc_no, struct pci_epf_bar *bar)
{
	struct qbox_pcie_ep *ep = epc_get_drvdata(epc);

	if (qbox_pcie_ep_check_function(func_no, vfunc_no) ||
	    bar->barno != BAR_0)
		return;

	writel(0, ep->base + QBOX_EPC_BAR0_CTRL);
}

static int qbox_pcie_ep_map_addr(struct pci_epc *epc, u8 func_no,
				 u8 vfunc_no, phys_addr_t phys_addr,
				 u64 pci_addr, size_t size)
{
	struct qbox_pcie_ep *ep = epc_get_drvdata(epc);
	resource_size_t offset;
	unsigned int i;
	int ret;

	ret = qbox_pcie_ep_check_function(func_no, vfunc_no);
	if (ret)
		return ret;
	if (!size || size > U32_MAX || phys_addr < ep->mem_start)
		return -EINVAL;

	offset = phys_addr - ep->mem_start;
	if (offset >= ep->mem_size || size > ep->mem_size - offset)
		return -EINVAL;

	for (i = 0; i < QBOX_EPC_OB_COUNT; i++)
		if (!ep->ob_used[i])
			break;
	if (i == QBOX_EPC_OB_COUNT)
		return -ENOSPC;

	qbox_pcie_ep_clear_error(ep);
	writel(offset, ep->base + QBOX_EPC_OB_LOCAL_OFFSET(i));
	writel(lower_32_bits(pci_addr), ep->base + QBOX_EPC_OB_PCI_ADDR_LO(i));
	writel(upper_32_bits(pci_addr), ep->base + QBOX_EPC_OB_PCI_ADDR_HI(i));
	writel(size, ep->base + QBOX_EPC_OB_SIZE(i));
	writel(QBOX_EPC_OB_ENABLE, ep->base + QBOX_EPC_OB_CTRL(i));
	ret = qbox_pcie_ep_check_error(ep);
	if (ret) {
		writel(0, ep->base + QBOX_EPC_OB_CTRL(i));
		return ret;
	}

	ep->ob_phys[i] = phys_addr;
	ep->ob_used[i] = true;
	return 0;
}

static void qbox_pcie_ep_unmap_addr(struct pci_epc *epc, u8 func_no,
				    u8 vfunc_no, phys_addr_t phys_addr)
{
	struct qbox_pcie_ep *ep = epc_get_drvdata(epc);
	unsigned int i;

	if (qbox_pcie_ep_check_function(func_no, vfunc_no))
		return;

	for (i = 0; i < QBOX_EPC_OB_COUNT; i++) {
		if (!ep->ob_used[i] || ep->ob_phys[i] != phys_addr)
			continue;

		writel(0, ep->base + QBOX_EPC_OB_CTRL(i));
		ep->ob_used[i] = false;
		return;
	}
}

static int qbox_pcie_ep_set_msi(struct pci_epc *epc, u8 func_no,
				u8 vfunc_no, u8 nr_irqs)
{
	struct qbox_pcie_ep *ep = epc_get_drvdata(epc);
	int ret;

	ret = qbox_pcie_ep_check_function(func_no, vfunc_no);
	if (ret)
		return ret;
	if (!is_power_of_2(nr_irqs) || nr_irqs > 32)
		return -EINVAL;

	qbox_pcie_ep_clear_error(ep);
	writel(nr_irqs, ep->base + QBOX_EPC_MSI_REQUEST_COUNT);
	return qbox_pcie_ep_check_error(ep);
}

static int qbox_pcie_ep_get_msi(struct pci_epc *epc, u8 func_no,
				u8 vfunc_no)
{
	struct qbox_pcie_ep *ep = epc_get_drvdata(epc);

	if (qbox_pcie_ep_check_function(func_no, vfunc_no))
		return -EINVAL;

	return readl(ep->base + QBOX_EPC_MSI_ENABLED_COUNT);
}

static int qbox_pcie_ep_raise_irq(struct pci_epc *epc, u8 func_no,
				  u8 vfunc_no, unsigned int type,
				  u16 interrupt_num)
{
	struct qbox_pcie_ep *ep = epc_get_drvdata(epc);
	u32 count;
	int ret;

	ret = qbox_pcie_ep_check_function(func_no, vfunc_no);
	if (ret)
		return ret;
	if (type != PCI_IRQ_MSI)
		return -EOPNOTSUPP;

	count = readl(ep->base + QBOX_EPC_MSI_ENABLED_COUNT);
	if (!interrupt_num || interrupt_num > count)
		return -EINVAL;

	qbox_pcie_ep_clear_error(ep);
	writel(interrupt_num, ep->base + QBOX_EPC_MSI_RAISE);
	return qbox_pcie_ep_check_error(ep);
}

static int qbox_pcie_ep_start(struct pci_epc *epc)
{
	struct qbox_pcie_ep *ep = epc_get_drvdata(epc);
	u32 status;

	status = readl(ep->base + QBOX_EPC_STATUS);
	if (!(status & QBOX_EPC_STATUS_LINK_PRESENT))
		return -ENODEV;

	qbox_pcie_ep_clear_error(ep);
	writel(QBOX_EPC_COMMAND_START, ep->base + QBOX_EPC_COMMAND);
	status = readl(ep->base + QBOX_EPC_STATUS);
	if (status & QBOX_EPC_STATUS_ERROR)
		return -EIO;

	return status & QBOX_EPC_STATUS_LINK_STARTED ? 0 : -EIO;
}

static void qbox_pcie_ep_stop(struct pci_epc *epc)
{
	struct qbox_pcie_ep *ep = epc_get_drvdata(epc);

	writel(QBOX_EPC_COMMAND_STOP, ep->base + QBOX_EPC_COMMAND);
}

static const struct pci_epc_features *
qbox_pcie_ep_get_features(struct pci_epc *epc, u8 func_no, u8 vfunc_no)
{
	if (qbox_pcie_ep_check_function(func_no, vfunc_no))
		return NULL;

	return &qbox_pcie_epc_features;
}

static const struct pci_epc_ops qbox_pcie_epc_ops = {
	.write_header = qbox_pcie_ep_write_header,
	.set_bar = qbox_pcie_ep_set_bar,
	.clear_bar = qbox_pcie_ep_clear_bar,
	.map_addr = qbox_pcie_ep_map_addr,
	.unmap_addr = qbox_pcie_ep_unmap_addr,
	.set_msi = qbox_pcie_ep_set_msi,
	.get_msi = qbox_pcie_ep_get_msi,
	.raise_irq = qbox_pcie_ep_raise_irq,
	.start = qbox_pcie_ep_start,
	.stop = qbox_pcie_ep_stop,
	.get_features = qbox_pcie_ep_get_features,
	.owner = THIS_MODULE,
};

static int qbox_pcie_ep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct qbox_pcie_ep *ep;
	struct resource *mem;
	u32 status;
	int ret;

	ep = devm_kzalloc(dev, sizeof(*ep), GFP_KERNEL);
	if (!ep)
		return -ENOMEM;

	ep->base = devm_platform_ioremap_resource_byname(pdev, "ctrl");
	if (IS_ERR(ep->base))
		return PTR_ERR(ep->base);

	if (readl(ep->base + QBOX_EPC_ID) != QBOX_EPC_ID_VALUE ||
	    readl(ep->base + QBOX_EPC_VERSION) != QBOX_EPC_VERSION_1_0)
		return dev_err_probe(dev, -ENODEV,
				     "unsupported virtual EPC\n");

	status = readl(ep->base + QBOX_EPC_STATUS);
	if (!(status & QBOX_EPC_STATUS_LINK_PRESENT)) {
		dev_dbg(dev, "endpoint loopback is disabled\n");
		return -ENODEV;
	}

	mem = platform_get_resource_byname(pdev, IORESOURCE_MEM, "mem");
	if (!mem)
		return dev_err_probe(dev, -EINVAL,
				     "missing outbound memory aperture\n");
	ep->mem_start = mem->start;
	ep->mem_size = resource_size(mem);

	ep->epc = devm_pci_epc_create(dev, &qbox_pcie_epc_ops);
	if (IS_ERR(ep->epc))
		return dev_err_probe(dev, PTR_ERR(ep->epc),
				     "failed to create EPC\n");

	ep->epc->max_functions = 1;
	epc_set_drvdata(ep->epc, ep);
	platform_set_drvdata(pdev, ep);

	ret = pci_epc_mem_init(ep->epc, ep->mem_start, ep->mem_size,
			       QBOX_EPC_OB_PAGE_SIZE);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to initialize outbound memory\n");

	pci_epc_init_notify(ep->epc);
	dev_info(dev, "QBox PCIe endpoint controller ready\n");
	return 0;
}

static void qbox_pcie_ep_remove(struct platform_device *pdev)
{
	struct qbox_pcie_ep *ep = platform_get_drvdata(pdev);

	pci_epc_deinit_notify(ep->epc);
	qbox_pcie_ep_stop(ep->epc);
	pci_epc_mem_exit(ep->epc);
}

static const struct of_device_id qbox_pcie_ep_of_match[] = {
	{ .compatible = "qbox,pcie-epc" },
	{ }
};
MODULE_DEVICE_TABLE(of, qbox_pcie_ep_of_match);

static struct platform_driver qbox_pcie_ep_driver = {
	.probe = qbox_pcie_ep_probe,
	.remove = qbox_pcie_ep_remove,
	.driver = {
		.name = "qbox-pcie-epc",
		.of_match_table = qbox_pcie_ep_of_match,
	},
};
module_platform_driver(qbox_pcie_ep_driver);

MODULE_DESCRIPTION("QBox virtual PCIe endpoint controller");
MODULE_LICENSE("GPL");
