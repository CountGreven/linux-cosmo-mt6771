/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MediaTek connsys/modem message bridge: the in-kernel router between the modem's IPC channel and
 * the CONNSYS WMT core that carries LTE coexistence (IDC) messages. It takes the place of the
 * vendor's conn_md, with the same functions and ILM layout, so the vendor WMT code can call it.
 */

#ifndef __LINUX_SOC_MEDIATEK_MTK_CONN_MD_H
#define __LINUX_SOC_MEDIATEK_MTK_CONN_MD_H

#include <linux/bits.h>
#include <linux/types.h>

/* ccci_ipc_task_ID.h: AP tasks carry bit 31, modem modules do not */
#define MTK_CONN_MD_AP			BIT(31)
#define MTK_CONN_MD_MD_EL1		5
#define MTK_CONN_MD_AP_WMT		(MTK_CONN_MD_AP | 3)

/* port_ipc.h, with the MT6771 L4C range of 0x40 */
#define MTK_CONN_MD_MSG_L4C_BEGIN	0x80000000U
#define MTK_CONN_MD_MSG_EL1_BEGIN	0x80000040U
#define MTK_CONN_MD_MSG_EL1_RANGE	0x20

/* struct local_para: msg_len counts this header too */
struct mtk_conn_md_para {
	u8 ref_count;
	u8 _stub;
	u16 msg_len;
	u8 data[];
} __packed;

/* struct ipc_ilm */
struct mtk_conn_md_ilm {
	u32 src_mod_id;
	u32 dest_mod_id;
	u32 sap_id;
	u32 msg_id;
	struct mtk_conn_md_para *local_para_ptr;
	void *peer_buff_ptr;
};

struct mtk_conn_md_ops {
	int (*rx_cb)(struct mtk_conn_md_ilm *ilm);
};

int mtk_conn_md_bridge_reg(u32 id, const struct mtk_conn_md_ops *ops);
int mtk_conn_md_bridge_unreg(u32 id);
int mtk_conn_md_bridge_send_msg(const struct mtk_conn_md_ilm *ilm);

#endif /* __LINUX_SOC_MEDIATEK_MTK_CONN_MD_H */
