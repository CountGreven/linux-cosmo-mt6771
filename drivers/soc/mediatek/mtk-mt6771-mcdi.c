// SPDX-License-Identifier: GPL-2.0
/*
 * MT6771 SSPM multi-core deep idle (MCDI) mailbox.
 *
 * The stock ATF handles PSCI CPU_SUSPEND 0x00010001 by arming the SSPM and parking the core in WFI;
 * the SSPM MCDI task cuts and restores the core's power. The vendor kernel tells that task which
 * cores it manages (AVAIL_CPU_MASK = online cores) before any core-off entry and after every
 * hotplug, and pauses the task around deep suspend. This driver does the same, and keeps the DT's
 * deep idle states disabled per CPU until the idle_cpus parameter allows them: a core the SSPM
 * does not serve never leaves the ATF's WFI.
 */
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/cpuidle.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/seq_file.h>
#include <linux/soc/mediatek/mtk-mt6771-mcdi.h>
#include <linux/spinlock.h>
#include <linux/suspend.h>
#include <linux/syscore_ops.h>
#include <linux/workqueue.h>

#include "mtk-mt6771-mcdi.h"

#define MCDI_PAUSE_TIMEOUT_US	10000

static void __iomem *mcdi_mbox;
static bool mcdi_ready;
static bool mcdi_gate_live;
static DEFINE_MUTEX(mcdi_gate_lock);

/* Held at 0 from subsys_initcall until the per-CPU state gate is in place, and during hotplug */
static struct pm_qos_request mcdi_qos;
static unsigned int mcdi_paused;
static DEFINE_MUTEX(mcdi_pause_lock);

static unsigned int idle_cpus;

/* SSPM task pause holders: this driver's syscore and the deep sleep driver */
static DEFINE_RAW_SPINLOCK(mcdi_hold_lock);
static unsigned int mcdi_holds;
static bool mcdi_suspend_open;
static bool mcdi_task_held;

static u32 mcdi_read(unsigned int slot)
{
	return readl(mcdi_mbox + slot * 4);
}

static void mcdi_write(unsigned int slot, u32 val)
{
	writel(val, mcdi_mbox + slot * 4);
}

static u32 mcdi_state_param(unsigned int cpu, int idx)
{
	struct device_node *cpu_np, *state_np;
	u32 param = MCDI_PSCI_PARAM_NONE;

	cpu_np = of_get_cpu_node(cpu, NULL);
	if (!cpu_np)
		return param;
	/* cpuidle-psci state idx is cpu-idle-states entry idx - 1; state 0 is WFI */
	state_np = of_parse_phandle(cpu_np, "cpu-idle-states", idx - 1);
	if (state_np && of_property_read_u32(state_np, "arm,psci-suspend-param", &param))
		param = MCDI_PSCI_PARAM_NONE;
	of_node_put(state_np);
	of_node_put(cpu_np);
	return param;
}

static void mcdi_apply_gate(void)
{
	unsigned int cpu;

	mutex_lock(&mcdi_gate_lock);
	if (!mcdi_gate_live)
		goto out;

	cpuidle_pause_and_lock();
	for_each_possible_cpu(cpu) {
		struct cpuidle_device *dev = per_cpu(cpuidle_devices, cpu);
		struct cpuidle_driver *drv = dev ? cpuidle_get_cpu_driver(dev) : NULL;
		int i;

		if (!drv)
			continue;
		for (i = 1; i < drv->state_count; i++) {
			if (mcdi_ready &&
			    mcdi_state_allowed(mcdi_state_param(cpu, i), cpu, idle_cpus,
					       mcdi_suspend_open, READ_ONCE(mcdi_task_held)))
				dev->states_usage[i].disable &= ~CPUIDLE_STATE_DISABLED_BY_DRIVER;
			else
				dev->states_usage[i].disable |= CPUIDLE_STATE_DISABLED_BY_DRIVER;
		}
	}
	cpuidle_resume_and_unlock();
out:
	mutex_unlock(&mcdi_gate_lock);
}

static void mcdi_gate_work_fn(struct work_struct *work)
{
	mcdi_apply_gate();
}
static DECLARE_WORK(mcdi_gate_work, mcdi_gate_work_fn);

static int idle_cpus_set(const char *val, const struct kernel_param *kp)
{
	int ret = param_set_uint(val, kp);

	/* a cmdline value arrives before any lock is usable; the gate init applies it */
	if (!ret && READ_ONCE(mcdi_gate_live))
		mcdi_apply_gate();
	return ret;
}

static int idle_cpus_get(char *buf, const struct kernel_param *kp)
{
	return sysfs_emit(buf, "0x%02x\n", *(unsigned int *)kp->arg);
}

static const struct kernel_param_ops idle_cpus_ops = {
	.set = idle_cpus_set,
	.get = idle_cpus_get,
};
module_param_cb(idle_cpus, &idle_cpus_ops, &idle_cpus, 0644);
MODULE_PARM_DESC(idle_cpus, "CPUs allowed into core-off idle (bit mask, default none)");

