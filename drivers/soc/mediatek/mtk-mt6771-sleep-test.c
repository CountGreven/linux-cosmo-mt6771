// SPDX-License-Identifier: GPL-2.0-only
#include <kunit/test.h>

#include "mtk-mt6771-sleep.h"

static void slp_test_pcm_flags(struct kunit *test)
{
	/* vendor slp_spm_flags: DIS_VCORE_DVS | DIS_VCORE_DFS | DIS_ATF_ABORT | SUSPEND_OPTION */
	KUNIT_EXPECT_EQ(test, slp_pcm_flags(true), 0x80098U);
	KUNIT_EXPECT_EQ(test, slp_pcm_flags(false), 0x8009aU);
	KUNIT_EXPECT_EQ(test, slp_pcm_flags1(false), 0U);
	KUNIT_EXPECT_EQ(test, slp_pcm_flags1(true), 0x300U);
}

static void slp_test_timer(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, slp_timer_val(5401), 5401U * 32768);
	KUNIT_EXPECT_EQ(test, slp_timer_val(30), 30U * 32768);
	KUNIT_EXPECT_EQ(test, slp_timer_val(0), 600U * 32768);
	/* vendor cap 36 h, still fits the 32-bit PCM timer */
	KUNIT_EXPECT_EQ(test, slp_timer_val(200000), 129600U * 32768);
}

static void slp_test_payload(struct kunit *test)
{
	u32 w[SLP_SSPM_WORDS] = { [7] = 0xdead };

	slp_sspm_payload(w, SLP_SSPM_SUSPEND_PREPARE, 0x100000002ULL, 0x300000004ULL, 4);
	KUNIT_EXPECT_EQ(test, w[0], 8U);
	KUNIT_EXPECT_EQ(test, w[1], 2U);
	KUNIT_EXPECT_EQ(test, w[2], 1U);
	KUNIT_EXPECT_EQ(test, w[3], 4U);
	KUNIT_EXPECT_EQ(test, w[4], 3U);
	KUNIT_EXPECT_EQ(test, w[5], 4U);
	KUNIT_EXPECT_EQ(test, w[6], 0U);
	KUNIT_EXPECT_EQ(test, w[7], 0U);
}

static void slp_test_lp_ops(struct kunit *test)
{
	const struct slp_lp_entry vs2 = SLP_HW(VS2, SRCLKEN0, LP);
	const struct slp_lp_entry vbif = SLP_HW(VBIF28, SRCLKEN2, OFF);
	const struct slp_lp_entry vcore = SLP_SW(VCORE);
	struct slp_reg_op ops[2];

	KUNIT_ASSERT_EQ(test, slp_lp_ops(&vs2, ops), 2U);
	KUNIT_EXPECT_EQ(test, ops[0].reg, 0x1710);
	KUNIT_EXPECT_EQ(test, ops[0].mask, 0x2);
	KUNIT_EXPECT_EQ(test, ops[0].val, 0x2);
	KUNIT_EXPECT_EQ(test, ops[1].reg, 0x1716);
	KUNIT_EXPECT_EQ(test, ops[1].mask, 0x2);
	KUNIT_EXPECT_EQ(test, ops[1].val, 0x2);

	KUNIT_ASSERT_EQ(test, slp_lp_ops(&vbif, ops), 2U);
	KUNIT_EXPECT_EQ(test, ops[0].reg, 0x1da0);
	KUNIT_EXPECT_EQ(test, ops[0].val, 0x8);
	KUNIT_EXPECT_EQ(test, ops[1].reg, 0x1da6);
	KUNIT_EXPECT_EQ(test, ops[1].mask, 0x8);
	KUNIT_EXPECT_EQ(test, ops[1].val, 0);

	KUNIT_ASSERT_EQ(test, slp_lp_ops(&vcore, ops), 1U);
	KUNIT_EXPECT_EQ(test, ops[0].reg, 0x1490);
	KUNIT_EXPECT_EQ(test, ops[0].mask, 0x1);
	KUNIT_EXPECT_EQ(test, ops[0].val, 0x1);
}

struct slp_test_reg {
	u16 reg;
	u16 mainline;
	u16 expect;
};

