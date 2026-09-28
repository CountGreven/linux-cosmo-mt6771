// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "mt6358-battery.h"

static void mt6358_bat_test_current(struct kunit *test)
{
	/* 381.47 uA per LSB times CAR_TUNE 0.93 */
	KUNIT_EXPECT_EQ(test, mt6358_bat_current_ua(0), 0);
	KUNIT_EXPECT_EQ(test, mt6358_bat_current_ua(16), 5676);
	/* two's complement with the vendor's 65535 bias: 0xfc9f is 864 LSB of discharge */
	KUNIT_EXPECT_EQ(test, mt6358_bat_current_ua(0xfc9f), -306518);
}

static void mt6358_bat_test_car(struct kunit *test)
{
	/* the value read on the phone: magnitude 0xfffff - 0xffff4 = 11 LSB of discharge */
	KUNIT_EXPECT_EQ(test, mt6358_bat_car_uah(0xa5a7, 0xffff), -1110LL);
	/* 0 and 0xfffff both mean an empty counter */
	KUNIT_EXPECT_EQ(test, mt6358_bat_car_uah(0x0000, 0x0000), 0LL);
	KUNIT_EXPECT_EQ(test, mt6358_bat_car_uah(0xf800, 0x7fff), 0LL);
	/* 1000 LSB of charge */
	KUNIT_EXPECT_EQ(test, mt6358_bat_car_uah(1000 << 11 & 0xffff, 1000 >> 5), 100911LL);
}

static void mt6358_bat_test_soc(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, mt6358_bat_soc_permille(500, 0, -427590, 4275900), 400);
	KUNIT_EXPECT_EQ(test, mt6358_bat_soc_permille(990, 0, 427590, 4275900), 1000);
	KUNIT_EXPECT_EQ(test, mt6358_bat_soc_permille(10, 0, -427590, 4275900), 0);
}

/* vendor charger manager: stop at >= 55 C or < -10 C, resume below 50 C and at >= 0 C */
static void mt6358_bat_test_temp(struct kunit *test)
{
	KUNIT_EXPECT_FALSE(test, mt6358_bat_temp_inhibit(false, 549));
	KUNIT_EXPECT_TRUE(test, mt6358_bat_temp_inhibit(false, 550));
	KUNIT_EXPECT_TRUE(test, mt6358_bat_temp_inhibit(true, 500));
	KUNIT_EXPECT_FALSE(test, mt6358_bat_temp_inhibit(true, 499));
	KUNIT_EXPECT_FALSE(test, mt6358_bat_temp_inhibit(false, -100));
	KUNIT_EXPECT_TRUE(test, mt6358_bat_temp_inhibit(false, -101));
	KUNIT_EXPECT_TRUE(test, mt6358_bat_temp_inhibit(true, -1));
	KUNIT_EXPECT_FALSE(test, mt6358_bat_temp_inhibit(true, 0));
}

static struct kunit_case mt6358_bat_test_cases[] = {
	KUNIT_CASE(mt6358_bat_test_temp),
	KUNIT_CASE(mt6358_bat_test_current),
	KUNIT_CASE(mt6358_bat_test_car),
	KUNIT_CASE(mt6358_bat_test_soc),
	{}
};

static struct kunit_suite mt6358_bat_test_suite = {
	.name = "mt6358-battery",
	.test_cases = mt6358_bat_test_cases,
};
kunit_test_suite(mt6358_bat_test_suite);

MODULE_DESCRIPTION("KUnit tests for the MT6358 fuel gauge conversions");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("EXPORTED_FOR_KUNIT_TESTING");
