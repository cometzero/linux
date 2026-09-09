// SPDX-License-Identifier: GPL-2.0-only
/* HSOC GPIO, pin controller and interrupt driver */

#include <linux/gpio/driver.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pinctrl/pinconf-generic.h>
#include <linux/pinctrl/pinconf.h>
#include <linux/pinctrl/pinctrl.h>
#include <linux/pinctrl/pinmux.h>
#include <linux/spinlock.h>

#include "pinconf.h"
#include "pinctrl-utils.h"

#define HSOC_BANK_BASE(n)	((n) * 0x1000)
#define HSOC_PROT		0x000
#define HSOC_SEL		0x100
#define HSOC_DAT		0x104
#define HSOC_PS			0x108
#define HSOC_PE			0x10c
#define HSOC_DS			0x110
#define HSOC_IS			0x114
#define HSOC_IE			0x118
#define HSOC_INTR_CON		0x200
#define HSOC_INTR_PEND		0x204
#define HSOC_INTR_MIRR_PEND	0x208
#define HSOC_INTR_MASK		0x20c
#define HSOC_INTR_FLT_TYP	0x210
#define HSOC_INTR_FLT_DEPTH	0x214

#define HSOC_MUX_WIDTH		4
#define HSOC_DRIVE_WIDTH	2
#define HSOC_IRQ_WIDTH		4
#define HSOC_FILTER_DEPTH_WIDTH	4

#define HSOC_MAX_BANKS		14
#define HSOC_NUM_FUNCTIONS	16

#define PIN_CONFIG_HSOC_DRIVE_STRENGTH	(PIN_CONFIG_END + 1)
#define PIN_CONFIG_HSOC_FILTER_TYPE	(PIN_CONFIG_END + 2)
#define PIN_CONFIG_HSOC_FILTER_DEPTH	(PIN_CONFIG_END + 3)

static const struct pinconf_generic_params hsoc_custom_params[] = {
	{ "hsoc,drive-strength", PIN_CONFIG_HSOC_DRIVE_STRENGTH, 0 },
	{ "hsoc,interrupt-filter-type", PIN_CONFIG_HSOC_FILTER_TYPE, 0 },
	{ "hsoc,interrupt-filter-depth", PIN_CONFIG_HSOC_FILTER_DEPTH, 0 },
};

struct hsoc_pinctrl;

struct hsoc_gpio_bank {
	struct hsoc_pinctrl *pctl;
	struct gpio_chip gc;
	void __iomem *base;
	unsigned int index;
	unsigned int pin_base;
	unsigned int npins;
	int parent_irq;
};

struct hsoc_pinctrl {
	struct device *dev;
	void __iomem *base;
	struct pinctrl_desc desc;
	struct pinctrl_dev *pctldev;
	struct pinctrl_pin_desc *pins;
	const char **groups;
	unsigned int *group_pins;
	struct irq_chip irq_chip;
	struct hsoc_gpio_bank banks[HSOC_MAX_BANKS];
	const char *instance;
	unsigned int nbanks;
	unsigned int npins;
	raw_spinlock_t lock;
};

static const char * const hsoc_functions[] = {
	"gpio-input", "gpio-output", "function-2", "function-3",
	"function-4", "function-5", "function-6", "function-7",
	"function-8", "function-9", "function-10", "function-11",
	"function-12", "function-13", "function-14", "function-15",
};

static int hsoc_pin_to_group(struct hsoc_pinctrl *pctl, unsigned int pin)
{
	return pin < pctl->desc.npins ? pin : -EINVAL;
}

static int hsoc_hw_pin_to_pin(struct hsoc_pinctrl *pctl, unsigned int hw_pin)
{
	unsigned int bank = hw_pin / 8;
	unsigned int offset = hw_pin % 8;

	if (bank >= pctl->nbanks || offset >= pctl->banks[bank].npins)
		return -EINVAL;

	return pctl->banks[bank].pin_base + offset;
}

