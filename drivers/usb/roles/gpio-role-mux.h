/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _GPIO_ROLE_MUX_H
#define _GPIO_ROLE_MUX_H

#include <linux/usb/role.h>

#define GPIO_ROLE_MUX_INPUTS	2
#define GPIO_ROLE_MUX_NONE	(-1)

#if IS_ENABLED(CONFIG_KUNIT)
int gpio_role_mux_select(const enum usb_role *req, int active, int forced);
#endif

#endif
