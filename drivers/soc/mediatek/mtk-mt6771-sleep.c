// SPDX-License-Identifier: GPL-2.0
/*
 * MT6771 SPM deep sleep around PSCI SYSTEM_SUSPEND.
 *
 * The stock ATF implements SYSTEM_SUSPEND as a power-down state that does not run the SPM
 * suspend program, so the last CPU would wait in WFI for a power cut nobody makes. The same ATF
 * arms and disarms that program through SPM_ARGS SUSPEND / SUSPEND_FINISH. This driver does, from
 * syscore right before SYSTEM_SUSPEND, what the vendor kernel does before its own suspend entry
 * (hw-spec/power.org, System suspend): SSPM notification, MCDI task pause, pcm flags and wake
 * timer, then arms the SPM; syscore resume undoes it in reverse.
 *
 * Nothing happens unless deep_enable is set and the suspend is "deep" (mem_sleep).
 */
#include <linux/arm-smccc.h>
#include <linux/debugfs.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/mfd/syscon.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/soc/mediatek/mtk-mt6771-mcdi.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>

#include <asm/arch_timer.h>

#include "mtk-mt6771-sleep.h"

#define MTK_SIP_SPM_SUSPEND_ARGS	0xc200021a
#define MTK_SIP_SPM_FIRMWARE_STATUS	0xc200021b
#define MTK_SIP_SPM_ARGS		0xc2000228

#define SPM_ARGS_SPMFW_IDX		0
#define SPM_ARGS_SUSPEND		2
#define SPM_ARGS_SUSPEND_FINISH		3
#define SPMFW_LP4X_2CH_3733		0

#define SPM_PCM_REG13_DATA		0x134
#define SPM_PCM_REG15_DATA		0x13c
#define SPM_WAKEUP_STA			0x15c
#define SPM_SW_RSV_0			0x608

/* SSPM mailbox 1: IPI_ID_SPM_SUSPEND is pin 1 of that mailbox, slots 2..9, polled */
#define SSPM_MBOX_IN_IRQ		0x0
#define SSPM_MBOX_OUT_IRQ		0x4
#define SSPM_SPM_SUSPEND_IRQ		BIT(1)
#define SSPM_SPM_SUSPEND_SLOT		2
#define SSPM_ACK_TIMEOUT_US		2000000

struct mt6771_sleep {
	struct device *dev;
	void __iomem *mbox;
	void __iomem *mbox_ctrl;
	struct regmap *spm;
	struct regmap *pmic;
	bool armed;
	bool lp_applied;
	struct slp_reg_op lp_undo[2 * ARRAY_SIZE(slp_lp_table)];
	unsigned int lp_nundo;
	u32 fw_status;
	u32 cycles;
	int last_err;
	u32 wake_r12;
	u32 wake_sta;
	u32 wake_r13;
	u32 wake_r15;
};

static struct mt6771_sleep *slp;
static DEFINE_MUTEX(slp_lock);

static bool deep_enable;
static bool lp_table;
static unsigned int wake_sec = 5401;
static bool infra_pdn;
static bool spm_big_buck;

static unsigned long slp_smc(unsigned long id, unsigned long a1, unsigned long a2,
			     unsigned long a3)
{
	struct arm_smccc_res res;

	arm_smccc_smc(id, a1, a2, a3, 0, 0, 0, 0, &res);
	return res.a0;
}

static u32 slp_spm_read(unsigned int reg)
{
	u32 val = 0;

	regmap_read(slp->spm, reg, &val);
	return val;
}

