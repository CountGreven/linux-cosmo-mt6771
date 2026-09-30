// SPDX-License-Identifier: GPL-2.0-only
/* KUnit tests for the Fn + power-line decision logic. */

#include <kunit/test.h>

#include "fn-power-combiner.h"

static void fnp_plain_line_key(struct kunit *test)
{
	struct fnp_state s = {};

	KUNIT_EXPECT_FALSE(test, fnp_line_is_power(&s, 1));
	KUNIT_EXPECT_FALSE(test, fnp_line_is_power(&s, 0));
	KUNIT_EXPECT_FALSE(test, s.power_active);
}

static void fnp_fn_line_key(struct kunit *test)
{
	struct fnp_state s = { .fn_held = true };

	KUNIT_EXPECT_TRUE(test, fnp_line_is_power(&s, 1));
	KUNIT_EXPECT_TRUE(test, fnp_line_is_power(&s, 2));
	KUNIT_EXPECT_TRUE(test, fnp_line_is_power(&s, 0));
	KUNIT_EXPECT_FALSE(test, s.power_active);
}

static void fnp_power_outlives_fn(struct kunit *test)
{
	struct fnp_state s = { .fn_held = true };

	KUNIT_EXPECT_TRUE(test, fnp_line_is_power(&s, 1));
	s.fn_held = false;
	KUNIT_EXPECT_TRUE(test, fnp_line_is_power(&s, 2));
	KUNIT_EXPECT_TRUE(test, fnp_line_is_power(&s, 0));
}

static void fnp_esc_outlives_fn(struct kunit *test)
{
	struct fnp_state s = {};

	KUNIT_EXPECT_FALSE(test, fnp_line_is_power(&s, 1));
	s.fn_held = true;
	KUNIT_EXPECT_FALSE(test, fnp_line_is_power(&s, 0));
}

static void fnp_next_press_decides_again(struct kunit *test)
{
	struct fnp_state s = { .fn_held = true };

	KUNIT_EXPECT_TRUE(test, fnp_line_is_power(&s, 1));
	KUNIT_EXPECT_TRUE(test, fnp_line_is_power(&s, 0));
	s.fn_held = false;
	KUNIT_EXPECT_FALSE(test, fnp_line_is_power(&s, 1));
	KUNIT_EXPECT_FALSE(test, fnp_line_is_power(&s, 0));
}

static void fnp_stray_release(struct kunit *test)
{
	struct fnp_state s = { .fn_held = true };

	KUNIT_EXPECT_FALSE(test, fnp_line_is_power(&s, 0));
}

static struct kunit_case fnp_test_cases[] = {
	KUNIT_CASE(fnp_plain_line_key),
	KUNIT_CASE(fnp_fn_line_key),
	KUNIT_CASE(fnp_power_outlives_fn),
	KUNIT_CASE(fnp_esc_outlives_fn),
	KUNIT_CASE(fnp_next_press_decides_again),
	KUNIT_CASE(fnp_stray_release),
	{}
};

static struct kunit_suite fnp_test_suite = {
	.name = "fn-power-combiner",
	.test_cases = fnp_test_cases,
};
kunit_test_suite(fnp_test_suite);

MODULE_DESCRIPTION("KUnit tests for the Fn + power-line combiner");
MODULE_LICENSE("GPL");
