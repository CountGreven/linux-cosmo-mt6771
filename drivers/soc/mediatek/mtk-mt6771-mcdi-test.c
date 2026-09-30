// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "mtk-mt6771-mcdi.h"

static void mcdi_test_avail_mask(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, mcdi_avail_mask(0xff), 0xffU);
	KUNIT_EXPECT_EQ(test, mcdi_avail_mask(0xfd), 0xfdU);
	KUNIT_EXPECT_EQ(test, mcdi_avail_mask(0x1), 0x1U);
	/* only eight cores exist; the vendor mask is 8 bits wide */
	KUNIT_EXPECT_EQ(test, mcdi_avail_mask(0x3ff), 0xffU);
}

/* slot 9 ACTION_STAT: 0 init, 1 paused, 2 waiting, 3 working */
static void mcdi_test_task_ready(struct kunit *test)
{
	KUNIT_EXPECT_TRUE(test, mcdi_task_ready(2, 0));
	KUNIT_EXPECT_TRUE(test, mcdi_task_ready(3, 0));
	KUNIT_EXPECT_FALSE(test, mcdi_task_ready(0, 0));
	KUNIT_EXPECT_FALSE(test, mcdi_task_ready(1, 0));
	KUNIT_EXPECT_FALSE(test, mcdi_task_ready(2, 1));
	KUNIT_EXPECT_FALSE(test, mcdi_task_ready(4, 0));
}

static void mcdi_test_psci_level(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, mcdi_psci_level(0x00010001), 0U);
	KUNIT_EXPECT_EQ(test, mcdi_psci_level(0x01010001), 1U);
	KUNIT_EXPECT_EQ(test, mcdi_psci_level(0x01010005), 1U);
	KUNIT_EXPECT_EQ(test, mcdi_psci_level(0x02010000), 2U);
	KUNIT_EXPECT_EQ(test, mcdi_psci_level(0x00000001), 0U);
}

static void mcdi_test_state_allowed(struct kunit *test)
{
	/* normal states: open=0 works for level-0 */
	KUNIT_EXPECT_FALSE(test, mcdi_state_allowed(0x00010001, 7, 0, false));
	KUNIT_EXPECT_TRUE(test, mcdi_state_allowed(0x00010001, 7, 0x80, false));
	KUNIT_EXPECT_FALSE(test, mcdi_state_allowed(0x00010001, 6, 0x80, false));
	KUNIT_EXPECT_TRUE(test, mcdi_state_allowed(0x00010001, 0, 0xff, false));
	/* cluster level needs CLUSTER_n_CAN_POWER_OFF per entry, which is not written */
	KUNIT_EXPECT_FALSE(test, mcdi_state_allowed(0x01010001, 7, 0xff, false));
	KUNIT_EXPECT_FALSE(test, mcdi_state_allowed(0x00010001, 8, 0x1ff, false));
	KUNIT_EXPECT_FALSE(test, mcdi_state_allowed(MCDI_PSCI_PARAM_NONE, 0, 0xff, false));
	/* suspend param: closed by default */
	KUNIT_EXPECT_FALSE(test, mcdi_state_allowed(MCDI_PSCI_PARAM_SUSPEND, 0, 0, false));
	KUNIT_EXPECT_TRUE(test, mcdi_state_allowed(MCDI_PSCI_PARAM_SUSPEND, 0, 0, true));
	/* never for cpu 1..7 */
	KUNIT_EXPECT_FALSE(test, mcdi_state_allowed(MCDI_PSCI_PARAM_SUSPEND, 1, 0xff, true));
	KUNIT_EXPECT_FALSE(test, mcdi_state_allowed(MCDI_PSCI_PARAM_SUSPEND, 7, 0x80, true));
	/* level-0 states unaffected by suspend_open */
	KUNIT_EXPECT_TRUE(test, mcdi_state_allowed(0x00010001, 3, 0xff, false));
	KUNIT_EXPECT_TRUE(test, mcdi_state_allowed(0x00010001, 3, 0xff, true));
}

static struct kunit_case mcdi_test_cases[] = {
	KUNIT_CASE(mcdi_test_avail_mask),
	KUNIT_CASE(mcdi_test_task_ready),
	KUNIT_CASE(mcdi_test_psci_level),
	KUNIT_CASE(mcdi_test_state_allowed),
	{}
};

static struct kunit_suite mcdi_test_suite = {
	.name = "mtk-mt6771-mcdi",
	.test_cases = mcdi_test_cases,
};
kunit_test_suite(mcdi_test_suite);

MODULE_DESCRIPTION("KUnit tests for the MT6771 MCDI mailbox helpers");
MODULE_LICENSE("GPL");