/* Vendor sspm_ipi_send_sync(IPI_ID_SPM_SUSPEND, IPI_OPT_POLLING, ...); the ack word is an int */
static int slp_sspm_send(u32 cmd)
{
	u32 w[SLP_SSPM_WORDS], out;
	unsigned int i;
	int ret;

	if (readl(slp->mbox_ctrl + SSPM_MBOX_OUT_IRQ) & SSPM_SPM_SUSPEND_IRQ)
		return -EBUSY;

	slp_sspm_payload(w, cmd, sched_clock(), __arch_counter_get_cntvct(), 0);
	for (i = 0; i < SLP_SSPM_WORDS; i++)
		writel(w[i], slp->mbox + (SSPM_SPM_SUSPEND_SLOT + i) * 4);
	writel(SSPM_SPM_SUSPEND_IRQ, slp->mbox_ctrl + SSPM_MBOX_IN_IRQ);

	ret = readl_poll_timeout_atomic(slp->mbox_ctrl + SSPM_MBOX_OUT_IRQ, out,
					out & SSPM_SPM_SUSPEND_IRQ, 1, SSPM_ACK_TIMEOUT_US);
	if (ret)
		return ret;
	writel(SSPM_SPM_SUSPEND_IRQ, slp->mbox_ctrl + SSPM_MBOX_OUT_IRQ);
	ret = (int)readl(slp->mbox + SSPM_SPM_SUSPEND_SLOT * 4);
	return ret < 0 ? ret : 0;
}

static int slp_syscore_suspend(void *data)
{
	u32 flags, flags1, timer;
	int ret;

	if (!READ_ONCE(deep_enable) || pm_suspend_target_state != PM_SUSPEND_MEM)
		return 0;

	slp->cycles++;
	slp->fw_status = slp_smc(MTK_SIP_SPM_FIRMWARE_STATUS, 0, 0, 0);
	if (!slp->fw_status) {
		ret = -EBUSY;
		pr_err("mt6771-sleep: SPM firmware not loaded\n");
		goto out;
	}

	ret = mtk_mt6771_mcdi_task_hold(true);
	if (ret)
		goto out;

	/* Vendor PM notifier sends PREPARE; here it goes with SUSPEND, interrupts already off */
	ret = slp_sspm_send(SLP_SSPM_SUSPEND_PREPARE);
	if (ret) {
		pr_err("mt6771-sleep: SSPM SUSPEND_PREPARE %d\n", ret);
		goto release;
	}
	ret = slp_sspm_send(SLP_SSPM_SUSPEND);
	if (ret) {
		pr_err("mt6771-sleep: SSPM SUSPEND %d\n", ret);
		slp_sspm_send(SLP_SSPM_POST_SUSPEND);
		goto release;
	}

	flags = slp_pcm_flags(READ_ONCE(infra_pdn));
	flags1 = slp_pcm_flags1(READ_ONCE(spm_big_buck));
	timer = slp_timer_val(READ_ONCE(wake_sec));
	slp_smc(MTK_SIP_SPM_ARGS, SPM_ARGS_SPMFW_IDX, SPMFW_LP4X_2CH_3733, 0);
	slp_smc(MTK_SIP_SPM_SUSPEND_ARGS, flags, flags1, timer);
	slp_smc(MTK_SIP_SPM_ARGS, SPM_ARGS_SUSPEND, 0, 0);
	slp->armed = true;
	pr_info("mt6771-sleep: SPM armed, flags 0x%x 0x%x timer %u r15 0x%x\n", flags, flags1,
		timer, slp_spm_read(SPM_PCM_REG15_DATA));
	slp->last_err = 0;
	return 0;

release:
	mtk_mt6771_mcdi_task_hold(false);
out:
	slp->last_err = ret;
	return ret;
}

static void slp_syscore_resume(void *data)
{
	int ret;

	if (!slp->armed)
		return;
	slp->armed = false;

	slp_smc(MTK_SIP_SPM_ARGS, SPM_ARGS_SUSPEND_FINISH, 0, 0);
	/* SW_RSV_0 is the firmware's copy of R12, the wake event bits */
	slp->wake_r12 = slp_spm_read(SPM_SW_RSV_0);
	slp->wake_sta = slp_spm_read(SPM_WAKEUP_STA);
	slp->wake_r13 = slp_spm_read(SPM_PCM_REG13_DATA);
	slp->wake_r15 = slp_spm_read(SPM_PCM_REG15_DATA);

	ret = slp_sspm_send(SLP_SSPM_RESUME);
	if (ret)
		pr_err("mt6771-sleep: SSPM RESUME %d\n", ret);
	ret = slp_sspm_send(SLP_SSPM_POST_SUSPEND);
	if (ret)
		pr_err("mt6771-sleep: SSPM POST_SUSPEND %d\n", ret);
	mtk_mt6771_mcdi_task_hold(false);

	pr_info("mt6771-sleep: woke, r12 0x%x wakeup_sta 0x%x r13 0x%x r15 0x%x\n",
		slp->wake_r12, slp->wake_sta, slp->wake_r13, slp->wake_r15);
}