static int hsoc_pin_to_bank(struct hsoc_pinctrl *pctl, unsigned int pin,
			    struct hsoc_gpio_bank **bank,
			    unsigned int *offset)
{
	unsigned int i;

	if (pin >= pctl->npins)
		return -EINVAL;

	for (i = 0; i < pctl->nbanks; i++) {
		if (pin < pctl->banks[i].pin_base + pctl->banks[i].npins) {
			*bank = &pctl->banks[i];
			*offset = pin - pctl->banks[i].pin_base;
			return 0;
		}
	}

	return -EINVAL;
}

static void hsoc_update_bits(struct hsoc_pinctrl *pctl, void __iomem *reg,
			     u32 mask, u32 value)
{
	unsigned long flags;
	u32 val;

	raw_spin_lock_irqsave(&pctl->lock, flags);
	val = readl(reg);
	val = (val & ~mask) | (value & mask);
	writel(val, reg);
	raw_spin_unlock_irqrestore(&pctl->lock, flags);
}

static int hsoc_set_pin_mux(struct hsoc_pinctrl *pctl, unsigned int pin,
			    unsigned int mux)
{
	struct hsoc_gpio_bank *bank;
	unsigned int offset;
	u32 shift, mask;
	int ret;

	ret = hsoc_pin_to_bank(pctl, pin, &bank, &offset);
	if (ret || mux >= HSOC_NUM_FUNCTIONS)
		return -EINVAL;

	shift = offset * HSOC_MUX_WIDTH;
	mask = GENMASK(shift + HSOC_MUX_WIDTH - 1, shift);
	hsoc_update_bits(pctl, bank->base + HSOC_SEL, mask, mux << shift);

	return 0;
}

static int hsoc_set_gpio_direction(struct hsoc_pinctrl *pctl,
				   unsigned int pin, bool input)
{
	struct hsoc_gpio_bank *bank;
	unsigned int offset;
	int ret;

	ret = hsoc_pin_to_bank(pctl, pin, &bank, &offset);
	if (ret)
		return ret;
	if (input)
		hsoc_update_bits(pctl, bank->base + HSOC_IE, BIT(offset),
				 BIT(offset));
	return hsoc_set_pin_mux(pctl, pin, input ? 0 : 1);
}

static int hsoc_get_groups_count(struct pinctrl_dev *pctldev)
{
	struct hsoc_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

	return pctl->desc.npins;
}

static const char *hsoc_get_group_name(struct pinctrl_dev *pctldev,
				       unsigned int selector)
{
	struct hsoc_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

	return selector < pctl->desc.npins ? pctl->groups[selector] : NULL;
}

static int hsoc_get_group_pins(struct pinctrl_dev *pctldev,
			       unsigned int selector,
			       const unsigned int **pins,
			       unsigned int *num_pins)
{
	struct hsoc_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

	if (selector >= pctl->desc.npins)
		return -EINVAL;

	*pins = &pctl->group_pins[selector];
	*num_pins = 1;
	return 0;
}

static int hsoc_dt_node_to_map(struct pinctrl_dev *pctldev,
			       struct device_node *np,
			       struct pinctrl_map **map,
			       unsigned int *num_maps)
{
	struct hsoc_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	unsigned long *configs = NULL;
	unsigned int nconfigs = 0, reserved = 0;
	int count, ret, i;

	count = of_property_count_u32_elems(np, "pinmux");
	if (count <= 0)
		return count ? count : -EINVAL;

	ret = pinconf_generic_parse_dt_config(np, pctldev, &configs, &nconfigs);
	if (ret)
		return ret;

	*map = NULL;
	*num_maps = 0;
	ret = pinctrl_utils_reserve_map(pctldev, map, &reserved, num_maps,
					count * (nconfigs ? 2 : 1));
	if (ret)
		goto out;

	for (i = 0; i < count; i++) {
		u32 value, function;
		int pin;
		int group;

		ret = of_property_read_u32_index(np, "pinmux", i, &value);
		if (ret)
			goto free_map;

		pin = hsoc_hw_pin_to_pin(pctl, value >> 8);
		function = value & 0xff;
		if (pin < 0) {
			ret = pin;
			goto free_map;
		}
		group = hsoc_pin_to_group(pctl, pin);
		if (group < 0 || function >= HSOC_NUM_FUNCTIONS) {
			ret = -EINVAL;
			goto free_map;
		}

		ret = pinctrl_utils_add_map_mux(pctldev, map, &reserved,
						num_maps, pctl->groups[group],
						hsoc_functions[function]);
		if (ret)
			goto free_map;

		if (nconfigs) {
			ret = pinctrl_utils_add_map_configs(pctldev, map,
							    &reserved, num_maps,
							    pctl->groups[group], configs,
							    nconfigs,
							    PIN_MAP_TYPE_CONFIGS_GROUP);
			if (ret)
				goto free_map;
		}
	}

	ret = 0;
	goto out;

free_map:
	pinctrl_utils_free_map(pctldev, *map, *num_maps);
	*map = NULL;
	*num_maps = 0;
out:
	kfree(configs);
	return ret;
}

