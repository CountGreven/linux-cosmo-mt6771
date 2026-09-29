// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "gpio-role-mux.h"

#define N	USB_ROLE_NONE
#define H	USB_ROLE_HOST
#define D	USB_ROLE_DEVICE
#define NONE	GPIO_ROLE_MUX_NONE
#define AUTO	GPIO_ROLE_MUX_NONE

static int sel(enum usb_role a, enum usb_role b, int active, int forced)
{
	enum usb_role req[GPIO_ROLE_MUX_INPUTS] = { a, b };

	return gpio_role_mux_select(req, active, forced);
}

static void gpio_role_mux_test_idle(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sel(N, N, NONE, AUTO), NONE);
	KUNIT_EXPECT_EQ(test, sel(N, N, 1, AUTO), NONE);
}

static void gpio_role_mux_test_first_come(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sel(H, N, NONE, AUTO), 0);
	KUNIT_EXPECT_EQ(test, sel(N, H, NONE, AUTO), 1);
	/* a hub on one side does not steal the other side's hub */
	KUNIT_EXPECT_EQ(test, sel(H, H, 0, AUTO), 0);
	KUNIT_EXPECT_EQ(test, sel(H, H, 1, AUTO), 1);
	/* both at once (boot): input 0 */
	KUNIT_EXPECT_EQ(test, sel(H, H, NONE, AUTO), 0);
	KUNIT_EXPECT_EQ(test, sel(D, D, NONE, AUTO), 0);
}

/* The vendor's "left_charging_and_right_otg": a host request beats a device one */
static void gpio_role_mux_test_host_beats_device(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sel(D, H, 0, AUTO), 1);
	KUNIT_EXPECT_EQ(test, sel(H, D, 1, AUTO), 0);
	KUNIT_EXPECT_EQ(test, sel(D, H, 1, AUTO), 1);
	KUNIT_EXPECT_EQ(test, sel(D, N, NONE, AUTO), 0);
	KUNIT_EXPECT_EQ(test, sel(N, D, NONE, AUTO), 1);
	KUNIT_EXPECT_EQ(test, sel(D, H, NONE, AUTO), 1);
}

static void gpio_role_mux_test_release(struct kunit *test)
{
	/* the active side leaves, the waiting side gets the lines */
	KUNIT_EXPECT_EQ(test, sel(N, H, 0, AUTO), 1);
	KUNIT_EXPECT_EQ(test, sel(D, N, 1, AUTO), 0);
}

static void gpio_role_mux_test_forced(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, sel(H, N, 0, 1), 1);
	KUNIT_EXPECT_EQ(test, sel(H, H, 0, 1), 1);
	KUNIT_EXPECT_EQ(test, sel(N, H, 1, 0), 0);
	KUNIT_EXPECT_EQ(test, sel(N, N, NONE, 0), 0);
}

static struct gpio_role_mux_plan plan(enum usb_role a, enum usb_role b, int active,
				      enum usb_role out)
{
	enum usb_role req[GPIO_ROLE_MUX_INPUTS] = { a, b };
	struct gpio_role_mux_plan p;

	gpio_role_mux_plan(req, active, AUTO, out, &p);
	return p;
}

/*
 * Vendor rows 6c and 7 (one shared tcpc_otg_attached): a left OTG device or charger leaving
 * unloads xHCI under a working right host. Here the right host keeps the lines and the
 * controller is never set to none.
 */
static void gpio_role_mux_test_left_detach_keeps_right_host(struct kunit *test)
{
	struct gpio_role_mux_plan p;

	/* left OTG device arrives while the right hub is hosted */
	p = plan(H, H, 1, H);
	KUNIT_EXPECT_EQ(test, p.sel, 1);
	KUNIT_EXPECT_EQ(test, p.role, H);
	KUNIT_EXPECT_FALSE(test, p.drop);

	/* and leaves */
	p = plan(N, H, 1, H);
	KUNIT_EXPECT_EQ(test, p.sel, 1);
	KUNIT_EXPECT_EQ(test, p.role, H);
	KUNIT_EXPECT_FALSE(test, p.drop);

	/* left charger or PC leaves */
	p = plan(N, H, 1, H);
	KUNIT_EXPECT_FALSE(test, p.drop);
	p = plan(D, H, 1, H);
	KUNIT_EXPECT_EQ(test, p.sel, 1);
	KUNIT_EXPECT_FALSE(test, p.drop);
}

/* Left host drops while the right one waits: the lines go right, the controller ends as host */
static void gpio_role_mux_test_left_host_hands_over(struct kunit *test)
{
	struct gpio_role_mux_plan p;

	p = plan(N, H, 0, H);
	KUNIT_EXPECT_EQ(test, p.sel, 1);
	KUNIT_EXPECT_EQ(test, p.role, H);
	KUNIT_EXPECT_TRUE(test, p.drop);

	/* nothing left on either side */
	p = plan(N, N, 1, H);
	KUNIT_EXPECT_EQ(test, p.sel, NONE);
	KUNIT_EXPECT_EQ(test, p.role, N);
	KUNIT_EXPECT_TRUE(test, p.drop);
}

static struct kunit_case gpio_role_mux_test_cases[] = {
	KUNIT_CASE(gpio_role_mux_test_idle),
	KUNIT_CASE(gpio_role_mux_test_first_come),
	KUNIT_CASE(gpio_role_mux_test_host_beats_device),
	KUNIT_CASE(gpio_role_mux_test_release),
	KUNIT_CASE(gpio_role_mux_test_forced),
	KUNIT_CASE(gpio_role_mux_test_left_detach_keeps_right_host),
	KUNIT_CASE(gpio_role_mux_test_left_host_hands_over),
	{}
};

static struct kunit_suite gpio_role_mux_test_suite = {
	.name = "gpio-role-mux",
	.test_cases = gpio_role_mux_test_cases,
};
kunit_test_suite(gpio_role_mux_test_suite);

MODULE_DESCRIPTION("KUnit tests for the GPIO USB role switch multiplexer");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
