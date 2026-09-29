// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "fusb301.h"

/* Modes register values: the vendor kernel writes 0x10 for DRP, the boot loader leaves 0x04 */
static void fusb301_test_mode(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, fusb301_mode(TYPEC_PORT_DRP), 0x10);
	KUNIT_EXPECT_EQ(test, fusb301_mode(TYPEC_PORT_SNK), 0x04);
	KUNIT_EXPECT_EQ(test, fusb301_mode(TYPEC_PORT_SRC), 0x01);
}

/* Type 0x10: the partner is a sink (hub, stick, HDMI adapter), so we source and are host */
static void fusb301_test_sink_partner(struct kunit *test)
{
	struct fusb301_state st;

	fusb301_decode(0x10, TYPEC_SINK, false, false, &st);
	KUNIT_EXPECT_TRUE(test, st.attached);
	KUNIT_EXPECT_EQ(test, st.pwr_role, TYPEC_SOURCE);
	KUNIT_EXPECT_EQ(test, st.usb_role, USB_ROLE_HOST);
}

/* Type 0x08: a charger or a PC; the vendor never runs the gadget on this port */
static void fusb301_test_source_partner(struct kunit *test)
{
	struct fusb301_state st;

	fusb301_decode(0x08, TYPEC_SINK, false, false, &st);
	KUNIT_EXPECT_TRUE(test, st.attached);
	KUNIT_EXPECT_EQ(test, st.pwr_role, TYPEC_SINK);
	KUNIT_EXPECT_EQ(test, st.usb_role, USB_ROLE_NONE);
}

static void fusb301_test_detached(struct kunit *test)
{
	struct fusb301_state st;

	fusb301_decode(0x00, TYPEC_SINK, false, false, &st);
	KUNIT_EXPECT_FALSE(test, st.attached);
	KUNIT_EXPECT_EQ(test, st.pwr_role, TYPEC_SINK);
	KUNIT_EXPECT_EQ(test, st.usb_role, USB_ROLE_NONE);

	/* bits 7:5 and 2 are not partner types */
	fusb301_decode(0xe4, TYPEC_SINK, false, false, &st);
	KUNIT_EXPECT_FALSE(test, st.attached);
}

static void fusb301_test_accessories(struct kunit *test)
{
	struct fusb301_state st;

	fusb301_decode(0x01, TYPEC_SINK, false, false, &st);
	KUNIT_EXPECT_TRUE(test, st.attached);
	KUNIT_EXPECT_EQ(test, st.accessory, TYPEC_ACCESSORY_AUDIO);
	KUNIT_EXPECT_EQ(test, st.usb_role, USB_ROLE_NONE);

	fusb301_decode(0x02, TYPEC_SINK, false, false, &st);
	KUNIT_EXPECT_EQ(test, st.accessory, TYPEC_ACCESSORY_DEBUG);
	KUNIT_EXPECT_EQ(test, st.usb_role, USB_ROLE_NONE);
}

/* Vendor test_right_usb (usb_typec.c:410-428): only Type & 0x18 decides source vs not */
static void fusb301_test_partner_kind(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, fusb301_partner_kind(0x00), FUSB301_PARTNER_NONE);
	KUNIT_EXPECT_EQ(test, fusb301_partner_kind(0x10), FUSB301_PARTNER_SINK);
	KUNIT_EXPECT_EQ(test, fusb301_partner_kind(0x08), FUSB301_PARTNER_SOURCE);
	KUNIT_EXPECT_EQ(test, fusb301_partner_kind(0x09), FUSB301_PARTNER_SOURCE);
	KUNIT_EXPECT_EQ(test, fusb301_partner_kind(0x0a), FUSB301_PARTNER_SOURCE);
	KUNIT_EXPECT_EQ(test, fusb301_partner_kind(0x01), FUSB301_PARTNER_AUDIO);
	KUNIT_EXPECT_EQ(test, fusb301_partner_kind(0x02), FUSB301_PARTNER_DEBUG);
	KUNIT_EXPECT_EQ(test, fusb301_partner_kind(0x18), FUSB301_PARTNER_NONE);
	KUNIT_EXPECT_EQ(test, fusb301_partner_kind(0xe4), FUSB301_PARTNER_NONE);
}

struct fusb301_vbus_case {
	unsigned int type;
	bool idle_on;
	bool vbus;
	bool source_notify;
};

