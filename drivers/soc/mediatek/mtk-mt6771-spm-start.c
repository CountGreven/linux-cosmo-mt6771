// SPDX-License-Identifier: GPL-2.0
/*
 * Start the MT6771 SPM program (PCM) through the secure firmware.
 *
 * Android does this at late_initcall from spm_vcorefs_init(): SPMFW_IDX, the VCOREFS PWRAP
 * voltage selects, INIT and GO (spm_v4/mtk_spm.c:669-671, mtk_spm_vcorefs.c:762-784, :690-698).
 * Everything the firmware does at suspend (PSCI 0x01010005) assumes the program is running.
 */
#include <linux/arm-smccc.h>
#include <linux/bits.h>
#include <linux/export.h>
#include <linux/module.h>
#include <linux/printk.h>
#include <linux/regmap.h>
#include <linux/soc/mediatek/mtk-mt6771-spm-start.h>

#define SPM_PCM_REG15_DATA		0x013c	/* non-zero while the SPM firmware runs */

#define MTK_SIP_KERNEL_SPM_VCOREFS_ARGS	0xc2000220
#define MTK_SIP_KERNEL_SPM_ARGS		0xc2000228
#define SPM_ARGS_SPMFW_IDX		0
#define SPMFW_LP4X_2CH_3733		0
#define VCOREFS_SMC_CMD_INIT		0	/* argument: the current operating point */
#define VCOREFS_SMC_CMD_GO		1	/* argument: the flags below */
#define VCOREFS_SMC_CMD_PWRAP		3	/* arguments: slot, PMIC vcore selector */
#define SPM_FLAG_RUN_COMMON_SCENARIO	BIT(10)
#define SPM_FLAG_DISABLE_MMSYS_DVFS	BIT(15)
/* spm_vcorefs_pwarp_cmd(): (uV - 500000 + 6249) / 6250 for opp 3 (725 mV) and opp 0 (800 mV) */
#define VCOREFS_PMIC_VSEL_0725		0x24
#define VCOREFS_PMIC_VSEL_0800		0x30

#define SPM_START_POLL_US		1000
#define SPM_START_TIMEOUT_US		1000000

/* Idempotent: returns 0 at once when the program already runs. Process context. */
int mtk_mt6771_spm_start(struct regmap *spm)
{
	struct arm_smccc_res res;
	u32 v;

	regmap_read(spm, SPM_PCM_REG15_DATA, &v);
	if (v)
		return 0;

	arm_smccc_smc(MTK_SIP_KERNEL_SPM_ARGS, SPM_ARGS_SPMFW_IDX, SPMFW_LP4X_2CH_3733, 0,
		      0, 0, 0, 0, &res);
	arm_smccc_smc(MTK_SIP_KERNEL_SPM_VCOREFS_ARGS, VCOREFS_SMC_CMD_PWRAP, 0,
		      VCOREFS_PMIC_VSEL_0725, 0, 0, 0, 0, &res);
	arm_smccc_smc(MTK_SIP_KERNEL_SPM_VCOREFS_ARGS, VCOREFS_SMC_CMD_PWRAP, 1,
		      VCOREFS_PMIC_VSEL_0800, 0, 0, 0, 0, &res);
	arm_smccc_smc(MTK_SIP_KERNEL_SPM_VCOREFS_ARGS, VCOREFS_SMC_CMD_INIT, 0, 0,
		      0, 0, 0, 0, &res);
	arm_smccc_smc(MTK_SIP_KERNEL_SPM_VCOREFS_ARGS, VCOREFS_SMC_CMD_GO,
		      SPM_FLAG_RUN_COMMON_SCENARIO | SPM_FLAG_DISABLE_MMSYS_DVFS, 0,
		      0, 0, 0, 0, &res);
	if (regmap_read_poll_timeout(spm, SPM_PCM_REG15_DATA, v, v, SPM_START_POLL_US,
				     SPM_START_TIMEOUT_US)) {
		pr_err("mt6771-spm: the firmware did not start\n");
		return -ETIMEDOUT;
	}
	pr_info("mt6771-spm: firmware started (r15 %#x)\n", v);
	return 0;
}
EXPORT_SYMBOL_GPL(mtk_mt6771_spm_start);

MODULE_DESCRIPTION("MediaTek MT6771 SPM program start");
MODULE_LICENSE("GPL");