/*
 * Mainline and vendor values from probes/pmic-lp-diff-2026-09-29.txt. The VRF/VFE bit 0 and VFE28
 * OP_CFG bit 8, and VCN28 bit 1 (consys driver) are cleared or set outside PMIC_LP_INIT_SETTING,
 * so the table alone leaves those as expected here.
 */
static const struct slp_test_reg slp_test_regs[] = {
	{ 0x1610, 0x0001, 0x000b }, { 0x1616, 0x0100, 0x010a },	/* VDRAM1 */
	{ 0x1710, 0x0001, 0x000b }, { 0x1716, 0x0100, 0x010a },	/* VS2 */
	{ 0x1a8a, 0x0001, 0x000b }, { 0x1a90, 0x0000, 0x000a },	/* VXO22 */
	{ 0x1a9e, 0x0001, 0x000b }, { 0x1aa4, 0x0000, 0x000a },	/* VA12 */
	{ 0x1ab2, 0x0001, 0x000b }, { 0x1ab8, 0x0000, 0x000a },	/* VAUX18 */
	{ 0x1ac6, 0x0001, 0x0003 }, { 0x1acc, 0x0000, 0x0002 },	/* VAUD28 */
	{ 0x1b0a, 0x0001, 0x000b }, { 0x1b10, 0x0000, 0x000a },	/* VDRAM2 */
	{ 0x1b32, 0x0001, 0x000b }, { 0x1b38, 0x0000, 0x000a },	/* VUSB */
	{ 0x1bae, 0x0001, 0x000b }, { 0x1bb4, 0x0000, 0x000a },	/* VSRAM_OTHERS */
	{ 0x1da0, 0x0001, 0x000b }, { 0x1da6, 0x0000, 0x0000 },	/* VBIF28 */
	{ 0x1c0a, 0x0001, 0x0005 }, { 0x1c10, 0x0100, 0x0100 },	/* VFE28 (vendor 0x4 / 0) */
	{ 0x1c1e, 0x0001, 0x0005 },				/* VRF18 (vendor 0x4) */
	{ 0x1c32, 0x0001, 0x0005 },				/* VRF12 (vendor 0x4) */
	{ 0x1d8a, 0x0001, 0x0001 },				/* VCN28 (vendor 0x3) */
	{ 0x1490, 0x0001, 0x0001 },				/* VCORE, SW only */
};

static void slp_test_lp_table(struct kunit *test)
{
	u16 regs[ARRAY_SIZE(slp_test_regs)];
	unsigned int i, j, k, n;
	struct slp_reg_op ops[2];

	for (i = 0; i < ARRAY_SIZE(slp_test_regs); i++)
		regs[i] = slp_test_regs[i].mainline;

	for (i = 0; i < ARRAY_SIZE(slp_lp_table); i++) {
		KUNIT_EXPECT_LE(test, slp_lp_table[i].user, SLP_LP_SRCLKEN2);
		n = slp_lp_ops(&slp_lp_table[i], ops);
		for (k = 0; k < n; k++)
			for (j = 0; j < ARRAY_SIZE(slp_test_regs); j++)
				if (slp_test_regs[j].reg == ops[k].reg)
					regs[j] = (regs[j] & ~ops[k].mask) | ops[k].val;
	}

	for (i = 0; i < ARRAY_SIZE(slp_test_regs); i++)
		KUNIT_EXPECT_EQ_MSG(test, regs[i], slp_test_regs[i].expect, "reg 0x%04x",
				    slp_test_regs[i].reg);
	/* 40 vendor calls per block, VPA has no register */
	KUNIT_EXPECT_EQ(test, ARRAY_SIZE(slp_lp_table), 2 * 39);
}

static struct kunit_case slp_test_cases[] = {
	KUNIT_CASE(slp_test_pcm_flags),
	KUNIT_CASE(slp_test_timer),
	KUNIT_CASE(slp_test_payload),
	KUNIT_CASE(slp_test_lp_ops),
	KUNIT_CASE(slp_test_lp_table),
	{}
};

static struct kunit_suite slp_test_suite = {
	.name = "mtk-mt6771-sleep",
	.test_cases = slp_test_cases,
};
kunit_test_suite(slp_test_suite);

MODULE_DESCRIPTION("KUnit tests for the MT6771 deep sleep helpers");
MODULE_LICENSE("GPL");
