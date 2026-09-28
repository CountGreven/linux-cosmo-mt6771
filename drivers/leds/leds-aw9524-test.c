// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "leds-aw9524.h"

/* Pins 0-7 are P0_0..P0_7, pins 8-15 are P1_0..P1_7 */
static void aw9524_test_dim_reg(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, aw9524_dim_reg(0), 0x24);
	KUNIT_EXPECT_EQ(test, aw9524_dim_reg(7), 0x2b);
	KUNIT_EXPECT_EQ(test, aw9524_dim_reg(8), 0x20);
	KUNIT_EXPECT_EQ(test, aw9524_dim_reg(11), 0x23);
	KUNIT_EXPECT_EQ(test, aw9524_dim_reg(12), 0x2c);
	KUNIT_EXPECT_EQ(test, aw9524_dim_reg(15), 0x2f);
	KUNIT_EXPECT_LT(test, aw9524_dim_reg(16), 0);
}

static struct kunit_case aw9524_test_cases[] = {
	KUNIT_CASE(aw9524_test_dim_reg),
	{}
};

static struct kunit_suite aw9524_test_suite = {
	.name = "leds-aw9524",
	.test_cases = aw9524_test_cases,
};
kunit_test_suite(aw9524_test_suite);

MODULE_DESCRIPTION("KUnit tests for the AW9524 LED driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