static const struct pinctrl_ops hsoc_pinctrl_ops = {
	.get_groups_count = hsoc_get_groups_count,
	.get_group_name = hsoc_get_group_name,
	.get_group_pins = hsoc_get_group_pins,
	.dt_node_to_map = hsoc_dt_node_to_map,
	.dt_free_map = pinctrl_utils_free_map,
};

static int hsoc_get_functions_count(struct pinctrl_dev *pctldev)
{
	return ARRAY_SIZE(hsoc_functions);
}

static const char *hsoc_get_function_name(struct pinctrl_dev *pctldev,
					  unsigned int selector)
{
	return selector < ARRAY_SIZE(hsoc_functions) ? hsoc_functions[selector] : NULL;
}

static int hsoc_get_function_groups(struct pinctrl_dev *pctldev,
				    unsigned int selector,
				    const char * const **groups,
				    unsigned int *num_groups)
{
	struct hsoc_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

	if (selector >= ARRAY_SIZE(hsoc_functions))
		return -EINVAL;

	*groups = pctl->groups;
	*num_groups = pctl->desc.npins;
	return 0;
}

static int hsoc_set_mux(struct pinctrl_dev *pctldev, unsigned int function,
			unsigned int group)
{
	struct hsoc_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

	if (group >= pctl->desc.npins)
		return -EINVAL;

	return hsoc_set_pin_mux(pctl, pctl->group_pins[group], function);
}

static int hsoc_gpio_request_enable(struct pinctrl_dev *pctldev,
				    struct pinctrl_gpio_range *range,
				    unsigned int pin)
{
	struct hsoc_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

	return hsoc_set_gpio_direction(pctl, pin, true);
}

static int hsoc_gpio_set_direction(struct pinctrl_dev *pctldev,
				   struct pinctrl_gpio_range *range,
				   unsigned int pin, bool input)
{
	struct hsoc_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

	return hsoc_set_gpio_direction(pctl, pin, input);
}

static const struct pinmux_ops hsoc_pinmux_ops = {
	.get_functions_count = hsoc_get_functions_count,
	.get_function_name = hsoc_get_function_name,
	.get_function_groups = hsoc_get_function_groups,
	.set_mux = hsoc_set_mux,
	.gpio_request_enable = hsoc_gpio_request_enable,
	.gpio_set_direction = hsoc_gpio_set_direction,
	.strict = true,
};

