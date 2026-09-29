/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MTK_MT6771_SLEEP_H
#define __MTK_MT6771_SLEEP_H

#include <linux/bits.h>
#include <linux/types.h>

/* SPM pcm_flags / pcm_flags1, vendor spm_v4/sleep_def_mt6771.h */
#define SLP_FLAG_DIS_INFRA_PDN		BIT(1)
#define SLP_FLAG_DIS_VCORE_DVS		BIT(3)
#define SLP_FLAG_DIS_VCORE_DFS		BIT(4)
#define SLP_FLAG_DIS_ATF_ABORT		BIT(7)
#define SLP_FLAG_SUSPEND_OPTION		BIT(19)
#define SLP_FLAG1_BIG_BUCK_OFF_ENABLE	BIT(8)
#define SLP_FLAG1_BIG_BUCK_ON_ENABLE	BIT(9)

#define SLP_PCM_TIMER_HZ		32768
#define SLP_WAKE_SEC_DEFAULT		600
#define SLP_WAKE_SEC_MAX		(36 * 3600)

/* SSPM IPI_ID_SPM_SUSPEND commands, vendor spm_v4/mtk_spm_internal.h */
#define SLP_SSPM_SUSPEND		0
#define SLP_SSPM_RESUME			1
#define SLP_SSPM_SUSPEND_PREPARE	8
#define SLP_SSPM_POST_SUSPEND		9
#define SLP_SSPM_WORDS			8

/* Vendor mtk_sleep.c slp_spm_flags; infra_pdn false keeps INFRA powered in sleep */
static inline u32 slp_pcm_flags(bool infra_pdn)
{
	u32 flags = SLP_FLAG_DIS_VCORE_DVS | SLP_FLAG_DIS_VCORE_DFS | SLP_FLAG_DIS_ATF_ABORT |
		    SLP_FLAG_SUSPEND_OPTION;

	return infra_pdn ? flags : flags | SLP_FLAG_DIS_INFRA_PDN;
}

/*
 * Vendor __sync_big_buck_ctrl_pcm_flag: the SPM switches the big-cluster buck only when the
 * kernel did not already cut it at hotplug (its suspend always did, so it sent 0).
 */
static inline u32 slp_pcm_flags1(bool spm_big_buck)
{
	return spm_big_buck ? SLP_FLAG1_BIG_BUCK_OFF_ENABLE | SLP_FLAG1_BIG_BUCK_ON_ENABLE : 0;
}

/* Vendor _spm_get_wake_period and timer_val = sec * 32768 */
static inline u32 slp_timer_val(unsigned int sec)
{
	if (!sec)
		sec = SLP_WAKE_SEC_DEFAULT;
	if (sec > SLP_WAKE_SEC_MAX)
		sec = SLP_WAKE_SEC_MAX;
	return sec * SLP_PCM_TIMER_HZ;
}

/* struct spm_data: cmd, then suspend.{timestamp_l, _h, src_clk_l, _h, spm_opt} */
static inline void slp_sspm_payload(u32 *w, u32 cmd, u64 ts, u64 clk, u32 opt)
{
	unsigned int i;

	for (i = 0; i < SLP_SSPM_WORDS; i++)
		w[i] = 0;
	w[0] = cmd;
	w[1] = lower_32_bits(ts);
	w[2] = upper_32_bits(ts);
	w[3] = lower_32_bits(clk);
	w[4] = upper_32_bits(clk);
	w[5] = opt;
}

/*
 * MT6358 rail low-power users, vendor pmic_lp_api.c: OP_EN bit <user> enables that user,
 * OP_CFG (OP_EN + 6) bit <user> is HW_LP (1) or HW_OFF (0), written for SRCLKEN users only.
 */
enum slp_lp_user {
	SLP_LP_SW,
	SLP_LP_SRCLKEN0,
	SLP_LP_SRCLKEN1,
	SLP_LP_SRCLKEN2,
};

#define SLP_LP_OP_CFG_OFS	6
#define SLP_HW_OFF		0
#define SLP_HW_LP		1

struct slp_lp_entry {
	u16 op_en;
	u8 user;
	u8 cfg;
};

struct slp_reg_op {
	u16 reg;
	u16 mask;
	u16 val;
};

/* MT6358 *_OP_EN, vendor mt6771 upmu_hw.h */
#define MT6358_VPROC11_OP_EN	0x1390
#define MT6358_VPROC12_OP_EN	0x1410
#define MT6358_VCORE_OP_EN	0x1490
#define MT6358_VGPU_OP_EN	0x1510
#define MT6358_VMODEM_OP_EN	0x1590
#define MT6358_VDRAM1_OP_EN	0x1610
#define MT6358_VS1_OP_EN	0x1690
#define MT6358_VS2_OP_EN	0x1710
#define MT6358_VXO22_OP_EN	0x1a8a
#define MT6358_VA12_OP_EN	0x1a9e
#define MT6358_VAUX18_OP_EN	0x1ab2
#define MT6358_VAUD28_OP_EN	0x1ac6
#define MT6358_VIO28_OP_EN	0x1ada
#define MT6358_VIO18_OP_EN	0x1aee
#define MT6358_VDRAM2_OP_EN	0x1b0a
#define MT6358_VEMC_OP_EN	0x1b1e
#define MT6358_VUSB_OP_EN	0x1b32
#define MT6358_VSRAM_PROC11_OP_EN 0x1b4e
#define MT6358_VSRAM_PROC12_OP_EN 0x1b90
#define MT6358_VSRAM_OTHERS_OP_EN 0x1bae
#define MT6358_VSRAM_GPU_OP_EN	0x1bd0
#define MT6358_VFE28_OP_EN	0x1c0a
#define MT6358_VRF18_OP_EN	0x1c1e
#define MT6358_VRF12_OP_EN	0x1c32
#define MT6358_VEFUSE_OP_EN	0x1c46
#define MT6358_VCN18_OP_EN	0x1c5a
#define MT6358_VCAMA1_OP_EN	0x1c6e
#define MT6358_VCAMA2_OP_EN	0x1c8a
#define MT6358_VCAMD_OP_EN	0x1c9e
#define MT6358_VCAMIO_OP_EN	0x1cb2
#define MT6358_VMC_OP_EN	0x1cc6
#define MT6358_VMCH_OP_EN	0x1cda
#define MT6358_VIBR_OP_EN	0x1d0a
#define MT6358_VCN33_OP_EN	0x1d1e
#define MT6358_VLDO28_OP_EN	0x1d34
#define MT6358_VSIM1_OP_EN	0x1d4a
#define MT6358_VSIM2_OP_EN	0x1d5e
#define MT6358_VCN28_OP_EN	0x1d8a
#define MT6358_VBIF28_OP_EN	0x1da0