static const struct syscore_ops slp_syscore_ops = {
	.suspend = slp_syscore_suspend,
	.resume = slp_syscore_resume,
};

static struct syscore slp_syscore = {
	.ops = &slp_syscore_ops,
};

static void slp_lp_restore(void)
{
	/* reverse order and only the bits the table set, so other drivers' changes survive */
	while (slp->lp_nundo) {
		const struct slp_reg_op *u = &slp->lp_undo[--slp->lp_nundo];

		regmap_update_bits(slp->pmic, u->reg, u->mask, u->val);
	}
	slp->lp_applied = false;
}

static int slp_lp_apply(void)
{
	struct slp_reg_op ops[2];
	unsigned int i, k, n, val;
	int ret;

	for (i = 0; i < ARRAY_SIZE(slp_lp_table); i++) {
		n = slp_lp_ops(&slp_lp_table[i], ops);
		for (k = 0; k < n; k++) {
			ret = regmap_read(slp->pmic, ops[k].reg, &val);
			if (!ret)
				ret = regmap_update_bits(slp->pmic, ops[k].reg, ops[k].mask,
							 ops[k].val);
			if (ret) {
				slp_lp_restore();
				return ret;
			}
			slp->lp_undo[slp->lp_nundo++] =
				(struct slp_reg_op){ ops[k].reg, ops[k].mask, val & ops[k].mask };
		}
	}
	slp->lp_applied = true;
	return 0;
}

static void slp_lp_update(void)
{
	bool want;
	int ret;

	mutex_lock(&slp_lock);
	if (!slp)
		goto out;
	want = READ_ONCE(deep_enable) && READ_ONCE(lp_table);
	if (want && !slp->lp_applied) {
		ret = slp_lp_apply();
		if (ret)
			dev_err(slp->dev, "PMIC LP table: %d\n", ret);
		else
			dev_info(slp->dev, "PMIC LP table applied\n");
	} else if (!want && slp->lp_applied) {
		slp_lp_restore();
		dev_info(slp->dev, "PMIC LP table restored\n");
	}
out:
	mutex_unlock(&slp_lock);
}

static int slp_gate_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_bool(val, kp);

	if (!ret)
		slp_lp_update();
	return ret;
}

static const struct kernel_param_ops slp_gate_ops = {
	.set = slp_gate_set,
	.get = param_get_bool,
};

module_param_cb(deep_enable, &slp_gate_ops, &deep_enable, 0644);
MODULE_PARM_DESC(deep_enable, "Arm the SPM for mem_sleep \"deep\" (default 0: no effect)");
module_param_cb(lp_table, &slp_gate_ops, &lp_table, 0644);
MODULE_PARM_DESC(lp_table, "With deep_enable, apply the MT6358 low-power rail table (default 0)");
module_param(wake_sec, uint, 0644);
MODULE_PARM_DESC(wake_sec, "SPM PCM wake timer in seconds (vendor 5401)");
module_param(infra_pdn, bool, 0644);
MODULE_PARM_DESC(infra_pdn, "Let the SPM power INFRA down (vendor 1, default 0)");
module_param(spm_big_buck, bool, 0644);
MODULE_PARM_DESC(spm_big_buck, "Let the SPM switch the big-cluster buck (default 0)");

