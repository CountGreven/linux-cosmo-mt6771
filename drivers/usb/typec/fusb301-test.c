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

	fusb301_decode(0x10, TYPEC_SINK, &st);
	KUNIT_EXPECT_TRUE(test, st.attached);
	KUNIT_EXPECT_EQ(test, st.pwr_role, TYPEC_SOURCE);
	KUNIT_EXPECT_EQ(test, st.usb_role, USB_ROLE_HOST);
}

/* Type 0x08: a charger or a PC; the vendor never runs the gadget on this port */
static void fusb301_test_source_partner(struct kunit *test)
{
	struct fusb301_state st;

	fusb301_decode(0x08, TYPEC_SINK, &st);
	KUNIT_EXPECT_TRUE(test, st.attached);
	KUNIT_EXPECT_EQ(test, st.pwr_role, TYPEC_SINK);
	KUNIT_EXPECT_EQ(test, st.usb_role, USB_ROLE_NONE);
}

static void fusb301_test_detached(struct kunit *test)
{
	struct fusb301_state st;

	fusb301_decode(0x00, TYPEC_SINK, &st);
	KUNIT_EXPECT_FALSE(test, st.attached);
	KUNIT_EXPECT_EQ(test, st.pwr_role, TYPEC_SINK);
	KUNIT_EXPECT_EQ(test, st.usb_role, USB_ROLE_NONE);

	/* bits 7:5 and 2 are not partner types */
	fusb301_decode(0xe4, TYPEC_SINK, &st);
	KUNIT_EXPECT_FALSE(test, st.attached);
}

static void fusb301_test_accessories(struct kunit *test)
{
	struct fusb301_state st;

	fusb301_decode(0x01, TYPEC_SINK, &st);
	KUNIT_EXPECT_TRUE(test, st.attached);
	KUNIT_EXPECT_EQ(test, st.accessory, TYPEC_ACCESSORY_AUDIO);
	KUNIT_EXPECT_EQ(test, st.usb_role, USB_ROLE_NONE);

	fusb301_decode(0x02, TYPEC_SINK, &st);
	KUNIT_EXPECT_EQ(test, st.accessory, TYPEC_ACCESSORY_DEBUG);
	KUNIT_EXPECT_EQ(test, st.usb_role, USB_ROLE_NONE);
}

static struct kunit_case fusb301_test_cases[] = {
	KUNIT_CASE(fusb301_test_mode),
	KUNIT_CASE(fusb301_test_sink_partner),
	KUNIT_CASE(fusb301_test_source_partner),
	KUNIT_CASE(fusb301_test_detached),
	KUNIT_CASE(fusb301_test_accessories),
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
