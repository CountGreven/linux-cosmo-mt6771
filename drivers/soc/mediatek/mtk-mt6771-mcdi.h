/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __MTK_MT6771_MCDI_H
#define __MTK_MT6771_MCDI_H

#include <linux/bits.h>
#include <linux/types.h>

/* SSPM mailbox 3 slots, vendor mcdi_v1/mtk_mcdi_mbox.h */
#define MCDI_SLOT_CAN_POWER_OFF(cluster)	(0 + (cluster))
#define MCDI_SLOT_BUCK_POWER_OFF_MASK		2
#define MCDI_SLOT_ATF_ACTION_DONE(cluster)	(3 + (cluster))
#define MCDI_SLOT_PAUSE_ACTION			6
#define MCDI_SLOT_AVAIL_CPU_MASK		7
#define MCDI_SLOT_ACTION_STAT			9
#define MCDI_SLOT_CPU_ISOLATION_MASK		12
#define MCDI_SLOT_PAUSE_ACK			13
#define MCDI_NR_SLOTS				16

#define MCDI_ACTION_WAIT_EVENT			2
#define MCDI_ACTION_WORKING			3

#define MCDI_NR_CPUS				8
#define MCDI_PSCI_PARAM_NONE			U32_MAX

static inline u32 mcdi_avail_mask(unsigned long online)
{
	return online & GENMASK(MCDI_NR_CPUS - 1, 0);
}

static inline bool mcdi_task_ready(u32 action_stat, u32 pause_ack)
{
	return (action_stat == MCDI_ACTION_WAIT_EVENT || action_stat == MCDI_ACTION_WORKING) &&
	       !pause_ack;
}

/* PSCI original power_state format: power level in bits 25:24 */
static inline unsigned int mcdi_psci_level(u32 param)
{
	return (param >> 24) & 3;
}

/*
 * Only core-level states: a cluster-level entry also needs CLUSTER_n_CAN_POWER_OFF written
 * right before it, which cpuidle-psci gives no hook for.
 */
static inline bool mcdi_state_allowed(u32 param, unsigned int cpu, u32 idle_cpus)
{
	return param != MCDI_PSCI_PARAM_NONE && cpu < MCDI_NR_CPUS && (idle_cpus & BIT(cpu)) &&
	       mcdi_psci_level(param) == 0;
}

#endif
