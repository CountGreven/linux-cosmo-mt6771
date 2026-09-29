/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _GPIO_ROLE_MUX_H
#define _GPIO_ROLE_MUX_H

#include <linux/usb/role.h>

#define GPIO_ROLE_MUX_INPUTS	2
#define GPIO_ROLE_MUX_NONE	(-1)

struct gpio_role_mux_plan {
	int sel;
	enum usb_role role;
	bool drop;
};

#if IS_ENABLED(CONFIG_KUNIT)
int gpio_role_mux_select(const enum usb_role *req, int active, int forced);
void gpio_role_mux_plan(const enum usb_role *req, int active, int forced, enum usb_role out_role,
			struct gpio_role_mux_plan *p);
#endif

#endif