static int hsoc_pin_config_get(struct pinctrl_dev *pctldev, unsigned int pin,
			       unsigned long *config)
{
	struct hsoc_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	struct hsoc_gpio_bank *bank;
	unsigned int offset;
	unsigned int param = pinconf_to_config_param(*config);
	u32 value, arg = 1, shift;

	if (hsoc_pin_to_bank(pctl, pin, &bank, &offset))
		return -EINVAL;

	switch (param) {
	case PIN_CONFIG_BIAS_DISABLE:
		if (!(readl(bank->base + HSOC_PE) & BIT(offset)))
			return -EINVAL;
		break;
	case PIN_CONFIG_BIAS_PULL_DOWN:
		if ((readl(bank->base + HSOC_PE) & BIT(offset)) ||
		    (readl(bank->base + HSOC_PS) & BIT(offset)))
			return -EINVAL;
		break;
	case PIN_CONFIG_BIAS_PULL_UP:
		if ((readl(bank->base + HSOC_PE) & BIT(offset)) ||
		    !(readl(bank->base + HSOC_PS) & BIT(offset)))
			return -EINVAL;
		break;
	case PIN_CONFIG_DRIVE_PUSH_PULL:
		break;
	case PIN_CONFIG_INPUT_ENABLE:
		arg = !!(readl(bank->base + HSOC_IE) & BIT(offset));
		break;
	case PIN_CONFIG_INPUT_SCHMITT:
	case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
		arg = !!(readl(bank->base + HSOC_IS) & BIT(offset));
		break;
	case PIN_CONFIG_HSOC_DRIVE_STRENGTH:
		shift = offset * HSOC_DRIVE_WIDTH;
		value = readl(bank->base + HSOC_DS);
		arg = (value >> shift) & GENMASK(HSOC_DRIVE_WIDTH - 1, 0);
		break;
	case PIN_CONFIG_HSOC_FILTER_TYPE:
		arg = !!(readl(bank->base + HSOC_INTR_FLT_TYP) & BIT(offset));
		break;
	case PIN_CONFIG_HSOC_FILTER_DEPTH:
		shift = offset * HSOC_FILTER_DEPTH_WIDTH;
		value = readl(bank->base + HSOC_INTR_FLT_DEPTH);
		arg = (value >> shift) &
		      GENMASK(HSOC_FILTER_DEPTH_WIDTH - 1, 0);
		break;
	default:
		return -ENOTSUPP;
	}

	*config = pinconf_to_config_packed(param, arg);
	return 0;
}

static int hsoc_pin_config_set(struct pinctrl_dev *pctldev, unsigned int pin,
			       unsigned long *configs,
			       unsigned int num_configs)
{
	struct hsoc_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);
	struct hsoc_gpio_bank *bank;
	unsigned int offset;
	unsigned int i;

	if (hsoc_pin_to_bank(pctl, pin, &bank, &offset))
		return -EINVAL;

	for (i = 0; i < num_configs; i++) {
		unsigned int param = pinconf_to_config_param(configs[i]);
		u32 arg = pinconf_to_config_argument(configs[i]);
		u32 shift, mask;

		switch (param) {
		case PIN_CONFIG_DRIVE_PUSH_PULL:
			break;
		case PIN_CONFIG_BIAS_DISABLE:
			hsoc_update_bits(pctl, bank->base + HSOC_PE,
					 BIT(offset), BIT(offset));
			break;
		case PIN_CONFIG_BIAS_PULL_DOWN:
		case PIN_CONFIG_BIAS_PULL_UP:
			if (!arg)
				return -EINVAL;
			hsoc_update_bits(pctl, bank->base + HSOC_PS,
					 BIT(offset),
					 param == PIN_CONFIG_BIAS_PULL_UP ?
					 BIT(offset) : 0);
			hsoc_update_bits(pctl, bank->base + HSOC_PE,
					 BIT(offset), 0);
			break;
		case PIN_CONFIG_INPUT_ENABLE:
			if (arg > 1)
				return -EINVAL;
			hsoc_update_bits(pctl, bank->base + HSOC_IE,
					 BIT(offset), arg ? BIT(offset) : 0);
			break;
		case PIN_CONFIG_INPUT_SCHMITT:
		case PIN_CONFIG_INPUT_SCHMITT_ENABLE:
			if (arg > 1)
				return -EINVAL;
			hsoc_update_bits(pctl, bank->base + HSOC_IS,
					 BIT(offset), arg ? BIT(offset) : 0);
			break;
		case PIN_CONFIG_HSOC_DRIVE_STRENGTH:
			if (arg > 3)
				return -EINVAL;
			shift = offset * HSOC_DRIVE_WIDTH;
			mask = GENMASK(shift + HSOC_DRIVE_WIDTH - 1, shift);
			hsoc_update_bits(pctl, bank->base + HSOC_DS, mask,
					 arg << shift);
			break;
		case PIN_CONFIG_HSOC_FILTER_TYPE:
			if (arg > 1)
				return -EINVAL;
			hsoc_update_bits(pctl, bank->base + HSOC_INTR_FLT_TYP,
					 BIT(offset), arg ? BIT(offset) : 0);
			break;
		case PIN_CONFIG_HSOC_FILTER_DEPTH:
			if (arg > 15)
				return -EINVAL;
			shift = offset * HSOC_FILTER_DEPTH_WIDTH;
			mask = GENMASK(shift + HSOC_FILTER_DEPTH_WIDTH - 1,
				       shift);
			hsoc_update_bits(pctl, bank->base + HSOC_INTR_FLT_DEPTH,
					 mask, arg << shift);
			break;
		default:
			return -ENOTSUPP;
		}
	}
	return 0;
}