#define SLP_SW(rail)		{ MT6358_##rail##_OP_EN, SLP_LP_SW, 0 }
#define SLP_HW(rail, u, c)	{ MT6358_##rail##_OP_EN, SLP_LP_##u, SLP_HW_##c }

/*
 * PMIC_LP_INIT_SETTING, vendor mt6358/v1/pmic_initial_setting.c:138-227, in its order (suspend
 * block, then deep idle block). VPA has no OP_EN register (address 0 in pmic_lp_api.c).
 */
static const struct slp_lp_entry slp_lp_table[] = {
	/* suspend */
	SLP_SW(VPROC11), SLP_SW(VCORE), SLP_SW(VGPU), SLP_SW(VMODEM), SLP_SW(VS1),
	SLP_HW(VS2, SRCLKEN0, LP), SLP_HW(VDRAM1, SRCLKEN0, LP), SLP_SW(VPROC12),
	SLP_SW(VSRAM_GPU), SLP_HW(VSRAM_OTHERS, SRCLKEN0, LP), SLP_SW(VSRAM_PROC11),
	SLP_HW(VXO22, SRCLKEN0, LP), SLP_HW(VRF18, SRCLKEN1, OFF), SLP_HW(VRF12, SRCLKEN1, OFF),
	SLP_SW(VEFUSE), SLP_SW(VCN33), SLP_SW(VCN28), SLP_SW(VCN18), SLP_SW(VCAMA1),
	SLP_SW(VCAMD), SLP_SW(VCAMA2), SLP_SW(VSRAM_PROC12), SLP_SW(VCAMIO), SLP_SW(VLDO28),
	SLP_HW(VA12, SRCLKEN0, LP), SLP_HW(VAUX18, SRCLKEN0, LP), SLP_HW(VAUD28, SRCLKEN0, LP),
	SLP_SW(VIO28), SLP_SW(VIO18), SLP_HW(VFE28, SRCLKEN1, OFF), SLP_HW(VDRAM2, SRCLKEN0, LP),
	SLP_SW(VMC), SLP_SW(VMCH), SLP_SW(VEMC), SLP_SW(VSIM1), SLP_SW(VSIM2), SLP_SW(VIBR),
	SLP_HW(VUSB, SRCLKEN0, LP), SLP_HW(VBIF28, SRCLKEN0, OFF),
	/* deep idle */
	SLP_SW(VPROC11), SLP_SW(VCORE), SLP_SW(VGPU), SLP_SW(VMODEM), SLP_SW(VS1),
	SLP_HW(VS2, SRCLKEN2, LP), SLP_HW(VDRAM1, SRCLKEN2, LP), SLP_SW(VPROC12),
	SLP_SW(VSRAM_GPU), SLP_HW(VSRAM_OTHERS, SRCLKEN2, LP), SLP_SW(VSRAM_PROC11),
	SLP_HW(VXO22, SRCLKEN2, LP), SLP_HW(VRF18, SRCLKEN1, OFF), SLP_HW(VRF12, SRCLKEN1, OFF),
	SLP_SW(VEFUSE), SLP_SW(VCN33), SLP_SW(VCN28), SLP_SW(VCN18), SLP_SW(VCAMA1),
	SLP_SW(VCAMD), SLP_SW(VCAMA2), SLP_SW(VSRAM_PROC12), SLP_SW(VCAMIO), SLP_SW(VLDO28),
	SLP_HW(VA12, SRCLKEN2, LP), SLP_HW(VAUX18, SRCLKEN2, LP), SLP_SW(VAUD28),
	SLP_SW(VIO28), SLP_SW(VIO18), SLP_HW(VFE28, SRCLKEN1, OFF), SLP_HW(VDRAM2, SRCLKEN2, LP),
	SLP_SW(VMC), SLP_SW(VMCH), SLP_SW(VEMC), SLP_SW(VSIM1), SLP_SW(VSIM2), SLP_SW(VIBR),
	SLP_HW(VUSB, SRCLKEN2, LP), SLP_HW(VBIF28, SRCLKEN2, OFF),
};

/* One vendor pmic_lp_type_set() call as register updates; returns the number of ops (1 or 2) */
static inline unsigned int slp_lp_ops(const struct slp_lp_entry *e, struct slp_reg_op *ops)
{
	u16 bit = BIT(e->user);

	ops[0] = (struct slp_reg_op){ e->op_en, bit, bit };
	if (e->user == SLP_LP_SW)
		return 1;
	ops[1] = (struct slp_reg_op){ e->op_en + SLP_LP_OP_CFG_OFS, bit, e->cfg ? bit : 0 };
	return 2;
}

#endif
