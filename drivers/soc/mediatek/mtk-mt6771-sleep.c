// SPDX-License-Identifier: GPL-2.0
/*
 * MT6771 SPM deep sleep through s2idle and cpuidle.
 *
 * The firmware arms and disarms the sleep controller itself when CPU0 enters the PSCI state
 * 0x01010005, so noirq PM callbacks only set up the arguments and talk to the SSPM, then open
 * that cpuidle state (mcdi driver) for CPU0 while CPUs 1-7 are offline. Resume undoes it.
 */
#include <linux/arm-smccc.h>
#include <linux/console.h>
#include <linux/cpu.h>
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
#include <linux/pm_wakeup.h>
#include <linux/regmap.h>
#include <linux/sched/clock.h>
#include <linux/seq_file.h>
#include <linux/soc/mediatek/mtk-mt6771-mcdi.h>
#include <linux/soc/mediatek/mtk-mt6771-spm-start.h>
#include <linux/suspend.h>
#include <linux/usb.h>

#include <asm/arch_timer.h>

#include "mtk-mt6771-sleep.h"

#define MTK_SIP_SPM_SUSPEND_ARGS	0xc200021a
#define MTK_SIP_SPM_FIRMWARE_STATUS	0xc200021b
#define MTK_SIP_SPM_ARGS		0xc2000228

#define SPM_ARGS_SPMFW_IDX		0
#define SPM_ARGS_PCM_WDT		8
#define SPM_PCM_WDT_SEC			30

/* RGU block; mtk_wdt owns it, we touch the SPM request registers and the debug net */
#define RGU_BASE			0x10007000
#define RGU_SIZE			0x40
#define RGU_WDT_MODE			0x0
#define RGU_WDT_LENGTH			0x4
#define RGU_WDT_RESTART			0x8
#define RGU_WDT_MODE_KEY		0x22000000
#define RGU_WDT_MODE_EN			BIT(0)
#define RGU_WDT_MODE_DUAL		BIT(6)
#define RGU_WDT_LENGTH_31S		((0x7c0 << 5) | 0x08)
#define RGU_WDT_RESTART_KEY		0x1971
#define RGU_REQ_MODE			0x30
#define RGU_REQ_IRQ_EN			0x34
#define RGU_REQ_MODE_KEY		0x33000000
#define RGU_REQ_IRQ_KEY			0x44000000
#define RGU_REQ_SPM_WDT			BIT(1)
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
	void __iomem *rgu;
	u32 rgu_mode;
	u32 rgu_irq;
	u32 rgu_mode_set;
	u32 rgu_irq_set;
	struct regmap *spm;
	struct regmap *pmic;
	struct notifier_block pm_nb;
	struct cpumask offlined;
	bool infra_ok;
	bool infra_last;
	const char *infra_why;
	bool armed;
	bool wdt_armed;
	u32 wdt_mode;
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
	u32 skipped;
};

static struct mt6771_sleep *slp;
static DEFINE_MUTEX(slp_lock);

static bool deep_enable;
static bool lp_table;
/* bring-up value; the vendor uses 5401 */
static unsigned int wake_sec = 30;
static int infra_pdn = 1;
static bool spm_big_buck;
static bool spm_wdt_irq;
static bool wdt_net;
static unsigned int skip;

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

/*
 * Vendor: PCM watchdog 30 s plus the RGU SPM request in reset mode around sleep
 * (spm_v4/mtk_spm_sleep.c:332-338, 417-424). spm_wdt_irq turns an expiry into an
 * RGU interrupt instead of a reset.
 */
static void slp_rgu_spm_wdt(bool sleep)
{
	u32 mode, irq;

	if (sleep) {
		slp->rgu_mode = readl(slp->rgu + RGU_REQ_MODE);
		slp->rgu_irq = readl(slp->rgu + RGU_REQ_IRQ_EN);
		mode = slp->rgu_mode | RGU_REQ_SPM_WDT;
		irq = READ_ONCE(spm_wdt_irq) ? slp->rgu_irq | RGU_REQ_SPM_WDT :
					      slp->rgu_irq & ~RGU_REQ_SPM_WDT;
	} else {
		mode = slp->rgu_mode;
		irq = slp->rgu_irq;
	}
	writel(RGU_REQ_IRQ_KEY | (irq & 0xffffff), slp->rgu + RGU_REQ_IRQ_EN);
	writel(RGU_REQ_MODE_KEY | (mode & 0xffffff), slp->rgu + RGU_REQ_MODE);
	if (sleep) {
		slp->rgu_mode_set = readl(slp->rgu + RGU_REQ_MODE);
		slp->rgu_irq_set = readl(slp->rgu + RGU_REQ_IRQ_EN);
	}
}