static int hsoc_group_config_get(struct pinctrl_dev *pctldev,
				 unsigned int group, unsigned long *config)
{
	struct hsoc_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

	if (group >= pctl->desc.npins)
		return -EINVAL;
	return hsoc_pin_config_get(pctldev, pctl->group_pins[group], config);
}

static int hsoc_group_config_set(struct pinctrl_dev *pctldev,
				 unsigned int group, unsigned long *configs,
				 unsigned int num_configs)
{
	struct hsoc_pinctrl *pctl = pinctrl_dev_get_drvdata(pctldev);

	if (group >= pctl->desc.npins)
		return -EINVAL;
	return hsoc_pin_config_set(pctldev, pctl->group_pins[group], configs,
				   num_configs);
}

static const struct pinconf_ops hsoc_pinconf_ops = {
	.is_generic = true,
	.pin_config_get = hsoc_pin_config_get,
	.pin_config_set = hsoc_pin_config_set,
	.pin_config_group_get = hsoc_group_config_get,
	.pin_config_group_set = hsoc_group_config_set,
};

static int hsoc_gpio_get_direction(struct gpio_chip *gc, unsigned int offset)
{
	struct hsoc_gpio_bank *bank = gpiochip_get_data(gc);
	u32 mux = (readl(bank->base + HSOC_SEL) >>
		   (offset * HSOC_MUX_WIDTH)) &
		  GENMASK(HSOC_MUX_WIDTH - 1, 0);

	if (mux == 0)
		return GPIO_LINE_DIRECTION_IN;
	if (mux == 1)
		return GPIO_LINE_DIRECTION_OUT;
	return -EINVAL;
}

static int hsoc_gpio_direction_input(struct gpio_chip *gc, unsigned int offset)
{
	struct hsoc_gpio_bank *bank = gpiochip_get_data(gc);

	return hsoc_set_gpio_direction(bank->pctl, bank->pin_base + offset, true);
}

static int hsoc_gpio_set(struct gpio_chip *gc, unsigned int offset, int value)
{
	struct hsoc_gpio_bank *bank = gpiochip_get_data(gc);

	hsoc_update_bits(bank->pctl, bank->base + HSOC_DAT, BIT(offset),
			 value ? BIT(offset) : 0);
	return 0;
}

static int hsoc_gpio_direction_output(struct gpio_chip *gc, unsigned int offset,
				      int value)
{
	struct hsoc_gpio_bank *bank = gpiochip_get_data(gc);

	hsoc_gpio_set(gc, offset, value);
	return hsoc_set_gpio_direction(bank->pctl, bank->pin_base + offset, false);
}

static int hsoc_gpio_get(struct gpio_chip *gc, unsigned int offset)
{
	struct hsoc_gpio_bank *bank = gpiochip_get_data(gc);

	return !!(readl(bank->base + HSOC_DAT) & BIT(offset));
}