static int slp_status_show(struct seq_file *s, void *unused)
{
	seq_printf(s, "deep_enable %d lp_table %d lp_applied %d\n", deep_enable, lp_table,
		   slp->lp_applied);
	seq_printf(s, "firmware_status %lu\n", slp_smc(MTK_SIP_SPM_FIRMWARE_STATUS, 0, 0, 0));
	seq_printf(s, "pcm_reg13 0x%08x pcm_reg15 0x%08x sw_rsv_0 0x%08x wakeup_sta 0x%08x\n",
		   slp_spm_read(SPM_PCM_REG13_DATA), slp_spm_read(SPM_PCM_REG15_DATA),
		   slp_spm_read(SPM_SW_RSV_0), slp_spm_read(SPM_WAKEUP_STA));
	seq_printf(s, "sspm_out_irq 0x%08x\n", readl(slp->mbox_ctrl + SSPM_MBOX_OUT_IRQ));
	seq_printf(s, "cycles %u last_err %d\n", slp->cycles, slp->last_err);
	seq_printf(s, "last_wake r12 0x%08x wakeup_sta 0x%08x r13 0x%08x r15 0x%08x\n",
		   slp->wake_r12, slp->wake_sta, slp->wake_r13, slp->wake_r15);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(slp_status);

static struct regmap *slp_pmic_regmap(struct device *dev)
{
	struct device_node *np;
	struct platform_device *pdev;
	struct regmap *map;

	np = of_parse_phandle(dev->of_node, "mediatek,pmic", 0);
	if (!np)
		return ERR_PTR(-ENODEV);
	pdev = of_find_device_by_node(np);
	of_node_put(np);
	if (!pdev)
		return ERR_PTR(-EPROBE_DEFER);
	/* the MT6358 MFD shares the PMIC wrapper's regmap */
	map = pdev->dev.parent ? dev_get_regmap(pdev->dev.parent, NULL) : NULL;
	put_device(&pdev->dev);
	return map ?: ERR_PTR(-EPROBE_DEFER);
}

static int mt6771_sleep_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6771_sleep *s;

	s = devm_kzalloc(dev, sizeof(*s), GFP_KERNEL);
	if (!s)
		return -ENOMEM;
	s->dev = dev;

	s->mbox = devm_platform_ioremap_resource_byname(pdev, "mbox");
	if (IS_ERR(s->mbox))
		return PTR_ERR(s->mbox);
	s->mbox_ctrl = devm_platform_ioremap_resource_byname(pdev, "mbox-ctrl");
	if (IS_ERR(s->mbox_ctrl))
		return PTR_ERR(s->mbox_ctrl);
	s->spm = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,spm");
	if (IS_ERR(s->spm))
		return dev_err_probe(dev, PTR_ERR(s->spm), "SPM syscon\n");
	s->pmic = slp_pmic_regmap(dev);
	if (IS_ERR(s->pmic))
		return dev_err_probe(dev, PTR_ERR(s->pmic), "PMIC regmap\n");

	s->fw_status = slp_smc(MTK_SIP_SPM_FIRMWARE_STATUS, 0, 0, 0);
	mutex_lock(&slp_lock);
	slp = s;
	mutex_unlock(&slp_lock);

	slp_lp_update();
	register_syscore(&slp_syscore);
	debugfs_create_file("mt6771-sleep", 0400, NULL, NULL, &slp_status_fops);
	dev_info(dev, "SPM firmware status %u, r15 0x%x, deep %s\n", s->fw_status,
		 slp_spm_read(SPM_PCM_REG15_DATA), deep_enable ? "enabled" : "gated");
	return 0;
}

static void mt6771_sleep_remove(struct platform_device *pdev)
{
	debugfs_lookup_and_remove("mt6771-sleep", NULL);
	unregister_syscore(&slp_syscore);
	mutex_lock(&slp_lock);
	if (slp->lp_applied)
		slp_lp_restore();
	slp = NULL;
	mutex_unlock(&slp_lock);
}

static const struct of_device_id mt6771_sleep_of_match[] = {
	{ .compatible = "mediatek,mt6771-sleep" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6771_sleep_of_match);

static struct platform_driver mt6771_sleep_driver = {
	.probe = mt6771_sleep_probe,
	.remove = mt6771_sleep_remove,
	.driver = {
		.name = "mt6771-sleep",
		.of_match_table = mt6771_sleep_of_match,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(mt6771_sleep_driver);

MODULE_DESCRIPTION("MediaTek MT6771 SPM deep sleep");
MODULE_LICENSE("GPL");
