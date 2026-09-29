/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _FUSB301_H
#define _FUSB301_H

#include <linux/types.h>
#include <linux/usb/role.h>
#include <linux/usb/typec.h>

struct fusb301_state {
	bool attached;
	enum typec_role pwr_role;
	enum usb_role usb_role;
	enum typec_accessory accessory;
};

#if IS_ENABLED(CONFIG_KUNIT)
unsigned int fusb301_mode(enum typec_port_type type);
void fusb301_decode(unsigned int type, enum typec_role default_role,
		    struct fusb301_state *st);
#endif

#endif