/*
 * With idle_on (Cosmo GPIO178, shared with the keyboard light) the rail is up unless a source is
 * attached; GPIO52 follows the source decision in both settings.
 */
static const struct fusb301_vbus_case fusb301_vbus_cases[] = {
	{ 0x00, true,  true,  false },
	{ 0x10, true,  true,  false },
	{ 0x08, true,  false, true  },
	{ 0x01, true,  true,  false },
	{ 0x18, true,  true,  false },
	{ 0x00, false, false, false },
	{ 0x10, false, true,  false },
	{ 0x08, false, false, true  },
	{ 0x02, false, false, false },
};

static void fusb301_test_vbus_table(struct kunit *test)
{
	struct fusb301_state st;
	int i;

	for (i = 0; i < ARRAY_SIZE(fusb301_vbus_cases); i++) {
		const struct fusb301_vbus_case *c = &fusb301_vbus_cases[i];

		fusb301_decode(c->type, TYPEC_SINK, c->idle_on, false, &st);
		KUNIT_EXPECT_EQ_MSG(test, st.vbus, c->vbus, "type 0x%02x idle_on %d",
				    c->type, c->idle_on);
		KUNIT_EXPECT_EQ_MSG(test, st.source_notify, c->source_notify,
				    "type 0x%02x idle_on %d", c->type, c->idle_on);
	}
}

/* A source partner: no data role, VBUS released, GPIO52 high */
static void fusb301_test_source_releases(struct kunit *test)
{
	struct fusb301_state st;

	fusb301_decode(0x08, TYPEC_SINK, true, false, &st);
	KUNIT_EXPECT_EQ(test, st.partner, FUSB301_PARTNER_SOURCE);
	KUNIT_EXPECT_EQ(test, st.usb_role, USB_ROLE_NONE);
	KUNIT_EXPECT_FALSE(test, st.vbus);
	KUNIT_EXPECT_TRUE(test, st.source_notify);
}

/* Planet HDMI adapter: a sink with GPIO54 high keeps VBUS but takes no USB role */
static void fusb301_test_hdmi_gate(struct kunit *test)
{
	struct fusb301_state st;

	fusb301_decode(0x10, TYPEC_SINK, true, true, &st);
	KUNIT_EXPECT_TRUE(test, st.attached);
	KUNIT_EXPECT_EQ(test, st.partner, FUSB301_PARTNER_SINK);
	KUNIT_EXPECT_EQ(test, st.pwr_role, TYPEC_SOURCE);
	KUNIT_EXPECT_EQ(test, st.usb_role, USB_ROLE_NONE);
	KUNIT_EXPECT_TRUE(test, st.vbus);
	KUNIT_EXPECT_FALSE(test, st.source_notify);

	fusb301_decode(0x10, TYPEC_SINK, true, false, &st);
	KUNIT_EXPECT_EQ(test, st.usb_role, USB_ROLE_HOST);

	/* GPIO54 means nothing without a sink partner */
	fusb301_decode(0x08, TYPEC_SINK, true, true, &st);
	KUNIT_EXPECT_EQ(test, st.usb_role, USB_ROLE_NONE);
	KUNIT_EXPECT_FALSE(test, st.vbus);
	fusb301_decode(0x00, TYPEC_SINK, true, true, &st);
	KUNIT_EXPECT_FALSE(test, st.attached);
	KUNIT_EXPECT_EQ(test, st.usb_role, USB_ROLE_NONE);
}

static struct kunit_case fusb301_test_cases[] = {
	KUNIT_CASE(fusb301_test_mode),
	KUNIT_CASE(fusb301_test_sink_partner),
	KUNIT_CASE(fusb301_test_source_partner),
	KUNIT_CASE(fusb301_test_detached),
	KUNIT_CASE(fusb301_test_accessories),
	KUNIT_CASE(fusb301_test_partner_kind),
	KUNIT_CASE(fusb301_test_vbus_table),
	KUNIT_CASE(fusb301_test_source_releases),
	KUNIT_CASE(fusb301_test_hdmi_gate),
	{}
};

static struct kunit_suite fusb301_test_suite = {
	.name = "fusb301",
	.test_cases = fusb301_test_cases,
};
kunit_test_suite(fusb301_test_suite);

MODULE_DESCRIPTION("KUnit tests for the FUSB301 Type-C driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
