/* SPDX-License-Identifier: (GPL-2.0-only OR BSD-2-Clause) */
#ifndef __DT_BINDINGS_PINCTRL_HSOC_H
#define __DT_BINDINGS_PINCTRL_HSOC_H

#define HSOC_PINMUX(bank, pin, function) \
	((((bank) * 8 + (pin)) << 8) | (function))

#endif /* __DT_BINDINGS_PINCTRL_HSOC_H */