static void hsoc_irq_ack(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct hsoc_gpio_bank *bank = gpiochip_get_data(gc);

	writel(BIT(irqd_to_hwirq(d)), bank->base + HSOC_INTR_PEND);
}

static void hsoc_irq_mask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct hsoc_gpio_bank *bank = gpiochip_get_data(gc);
	u32 bit = BIT(irqd_to_hwirq(d));

	hsoc_update_bits(bank->pctl, bank->base + HSOC_INTR_MASK, bit, bit);
	gpiochip_disable_irq(gc, irqd_to_hwirq(d));
}

static void hsoc_irq_unmask(struct irq_data *d)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct hsoc_gpio_bank *bank = gpiochip_get_data(gc);
	u32 bit = BIT(irqd_to_hwirq(d));

	gpiochip_enable_irq(gc, irqd_to_hwirq(d));
	hsoc_update_bits(bank->pctl, bank->base + HSOC_INTR_MASK, bit, 0);
}

static int hsoc_irq_set_type(struct irq_data *d, unsigned int type)
{
	struct gpio_chip *gc = irq_data_get_irq_chip_data(d);
	struct hsoc_gpio_bank *bank = gpiochip_get_data(gc);
	u32 hwirq = irqd_to_hwirq(d);
	u32 shift = hwirq * HSOC_IRQ_WIDTH;
	u32 mask = GENMASK(shift + HSOC_IRQ_WIDTH - 1, shift);
	u32 config;

	switch (type & IRQ_TYPE_SENSE_MASK) {
	case IRQ_TYPE_LEVEL_HIGH:
		config = 0;
		break;
	case IRQ_TYPE_LEVEL_LOW:
		config = 1;
		break;
	case IRQ_TYPE_EDGE_FALLING:
		config = 2;
		break;
	case IRQ_TYPE_EDGE_RISING:
		config = 3;
		break;
	case IRQ_TYPE_EDGE_BOTH:
		config = 4;
		break;
	default:
		return -EINVAL;
	}

	hsoc_update_bits(bank->pctl, bank->base + HSOC_INTR_CON, mask,
			 config << shift);
	writel(BIT(hwirq), bank->base + HSOC_INTR_PEND);

	if (type & IRQ_TYPE_LEVEL_MASK)
		irq_set_handler_locked(d, handle_level_irq);
	else
		irq_set_handler_locked(d, handle_edge_irq);

	return 0;
}

static const struct irq_chip hsoc_irq_chip_template = {
	.irq_ack = hsoc_irq_ack,
	.irq_mask = hsoc_irq_mask,
	.irq_unmask = hsoc_irq_unmask,
	.irq_set_type = hsoc_irq_set_type,
	.flags = IRQCHIP_IMMUTABLE,
	GPIOCHIP_IRQ_RESOURCE_HELPERS,
};

static void hsoc_irq_handler(struct irq_desc *desc)
{
	struct hsoc_gpio_bank *bank = irq_desc_get_handler_data(desc);
	struct irq_chip *chip = irq_desc_get_chip(desc);
	u32 pending;
	unsigned int bit;

	chained_irq_enter(chip, desc);
	pending = readl(bank->base + HSOC_INTR_MIRR_PEND) &
		  ~readl(bank->base + HSOC_INTR_MASK) &
		  GENMASK(bank->npins - 1, 0);
	while (pending) {
		bit = __ffs(pending);
		generic_handle_domain_irq(bank->gc.irq.domain, bit);
		pending &= ~BIT(bit);
	}
	chained_irq_exit(chip, desc);
}

static int hsoc_add_gpio_bank(struct platform_device *pdev,
			      struct hsoc_pinctrl *pctl,
			      struct device_node *np)
{
	struct device *dev = &pdev->dev;
	struct hsoc_gpio_bank *bank;
	struct gpio_irq_chip *girq;
	u32 index, npins;
	int ret;

	ret = of_property_read_u32(np, "reg", &index);
	if (ret || index >= pctl->nbanks)
		return dev_err_probe(dev, ret ?: -EINVAL,
				     "%pOF: invalid bank index\n", np);

