// SPDX-License-Identifier: GPL-2.0
/*
 * MT6771 core power-down after CPU hotplug.
 *
 * On MT6771 the trusted firmware's PSCI CPU_OFF parks the core in WFI but does not cut its power:
 * the vendor kernel (drivers/misc/mediatek/base/power/hps_v3/mtk_hotplug_cb.c, CPU_DEAD) issues a
 * SiP call, MTK_SIP_POWER_DOWN_CORE, once the core is dead, and only then is the core powered off.
 * Without it the SPM keeps reporting the core ON, a later PSCI CPU_ON finds nothing to reset, the
 * core never runs the kernel's secondary entry, and every further CPU_ON is refused (-EINVAL).
 * Seen on the Planet Cosmo Communicator: hotplug off/on of any secondary, and kexec, both lose
 * the cores for good (notes: projects/cosmo/19-kexec.org).
 *
 * The vendor also powers a cluster down (MTK_SIP_POWER_DOWN_CLUSTER) when its last core dies, with
 * ARM PLL and cluster-suspend work around it. That is left out here: a cluster kept powered with
 * its cores off costs some power and nothing else, and needs no cluster power-up sequence either
 * (the vendor compiles MTK_SIP_POWER_UP_CLUSTER out for MT6771).
 */
#include <linux/arm-smccc.h>
#include <linux/cpu.h>
#include <linux/cpuhotplug.h>
#include <linux/init.h>
#include <linux/of.h>
#include <linux/printk.h>

#define MTK_SIP_POWER_DOWN_CORE	0xC2000218	/* 0x82000218 | SMC64 */

static int mt6771_cpu_dead(unsigned int cpu)
{
	struct arm_smccc_res res;

	arm_smccc_smc(MTK_SIP_POWER_DOWN_CORE, cpu, 0, 0, 0, 0, 0, 0, &res);
	pr_info("mt6771-cpu-pm: core %u powered down via SiP: %#lx\n", cpu, res.a0);
	return 0;
}

static int __init mt6771_cpu_pm_init(void)
{
	int ret;

	if (!of_machine_is_compatible("mediatek,mt6771"))
		return 0;

	/*
	 * A BP_PREPARE_DYN state: its teardown runs on a surviving CPU after the dead one has
	 * completed CPU_OFF and been confirmed off through AFFINITY_INFO (takedown_cpu, __cpu_die).
	 */
	ret = cpuhp_setup_state_nocalls(CPUHP_BP_PREPARE_DYN, "soc/mt6771-cpu-pm:dead",
					NULL, mt6771_cpu_dead);
	if (ret < 0) {
		pr_err("mt6771-cpu-pm: cpuhp state: %d\n", ret);
		return ret;
	}
	pr_info("mt6771-cpu-pm: powering dead cores down through the firmware\n");
	return 0;
}
subsys_initcall(mt6771_cpu_pm_init);