#if IS_REACHABLE(CONFIG_USB)
static int slp_count_usb_dev(struct usb_device *udev, void *data)
{
	if (udev->parent)
		(*(unsigned int *)data)++;
	return 0;
}
#endif

/* A USB host loses its state when INFRA powers down, so any attached device blocks it */
static bool mt6771_sleep_infra_pdn_ok(const char **why)
{
#if IS_REACHABLE(CONFIG_USB)
	unsigned int count = 0;

	usb_for_each_dev(&count, slp_count_usb_dev);
	if (count) {
		*why = "usb device attached";
		return false;
	}
#endif
	if (!console_suspend_enabled) {
		*why = "console stays awake";
		return false;
	}
	*why = "idle";
	return true;
}

/* usb_for_each_dev() sleeps, so the decision is taken here and only read in noirq */
static int mt6771_sleep_prepare(struct device *dev)
{
	if (READ_ONCE(infra_pdn) == 1)
		slp->infra_ok = mt6771_sleep_infra_pdn_ok(&slp->infra_why);
	return 0;
}

static int mt6771_sleep_suspend_noirq(struct device *dev)
{
	u32 flags, flags1, timer;
	int ret, mode;

	if (!READ_ONCE(deep_enable))
		return 0;
	if (pm_suspend_target_state != PM_SUSPEND_TO_IDLE)
		return 0;
	if (num_online_cpus() != 1) {
		dev_err(dev, "deep sleep needs CPU1-7 offline\n");
		return -EBUSY;
	}
	/* The firmware hook only re-parameterises a running PCM; without one CPU0 never wakes */
	if (!slp_spm_read(SPM_PCM_REG15_DATA)) {
		pr_warn_once("mt6771-sleep: SPM program not running (PCM_REG15 0), not arming\n");
		slp->last_err = -ENXIO;
		return 0;
	}

	slp->cycles++;
	slp->fw_status = slp_smc(MTK_SIP_SPM_FIRMWARE_STATUS, 0, 0, 0);
	pr_emerg("mt6771-sleep: bc1 fw %u\n", slp->fw_status);
	if (!slp->fw_status) {
		ret = -EBUSY;
		pr_err("mt6771-sleep: SPM firmware not loaded\n");
		goto out;
	}

	slp->skipped = READ_ONCE(skip);
	if (!(slp->skipped & 1)) {
		ret = mtk_mt6771_mcdi_task_hold(true);
		pr_emerg("mt6771-sleep: bc2 mcdi held\n");
	} else {
		ret = 0;
		pr_emerg("mt6771-sleep: bc2 mcdi held (skipped)\n");
	}
	if (ret)
		goto out;

	if (!(slp->skipped & 2)) {
		ret = slp_sspm_send(SLP_SSPM_SUSPEND_PREPARE);
		pr_emerg("mt6771-sleep: bc3 sspm prepare %d\n", ret);
	} else {
		ret = 0;
		pr_emerg("mt6771-sleep: bc3 sspm prepare 0 (skipped)\n");
	}
	if (ret) {
		pr_err("mt6771-sleep: SSPM SUSPEND_PREPARE %d\n", ret);
		goto release;
	}
	if (!(slp->skipped & 2)) {
		ret = slp_sspm_send(SLP_SSPM_SUSPEND);
		pr_emerg("mt6771-sleep: bc4 sspm suspend %d\n", ret);
	} else {
		ret = 0;
		pr_emerg("mt6771-sleep: bc4 sspm suspend 0 (skipped)\n");
	}
	if (ret) {
		pr_err("mt6771-sleep: SSPM SUSPEND %d\n", ret);
		slp_sspm_send(SLP_SSPM_POST_SUSPEND);
		goto release;
	}

	mode = READ_ONCE(infra_pdn);
	if (mode == 1) {
		slp->infra_last = slp->infra_ok;
	} else {
		slp->infra_last = mode == 2;
		slp->infra_why = mode == 2 ? "forced" : "disabled";
	}
	flags = slp_pcm_flags(slp->infra_last);
	flags1 = slp_pcm_flags1(READ_ONCE(spm_big_buck));
	timer = slp_timer_val(READ_ONCE(wake_sec));
	if (!(slp->skipped & 16)) {
		slp_rgu_spm_wdt(true);
		pr_emerg("mt6771-sleep: bc5 rgu mode 0x%08x irq 0x%08x\n", slp->rgu_mode_set, slp->rgu_irq_set);
	} else {
		pr_emerg("mt6771-sleep: bc5 rgu mode 0x%08x irq 0x%08x (skipped)\n", slp->rgu_mode_set, slp->rgu_irq_set);
	}
	if (!(slp->skipped & 4)) {
		slp_smc(MTK_SIP_SPM_ARGS, SPM_ARGS_SPMFW_IDX, SPMFW_LP4X_2CH_3733, 0);
		slp_smc(MTK_SIP_SPM_ARGS, SPM_ARGS_PCM_WDT, 1, SPM_PCM_WDT_SEC);
		slp_smc(MTK_SIP_SPM_SUSPEND_ARGS, flags, flags1, timer);
	}
	if (slp->skipped & 4)
		pr_emerg("mt6771-sleep: bc6 armed flags 0x%x 0x%x timer %u (skipped)\n", flags, flags1, timer);
	else
		pr_emerg("mt6771-sleep: bc6 armed flags 0x%x 0x%x timer %u\n", flags, flags1, timer);
	if (!(slp->skipped & 8))
		mtk_mt6771_mcdi_suspend_state(true);
	slp->armed = true;
	pr_info("mt6771-sleep: SPM armed, flags 0x%x 0x%x timer %u r15 0x%x\n", flags, flags1,
		timer, slp_spm_read(SPM_PCM_REG15_DATA));
	slp->last_err = 0;
	if (READ_ONCE(wdt_net)) {
		slp->wdt_mode = readl(slp->rgu + RGU_WDT_MODE);
		writel(RGU_WDT_LENGTH_31S, slp->rgu + RGU_WDT_LENGTH);
		writel(RGU_WDT_RESTART_KEY, slp->rgu + RGU_WDT_RESTART);
		writel(RGU_WDT_MODE_KEY | (((slp->wdt_mode & ~RGU_WDT_MODE_DUAL) | RGU_WDT_MODE_EN) &
					   0xffffff), slp->rgu + RGU_WDT_MODE);
		slp->wdt_armed = true;
		pr_emerg("mt6771-sleep: bc6b wdt net armed, mode 0x%x\n", slp->wdt_mode);
	}
	return 0;

release:
	if (!(slp->skipped & 1))
		mtk_mt6771_mcdi_task_hold(false);
out:
	slp->last_err = ret;
	return ret;
}

