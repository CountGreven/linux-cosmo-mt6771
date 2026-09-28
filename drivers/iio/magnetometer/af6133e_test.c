// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "af6133e.h"

static void af6133e_test_cal(struct kunit *test)
{
	static const s16 bx[3] = { 843, 10, 20 };
	static const s16 by[3] = { -80, 813, 30 };
	static const s16 bz[3] = { 5, 6, 844 };
	struct af6133e_cal cal;

	KUNIT_ASSERT_TRUE(test, af6133e_cal_from_bist(&cal, bx, by, bz));
	KUNIT_EXPECT_EQ(test, cal.gain[0], 100);
	KUNIT_EXPECT_EQ(test, cal.gain[1], 100);
	KUNIT_EXPECT_EQ(test, cal.gain[2], 99);
	KUNIT_EXPECT_EQ(test, cal.comp[0], 9);
	KUNIT_EXPECT_EQ(test, cal.comp[1], -1);
	KUNIT_EXPECT_EQ(test, cal.comp[2], -2);
	KUNIT_EXPECT_EQ(test, cal.comp[3], -3);
}

static void af6133e_test_cal_rejected(struct kunit *test)
{
	static const s16 bx[3] = { 0, 10, 20 };
	static const s16 by[3] = { -80, 813, 30 };
	static const s16 bz[3] = { 5, 6, 844 };
	static const s16 weak[3] = { 50, 0, 0 };
	struct af6133e_cal cal;

	KUNIT_EXPECT_FALSE(test, af6133e_cal_from_bist(&cal, bx, by, bz));
	KUNIT_EXPECT_EQ(test, cal.gain[0], 100);
	KUNIT_EXPECT_EQ(test, cal.comp[0], 0);

	/* gain 84388 / 50 is out of 60..600 */
	KUNIT_EXPECT_FALSE(test, af6133e_cal_from_bist(&cal, weak, by, bz));
	KUNIT_EXPECT_EQ(test, cal.gain[0], 100);
}

static void af6133e_test_compensate(struct kunit *test)
{
	static const s16 raw[3] = { 1000, -500, 2000 };
	struct af6133e_cal cal = {
		.gain = { 100, 100, 99 },
		.comp = { 9, -1, -2, -3 },
	};
	s32 out[3];

	af6133e_compensate(&cal, raw, out);
	KUNIT_EXPECT_EQ(test, out[0], 955);
	KUNIT_EXPECT_EQ(test, out[1], -510);
	KUNIT_EXPECT_EQ(test, out[2], 1975);

	af6133e_cal_default(&cal);
	af6133e_compensate(&cal, raw, out);
	KUNIT_EXPECT_EQ(test, out[0], 1000);
	KUNIT_EXPECT_EQ(test, out[1], -500);
	KUNIT_EXPECT_EQ(test, out[2], 2000);
}

static struct kunit_case af6133e_test_cases[] = {
	KUNIT_CASE(af6133e_test_cal),
	KUNIT_CASE(af6133e_test_cal_rejected),
	KUNIT_CASE(af6133e_test_compensate),
	{}
};

static struct kunit_suite af6133e_test_suite = {
	.name = "af6133e",
	.test_cases = af6133e_test_cases,
};
kunit_test_suite(af6133e_test_suite);

MODULE_DESCRIPTION("KUnit tests for the AF6133E calibration math");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
