/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __SOC_MEDIATEK_MTK_MT6771_MCDI_H
#define __SOC_MEDIATEK_MTK_MT6771_MCDI_H

#include <linux/errno.h>
#include <linux/types.h>

#if IS_ENABLED(CONFIG_MTK_MT6771_MCDI)
int mtk_mt6771_mcdi_task_hold(bool hold);
void mtk_mt6771_mcdi_suspend_state(bool open);
#else
static inline int mtk_mt6771_mcdi_task_hold(bool hold)
{
	return -ENODEV;
}

static inline void mtk_mt6771_mcdi_suspend_state(bool open)
{
}

#endif

#endif