static int mt6771_sleep_resume_noirq(struct device *dev)
{
	int ret = 0;

	if (!slp->armed)
		return 0;
	slp->armed = false;
	if (slp->wdt_armed) {
		writel(RGU_WDT_RESTART_KEY, slp->rgu + RGU_WDT_RESTART);
		slp->wdt_armed = false;
	}
	if (!(slp->skipped & 8))
		mtk_mt6771_mcdi_suspend_state(false);

	if (!(slp->skipped & 4))
		slp_smc(MTK_SIP_SPM_ARGS, SPM_ARGS_PCM_WDT, 0, 0);
	if (!(slp->skipped & 16))
		slp_rgu_spm_wdt(false);
	/* SW_RSV_0 is the firmware's copy of R12, the wake event bits */
	slp->wake_r12 = slp_spm_read(SPM_SW_RSV_0);
	slp->wake_sta = slp_spm_read(SPM_WAKEUP_STA);
	slp->wake_r13 = slp_spm_read(SPM_PCM_REG13_DATA);
	slp->wake_r15 = slp_spm_read(SPM_PCM_REG15_DATA);
	pr_emerg("mt6771-sleep: bc7 resume r12 0x%x sta 0x%x\n", slp->wake_r12, slp->wake_sta);
	/* The PCM timer has no Linux interrupt; without this s2idle would re-enter sleep */
	if (slp->wake_r12)
		pm_system_wakeup();

	if (!(slp->skipped & 2)) {
		ret = slp_sspm_send(SLP_SSPM_RESUME);
		if (ret)
			pr_err("mt6771-sleep: SSPM RESUME %d\n", ret);
	} else {
		pr_emerg("mt6771-sleep: bc8 sspm resume 0 (skipped)\n");
	}
	if (!(slp->skipped & 2)) {
		ret = slp_sspm_send(SLP_SSPM_POST_SUSPEND);
		if (ret)
			pr_err("mt6771-sleep: SSPM POST_SUSPEND %d\n", ret);
	} else {
		pr_emerg("mt6771-sleep: bc9 sspm post_suspend 0 (skipped)\n");
	}
	if (!(slp->skipped & 1))
		mtk_mt6771_mcdi_task_hold(false);

	pr_info("mt6771-sleep: woke, r12 0x%x wakeup_sta 0x%x r13 0x%x r15 0x%x\n",
		slp->wake_r12, slp->wake_sta, slp->wake_r13, slp->wake_r15);
	return 0;
}