static void mcdi_pause(bool pause)
{
	mutex_lock(&mcdi_pause_lock);
	if (pause ? mcdi_paused++ == 0 : --mcdi_paused == 0)
		cpu_latency_qos_update_request(&mcdi_qos, pause ? 0 : PM_QOS_DEFAULT_VALUE);
	mutex_unlock(&mcdi_pause_lock);
}

static void mcdi_update_avail(void)
{
	mcdi_write(MCDI_SLOT_AVAIL_CPU_MASK, mcdi_avail_mask(cpumask_bits(cpu_online_mask)[0]));
}

/* Vendor order: pause from UP/DOWN_PREPARE, update the mask and resume after ONLINE/DEAD */
static int mcdi_cpu_up_prepare(unsigned int cpu)
{
	mcdi_pause(true);
	return 0;
}

static int mcdi_cpu_dead(unsigned int cpu)
{
	mcdi_update_avail();
	mcdi_pause(false);
	return 0;
}

static int mcdi_cpu_online(unsigned int cpu)
{
	mcdi_update_avail();
	mcdi_pause(false);
	return 0;
}

static int mcdi_cpu_down_prepare(unsigned int cpu)
{
	mcdi_pause(true);
	return 0;
}

static int mcdi_task_pause(u32 pause)
{
	u32 ack;

	mcdi_write(MCDI_SLOT_PAUSE_ACTION, pause);
	return readl_poll_timeout_atomic(mcdi_mbox + MCDI_SLOT_PAUSE_ACK * 4, ack, ack == pause,
					 1, MCDI_PAUSE_TIMEOUT_US);
}

/*
 * Pause the SSPM MCDI task (vendor mcdi_task_pause) while any holder needs it paused; the task
 * resumes when the last holder lets go. Syscore context: last CPU, interrupts off.
 */
int mtk_mt6771_mcdi_task_hold(bool hold)
{
	unsigned long flags;
	bool changed;
	int ret = 0;

	if (!READ_ONCE(mcdi_ready))
		return -ENODEV;

	raw_spin_lock_irqsave(&mcdi_hold_lock, flags);
	if (hold) {
		if (!mcdi_holds) {
			ret = mcdi_task_pause(1);
			if (ret)
				mcdi_write(MCDI_SLOT_PAUSE_ACTION, 0);
		}
		if (!ret)
			mcdi_holds++;
	} else if (mcdi_holds && !--mcdi_holds) {
		ret = mcdi_task_pause(0);
	}
	changed = READ_ONCE(mcdi_task_held) != !!mcdi_holds;
	WRITE_ONCE(mcdi_task_held, !!mcdi_holds);
	raw_spin_unlock_irqrestore(&mcdi_hold_lock, flags);

	/* Core-off states stay closed while the task is paused; syscore cannot sleep to apply it */
	if (changed) {
		if (irqs_disabled() || !preemptible())
			schedule_work(&mcdi_gate_work);
		else
			mcdi_apply_gate();
	}

	if (ret)
		pr_err("mt6771-mcdi: SSPM did not ack %s\n", hold ? "pause" : "resume");
	return ret;
}
EXPORT_SYMBOL_GPL(mtk_mt6771_mcdi_task_hold);

/* Open or close the SPM suspend idle state (PSCI param 0x01010005) on CPU0; process context */
void mtk_mt6771_mcdi_suspend_state(bool open)
{
	WRITE_ONCE(mcdi_suspend_open, open);
	mcdi_apply_gate();
}
EXPORT_SYMBOL_GPL(mtk_mt6771_mcdi_suspend_state);

/* Deep suspend only, like the vendor's slp_suspend_ops_enter; s2idle never reaches syscore */
static int mcdi_syscore_suspend(void *data)
{
	if (pm_suspend_target_state != PM_SUSPEND_MEM)
		return 0;
	return mtk_mt6771_mcdi_task_hold(true);
}

static void mcdi_syscore_resume(void *data)
{
	if (pm_suspend_target_state == PM_SUSPEND_MEM)
		mtk_mt6771_mcdi_task_hold(false);
}

static const struct syscore_ops mcdi_syscore_ops = {
	.suspend = mcdi_syscore_suspend,
	.resume = mcdi_syscore_resume,
};

static struct syscore mcdi_syscore = {
	.ops = &mcdi_syscore_ops,
};

