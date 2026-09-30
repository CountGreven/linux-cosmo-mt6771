/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek CAMSV registers (MT8183 family, MT6771 layout).
 *
 * Offsets are relative to one 0x1000 page. Those marked "vendor" are used by
 * the production driver of the SoC vendor; the others follow the layout of
 * the earlier ISP generation moved to the MT6771 block positions and are
 * confirmed or corrected by the register dump on the device.
 */

#ifndef __MTK_CAMSV_REG_H__
#define __MTK_CAMSV_REG_H__

#include <linux/bits.h>

#define CAMSV_PAGE_SIZE			0x1000

/* IMGO DMA */
#define CAMSV_IMGO_BASE_ADDR		0x0020	/* vendor */
#define CAMSV_IMGO_XSIZE		0x0030
#define CAMSV_IMGO_YSIZE		0x0034
#define CAMSV_IMGO_STRIDE		0x0038

/* Frame buffer control */
#define CAMSV_FBC_IMGO_CTL1		0x0110	/* vendor */
#define CAMSV_FBC_IMGO_CTL2		0x0114	/* vendor */

/* Timing generator */
#define CAMSV_TG_SEN_MODE		0x0230	/* vendor */
#define  CAMSV_TG_SEN_MODE_CMOS_EN	BIT(0)
#define CAMSV_TG_VF_CON			0x0234	/* vendor */
#define  CAMSV_TG_VF_CON_VFDATA_EN	BIT(0)
#define CAMSV_TG_SEN_GRAB_PXL		0x0238
#define CAMSV_TG_SEN_GRAB_LIN		0x023c
#define  CAMSV_TG_GRAB_START		GENMASK(14, 0)
#define  CAMSV_TG_GRAB_END		GENMASK(30, 16)
#define CAMSV_TG_PATH_CFG		0x0240
#define  CAMSV_TG_PATH_CFG_DB_LOAD_DIS	BIT(4)
#define CAMSV_TG_FRMSIZE_ST		0x0268
#define CAMSV_TG_INTER_ST		0x026c	/* vendor */
#define  CAMSV_TG_INTER_ST_STATE	GENMASK(13, 8)
#define  CAMSV_TG_INTER_ST_IDLE		1
#define  CAMSV_TG_INTER_ST_FRAMES	GENMASK(23, 16)
#define CAMSV_TG_TIME_STAMP		0x02a0	/* vendor */

/* Top */
#define CAMSV_MODULE_EN			0x0510	/* vendor */
#define  CAMSV_MODULE_EN_IMGO		BIT(4)
#define  CAMSV_MODULE_EN_TG		BIT(0)
#define  CAMSV_MODULE_EN_DATA		(CAMSV_MODULE_EN_TG | BIT(3))
#define CAMSV_FMT_SEL			0x0514
#define CAMSV_INT_EN			0x0518	/* vendor */
#define CAMSV_INT_STATUS		0x051c	/* vendor, read to clear */
#define CAMSV_SW_CTL			0x0520	/* vendor */
#define  CAMSV_SW_CTL_IMGO_RST_TRIG	BIT(0)
#define  CAMSV_SW_CTL_IMGO_RST_ST	BIT(1)
#define  CAMSV_SW_CTL_SW_RST		BIT(2)
#define CAMSV_CLK_EN			0x0530
#define  CAMSV_CLK_EN_TG		BIT(0)
#define CAMSV_PAK			0x054c

/* Interrupt bits of INT_EN and INT_STATUS (vendor) */
#define CAMSV_INT_VS1			BIT(0)
#define CAMSV_INT_TG_ST1		BIT(1)
#define CAMSV_INT_TG_ST2		BIT(2)
#define CAMSV_INT_EXPDON		BIT(3)
#define CAMSV_INT_TG_ERR		BIT(4)
#define CAMSV_INT_TG_GBERR		BIT(5)
#define CAMSV_INT_SOF			BIT(7)
#define CAMSV_INT_HW_PASS1_DON		BIT(10)
#define CAMSV_INT_IMGO_ERR		BIT(16)
#define CAMSV_INT_IMGO_OVERRUN		BIT(17)
#define CAMSV_INT_SW_PASS1_DON		BIT(20)

#define CAMSV_INT_ERRORS	(CAMSV_INT_TG_ERR | CAMSV_INT_TG_GBERR | \
				 CAMSV_INT_IMGO_ERR | CAMSV_INT_IMGO_OVERRUN)
#define CAMSV_INT_ENABLED	(CAMSV_INT_SOF | CAMSV_INT_ERRORS)

#endif
