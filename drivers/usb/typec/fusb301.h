/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef _FUSB301_H
#define _FUSB301_H

#include <linux/types.h>
#include <linux/usb/role.h>
#include <linux/usb/typec.h>

enum fusb301_partner {
	FUSB301_PARTNER_NONE,
	FUSB301_PARTNER_SINK,
	FUSB301_PARTNER_SOURCE,
	FUSB301_PARTNER_AUDIO,
	FUSB301_PARTNER_DEBUG,
};

struct fusb301_state {
	bool attached;
	enum fusb301_partner partner;
	enum typec_role pwr_role;
	enum usb_role usb_role;
	enum typec_accessory accessory;
	bool vbus;
	bool source_notify;
};

#if IS_ENABLED(CONFIG_KUNIT)
unsigned int fusb301_mode(enum typec_port_type type);
enum fusb301_partner fusb301_partner_kind(unsigned int type);
void fusb301_decode(unsigned int type, enum typec_role default_role, bool vbus_idle_on,
		    bool hdmi, struct fusb301_state *st);
#endif

#endif