static int mcdi_mbox_show(struct seq_file *s, void *unused)
{
	unsigned int i;

	for (i = 0; i < MCDI_NR_SLOTS; i++)
		seq_printf(s, "%2u: 0x%08x\n", i, mcdi_read(i));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(mcdi_mbox);

static int mt6771_mcdi_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct resource *res;
	u32 stat, ack, avail;
	int prepare_state, ret = 0;

	mcdi_mbox = devm_platform_ioremap_resource_byname(pdev, "mbox");
	if (IS_ERR(mcdi_mbox))
		return PTR_ERR(mcdi_mbox);

	/* A reset while the task was paused leaves PAUSE_ACTION set in the SSPM's mailbox */
	if (mcdi_read(MCDI_SLOT_PAUSE_ACTION)) {
		if (!mcdi_task_pause(0))
			readl_poll_timeout_atomic(mcdi_mbox + MCDI_SLOT_ACTION_STAT * 4, stat,
						  mcdi_task_ready(stat, 0), 1,
						  MCDI_PAUSE_TIMEOUT_US);
		dev_info(dev, "cleared a stale MCDI pause request, task %u\n",
			 mcdi_read(MCDI_SLOT_ACTION_STAT));
	}
	stat = mcdi_read(MCDI_SLOT_ACTION_STAT);
	ack = mcdi_read(MCDI_SLOT_PAUSE_ACK);
	if (!mcdi_task_ready(stat, ack))
		return dev_err_probe(dev, -ENODEV, "SSPM MCDI task not running: action %u ack %u\n",
				     stat, ack);

	/* The vendor clears the SSPM's MCDI statistics area past its 8-byte header */
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "sysram");
	if (res && resource_size(res) > 8) {
		void __iomem *sysram = devm_ioremap_resource(dev, res);

		if (IS_ERR(sysram))
			return PTR_ERR(sysram);
		memset_io(sysram + 8, 0, resource_size(res) - 8);
	}

	cpus_read_lock();
	/* vendor mcdi_governor_init order: AVAIL_CPU_MASK, BUCK_POWER_OFF_MASK, ISOLATION_MASK */
	mcdi_update_avail();
	mcdi_write(MCDI_SLOT_BUCK_POWER_OFF_MASK, 0);
	mcdi_write(MCDI_SLOT_CPU_ISOLATION_MASK, 0);
	avail = mcdi_avail_mask(cpumask_bits(cpu_online_mask)[0]);

	if (mcdi_read(MCDI_SLOT_AVAIL_CPU_MASK) != avail) {
		ret = -EIO;
		goto unlock;
	}
	prepare_state = cpuhp_setup_state_nocalls_cpuslocked(CPUHP_BP_PREPARE_DYN,
							     "soc/mt6771-mcdi:prepare",
							     mcdi_cpu_up_prepare, mcdi_cpu_dead);
	if (prepare_state < 0) {
		ret = prepare_state;
		goto unlock;
	}
	ret = cpuhp_setup_state_nocalls_cpuslocked(CPUHP_AP_ONLINE_DYN, "soc/mt6771-mcdi:online",
						   mcdi_cpu_online, mcdi_cpu_down_prepare);
	if (ret < 0)
		cpuhp_remove_state_nocalls_cpuslocked(prepare_state);
unlock:
	cpus_read_unlock();
	if (ret == -EIO)
		return dev_err_probe(dev, ret, "AVAIL_CPU_MASK reads 0x%x, wrote 0x%x\n",
				     mcdi_read(MCDI_SLOT_AVAIL_CPU_MASK), avail);
	if (ret < 0)
		return dev_err_probe(dev, ret, "cpuhp states\n");

	register_syscore(&mcdi_syscore);
	debugfs_create_file("mt6771-mcdi-mbox", 0400, NULL, NULL, &mcdi_mbox_fops);

	mcdi_ready = true;
	dev_info(dev, "SSPM MCDI task %u, cores 0x%02x\n", stat, avail);
	mcdi_apply_gate();
	return 0;
}

static const struct of_device_id mt6771_mcdi_of_match[] = {
	{ .compatible = "mediatek,mt6771-mcdi" },
	{ }
};

static struct platform_driver mt6771_mcdi_driver = {
	.probe = mt6771_mcdi_probe,
	.driver = {
		.name = "mt6771-mcdi",
		.of_match_table = mt6771_mcdi_of_match,
		.suppress_bind_attrs = true,
	},
};

static int __init mt6771_mcdi_init(void)
{
	struct device_node *np;
	bool present;

	np = of_find_matching_node(NULL, mt6771_mcdi_of_match);
	present = np && of_device_is_available(np);
	of_node_put(np);
	if (!present)
		return 0;

	/* cpuidle-psci registers at device_initcall; nothing deep until the gate below */
	mcdi_paused = 1;
	cpu_latency_qos_add_request(&mcdi_qos, 0);
	return platform_driver_register(&mt6771_mcdi_driver);
}
subsys_initcall(mt6771_mcdi_init);

/* s2idle ignores latency QoS, so the lasting gate is the per-state disable */
static int __init mt6771_mcdi_gate_init(void)
{
	if (!cpu_latency_qos_request_active(&mcdi_qos))
		return 0;

	mutex_lock(&mcdi_gate_lock);
	WRITE_ONCE(mcdi_gate_live, true);
	mutex_unlock(&mcdi_gate_lock);
	mcdi_apply_gate();
	mcdi_pause(false);
	return 0;
}
late_initcall_sync(mt6771_mcdi_gate_init);