	ret = of_property_read_u32(np, "hsoc,npins", &npins);
	if (ret || npins != pctl->banks[index].npins)
		return dev_err_probe(dev, ret ?: -EINVAL,
				     "%pOF: invalid pin count\n", np);

	bank = &pctl->banks[index];

	bank->gc.label = devm_kasprintf(dev, GFP_KERNEL, "%s_bank%u", pctl->instance,
					index);
	if (!bank->gc.label)
		return -ENOMEM;
	bank->gc.parent = dev;
	bank->gc.fwnode = of_fwnode_handle(np);
	bank->gc.owner = THIS_MODULE;
	bank->gc.base = -1;
	bank->gc.ngpio = npins;
	bank->gc.request = gpiochip_generic_request;
	bank->gc.free = gpiochip_generic_free;
	bank->gc.get_direction = hsoc_gpio_get_direction;
	bank->gc.direction_input = hsoc_gpio_direction_input;
	bank->gc.direction_output = hsoc_gpio_direction_output;
	bank->gc.get = hsoc_gpio_get;
	bank->gc.set = hsoc_gpio_set;
	bank->gc.set_config = gpiochip_generic_config;

	girq = &bank->gc.irq;
	gpio_irq_chip_set_chip(girq, &pctl->irq_chip);
	girq->default_type = IRQ_TYPE_NONE;
	girq->num_parents = 1;
	girq->parents = &bank->parent_irq;
	girq->parent_handler = hsoc_irq_handler;
	girq->parent_handler_data = bank;

	return devm_gpiochip_add_data(dev, &bank->gc, bank);
}

static int hsoc_init_pins(struct hsoc_pinctrl *pctl)
{
	struct device *dev = pctl->dev;
	unsigned int bank, pin, i = 0;

	pctl->pins = devm_kcalloc(dev, pctl->npins, sizeof(*pctl->pins), GFP_KERNEL);
	pctl->groups = devm_kcalloc(dev, pctl->npins, sizeof(*pctl->groups), GFP_KERNEL);
	pctl->group_pins = devm_kcalloc(dev, pctl->npins,
					sizeof(*pctl->group_pins),
					GFP_KERNEL);
	if (!pctl->pins || !pctl->groups || !pctl->group_pins)
		return -ENOMEM;

	for (bank = 0; bank < pctl->nbanks; bank++) {
		pctl->banks[bank].pin_base = i;
		for (pin = 0; pin < pctl->banks[bank].npins; pin++, i++) {
			const char *name;

			name = devm_kasprintf(dev, GFP_KERNEL, "%s_bank%u_pin%u",
					      pctl->instance, bank, pin);
			if (!name)
				return -ENOMEM;
			pctl->pins[i].number = i;
			pctl->pins[i].name = name;
			pctl->groups[i] = name;
			pctl->group_pins[i] = i;
		}
	}

	pctl->desc.name = devm_kasprintf(dev, GFP_KERNEL, "hsoc-%s-pinctrl",
					 pctl->instance);
	if (!pctl->desc.name)
		return -ENOMEM;
	pctl->desc.owner = THIS_MODULE;
	pctl->desc.pins = pctl->pins;
	pctl->desc.npins = pctl->npins;
	pctl->desc.pctlops = &hsoc_pinctrl_ops;
	pctl->desc.pmxops = &hsoc_pinmux_ops;
	pctl->desc.confops = &hsoc_pinconf_ops;
	pctl->desc.custom_params = hsoc_custom_params;
	pctl->desc.num_custom_params = ARRAY_SIZE(hsoc_custom_params);

	return 0;
}

static int hsoc_parse_banks(struct platform_device *pdev,
			    struct hsoc_pinctrl *pctl)
{
	struct device *dev = &pdev->dev;
	struct device_node *np;
	unsigned int found = 0, total = 0, i;
	u32 index, npins;
	int ret;

