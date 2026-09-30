/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek SENINF CSI-2 receiver registers (MT8183 family layout).
 *
 * Offsets are relative to a 0x1000 page of the SENINF block. Page n holds the
 * CSI-2 decoder and the MUX n, page 0 additionally the top registers. The
 * offsets come from public MT8183 source; only fields that the driver
 * writes are named.
 */

#ifndef __MTK_SENINF_REG_H__
#define __MTK_SENINF_REG_H__

#include <linux/bits.h>

#define SENINF_PAGE_SIZE		0x1000
#define SENINF_NUM_PAGES		8

/* Top registers, page 0 */
#define SENINF_TOP_CTRL			0x0000
#define SENINF_TOP_MUX_CTRL		0x0008
#define SENINF_TOP_CAM_MUX_CTRL		0x0010
#define SENINF_TOP_PHY_CTL_CSI(n)	(0x001c + 4 * (n))
#define SENINF_TOP_CTRL_TM		0xc00
#define SENINF_TOP_PHY_CTL_4D1C_MASK	(BIT(31) | GENMASK(10, 8) | BIT(0))
#define SENINF_TOP_PHY_CTL_4D1C_VAL	0x80000200
#define SENINF_TOP_PHY_CTL_2D1C_MASK	(BIT(31) | GENMASK(13, 12) | GENMASK(10, 8) | BIT(0))
#define SENINF_TOP_PHY_CTL_2D1C_VAL	0x80001100

/* Per page, decoder control */
#define SENINF_CTRL			0x0200
#define SENINF_CTRL_EN			BIT(0)
#define SENINF_CTRL_SRC_MASK		GENMASK(15, 12)
#define SENINF_CTRL_RESET		BIT(7)
#define SENINF_CTRL_PAD_MASK		GENMASK(30, 28)
#define SENINF_CTRL_PAD_10BIT		0
#define SENINF_CTRL_EXT			0x0204
#define SENINF_CTRL_EXT_MASK		GENMASK(6, 5)
#define SENINF_CTRL_EXT_VAL		BIT(6)
#define SENINF_CTRL_EXT_TM		BIT(1)

/* Internal test model generator, page 0 */
#define SENINF_TG_TM_CTL		0x0608
#define SENINF_TG_TM_SIZE		0x060c
#define SENINF_TG_TM_CLK		0x0610
#define SENINF_TG_TM_STP		0x0614
#define SENINF_TG_TM_STP_NORMAL		0x67
#define SENINF_TG_TM_STP_TEST		0x01

/* Lane map, in the page of the decoder */
#define SENINF_MIPI_RX_CON24		0x0824
#define SENINF_LANE_MAP_MASK		GENMASK(31, 24)
#define SENINF_LANE_MAP_4D1C		0xc9
#define SENINF_LANE_MAP_2D1C		0xe4

/* CSI-2 decoder */
#define SENINF_CSI2_CTL			0x0a00
#define SENINF_CSI2_CTL_LANES		GENMASK(3, 0)
#define SENINF_CSI2_CTL_CLK_EN		BIT(4)
#define SENINF_CSI2_CTL_EN_MASK		GENMASK(4, 0)
#define SENINF_CSI2_CTL_HDR_ORDER	BIT(16)
#define SENINF_CSI2_CTL_BIT25		BIT(25)
#define SENINF_CSI2_CTL_CLR_MASK	(BIT(27) | BIT(7))
#define SENINF_CSI2_HS_TRAIL_VAL	8
#define SENINF_CSI2_LNRD_TIMING		0x0a08
#define SENINF_CSI2_LNRD_SETTLE		GENMASK(15, 8)
#define SENINF_CSI2_DPCM		0x0a0c
#define SENINF_CSI2_DPCM_NONE		BIT(7)
#define SENINF_CSI2_INT_EN		0x0a10
#define SENINF_CSI2_INT_STATUS		0x0a14
#define SENINF_CSI2_DGB_SEL		0x0a18
#define SENINF_CSI2_DGB_SEL_PKT		0x8000001a
#define SENINF_CSI2_DBG_PORT		0x0a1c
#define SENINF_CSI2_SPARE0		0x0a20
#define SENINF_CSI2_LNRC_FSM		0x0a28
#define SENINF_CSI2_LNRD_FSM		0x0a2c
#define SENINF_CSI2_FRAME_LINE_NUM	0x0a30
#define SENINF_CSI2_HSRX_DBG		0x0a38
#define SENINF_CSI2_HS_TRAIL		0x0a40
#define SENINF_CSI2_HS_TRAIL_MASK	GENMASK(7, 0)
#define SENINF_CSI2_RESYNC_MERGE_CTL	0x0a74
#define SENINF_CSI2_RESYNC_MASK		(GENMASK(11, 10) | GENMASK(2, 0))
#define SENINF_CSI2_RESYNC_VAL		0x3
#define SENINF_CSI2_MODE		0x0ae8
#define SENINF_CSI2_MODE_MASK		GENMASK(10, 0)
#define SENINF_CSI2_INT_EN_EXT		0x0b10
#define SENINF_CSI2_INT_EN_EXT_VAL	0x1f
#define SENINF_CSI2_INT_STATUS_EXT	0x0b14
#define SENINF_CSI2_DPHY_SYNC		0x0b20
#define SENINF_CSI2_DPHY_SYNC_VAL	0x1dff00

/* MUX */
#define SENINF_MUX_CTRL			0x0d00
#define SENINF_MUX_CTRL_EN		BIT(31)
#define SENINF_MUX_CTRL_FMT		GENMASK(29, 28)
#define SENINF_MUX_CTRL_FMT_RAW		2
#define SENINF_MUX_CTRL_SIZE		GENMASK(27, 16)
#define SENINF_MUX_CTRL_SIZE_RAW	0x6df
#define SENINF_MUX_CTRL_SEL		GENMASK(15, 12)
#define SENINF_MUX_CTRL_SEL_VAL		8
/* Pixel mode and sync polarity bits, all left at zero */
#define SENINF_MUX_CTRL_MODE_MASK	(GENMASK(10, 8) | GENMASK(1, 0))
#define SENINF_MUX_INTEN		0x0d04
#define SENINF_MUX_INTSTA		0x0d08
#define SENINF_MUX_SIZE			0x0d0c
#define SENINF_MUX_DEBUG_2		0x0d14
#define SENINF_MUX_DEBUG_3		0x0d18
#define SENINF_MUX_SPARE		0x0d2c
#define SENINF_MUX_CTRL_EXT		0x0d3c
#define SENINF_MUX_CTRL_EXT_MASK	(GENMASK(1, 0) | BIT(4))
#define SENINF_MUX_CTRL_EXT_VAL		1

/* Test model, values written as a whole */
#define SENINF_TM_CTRL			0x1001
#define SENINF_TM_MUX_CTRL		0x96df1080
#define SENINF_TM_MUX_INTEN		0x8000007f
#define SENINF_TM_MUX_CTRL_EXT		0xe2000
#define SENINF_TM_TG_CTL		0x404c1

#endif
