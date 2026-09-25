/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * MT6771 (MD generation 6293) CLDMA and CCIF registers, as the vendor driver uses them:
 * drivers/misc/mediatek/eccci/mt6771/cldma_reg.h and ccif_hif_platform.h in the 4.4 BSP.
 *
 * CLDMA is one engine split over two blocks. The always-on half ("AO", 0x10014000) keeps the
 * receive side and a backup of the transmit addresses across modem sleep; the power-down half
 * ("PD", 0x1021b000) holds the transmit side, commands and interrupt status. This generation is
 * not T7xx's: every offset, the queue count (4 TX, 1 RX) and the descriptor format differ.
 */

#ifndef __MTK_MD_REGS_H__
#define __MTK_MD_REGS_H__

#include <linux/bits.h>

/* CLDMA, always-on half */
#define CLDMA_AO_UL_START_ADDR_BK(q)	(0x0004 + 4 * (q))
#define CLDMA_AO_UL_START_ADDR_BK_4MSB	0x0014
#define CLDMA_AO_SO_CFG			0x0400
#define CLDMA_AO_SO_START_ADDR(q)	(0x0404 + 4 * (q))
#define CLDMA_AO_SO_CURRENT_ADDR(q)	(0x0408 + 4 * (q))
#define CLDMA_AO_SO_START_ADDR_4MSB	0x040c
#define CLDMA_AO_SO_STATUS		0x0414
#define CLDMA_AO_DL_MTU_SIZE		0x0418
#define CLDMA_AO_L2RIMR0		0x0800
#define CLDMA_AO_L2RIMCR0		0x0804
#define CLDMA_AO_L2RIMSR0		0x0808

/* CLDMA, power-down half */
#define CLDMA_PD_UL_START_ADDR(q)	(0x0000 + 4 * (q))
#define CLDMA_PD_UL_START_ADDR_4MSB	0x0010
#define CLDMA_PD_UL_CURRENT_ADDR(q)	(0x0014 + 4 * (q))
#define CLDMA_PD_UL_STATUS		0x0028
#define CLDMA_PD_UL_START_CMD		0x0030
#define CLDMA_PD_UL_RESUME_CMD		0x0034
#define CLDMA_PD_UL_STOP_CMD		0x0038
#define CLDMA_PD_UL_ERROR		0x003c
#define CLDMA_PD_UL_CFG			0x0040
#define CLDMA_PD_SO_ERROR		0x0400
#define CLDMA_PD_SO_START_CMD		0x0404
#define CLDMA_PD_SO_RESUME_CMD		0x0408
#define CLDMA_PD_SO_STOP_CMD		0x040c
#define CLDMA_PD_L2TISAR0		0x0800
#define CLDMA_PD_L2TIMR0		0x0804
#define CLDMA_PD_L2TIMCR0		0x0808
#define CLDMA_PD_L2TIMSR0		0x080c
#define CLDMA_PD_L3TISAR0		0x0810
#define CLDMA_PD_L3TISAR1		0x0814
#define CLDMA_PD_L3TIMCR0		0x0820
#define CLDMA_PD_L3TIMCR1		0x0824
#define CLDMA_PD_L3TIMSR0		0x0828
#define CLDMA_PD_L3TIMSR1		0x082c
#define CLDMA_PD_L2RISAR0		0x0830
#define CLDMA_PD_L3RISAR0		0x0838
#define CLDMA_PD_L3RISAR1		0x083c
#define CLDMA_PD_L3RIMCR0		0x0848
#define CLDMA_PD_L3RIMCR1		0x084c
#define CLDMA_PD_L3RIMSR0		0x0850
#define CLDMA_PD_L3RIMSR1		0x0854
#define CLDMA_PD_IP_BUSY		0x0860

#define CLDMA_TXQ_NUM			4
#define CLDMA_RXQ_NUM			1
#define CLDMA_ALL_QUEUES		0x0f	/* written to the commands, whatever the count */