	for_each_available_child_of_node(dev->of_node, np) {
		struct hsoc_gpio_bank *bank;

		if (!of_property_read_bool(np, "gpio-controller"))
			continue;
		ret = of_property_read_u32(np, "reg", &index);
		if (ret || index >= HSOC_MAX_BANKS)
			goto invalid;
		ret = of_property_read_u32(np, "hsoc,npins", &npins);
		if (ret || !npins || npins > 8)
			goto invalid;

		bank = &pctl->banks[index];
		if (bank->pctl)
			goto invalid;
		bank->pctl = pctl;
		bank->base = pctl->base + HSOC_BANK_BASE(index);
		bank->index = index;
		bank->npins = npins;
		bank->parent_irq = platform_get_irq(pdev, index);
		if (bank->parent_irq < 0) {
			ret = bank->parent_irq;
			goto out_put;
		}
		found++;
		total += npins;
		pctl->nbanks = max_t(unsigned int, pctl->nbanks, index + 1);
	}

	if (!found || found != pctl->nbanks)
		return dev_err_probe(dev, -EINVAL,
				     "GPIO bank indices must be contiguous\n");
	for (i = 0; i < pctl->nbanks; i++) {
		if (!pctl->banks[i].pctl)
			return dev_err_probe(dev, -EINVAL,
					     "missing GPIO bank %u\n", i);
	}
	pctl->npins = total;
	return 0;

invalid:
	ret = -EINVAL;
out_put:
	dev_err_probe(dev, ret, "%pOF: invalid GPIO bank\n", np);
	of_node_put(np);
	return ret;
}

static int hsoc_pinctrl_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct hsoc_pinctrl *pctl;
	struct device_node *np;
	unsigned int banks = 0;
	int ret;

	pctl = devm_kzalloc(dev, sizeof(*pctl), GFP_KERNEL);
	if (!pctl)
		return -ENOMEM;
	pctl->dev = dev;
	pctl->instance = device_get_match_data(dev);
	if (!pctl->instance)
		return -EINVAL;
	pctl->irq_chip = hsoc_irq_chip_template;
	pctl->irq_chip.name = devm_kasprintf(dev, GFP_KERNEL, "hsoc-%s",
					     pctl->instance);
	if (!pctl->irq_chip.name)
		return -ENOMEM;
	raw_spin_lock_init(&pctl->lock);

	pctl->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(pctl->base))
		return PTR_ERR(pctl->base);

	ret = hsoc_parse_banks(pdev, pctl);
	if (ret)
		return ret;
	ret = hsoc_init_pins(pctl);
	if (ret)
		return ret;
	ret = devm_pinctrl_register_and_init(dev, &pctl->desc, pctl,
					     &pctl->pctldev);
	if (ret)
		return ret;
	ret = pinctrl_enable(pctl->pctldev);
	if (ret)
		return ret;

	for_each_available_child_of_node(dev->of_node, np) {
		if (!of_property_read_bool(np, "gpio-controller"))
			continue;
		ret = hsoc_add_gpio_bank(pdev, pctl, np);
		if (ret) {
			of_node_put(np);
			return ret;
		}
		banks++;
	}
	if (banks != pctl->nbanks)
		return dev_err_probe(dev, -EINVAL, "missing GPIO banks\n");

	platform_set_drvdata(pdev, pctl);
	dev_info(dev, "registered %u pins in %u GPIO banks\n",
		 pctl->npins, pctl->nbanks);
	return 0;
}

static const struct of_device_id hsoc_pinctrl_of_match[] = {
	{ .compatible = "hsoc,peri0-gpio", .data = "peri0" },
	{ .compatible = "hsoc,peri1-gpio", .data = "peri1" },
	{ }
};
MODULE_DEVICE_TABLE(of, hsoc_pinctrl_of_match);

static struct platform_driver hsoc_pinctrl_driver = {
	.probe = hsoc_pinctrl_probe,
	.driver = {
		.name = "hsoc-gpio",
		.of_match_table = hsoc_pinctrl_of_match,
	},
};
module_platform_driver(hsoc_pinctrl_driver);

MODULE_DESCRIPTION("HSOC GPIO and pin controller driver");
MODULE_LICENSE("GPL");
