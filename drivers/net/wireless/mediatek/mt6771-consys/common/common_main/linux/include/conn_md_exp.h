/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The vendor conn_md and eccci names the WMT IDC code uses (conn_md_exp.h, port_ipc.h,
 * ccci_ipc_task_ID.h in the 4.4 BSP), on top of mtk_conn_md, which replaces conn_md.
 */

#ifndef __CONN_MD_EXP_H_
#define __CONN_MD_EXP_H_

#include <linux/soc/mediatek/mtk_conn_md.h>

#define local_para			mtk_conn_md_para
#define conn_md_bridge_ops		mtk_conn_md_ops
typedef struct mtk_conn_md_ilm conn_md_ipc_ilm_t;

#define MD_MOD_EL1			MTK_CONN_MD_MD_EL1
#define AP_MOD_WMT			MTK_CONN_MD_AP_WMT

#define IPC_L4C_MSG_ID_BEGIN		MTK_CONN_MD_MSG_L4C_BEGIN
#define IPC_EL1_MSG_ID_BEGIN		MTK_CONN_MD_MSG_EL1_BEGIN
#define IPC_EL1_MSG_ID_RANGE		MTK_CONN_MD_MSG_EL1_RANGE

#endif /* __CONN_MD_EXP_H_ */