/* CPUs cannot be unplugged from noirq context, so the notifier does it before the freeze */
static void slp_cpus_online(struct mt6771_sleep *s)
{
	unsigned int cpu;
	int ret;

	for_each_cpu(cpu, &s->offlined) {
		ret = add_cpu(cpu);
		if (ret)
			dev_warn(s->dev, "CPU%u did not come back: %d\n", cpu, ret);
	}
	cpumask_clear(&s->offlined);
}

static int slp_pm_notify(struct notifier_block *nb, unsigned long action, void *data)
{
	struct mt6771_sleep *s = container_of(nb, struct mt6771_sleep, pm_nb);
	unsigned int cpu;
	int ret;

	switch (action) {
	case PM_SUSPEND_PREPARE:
		if (!READ_ONCE(deep_enable) || !pm_suspend_default_s2idle())
			return NOTIFY_DONE;
		for_each_online_cpu(cpu) {
			if (!cpu)
				continue;
			ret = remove_cpu(cpu);
			if (ret) {
				dev_err(s->dev, "cannot offline CPU%u: %d\n", cpu, ret);
				slp_cpus_online(s);
				return NOTIFY_BAD;
			}
			cpumask_set_cpu(cpu, &s->offlined);
		}
		return NOTIFY_OK;
	case PM_POST_SUSPEND:
		slp_cpus_online(s);
		return NOTIFY_OK;
	}
	return NOTIFY_DONE;
}

