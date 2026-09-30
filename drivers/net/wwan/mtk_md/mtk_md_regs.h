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

/* SPM (scpsys): the MD1 power switch, clk-mt6771-pg.c:spm_mtcmos_ctrl_md1() in the 4.4 BSP */
#define SPM_PWR_STATUS			0x0180
#define SPM_PWR_STATUS_2ND		0x0184
#define SPM_MD1_PWR_CON			0x0320
#define SPM_MD_SRAM_ISO_CON		0x0394
#define SPM_MD_EXTRA_PWR_CON		0x0398
#define SPM_PWR_STATUS_MD1		BIT(0)
#define SPM_PWR_RST_B			BIT(0)
#define SPM_PWR_ISO			BIT(1)
#define SPM_PWR_ON			BIT(2)
#define SPM_PWR_ON_2ND			BIT(3)
#define SPM_PWR_CLK_DIS			BIT(4)
#define SPM_MD1_SRAM_PDN		BIT(8)

#define SPM_PCM_REG15_DATA		0x013c	/* non-zero while the SPM firmware runs */

#define SPM_SW_RSV_5			0x061c	/* [15:0]: the operating point the SPM settled on */
#define SPM_SW_RSV_5_OPP		GENMASK(15, 0)

/* DVFSRC (spm_v4/mtk_dvfsrc_reg_mt6771.h) */
#define DVFSRC_BASIC_CONTROL		0x000
#define DVFSRC_SW_REQ			0x004
#define DVFSRC_EMI_REQUEST		0x00c
#define DVFSRC_EMI_REQUEST2		0x010
#define DVFSRC_EMI_REQUEST3		0x014
#define DVFSRC_EMI_QOS0			0x024
#define DVFSRC_EMI_QOS1			0x028
#define DVFSRC_EMI_MD2SPM0		0x030
#define DVFSRC_EMI_MD2SPM1		0x034
#define DVFSRC_VCORE_REQUEST		0x048
#define DVFSRC_VCORE_REQUEST2		0x04c
#define DVFSRC_VCORE_MD2SPM0		0x068
#define DVFSRC_MD_SW_CONTROL		0x084
#define DVFSRC_INT_EN			0x09c
#define DVFSRC_TIMEOUT_NEXTREQ		0x0d8
#define DVFSRC_LEVEL			0x0dc
#define DVFSRC_LEVEL_LABEL(n)		(0x0e0 + 4 * (n))
#define DVFSRC_QOS_EN			0x180
#define DVFSRC_FORCE			0x300
#define DVFSRC_RSRV_1			0x604
#define DVFSRC_BASIC_CONTROL_RUN	0x017b
#define DVFSRC_LEVEL_BUSY		GENMASK(15, 0)
#define DVFSRC_VCORE_REQ2_OPP		GENMASK(25, 24)	/* pm_qos VCORE_OPP: 1 holds opp 0 */
#define DVFSRC_MD_SW_CONTROL_POLICY	(BIT(0) | BIT(3) | BIT(5))
#define DVFSRC_SW_REQ_OPP		GENMASK(3, 0)

/*
 * MT6358 PMIC: which inputs may switch an LDO on. The vendor kernel hands the
 * modem's RF supplies to SRCLKEN1, the modem's clock request (pmic_lp_api.c).
 */
#define MT6358_LDO_VFE28_OP_EN		0x1c0a
#define MT6358_LDO_VFE28_OP_CFG		0x1c10
#define MT6358_LDO_VRF18_OP_EN		0x1c1e
#define MT6358_LDO_VRF18_OP_CFG		0x1c24
#define MT6358_LDO_VRF12_OP_EN		0x1c32
#define MT6358_LDO_VRF12_OP_CFG		0x1c38
/* clk_buf_ctrl_bblpm_hw(): the baseband 26 MHz buffer's low-power mode, off while the modem runs */
#define MT6358_DCXO_CW23		0x07be
#define MT6358_XO_BB_LPM_CEL		BIT(0)
#define MT6358_LDO_OP_SW		BIT(0)
#define MT6358_LDO_OP_HW1		BIT(2)	/* SRCLKEN1 */

/* infracfg_ao */
#define INFRA_PERI2MD_PROT_STA		0x0228
#define INFRA_MD2PERI_PROT_STA		0x0258
#define INFRA_PERI2MD_PROT_SET		0x02a0
#define INFRA_PERI2MD_PROT_CLR		0x02a4
#define INFRA_MD2PERI_PROT_SET		0x02a8
#define INFRA_MD2PERI_PROT_CLR		0x02ac
#define INFRA_PERI2MD_PROT		BIT(7)
#define INFRA_MD1_PROT			(BIT(3) | BIT(4))
#define INFRA_MD2PERI_PROT		BIT(6)
#define INFRA_AP2MD_DUMMY		0x0370	/* bit 0: the modem may reach the AP */
#define INFRA_CLDMA_CTRL		0x0c00
#define INFRA_CLDMA_IP_BUSY_MASK	BIT(1)
#define INFRA_MD_SRCCLKENA		0x0f0c
#define INFRA_MD_SRCCLKENA_MASK		GENMASK(7, 0)
#define INFRA_MD_SRCCLKENA_MD1		0x21

/* topckgen CLK_MODE: the modem's 26 MHz and 32 kHz gates; apmixedsys AP_PLL_CON0 */
#define TOPCKGEN_CLK_MODE		0x0000
#define TOPCKGEN_MD_CLK_GATES		(BIT(8) | BIT(9))
#define APMIXED_AP_PLL_CON0		0x0000
#define APMIXED_CLKSQ1_LPF_EN		BIT(1)

/* The modem's own registers the AP writes before releasing it (md1_pll_init() and around) */
#define MD_PLL_VERSION			0x0000
#define MD_PLL_SRCLKENA_SETTLE		0x0004
#define MD_PLL_CLKSW_REQ		0x0010
#define MD_PLL_CLKSW_REQ_VAL		0x00100010
#define MD_PLL_CON(n)			(0x0040 + 8 * (n))	/* 0x40 .. 0x60 */
#define MD_PLL_CON_6			0x0064
#define MD_PLL_DFS			0x0104
#define MD_PLL_INT_MASK0		0x0314
#define MD_PLL_INT_MASK1		0x0318
#define MD_PLL_STATUS			0x0c00
#define MD_PLL_STATUS_BUSY		BIT(14)
#define MD_PLL_INIT_DONE		0x0f00
#define MD_PLL_INIT_DONE_VAL		0x62930000
#define MD_CLKSW_CKSEL			0x0020
#define MD_CLKSW_CKEN			0x0024
#define MD_CLKSW_CKEN2			0x0028
#define MD_CLKSW_STATUS			0x0084
#define MD_CLKSW_STATUS_READY		BIT(15)
#define MD_RGU_WDT_MODE			0x0100
#define MD_RGU_WDT_MODE_OFF		0x55000030
#define MD_BOOT_VECTOR_EN		0x0024

#endif /* __MTK_MD_REGS_H__ */