#define CLDMA_TX_INT_DONE		GENMASK(3, 0)
#define CLDMA_TX_INT_QUEUE_EMPTY	GENMASK(7, 4)
#define CLDMA_TX_INT_ERROR		GENMASK(11, 8)
#define CLDMA_RX_INT_DONE		BIT(0)
#define CLDMA_RX_INT_QUEUE_EMPTY	BIT(1)
#define CLDMA_RX_INT_ERROR		BIT(2)

#define CLDMA_SO_CFG_ENABLE		BIT(0)	/* cldma_reset(): "SO_CFG |= 0x1" */
#define CLDMA_MTU_SIZE			0xe00	/* NET_RX_BUF: 3456-byte CCCI MTU + 128 */

/*
 * Descriptors are 16 bytes, __packed, and carry 36-bit addresses: 32 bits in the pointer fields
 * and the top nibble of each in one shared byte (data pointer low nibble, next pointer high).
 */
struct cldma_tgpd {
	u8 gpd_flags;
	u8 non_used;		/* the vendor sets 1 */
	u8 msb;
	u8 netif;
	__le32 next_gpd_ptr;
	__le32 data_buff_bd_ptr;
	__le16 data_buff_len;
	__le16 psn;
} __packed;

struct cldma_rgpd {
	u8 gpd_flags;
	u8 non_used;
	__le16 data_allow_len;
	__le32 next_gpd_ptr;
	__le32 data_buff_bd_ptr;
	__le16 data_buff_len;
	u8 msb;
	u8 non_used2;
} __packed;

#define CLDMA_GPD_FLAG_HWO		BIT(0)	/* owned by the hardware */
#define CLDMA_GPD_FLAG_BDP		BIT(1)	/* points at a BD list, not a buffer */
#define CLDMA_GPD_FLAG_IOC		BIT(7)	/* interrupt on completion */
#define CLDMA_GPD_MSB_DATA		GENMASK(3, 0)
#define CLDMA_GPD_MSB_NEXT		GENMASK(7, 4)

/* CCIF: a doorbell per channel plus 512 bytes of SRAM, one block per side */
#define APCCIF_CON			0x00
#define APCCIF_BUSY			0x04
#define APCCIF_START			0x08
#define APCCIF_TCHNUM			0x0c
#define APCCIF_RCHNUM			0x10
#define APCCIF_ACK			0x14
#define APCCIF_CHDATA			0x100
#define CCIF_SRAM_SIZE			512

/* Where things sit in the SRAM on 6293 (struct ccif_sram_layout) */
#define CCIF_SRAM_DL_HEADER		0	/* MD -> AP: HS1 header and feature query */
#define CCIF_SRAM_UP_HEADER		240	/* AP -> MD: header and our answer */
#define CCIF_SRAM_DBG_MAGIC		500	/* ccci_reset_ccif_hw() leaves MDSS_DBG here */
#define CCIF_SRAM_DBG_MAGIC_VAL		0x7274626e

/* Channels: the low 16 are data (ring queues 0-7, SRAM 15), the rest control */
#define CCIF_CH_SRAM			15	/* H2D_SRAM and D2H_SRAM */
#define CCIF_CH_EXCEPTION_INIT		16
#define CCIF_CH_EXCEPTION_INIT_DONE	17
#define CCIF_CH_EXCEPTION_CLEARQ_DONE	18
#define CCIF_CH_EXCEPTION_ALLQ_RESET	19
#define CCIF_CH_PEER_WAKEUP		20
#define CCIF_CH_SEQ_ERROR		21
#define CCIF_DATA_CHANNELS		GENMASK(15, 0)
#define CCIF_CTRL_CHANNELS		GENMASK(30, 15)	/* md_cd_ccif_isr(): 0xffff << 15 */

#endif /* __MTK_MD_REGS_H__ */