static const struct dev_pm_ops mt6771_sleep_pm_ops = {
	.prepare = pm_sleep_ptr(mt6771_sleep_prepare),
	.suspend_noirq = pm_sleep_ptr(mt6771_sleep_suspend_noirq),
	.resume_noirq = pm_sleep_ptr(mt6771_sleep_resume_noirq),
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

/* Enabling starts the SPM program first; a failure leaves deep sleep off */
static int slp_deep_set(const char *val, const struct kernel_param *kp)
{
	bool on;
	int ret = kstrtobool(val, &on);

	if (ret)
		return ret;
	if (on && READ_ONCE(slp)) {
		mutex_lock(&slp_lock);
		ret = slp ? mtk_mt6771_spm_start(slp->spm) : 0;
		mutex_unlock(&slp_lock);
		if (ret)
			return ret;
	}
	WRITE_ONCE(deep_enable, on);
	slp_lp_update();
	return 0;
}

static const struct kernel_param_ops slp_deep_ops = {
	.set = slp_deep_set,
	.get = param_get_bool,
};

module_param_cb(deep_enable, &slp_deep_ops, &deep_enable, 0644);
MODULE_PARM_DESC(deep_enable, "Enter the SPM suspend state during s2idle (default 0: no effect)");
module_param_cb(lp_table, &slp_gate_ops, &lp_table, 0644);
MODULE_PARM_DESC(lp_table, "With deep_enable, apply the MT6358 low-power rail table (default 0)");
module_param(wake_sec, uint, 0644);
MODULE_PARM_DESC(wake_sec, "SPM PCM wake timer in seconds (vendor 5401)");
module_param(infra_pdn, int, 0644);
MODULE_PARM_DESC(infra_pdn, "SPM INFRA power-down: 0 never, 1 auto (not with a USB device or an awake console), 2 force (default 1)");
module_param(spm_big_buck, bool, 0644);
MODULE_PARM_DESC(spm_big_buck, "Let the SPM switch the big-cluster buck (default 0)");
module_param(spm_wdt_irq, bool, 0644);
MODULE_PARM_DESC(spm_wdt_irq, "SPM watchdog expiry in sleep raises an RGU IRQ instead of a reset (default off: reset mode like Android)");
module_param(wdt_net, bool, 0644);
MODULE_PARM_DESC(wdt_net, "debug: keep the RGU watchdog running (single stage, about 31 s) across a deep sleep");
module_param(skip, uint, 0644);
MODULE_PARM_DESC(skip, "debug: skip suspend steps: 1 MCDI hold, 2 SSPM messages, 4 SUSPEND_ARGS smc, 8 SPM idle state, 16 RGU request");

static int slp_status_show(struct seq_file *s, void *unused)
{
	seq_printf(s, "deep_enable %d lp_table %d lp_applied %d\n", deep_enable, lp_table,
		   slp->lp_applied);
	seq_printf(s, "firmware_status %lu\n", slp_smc(MTK_SIP_SPM_FIRMWARE_STATUS, 0, 0, 0));
	seq_printf(s, "rgu_mode 0x%08x rgu_irq_en 0x%08x\n", readl(slp->rgu + RGU_REQ_MODE),
		   readl(slp->rgu + RGU_REQ_IRQ_EN));
	seq_printf(s, "rgu_mode_set 0x%08x rgu_irq_set 0x%08x\n", slp->rgu_mode_set, slp->rgu_irq_set);
	seq_printf(s, "pcm_reg13 0x%08x pcm_reg15 0x%08x sw_rsv_0 0x%08x wakeup_sta 0x%08x\n",
		   slp_spm_read(SPM_PCM_REG13_DATA), slp_spm_read(SPM_PCM_REG15_DATA),
		   slp_spm_read(SPM_SW_RSV_0), slp_spm_read(SPM_WAKEUP_STA));
	seq_printf(s, "sspm_out_irq 0x%08x\n", readl(slp->mbox_ctrl + SSPM_MBOX_OUT_IRQ));
	seq_printf(s, "cycles %u last_err %d\n", slp->cycles, slp->last_err);
	seq_printf(s, "infra_pdn mode %d last %d (%s)\n", infra_pdn, slp->infra_last,
		   slp->infra_why ?: "none");
	seq_printf(s, "skip 0x%x wdt_net %d\n", skip, wdt_net);
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
	int ret;

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
	s->rgu = devm_ioremap(dev, RGU_BASE, RGU_SIZE);
	if (!s->rgu)
		return -ENOMEM;
	s->spm = syscon_regmap_lookup_by_phandle(dev->of_node, "mediatek,spm");
	if (IS_ERR(s->spm))
		return dev_err_probe(dev, PTR_ERR(s->spm), "SPM syscon\n");
	s->pmic = slp_pmic_regmap(dev);
	if (IS_ERR(s->pmic))
		return dev_err_probe(dev, PTR_ERR(s->pmic), "PMIC regmap\n");

	s->fw_status = slp_smc(MTK_SIP_SPM_FIRMWARE_STATUS, 0, 0, 0);
	s->pm_nb.notifier_call = slp_pm_notify;
	ret = register_pm_notifier(&s->pm_nb);
	if (ret)
		return ret;
	mutex_lock(&slp_lock);
	slp = s;
	mutex_unlock(&slp_lock);

	if (deep_enable && mtk_mt6771_spm_start(s->spm)) {
		dev_err(dev, "SPM program did not start, deep sleep stays off\n");
		WRITE_ONCE(deep_enable, false);
	}
	slp_lp_update();
	debugfs_create_file("mt6771-sleep", 0400, NULL, NULL, &slp_status_fops);
	dev_info(dev, "SPM firmware status %u, r15 0x%x, deep %s\n", s->fw_status,
		 slp_spm_read(SPM_PCM_REG15_DATA), deep_enable ? "enabled" : "gated");
	return 0;
}

static void mt6771_sleep_remove(struct platform_device *pdev)
{
	unregister_pm_notifier(&slp->pm_nb);
	debugfs_lookup_and_remove("mt6771-sleep", NULL);
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
		.pm = &mt6771_sleep_pm_ops,
	},
};
module_platform_driver(mt6771_sleep_driver);

MODULE_DESCRIPTION("MediaTek MT6771 SPM deep sleep");
MODULE_LICENSE("GPL");
