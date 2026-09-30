// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the Samsung S5K2X7 image sensor
 *
 * The register tables and the power sequence come from the GPL Android
 * driver of the Planet Cosmo Communicator (Samsung reference settings).
 * The structure follows the s5kjn1 driver.
 *
 * Copyright (C) 2026 Fredrik Lindlöf <fredrik.lindlof@gmail.com>
 */
#include <linux/array_size.h>
#include <linux/bits.h>
#include <linux/clk.h>
#include <linux/container_of.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>
#include <linux/units.h>

#include <media/v4l2-cci.h>
#include <media/v4l2-common.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

#define S5K2X7_REG_CHIP_ID		CCI_REG16(0x0000)
#define S5K2X7_CHIP_ID			0x2187

#define S5K2X7_REG_FRAME_COUNT		CCI_REG8(0x0005)
#define S5K2X7_FRAME_COUNT_STOPPED	0xff

#define S5K2X7_REG_MODE_SELECT		CCI_REG8(0x0100)
#define S5K2X7_MODE_STREAMING		BIT(0)

#define S5K2X7_REG_EXPOSURE		CCI_REG16(0x0202)
#define S5K2X7_EXPOSURE_MIN		4
#define S5K2X7_EXPOSURE_STEP		1
#define S5K2X7_EXPOSURE_MARGIN		5

#define S5K2X7_REG_AGAIN		CCI_REG16(0x0204)
/* One step is 1/32 of a gain of one. */
#define S5K2X7_AGAIN_MIN		0x20
#define S5K2X7_AGAIN_MAX		0x200
#define S5K2X7_AGAIN_STEP		1

#define S5K2X7_REG_VTS			CCI_REG16(0x0340)
#define S5K2X7_VTS_MAX			(0xffff - S5K2X7_EXPOSURE_MARGIN)

#define S5K2X7_REG_ORIENTATION		CCI_REG8(0x0101)
#define S5K2X7_ORIENTATION_HFLIP	BIT(0)
#define S5K2X7_ORIENTATION_VFLIP	BIT(1)

/* Analogue crop window of the array, inclusive end coordinates, and the output size after binning. */
#define S5K2X7_REG_X_START		CCI_REG16(0x0344)
#define S5K2X7_REG_Y_START		CCI_REG16(0x0346)
#define S5K2X7_REG_X_END		CCI_REG16(0x0348)
#define S5K2X7_REG_Y_END		CCI_REG16(0x034a)
#define S5K2X7_REG_X_OUTPUT		CCI_REG16(0x034c)
#define S5K2X7_REG_Y_OUTPUT		CCI_REG16(0x034e)

#define S5K2X7_REG_TEST_PATTERN		CCI_REG16(0x0600)

#define S5K2X7_NATIVE_WIDTH		5664
#define S5K2X7_NATIVE_HEIGHT		4256

/* The 2x2 binned mode halves the crop window; window edges and sizes are kept a multiple of 4. */
#define S5K2X7_BINNING			2
#define S5K2X7_CROP_ALIGN		4
#define S5K2X7_CROP_MIN_WIDTH		640
#define S5K2X7_CROP_MIN_HEIGHT		480

#define S5K2X7_MCLK_FREQ		(24 * HZ_PER_MHZ)
#define S5K2X7_DATA_LANES		4
/* The color order is the vendor's RAW_Gr, not measured. */
#define S5K2X7_MBUS_CODE		MEDIA_BUS_FMT_SGRBG10_1X10

#define S5K2X7_POWER_STEP_US		(5 * USEC_PER_MSEC)

#define to_s5k2x7(_sd)			container_of(_sd, struct s5k2x7, sd)

/*
 * UNKNOWN: the vendor driver never states the link rate. This is the output
 * PLL of the 30 fps setting: 24 MHz / 4 (0x0304, 0x030c) * 266 (0x030e) =
 * 1596 Mbit/s per lane, i.e. a 798 MHz clock. It is not measured.
 */
static const s64 s5k2x7_link_freq_menu[] = {
	798 * HZ_PER_MHZ,
};

/* Menu index to the value of register 0x0600; the vendor only uses 2. */
static const u8 s5k2x7_test_pattern_val[] = { 0, 2 };

static const char * const s5k2x7_test_pattern_menu[] = {
	"Disabled",
	"Color bars",
};

/* Rails in power-on order; the sensor's own DVDD is 1.0 V from an external LDO. */
static const char * const s5k2x7_supply_name[] = {
	"avdd",
	"dovdd",
	"dvdd",
};

struct s5k2x7_mode {
	u32 width;
	u32 height;
	u32 hts;
	u32 vts;
	u32 exposure;
	u64 pixel_rate;
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
};

static const struct cci_reg_sequence s5k2x7_init_regs_a[] = {
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x0000), 0x0013 },
	{ CCI_REG16(0x0000), 0x2187 },
	{ CCI_REG16(0x6214), 0x7971 },
	{ CCI_REG16(0x6218), 0x7150 },
};

static const struct cci_reg_sequence s5k2x7_init_regs_b[] = {
	{ CCI_REG16(0x0a02), 0x7000 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x3df4 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0449 },
	{ CCI_REG16(0x6f12), 0x0348 },
	{ CCI_REG16(0x6f12), 0x044a },
	{ CCI_REG16(0x6f12), 0x0860 },
	{ CCI_REG16(0x6f12), 0x101a },
	{ CCI_REG16(0x6f12), 0x8880 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x5eba },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x44f4 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x3280 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x6500 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x2de9 },
	{ CCI_REG16(0x6f12), 0xf05f },
	{ CCI_REG16(0x6f12), 0x9146 },
	{ CCI_REG16(0x6f12), 0x0e46 },
	{ CCI_REG16(0x6f12), 0x0446 },
	{ CCI_REG16(0x6f12), 0xdff8 },
	{ CCI_REG16(0x6f12), 0x08a4 },
	{ CCI_REG16(0x6f12), 0x30e0 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xbafa },
	{ CCI_REG16(0x6f12), 0x8346 },
	{ CCI_REG16(0x6f12), 0x4fea },
	{ CCI_REG16(0x6f12), 0x4835 },
	{ CCI_REG16(0x6f12), 0x4046 },
	{ CCI_REG16(0x6f12), 0xfe4f },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0xb021 },
	{ CCI_REG16(0x6f12), 0x05f5 },
	{ CCI_REG16(0x6f12), 0x0055 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xb4fa },
	{ CCI_REG16(0x6f12), 0x388d },
	{ CCI_REG16(0x6f12), 0xf98c },
	{ CCI_REG16(0x6f12), 0x411a },
	{ CCI_REG16(0x6f12), 0x4046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xb3fa },
	{ CCI_REG16(0x6f12), 0x788d },
	{ CCI_REG16(0x6f12), 0xb989 },
	{ CCI_REG16(0x6f12), 0x411a },
	{ CCI_REG16(0x6f12), 0x5046 },
	{ CCI_REG16(0x6f12), 0xaaf8 },
	{ CCI_REG16(0x6f12), 0x1a13 },
	{ CCI_REG16(0x6f12), 0xb0f8 },
	{ CCI_REG16(0x6f12), 0x1a13 },
	{ CCI_REG16(0x6f12), 0x0029 },
	{ CCI_REG16(0x6f12), 0xfbd1 },
	{ CCI_REG16(0x6f12), 0x7021 },
	{ CCI_REG16(0x6f12), 0xa0f8 },
	{ CCI_REG16(0x6f12), 0x0e13 },
	{ CCI_REG16(0x6f12), 0xc4f3 },
	{ CCI_REG16(0x6f12), 0x0c01 },
	{ CCI_REG16(0x6f12), 0xae42 },
	{ CCI_REG16(0x6f12), 0x00d2 },
	{ CCI_REG16(0x6f12), 0x3546 },
	{ CCI_REG16(0x6f12), 0x2d1b },
	{ CCI_REG16(0x6f12), 0x2b46 },
	{ CCI_REG16(0x6f12), 0x4a46 },
	{ CCI_REG16(0x6f12), 0x4046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xa0fa },
	{ CCI_REG16(0x6f12), 0x2c44 },
	{ CCI_REG16(0x6f12), 0xa944 },
	{ CCI_REG16(0x6f12), 0x5946 },
	{ CCI_REG16(0x6f12), 0x4046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x9ffa },
	{ CCI_REG16(0x6f12), 0x600b },
	{ CCI_REG16(0x6f12), 0x5fea },
	{ CCI_REG16(0x6f12), 0x0008 },
	{ CCI_REG16(0x6f12), 0x01d1 },
	{ CCI_REG16(0x6f12), 0xb442 },
	{ CCI_REG16(0x6f12), 0xc8d3 },
	{ CCI_REG16(0x6f12), 0xbde8 },
	{ CCI_REG16(0x6f12), 0xf09f },
	{ CCI_REG16(0x6f12), 0x2de9 },
	{ CCI_REG16(0x6f12), 0xf041 },
	{ CCI_REG16(0x6f12), 0x8046 },
	{ CCI_REG16(0x6f12), 0xe448 },
	{ CCI_REG16(0x6f12), 0x1646 },
	{ CCI_REG16(0x6f12), 0x0f46 },
	{ CCI_REG16(0x6f12), 0x0068 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0x84b2 },
	{ CCI_REG16(0x6f12), 0x050c },
	{ CCI_REG16(0x6f12), 0x2146 },
	{ CCI_REG16(0x6f12), 0x2846 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x8efa },
	{ CCI_REG16(0x6f12), 0x3246 },
	{ CCI_REG16(0x6f12), 0x3946 },
	{ CCI_REG16(0x6f12), 0x4046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x8efa },
	{ CCI_REG16(0x6f12), 0xdd4b },
	{ CCI_REG16(0x6f12), 0x16b9 },
	{ CCI_REG16(0x6f12), 0x0020 },
	{ CCI_REG16(0x6f12), 0x83f8 },
	{ CCI_REG16(0x6f12), 0x8b00 },
	{ CCI_REG16(0x6f12), 0x33f8 },
	{ CCI_REG16(0x6f12), 0xf80f },
	{ CCI_REG16(0x6f12), 0x1989 },
	{ CCI_REG16(0x6f12), 0x1a7c },
	{ CCI_REG16(0x6f12), 0x01fb },
	{ CCI_REG16(0x6f12), 0x0200 },
	{ CCI_REG16(0x6f12), 0x13f8 },
	{ CCI_REG16(0x6f12), 0x471c },
	{ CCI_REG16(0x6f12), 0x421a },
	{ CCI_REG16(0x6f12), 0xd749 },
	{ CCI_REG16(0x6f12), 0x8a80 },
	{ CCI_REG16(0x6f12), 0x5a88 },
	{ CCI_REG16(0x6f12), 0x5e89 },
	{ CCI_REG16(0x6f12), 0x9f7c },
	{ CCI_REG16(0x6f12), 0x06fb },
	{ CCI_REG16(0x6f12), 0x0722 },
	{ CCI_REG16(0x6f12), 0x13f8 },
	{ CCI_REG16(0x6f12), 0x456c },
	{ CCI_REG16(0x6f12), 0x961b },
	{ CCI_REG16(0x6f12), 0xce80 },
	{ CCI_REG16(0x6f12), 0x13f8 },
	{ CCI_REG16(0x6f12), 0x3c3c },
	{ CCI_REG16(0x6f12), 0x032b },
	{ CCI_REG16(0x6f12), 0x03d1 },
	{ CCI_REG16(0x6f12), 0x401e },
	{ CCI_REG16(0x6f12), 0x8880 },
	{ CCI_REG16(0x6f12), 0x521e },
	{ CCI_REG16(0x6f12), 0xca80 },
	{ CCI_REG16(0x6f12), 0x2146 },
	{ CCI_REG16(0x6f12), 0x2846 },
	{ CCI_REG16(0x6f12), 0xbde8 },
	{ CCI_REG16(0x6f12), 0xf041 },
	{ CCI_REG16(0x6f12), 0x0122 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x61ba },
	{ CCI_REG16(0x6f12), 0x2de9 },
	{ CCI_REG16(0x6f12), 0xf041 },
	{ CCI_REG16(0x6f12), 0x0646 },
	{ CCI_REG16(0x6f12), 0xc748 },
	{ CCI_REG16(0x6f12), 0x0d46 },
	{ CCI_REG16(0x6f12), 0x4268 },
	{ CCI_REG16(0x6f12), 0x140c },
	{ CCI_REG16(0x6f12), 0x97b2 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0x3946 },
	{ CCI_REG16(0x6f12), 0x2046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x54fa },
	{ CCI_REG16(0x6f12), 0xc348 },
	{ CCI_REG16(0x6f12), 0x90f8 },
	{ CCI_REG16(0x6f12), 0xbc10 },
	{ CCI_REG16(0x6f12), 0x0329 },
	{ CCI_REG16(0x6f12), 0x07d0 },
	{ CCI_REG16(0x6f12), 0x90f8 },
	{ CCI_REG16(0x6f12), 0x0b11 },
	{ CCI_REG16(0x6f12), 0x0129 },
	{ CCI_REG16(0x6f12), 0x03d8 },
	{ CCI_REG16(0x6f12), 0x90f8 },
	{ CCI_REG16(0x6f12), 0x0901 },
	{ CCI_REG16(0x6f12), 0x0128 },
	{ CCI_REG16(0x6f12), 0x01d9 },
	{ CCI_REG16(0x6f12), 0x0122 },
	{ CCI_REG16(0x6f12), 0x00e0 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0x2946 },
	{ CCI_REG16(0x6f12), 0x3046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x4afa },
	{ CCI_REG16(0x6f12), 0x3946 },
	{ CCI_REG16(0x6f12), 0x2046 },
	{ CCI_REG16(0x6f12), 0xbde8 },
	{ CCI_REG16(0x6f12), 0xf041 },
	{ CCI_REG16(0x6f12), 0x0122 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x39ba },
	{ CCI_REG16(0x6f12), 0x2de9 },
	{ CCI_REG16(0x6f12), 0xf041 },
	{ CCI_REG16(0x6f12), 0x0546 },
	{ CCI_REG16(0x6f12), 0xb348 },
	{ CCI_REG16(0x6f12), 0x0c46 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0x8168 },
	{ CCI_REG16(0x6f12), 0x0e0c },
	{ CCI_REG16(0x6f12), 0x8fb2 },
	{ CCI_REG16(0x6f12), 0x3946 },
	{ CCI_REG16(0x6f12), 0x3046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x2cfa },
	{ CCI_REG16(0x6f12), 0x2146 },
	{ CCI_REG16(0x6f12), 0x2846 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x37fa },
	{ CCI_REG16(0x6f12), 0x0122 },
	{ CCI_REG16(0x6f12), 0x3946 },
	{ CCI_REG16(0x6f12), 0x3046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x23fa },
	{ CCI_REG16(0x6f12), 0xac4a },
	{ CCI_REG16(0x6f12), 0xab48 },
	{ CCI_REG16(0x6f12), 0x02eb },
	{ CCI_REG16(0x6f12), 0x8505 },
	{ CCI_REG16(0x6f12), 0x0cb1 },
	{ CCI_REG16(0x6f12), 0xc088 },
	{ CCI_REG16(0x6f12), 0x0ae0 },
	{ CCI_REG16(0x6f12), 0x4088 },
	{ CCI_REG16(0x6f12), 0x08e0 },
	{ CCI_REG16(0x6f12), 0x1368 },
	{ CCI_REG16(0x6f12), 0x190c },
	{ CCI_REG16(0x6f12), 0x9bb2 },
	{ CCI_REG16(0x6f12), 0x44b1 },
	{ CCI_REG16(0x6f12), 0x8142 },
	{ CCI_REG16(0x6f12), 0x04d9 },
	{ CCI_REG16(0x6f12), 0x43ea },
	{ CCI_REG16(0x6f12), 0x0041 },
	{ CCI_REG16(0x6f12), 0x02c2 },
	{ CCI_REG16(0x6f12), 0xaa42 },
	{ CCI_REG16(0x6f12), 0xf4d1 },
	{ CCI_REG16(0x6f12), 0xbde8 },
	{ CCI_REG16(0x6f12), 0xf081 },
	{ CCI_REG16(0x6f12), 0x8142 },
	{ CCI_REG16(0x6f12), 0xf6d3 },
	{ CCI_REG16(0x6f12), 0xfae7 },
	{ CCI_REG16(0x6f12), 0x2de9 },
	{ CCI_REG16(0x6f12), 0xf041 },
	{ CCI_REG16(0x6f12), 0x0646 },
	{ CCI_REG16(0x6f12), 0x9b48 },
	{ CCI_REG16(0x6f12), 0x0d46 },
	{ CCI_REG16(0x6f12), 0xc268 },
	{ CCI_REG16(0x6f12), 0x140c },
	{ CCI_REG16(0x6f12), 0x97b2 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0x3946 },
	{ CCI_REG16(0x6f12), 0x2046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xfdf9 },
	{ CCI_REG16(0x6f12), 0x2946 },
	{ CCI_REG16(0x6f12), 0x3046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x0dfa },
	{ CCI_REG16(0x6f12), 0x0122 },
	{ CCI_REG16(0x6f12), 0x3946 },
	{ CCI_REG16(0x6f12), 0x2046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xf4f9 },
	{ CCI_REG16(0x6f12), 0x9648 },
	{ CCI_REG16(0x6f12), 0x964a },
	{ CCI_REG16(0x6f12), 0x30f8 },
	{ CCI_REG16(0x6f12), 0x1e1f },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0x4188 },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0x8188 },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0xc188 },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0x0189 },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0x4189 },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0x8189 },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0xc189 },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0x018a },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0x418a },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0x818a },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0xc18a },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0x018b },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0x418b },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0x818b },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0xc18b },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0x018c },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0x418c },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x921c },
	{ CCI_REG16(0x6f12), 0x818c },
	{ CCI_REG16(0x6f12), 0x1180 },
	{ CCI_REG16(0x6f12), 0x911c },
	{ CCI_REG16(0x6f12), 0xc08c },
	{ CCI_REG16(0x6f12), 0x0880 },
	{ CCI_REG16(0x6f12), 0xa5e7 },
	{ CCI_REG16(0x6f12), 0x7cb5 },
	{ CCI_REG16(0x6f12), 0x0346 },
	{ CCI_REG16(0x6f12), 0x7748 },
	{ CCI_REG16(0x6f12), 0x448f },
	{ CCI_REG16(0x6f12), 0x058f },
	{ CCI_REG16(0x6f12), 0x4ff0 },
	{ CCI_REG16(0x6f12), 0x8040 },
	{ CCI_REG16(0x6f12), 0x8089 },
	{ CCI_REG16(0x6f12), 0xa0f5 },
	{ CCI_REG16(0x6f12), 0x2061 },
	{ CCI_REG16(0x6f12), 0x5139 },
	{ CCI_REG16(0x6f12), 0x02d1 },
	{ CCI_REG16(0x6f12), 0x0824 },
	{ CCI_REG16(0x6f12), 0x41f2 },
	{ CCI_REG16(0x6f12), 0x5815 },
	{ CCI_REG16(0x6f12), 0x6a46 },
	{ CCI_REG16(0x6f12), 0x01a9 },
	{ CCI_REG16(0x6f12), 0x1846 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xbaf9 },
	{ CCI_REG16(0x6f12), 0xdde9 },
	{ CCI_REG16(0x6f12), 0x0021 },
	{ CCI_REG16(0x6f12), 0x2046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xbaf9 },
	{ CCI_REG16(0x6f12), 0x38b1 },
	{ CCI_REG16(0x6f12), 0xdde9 },
	{ CCI_REG16(0x6f12), 0x0021 },
	{ CCI_REG16(0x6f12), 0x2846 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xb4f9 },
	{ CCI_REG16(0x6f12), 0x08b1 },
	{ CCI_REG16(0x6f12), 0x0120 },
	{ CCI_REG16(0x6f12), 0x7cbd },
	{ CCI_REG16(0x6f12), 0x0020 },
	{ CCI_REG16(0x6f12), 0x7cbd },
	{ CCI_REG16(0x6f12), 0xfeb5 },
	{ CCI_REG16(0x6f12), 0x0446 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xb0f9 },
	{ CCI_REG16(0x6f12), 0x2046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xb2f9 },
	{ CCI_REG16(0x6f12), 0x4ff0 },
	{ CCI_REG16(0x6f12), 0x8040 },
	{ CCI_REG16(0x6f12), 0x8589 },
	{ CCI_REG16(0x6f12), 0x94f8 },
	{ CCI_REG16(0x6f12), 0x7700 },
	{ CCI_REG16(0x6f12), 0x0190 },
	{ CCI_REG16(0x6f12), 0x94f8 },
	{ CCI_REG16(0x6f12), 0x7800 },
	{ CCI_REG16(0x6f12), 0x0290 },
	{ CCI_REG16(0x6f12), 0x94f8 },
	{ CCI_REG16(0x6f12), 0x3c01 },
	{ CCI_REG16(0x6f12), 0x5d4e },
	{ CCI_REG16(0x6f12), 0x40f6 },
	{ CCI_REG16(0x6f12), 0x5127 },
	{ CCI_REG16(0x6f12), 0xd8b1 },
	{ CCI_REG16(0x6f12), 0x718f },
	{ CCI_REG16(0x6f12), 0xbd42 },
	{ CCI_REG16(0x6f12), 0x04d1 },
	{ CCI_REG16(0x6f12), 0x5b4a },
	{ CCI_REG16(0x6f12), 0x0821 },
	{ CCI_REG16(0x6f12), 0x0b20 },
	{ CCI_REG16(0x6f12), 0xa2f8 },
	{ CCI_REG16(0x6f12), 0x5001 },
	{ CCI_REG16(0x6f12), 0x94f8 },
	{ CCI_REG16(0x6f12), 0x2220 },
	{ CCI_REG16(0x6f12), 0x4ff2 },
	{ CCI_REG16(0x6f12), 0x5010 },
	{ CCI_REG16(0x6f12), 0x82b3 },
	{ CCI_REG16(0x6f12), 0x96f8 },
	{ CCI_REG16(0x6f12), 0x5420 },
	{ CCI_REG16(0x6f12), 0x6ab3 },
	{ CCI_REG16(0x6f12), 0x94f8 },
	{ CCI_REG16(0x6f12), 0x7620 },
	{ CCI_REG16(0x6f12), 0x042a },
	{ CCI_REG16(0x6f12), 0x21d0 },
	{ CCI_REG16(0x6f12), 0x94f8 },
	{ CCI_REG16(0x6f12), 0x3c31 },
	{ CCI_REG16(0x6f12), 0x21f0 },
	{ CCI_REG16(0x6f12), 0x0301 },
	{ CCI_REG16(0x6f12), 0x5a43 },
	{ CCI_REG16(0x6f12), 0x6b46 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x8cf9 },
	{ CCI_REG16(0x6f12), 0x94f8 },
	{ CCI_REG16(0x6f12), 0x3d01 },
	{ CCI_REG16(0x6f12), 0x48b3 },
	{ CCI_REG16(0x6f12), 0x94f8 },
	{ CCI_REG16(0x6f12), 0x7620 },
	{ CCI_REG16(0x6f12), 0x318f },
	{ CCI_REG16(0x6f12), 0x1346 },
	{ CCI_REG16(0x6f12), 0x4243 },
	{ CCI_REG16(0x6f12), 0xbd42 },
	{ CCI_REG16(0x6f12), 0x01d1 },
	{ CCI_REG16(0x6f12), 0x41f2 },
	{ CCI_REG16(0x6f12), 0x5811 },
	{ CCI_REG16(0x6f12), 0x94f8 },
	{ CCI_REG16(0x6f12), 0x2250 },
	{ CCI_REG16(0x6f12), 0x4ff2 },
	{ CCI_REG16(0x6f12), 0x6010 },
	{ CCI_REG16(0x6f12), 0x35b1 },
	{ CCI_REG16(0x6f12), 0x96f8 },
	{ CCI_REG16(0x6f12), 0x5450 },
	{ CCI_REG16(0x6f12), 0x1db1 },
	{ CCI_REG16(0x6f12), 0x042b },
	{ CCI_REG16(0x6f12), 0x21f0 },
	{ CCI_REG16(0x6f12), 0x0301 },
	{ CCI_REG16(0x6f12), 0x10d0 },
	{ CCI_REG16(0x6f12), 0x6b46 },
	{ CCI_REG16(0x6f12), 0x10e0 },
	{ CCI_REG16(0x6f12), 0x94f8 },
	{ CCI_REG16(0x6f12), 0x3c21 },
	{ CCI_REG16(0x6f12), 0x21f0 },
	{ CCI_REG16(0x6f12), 0x0301 },
	{ CCI_REG16(0x6f12), 0x9200 },
	{ CCI_REG16(0x6f12), 0x6b46 },
	{ CCI_REG16(0x6f12), 0x091f },
	{ CCI_REG16(0x6f12), 0xdbe7 },
	{ CCI_REG16(0x6f12), 0xffe7 },
	{ CCI_REG16(0x6f12), 0x94f8 },
	{ CCI_REG16(0x6f12), 0x7620 },
	{ CCI_REG16(0x6f12), 0x94f8 },
	{ CCI_REG16(0x6f12), 0x3c31 },
	{ CCI_REG16(0x6f12), 0x5a43 },
	{ CCI_REG16(0x6f12), 0xd3e7 },
	{ CCI_REG16(0x6f12), 0x6b46 },
	{ CCI_REG16(0x6f12), 0x091d },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x5ff9 },
	{ CCI_REG16(0x6f12), 0x96f8 },
	{ CCI_REG16(0x6f12), 0x5e00 },
	{ CCI_REG16(0x6f12), 0x0028 },
	{ CCI_REG16(0x6f12), 0x02d0 },
	{ CCI_REG16(0x6f12), 0x2046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x5df9 },
	{ CCI_REG16(0x6f12), 0xfebd },
	{ CCI_REG16(0x6f12), 0x70b5 },
	{ CCI_REG16(0x6f12), 0x4ff4 },
	{ CCI_REG16(0x6f12), 0x0141 },
	{ CCI_REG16(0x6f12), 0x0c20 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x5bf9 },
	{ CCI_REG16(0x6f12), 0x0446 },
	{ CCI_REG16(0x6f12), 0x314d },
	{ CCI_REG16(0x6f12), 0xc005 },
	{ CCI_REG16(0x6f12), 0x04d5 },
	{ CCI_REG16(0x6f12), 0x2888 },
	{ CCI_REG16(0x6f12), 0x401c },
	{ CCI_REG16(0x6f12), 0x2880 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x57f9 },
	{ CCI_REG16(0x6f12), 0x2004 },
	{ CCI_REG16(0x6f12), 0x06d5 },
	{ CCI_REG16(0x6f12), 0x288c },
	{ CCI_REG16(0x6f12), 0x401c },
	{ CCI_REG16(0x6f12), 0x2884 },
	{ CCI_REG16(0x6f12), 0xbde8 },
	{ CCI_REG16(0x6f12), 0x7040 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x53b9 },
	{ CCI_REG16(0x6f12), 0x70bd },
	{ CCI_REG16(0x6f12), 0x2de9 },
	{ CCI_REG16(0x6f12), 0xf041 },
	{ CCI_REG16(0x6f12), 0xdff8 },
	{ CCI_REG16(0x6f12), 0x8080 },
	{ CCI_REG16(0x6f12), 0x0220 },
	{ CCI_REG16(0x6f12), 0x4446 },
	{ CCI_REG16(0x6f12), 0x14f8 },
	{ CCI_REG16(0x6f12), 0x711f },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x3ef9 },
	{ CCI_REG16(0x6f12), 0x86b2 },
	{ CCI_REG16(0x6f12), 0x6078 },
	{ CCI_REG16(0x6f12), 0x0127 },
	{ CCI_REG16(0x6f12), 0x421e },
	{ CCI_REG16(0x6f12), 0x07fa },
	{ CCI_REG16(0x6f12), 0x02f1 },
	{ CCI_REG16(0x6f12), 0x3142 },
	{ CCI_REG16(0x6f12), 0x15d0 },
	{ CCI_REG16(0x6f12), 0x204c },
	{ CCI_REG16(0x6f12), 0x658f },
	{ CCI_REG16(0x6f12), 0x41ea },
	{ CCI_REG16(0x6f12), 0x0502 },
	{ CCI_REG16(0x6f12), 0x6287 },
	{ CCI_REG16(0x6f12), 0x4000 },
	{ CCI_REG16(0x6f12), 0x0322 },
	{ CCI_REG16(0x6f12), 0x801e },
	{ CCI_REG16(0x6f12), 0x8240 },
	{ CCI_REG16(0x6f12), 0xb4f8 },
	{ CCI_REG16(0x6f12), 0x8e00 },
	{ CCI_REG16(0x6f12), 0x20ea },
	{ CCI_REG16(0x6f12), 0x0203 },
	{ CCI_REG16(0x6f12), 0xa4f8 },
	{ CCI_REG16(0x6f12), 0x8e30 },
	{ CCI_REG16(0x6f12), 0x1043 },
	{ CCI_REG16(0x6f12), 0xa4f8 },
	{ CCI_REG16(0x6f12), 0x8e00 },
	{ CCI_REG16(0x6f12), 0x0220 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x21f9 },
	{ CCI_REG16(0x6f12), 0x6587 },
	{ CCI_REG16(0x6f12), 0x98f8 },
	{ CCI_REG16(0x6f12), 0x7500 },
	{ CCI_REG16(0x6f12), 0x3946 },
	{ CCI_REG16(0x6f12), 0x401e },
	{ CCI_REG16(0x6f12), 0x8740 },
	{ CCI_REG16(0x6f12), 0x3742 },
	{ CCI_REG16(0x6f12), 0x2cd0 },
	{ CCI_REG16(0x6f12), 0x114a },
	{ CCI_REG16(0x6f12), 0x908c },
	{ CCI_REG16(0x6f12), 0x401c },
	{ CCI_REG16(0x6f12), 0x9084 },
	{ CCI_REG16(0x6f12), 0x1148 },
	{ CCI_REG16(0x6f12), 0x0288 },
	{ CCI_REG16(0x6f12), 0x012a },
	{ CCI_REG16(0x6f12), 0x24d1 },
	{ CCI_REG16(0x6f12), 0x90f8 },
	{ CCI_REG16(0x6f12), 0x5421 },
	{ CCI_REG16(0x6f12), 0xfab1 },
	{ CCI_REG16(0x6f12), 0xbde8 },
	{ CCI_REG16(0x6f12), 0xf041 },
	{ CCI_REG16(0x6f12), 0x1ae0 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x4000 },
	{ CCI_REG16(0x6f12), 0x8000 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x3ca0 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x44c0 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x32c0 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x3b50 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0xa000 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x2f50 },
	{ CCI_REG16(0x6f12), 0x4000 },
	{ CCI_REG16(0x6f12), 0xac30 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x09f0 },
	{ CCI_REG16(0x6f12), 0x4000 },
	{ CCI_REG16(0x6f12), 0xf000 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x2790 },
	{ CCI_REG16(0x6f12), 0x4000 },
	{ CCI_REG16(0x6f12), 0x7000 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x2570 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xfdb8 },
	{ CCI_REG16(0x6f12), 0x80f8 },
	{ CCI_REG16(0x6f12), 0x5511 },
	{ CCI_REG16(0x6f12), 0xa4e6 },
	{ CCI_REG16(0x6f12), 0x4079 },
	{ CCI_REG16(0x6f12), 0x38b1 },
	{ CCI_REG16(0x6f12), 0x1f23 },
	{ CCI_REG16(0x6f12), 0x8343 },
	{ CCI_REG16(0x6f12), 0x0122 },
	{ CCI_REG16(0x6f12), 0xaff2 },
	{ CCI_REG16(0x6f12), 0xc701 },
	{ CCI_REG16(0x6f12), 0x0220 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xf5b8 },
	{ CCI_REG16(0x6f12), 0x1f21 },
	{ CCI_REG16(0x6f12), 0x0220 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xf6b8 },
	{ CCI_REG16(0x6f12), 0x70b5 },
	{ CCI_REG16(0x6f12), 0x0446 },
	{ CCI_REG16(0x6f12), 0x4279 },
	{ CCI_REG16(0x6f12), 0x8079 },
	{ CCI_REG16(0x6f12), 0x18b1 },
	{ CCI_REG16(0x6f12), 0x0121 },
	{ CCI_REG16(0x6f12), 0x401e },
	{ CCI_REG16(0x6f12), 0x8140 },
	{ CCI_REG16(0x6f12), 0x00e0 },
	{ CCI_REG16(0x6f12), 0x0021 },
	{ CCI_REG16(0x6f12), 0x3348 },
	{ CCI_REG16(0x6f12), 0x0a43 },
	{ CCI_REG16(0x6f12), 0x6271 },
	{ CCI_REG16(0x6f12), 0x0069 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0x86b2 },
	{ CCI_REG16(0x6f12), 0x050c },
	{ CCI_REG16(0x6f12), 0x3146 },
	{ CCI_REG16(0x6f12), 0x2846 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x91f8 },
	{ CCI_REG16(0x6f12), 0x2046 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xe3f8 },
	{ CCI_REG16(0x6f12), 0x0122 },
	{ CCI_REG16(0x6f12), 0x3146 },
	{ CCI_REG16(0x6f12), 0x2846 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x89f8 },
	{ CCI_REG16(0x6f12), 0x0120 },
	{ CCI_REG16(0x6f12), 0x70bd },
	{ CCI_REG16(0x6f12), 0x10b5 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0xaff2 },
	{ CCI_REG16(0x6f12), 0xb341 },
	{ CCI_REG16(0x6f12), 0x2748 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xdaf8 },
	{ CCI_REG16(0x6f12), 0x274c },
	{ CCI_REG16(0x6f12), 0x4bf6 },
	{ CCI_REG16(0x6f12), 0x5870 },
	{ CCI_REG16(0x6f12), 0xe18c },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xd9f8 },
	{ CCI_REG16(0x6f12), 0xe08c },
	{ CCI_REG16(0x6f12), 0x254a },
	{ CCI_REG16(0x6f12), 0x2449 },
	{ CCI_REG16(0x6f12), 0x42f8 },
	{ CCI_REG16(0x6f12), 0x2010 },
	{ CCI_REG16(0x6f12), 0x401c },
	{ CCI_REG16(0x6f12), 0xe084 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0xaff2 },
	{ CCI_REG16(0x6f12), 0x5541 },
	{ CCI_REG16(0x6f12), 0x2248 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xc7f8 },
	{ CCI_REG16(0x6f12), 0x1b4c },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0xaff2 },
	{ CCI_REG16(0x6f12), 0xef31 },
	{ CCI_REG16(0x6f12), 0x2060 },
	{ CCI_REG16(0x6f12), 0x1f48 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xbff8 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0xaff2 },
	{ CCI_REG16(0x6f12), 0xab31 },
	{ CCI_REG16(0x6f12), 0x6060 },
	{ CCI_REG16(0x6f12), 0x1c48 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xb8f8 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0xaff2 },
	{ CCI_REG16(0x6f12), 0x5d31 },
	{ CCI_REG16(0x6f12), 0xa060 },
	{ CCI_REG16(0x6f12), 0x1a48 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xb1f8 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0xaff2 },
	{ CCI_REG16(0x6f12), 0xbf21 },
	{ CCI_REG16(0x6f12), 0xe060 },
	{ CCI_REG16(0x6f12), 0x1748 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xaaf8 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0xaff2 },
	{ CCI_REG16(0x6f12), 0x8721 },
	{ CCI_REG16(0x6f12), 0x1548 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0xa4f8 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0xaff2 },
	{ CCI_REG16(0x6f12), 0xc511 },
	{ CCI_REG16(0x6f12), 0x1348 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x9ef8 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0xaff2 },
	{ CCI_REG16(0x6f12), 0xe501 },
	{ CCI_REG16(0x6f12), 0x1148 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x98f8 },
	{ CCI_REG16(0x6f12), 0x0022 },
	{ CCI_REG16(0x6f12), 0xaff2 },
	{ CCI_REG16(0x6f12), 0xd501 },
	{ CCI_REG16(0x6f12), 0x0f48 },
	{ CCI_REG16(0x6f12), 0x00f0 },
	{ CCI_REG16(0x6f12), 0x92f8 },
	{ CCI_REG16(0x6f12), 0x2061 },
	{ CCI_REG16(0x6f12), 0x10bd },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x44c0 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0xe7bd },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x3280 },
	{ CCI_REG16(0x6f12), 0xaff3 },
	{ CCI_REG16(0x6f12), 0x0080 },
	{ CCI_REG16(0x6f12), 0x2000 },
	{ CCI_REG16(0x6f12), 0x3cf0 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0xa4cd },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x58a5 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x463f },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x5637 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0xa01f },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0xc677 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x049f },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0641 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0xd7bb },
	{ CCI_REG16(0x6f12), 0x4ef2 },
	{ CCI_REG16(0x6f12), 0x8f3c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4ef2 },
	{ CCI_REG16(0x6f12), 0xed3c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4ef2 },
	{ CCI_REG16(0x6f12), 0x814c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4ef2 },
	{ CCI_REG16(0x6f12), 0xa95c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4ef2 },
	{ CCI_REG16(0x6f12), 0xe33c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x40f6 },
	{ CCI_REG16(0x6f12), 0x032c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4af2 },
	{ CCI_REG16(0x6f12), 0xcd4c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x45f6 },
	{ CCI_REG16(0x6f12), 0xa50c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x44f2 },
	{ CCI_REG16(0x6f12), 0x3f6c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x45f2 },
	{ CCI_REG16(0x6f12), 0x376c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4cf2 },
	{ CCI_REG16(0x6f12), 0x610c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4af2 },
	{ CCI_REG16(0x6f12), 0x070c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4cf2 },
	{ CCI_REG16(0x6f12), 0x6b4c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4cf2 },
	{ CCI_REG16(0x6f12), 0x275c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4cf2 },
	{ CCI_REG16(0x6f12), 0x3f3c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4cf2 },
	{ CCI_REG16(0x6f12), 0x094c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x40f2 },
	{ CCI_REG16(0x6f12), 0xe91c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4bf2 },
	{ CCI_REG16(0x6f12), 0x6b5c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x46f6 },
	{ CCI_REG16(0x6f12), 0x6d0c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4ff2 },
	{ CCI_REG16(0x6f12), 0xfd0c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x40f2 },
	{ CCI_REG16(0x6f12), 0x532c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x40f2 },
	{ CCI_REG16(0x6f12), 0xd12c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x4df2 },
	{ CCI_REG16(0x6f12), 0xbb7c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x000c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x43f2 },
	{ CCI_REG16(0x6f12), 0xbb1c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x010c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x43f2 },
	{ CCI_REG16(0x6f12), 0x5b1c },
	{ CCI_REG16(0x6f12), 0xc0f2 },
	{ CCI_REG16(0x6f12), 0x010c },
	{ CCI_REG16(0x6f12), 0x6047 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6028), 0x4000 },
	{ CCI_REG16(0x112c), 0x4220 },
	{ CCI_REG16(0x112e), 0x0000 },
	{ CCI_REG16(0x3842), 0x240f },
	{ CCI_REG16(0xf404), 0x0ff3 },
	{ CCI_REG16(0xf426), 0x00a0 },
	{ CCI_REG16(0xf428), 0x44c2 },
	{ CCI_REG16(0xf458), 0x0009 },
	{ CCI_REG16(0xf45a), 0x000e },
	{ CCI_REG16(0xf45c), 0x000e },
	{ CCI_REG16(0xf45e), 0x0018 },
	{ CCI_REG16(0xf460), 0x0010 },
	{ CCI_REG16(0xf462), 0x0018 },
	{ CCI_REG16(0x3850), 0x005a },
	{ CCI_REG16(0x3834), 0x0803 },
	{ CCI_REG16(0x3264), 0x0009 },
	{ CCI_REG16(0x3284), 0x0044 },
	{ CCI_REG16(0x3290), 0x0000 },
	{ CCI_REG16(0x3298), 0x0020 },
	{ CCI_REG16(0x33e0), 0x00b9 },
	{ CCI_REG16(0x33e4), 0x006f },
	{ CCI_REG16(0x33e8), 0x0076 },
	{ CCI_REG16(0x33ec), 0x011a },
	{ CCI_REG16(0x33f0), 0x0004 },
	{ CCI_REG16(0x33f4), 0x0000 },
	{ CCI_REG16(0x33f8), 0x0000 },
	{ CCI_REG16(0x33fc), 0x0000 },
	{ CCI_REG16(0x3400), 0x00b9 },
	{ CCI_REG16(0x3404), 0x006f },
	{ CCI_REG16(0x3408), 0x0076 },
	{ CCI_REG16(0x340c), 0x011a },
	{ CCI_REG16(0x3410), 0x0004 },
	{ CCI_REG16(0x3414), 0x0000 },
	{ CCI_REG16(0x3418), 0x0000 },
	{ CCI_REG16(0x341c), 0x0000 },
	{ CCI_REG16(0xf4ae), 0x003c },
	{ CCI_REG16(0xf4b0), 0x1124 },
	{ CCI_REG16(0xf4b2), 0x003d },
	{ CCI_REG16(0xf4b4), 0x1125 },
	{ CCI_REG16(0xf4b6), 0x0044 },
	{ CCI_REG16(0xf4b8), 0x112c },
	{ CCI_REG16(0xf4ba), 0x0045 },
	{ CCI_REG16(0xf4bc), 0x112d },
	{ CCI_REG16(0xf4be), 0x004c },
	{ CCI_REG16(0xf4c0), 0x1134 },
	{ CCI_REG16(0xf4c2), 0x004d },
	{ CCI_REG16(0xf4c4), 0x1135 },
	{ CCI_REG16(0xf486), 0x0480 },
	{ CCI_REG16(0x31e4), 0x0000 },
	{ CCI_REG16(0x31e2), 0x0000 },
	{ CCI_REG16(0x31e0), 0x0000 },
	{ CCI_REG16(0xf46c), 0x002d },
	{ CCI_REG16(0xf47a), 0x00fc },
	{ CCI_REG16(0xf47c), 0x0000 },
	{ CCI_REG16(0x31f8), 0x0008 },
	{ CCI_REG16(0x31fa), 0x1158 },
	{ CCI_REG16(0xf160), 0x000b },
	{ CCI_REG16(0xf150), 0x000b },
	{ CCI_REG16(0x3050), 0x0000 },
	{ CCI_REG16(0x3052), 0x0400 },
	{ CCI_REG16(0x0b00), 0x0080 },
	{ CCI_REG16(0x305e), 0x0000 },
	{ CCI_REG16(0x3076), 0x0000 },
	{ CCI_REG16(0x3054), 0x0000 },
	{ CCI_REG16(0x0b04), 0x0101 },
	{ CCI_REG16(0x0b08), 0x0000 },
	{ CCI_REG16(0x0b0e), 0x0000 },
	{ CCI_REG16(0xb134), 0x0180 },
	{ CCI_REG16(0xb13c), 0x0400 },
	{ CCI_REG16(0x3000), 0x0001 },
	{ CCI_REG16(0x655e), 0x03e8 },
	{ CCI_REG16(0x6592), 0x0010 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x20c0 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x19c8 },
	{ CCI_REG16(0x6f12), 0x0200 },
	{ CCI_REG16(0x602a), 0x00ea },
	{ CCI_REG16(0x6f12), 0xff08 },
	{ CCI_REG16(0x602a), 0x00f6 },
	{ CCI_REG16(0x6f12), 0x1000 },
	{ CCI_REG16(0x6f12), 0x4000 },
	{ CCI_REG16(0x6f12), 0x8000 },
	{ CCI_REG16(0x6f12), 0xc000 },
	{ CCI_REG16(0x6f12), 0xffff },
	{ CCI_REG16(0x602a), 0x02f4 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0032 },
	{ CCI_REG16(0x6f12), 0x0050 },
	{ CCI_REG16(0x6f12), 0x0050 },
	{ CCI_REG16(0x6f12), 0x00c8 },
	{ CCI_REG16(0x6f12), 0x00c8 },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x6f12), 0x0118 },
	{ CCI_REG16(0x6f12), 0x0030 },
	{ CCI_REG16(0x6f12), 0x0028 },
	{ CCI_REG16(0x6f12), 0x0028 },
	{ CCI_REG16(0x6f12), 0x0050 },
	{ CCI_REG16(0x6f12), 0x00b4 },
	{ CCI_REG16(0x6f12), 0x00b4 },
	{ CCI_REG16(0x6f12), 0x00b4 },
	{ CCI_REG16(0x6f12), 0x00c8 },
	{ CCI_REG16(0x6f12), 0x00fa },
	{ CCI_REG16(0x6f12), 0x00fa },
	{ CCI_REG16(0x6f12), 0x012c },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x6f12), 0x0028 },
	{ CCI_REG16(0x6f12), 0x0050 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0025 },
	{ CCI_REG16(0x6f12), 0x003c },
	{ CCI_REG16(0x6f12), 0x003c },
	{ CCI_REG16(0x6f12), 0x0096 },
	{ CCI_REG16(0x6f12), 0x0096 },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x6f12), 0x015b },
	{ CCI_REG16(0x6f12), 0x0035 },
	{ CCI_REG16(0x6f12), 0x0021 },
	{ CCI_REG16(0x6f12), 0x0021 },
	{ CCI_REG16(0x6f12), 0x0043 },
	{ CCI_REG16(0x6f12), 0x0096 },
	{ CCI_REG16(0x6f12), 0x0096 },
	{ CCI_REG16(0x6f12), 0x0096 },
	{ CCI_REG16(0x6f12), 0x00a7 },
	{ CCI_REG16(0x6f12), 0x00d0 },
	{ CCI_REG16(0x6f12), 0x00d0 },
	{ CCI_REG16(0x6f12), 0x00fa },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x6f12), 0x0028 },
	{ CCI_REG16(0x6f12), 0x0050 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0019 },
	{ CCI_REG16(0x6f12), 0x0028 },
	{ CCI_REG16(0x6f12), 0x0028 },
	{ CCI_REG16(0x6f12), 0x0064 },
	{ CCI_REG16(0x6f12), 0x0064 },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x6f12), 0x019d },
	{ CCI_REG16(0x6f12), 0x0039 },
	{ CCI_REG16(0x6f12), 0x001b },
	{ CCI_REG16(0x6f12), 0x001b },
	{ CCI_REG16(0x6f12), 0x0035 },
	{ CCI_REG16(0x6f12), 0x0078 },
	{ CCI_REG16(0x6f12), 0x0078 },
	{ CCI_REG16(0x6f12), 0x0078 },
	{ CCI_REG16(0x6f12), 0x0085 },
	{ CCI_REG16(0x6f12), 0x00a7 },
	{ CCI_REG16(0x6f12), 0x00a7 },
	{ CCI_REG16(0x6f12), 0x00c8 },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x6f12), 0x0028 },
	{ CCI_REG16(0x6f12), 0x0050 },
	{ CCI_REG16(0x602a), 0x004a },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0x03f4 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x6f12), 0x0020 },
	{ CCI_REG16(0x602a), 0x1360 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x1c8a },
	{ CCI_REG16(0x6f12), 0x0011 },
	{ CCI_REG16(0x6f12), 0x1010 },
	{ CCI_REG16(0x602a), 0x1c92 },
	{ CCI_REG16(0x6f12), 0x7700 },
	{ CCI_REG16(0x602a), 0x1ba0 },
	{ CCI_REG16(0x6f12), 0x0030 },
	{ CCI_REG16(0x602a), 0x1c06 },
	{ CCI_REG16(0x6f12), 0x0250 },
	{ CCI_REG16(0x6f12), 0x8060 },
	{ CCI_REG16(0x602a), 0x1bde },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x0100), 0x0000 },
};

static const struct cci_reg_sequence s5k2x7_2832x2128_30fps_regs[] = {
	{ CCI_REG16(0x6214), 0x7971 },
	{ CCI_REG16(0x6218), 0x7150 },
	{ CCI_REG16(0x0344), 0x0000 },
	{ CCI_REG16(0x0346), 0x0000 },
	{ CCI_REG16(0x0348), 0x161f },
	{ CCI_REG16(0x034a), 0x109f },
	{ CCI_REG16(0x034c), 0x0b10 },
	{ CCI_REG16(0x034e), 0x0850 },
	{ CCI_REG16(0x0340), 0x0a28 },
	{ CCI_REG16(0x0342), 0x1800 },
	{ CCI_REG16(0x3000), 0x0001 },
	{ CCI_REG16(0x3002), 0x0100 },
	{ CCI_REG16(0x0900), 0x0111 },
	{ CCI_REG16(0x0380), 0x0002 },
	{ CCI_REG16(0x0382), 0x0002 },
	{ CCI_REG16(0x0384), 0x0002 },
	{ CCI_REG16(0x0386), 0x0002 },
	{ CCI_REG16(0x3070), 0x0000 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x03e0 },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0x03e2 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x602a), 0x03e4 },
	{ CCI_REG16(0x6f12), 0x0000 },
	{ CCI_REG16(0x602a), 0x004a },
	{ CCI_REG16(0x6f12), 0x0001 },
	{ CCI_REG16(0x602a), 0x00e0 },
	{ CCI_REG16(0x6f12), 0x0100 },
	{ CCI_REG16(0x3072), 0x0000 },
	{ CCI_REG16(0x0b04), 0x0101 },
	{ CCI_REG16(0x306a), 0x0340 },
	{ CCI_REG16(0x306e), 0x0000 },
	{ CCI_REG16(0x3074), 0x0000 },
	{ CCI_REG16(0x3004), 0x0001 },
	{ CCI_REG16(0x0136), 0x1800 },
	{ CCI_REG16(0x013e), 0x0000 },
	{ CCI_REG16(0x0300), 0x0003 },
	{ CCI_REG16(0x0302), 0x0002 },
	{ CCI_REG16(0x0304), 0x0004 },
	{ CCI_REG16(0x0306), 0x00f0 },
	{ CCI_REG16(0x0308), 0x0008 },
	{ CCI_REG16(0x030a), 0x0001 },
	{ CCI_REG16(0x030c), 0x0004 },
	{ CCI_REG16(0x030e), 0x010a },
	{ CCI_REG16(0x300a), 0x0001 },
	{ CCI_REG16(0x300c), 0x0001 },
	{ CCI_REG16(0x324a), 0x0300 },
	{ CCI_REG16(0x31c0), 0x0004 },
	{ CCI_REG16(0x319e), 0x0100 },
	{ CCI_REG16(0x31a0), 0x0050 },
	{ CCI_REG16(0x6028), 0x2000 },
	{ CCI_REG16(0x602a), 0x1c04 },
	{ CCI_REG16(0x6f12), 0x0a01 },
	{ CCI_REG16(0x602a), 0x1ba2 },
	{ CCI_REG16(0x6f12), 0x0081 },
	{ CCI_REG16(0x0200), 0x0000 },
	{ CCI_REG16(0x0202), 0x0620 },
	{ CCI_REG16(0x021e), 0x0040 },
	{ CCI_REG16(0x021c), 0x0000 },
	{ CCI_REG16(0x0204), 0x0020 },
	{ CCI_REG16(0x0216), 0x0000 },
	{ CCI_REG16(0x3258), 0x0036 },
	{ CCI_REG16(0x3260), 0x002a },
	{ CCI_REG16(0x3268), 0x0001 },
	{ CCI_REG16(0x3270), 0x0024 },
	{ CCI_REG16(0x3288), 0x0030 },
	{ CCI_REG16(0x32a0), 0x0024 },
	{ CCI_REG16(0x32c4), 0x0029 },
	{ CCI_REG16(0x337c), 0x0066 },
	{ CCI_REG16(0x3458), 0x002e },
	{ CCI_REG16(0x382e), 0x060f },
	{ CCI_REG16(0x3830), 0x0805 },
	{ CCI_REG16(0x3832), 0x0605 },
	{ CCI_REG16(0x3834), 0x0803 },
	{ CCI_REG16(0x3840), 0x0064 },
	{ CCI_REG16(0x3844), 0x00bf },
	{ CCI_REG16(0xf404), 0x0ff3 },
	{ CCI_REG16(0xf424), 0x0000 },
	{ CCI_REG16(0x0612), 0x0000 },
	{ CCI_REG16(0x31c6), 0x1620 },
	{ CCI_REG16(0x0408), 0x0000 },
	{ CCI_REG16(0x040c), 0x0000 },
};

static const struct s5k2x7_mode s5k2x7_supported_modes[] = {
	{
		/* 2x2 binned readout of the whole array, 30 fps */
		.width = 2832,
		.height = 2128,
		.hts = 6144,
		.vts = 2600,
		.exposure = 0x0620,
		.pixel_rate = 480000000,
		.regs = s5k2x7_2832x2128_30fps_regs,
		.num_regs = ARRAY_SIZE(s5k2x7_2832x2128_30fps_regs),
	},
};

struct s5k2x7 {
	struct device *dev;
	struct regmap *regmap;
	struct clk *mclk;
	struct regulator_bulk_data supplies[ARRAY_SIZE(s5k2x7_supply_name)];
	struct regulator *afvdd;
	struct gpio_desc *reset_gpio;
	struct gpio_desc *powerdown_gpio;

	struct v4l2_subdev sd;
	struct media_pad pad;

	struct v4l2_ctrl_handler ctrl_handler;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;

	const struct s5k2x7_mode *mode;
	/* Output height of the active crop, the frame length is this plus VBLANK */
	u32 height;
};

static int s5k2x7_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct s5k2x7 *s5k2x7 = container_of(ctrl->handler, struct s5k2x7,
					     ctrl_handler);
	int ret;

	if (ctrl->id == V4L2_CID_VBLANK) {
		s64 exposure_max = s5k2x7->height + ctrl->val -
				   S5K2X7_EXPOSURE_MARGIN;

		__v4l2_ctrl_modify_range(s5k2x7->exposure,
					 s5k2x7->exposure->minimum,
					 exposure_max, s5k2x7->exposure->step,
					 s5k2x7->exposure->default_value);
	}

	/* Controls reach the sensor only while it is powered. */
	if (!pm_runtime_get_if_active(s5k2x7->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_ANALOGUE_GAIN:
		ret = cci_write(s5k2x7->regmap, S5K2X7_REG_AGAIN, ctrl->val,
				NULL);
		break;
	case V4L2_CID_EXPOSURE:
		ret = cci_write(s5k2x7->regmap, S5K2X7_REG_EXPOSURE,
				ctrl->val, NULL);
		break;
	case V4L2_CID_VBLANK:
		ret = cci_write(s5k2x7->regmap, S5K2X7_REG_VTS,
				ctrl->val + s5k2x7->height, NULL);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		ret = cci_write(s5k2x7->regmap, S5K2X7_REG_ORIENTATION,
				(s5k2x7->hflip->val ? S5K2X7_ORIENTATION_HFLIP : 0) |
				(s5k2x7->vflip->val ? S5K2X7_ORIENTATION_VFLIP : 0),
				NULL);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = cci_write(s5k2x7->regmap, S5K2X7_REG_TEST_PATTERN,
				s5k2x7_test_pattern_val[ctrl->val], NULL);
		break;
	default:
		ret = -EINVAL;
		break;
	}

	pm_runtime_put(s5k2x7->dev);

	return ret;
}

static const struct v4l2_ctrl_ops s5k2x7_ctrl_ops = {
	.s_ctrl = s5k2x7_set_ctrl,
};

static int s5k2x7_init_controls(struct s5k2x7 *s5k2x7)
{
	struct v4l2_ctrl_handler *ctrl_hdlr = &s5k2x7->ctrl_handler;
	const struct s5k2x7_mode *mode = s5k2x7->mode;
	struct v4l2_fwnode_device_properties props;
	s64 hblank = mode->hts - mode->width;
	s64 vblank = mode->vts - mode->height;
	s64 exposure_max = mode->vts - S5K2X7_EXPOSURE_MARGIN;
	int ret;

	v4l2_ctrl_handler_init(ctrl_hdlr, 11);

	s5k2x7->link_freq = v4l2_ctrl_new_int_menu(ctrl_hdlr, &s5k2x7_ctrl_ops,
			V4L2_CID_LINK_FREQ,
			ARRAY_SIZE(s5k2x7_link_freq_menu) - 1, 0,
			s5k2x7_link_freq_menu);
	if (s5k2x7->link_freq)
		s5k2x7->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	s5k2x7->pixel_rate = v4l2_ctrl_new_std(ctrl_hdlr, &s5k2x7_ctrl_ops,
			V4L2_CID_PIXEL_RATE, mode->pixel_rate,
			mode->pixel_rate, 1, mode->pixel_rate);
	if (s5k2x7->pixel_rate)
		s5k2x7->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	s5k2x7->hblank = v4l2_ctrl_new_std(ctrl_hdlr, &s5k2x7_ctrl_ops,
			V4L2_CID_HBLANK, hblank, hblank, 1, hblank);
	if (s5k2x7->hblank)
		s5k2x7->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	s5k2x7->vblank = v4l2_ctrl_new_std(ctrl_hdlr, &s5k2x7_ctrl_ops,
			V4L2_CID_VBLANK, vblank, S5K2X7_VTS_MAX - mode->height,
			1, vblank);

	s5k2x7->exposure = v4l2_ctrl_new_std(ctrl_hdlr, &s5k2x7_ctrl_ops,
			V4L2_CID_EXPOSURE, S5K2X7_EXPOSURE_MIN, exposure_max,
			S5K2X7_EXPOSURE_STEP, mode->exposure);

	v4l2_ctrl_new_std(ctrl_hdlr, &s5k2x7_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  S5K2X7_AGAIN_MIN, S5K2X7_AGAIN_MAX, S5K2X7_AGAIN_STEP,
			  S5K2X7_AGAIN_MIN);

	s5k2x7->hflip = v4l2_ctrl_new_std(ctrl_hdlr, &s5k2x7_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	s5k2x7->vflip = v4l2_ctrl_new_std(ctrl_hdlr, &s5k2x7_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (s5k2x7->hflip && s5k2x7->vflip)
		v4l2_ctrl_cluster(2, &s5k2x7->hflip);

	v4l2_ctrl_new_std_menu_items(ctrl_hdlr, &s5k2x7_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(s5k2x7_test_pattern_menu) - 1,
				     0, 0, s5k2x7_test_pattern_menu);

	if (ctrl_hdlr->error) {
		ret = ctrl_hdlr->error;
		goto error_free_hdlr;
	}

	ret = v4l2_fwnode_device_parse(s5k2x7->dev, &props);
	if (ret)
		goto error_free_hdlr;

	ret = v4l2_ctrl_new_fwnode_properties(ctrl_hdlr, &s5k2x7_ctrl_ops,
					      &props);
	if (ret)
		goto error_free_hdlr;

	s5k2x7->sd.ctrl_handler = ctrl_hdlr;

	return 0;

error_free_hdlr:
	v4l2_ctrl_handler_free(ctrl_hdlr);

	return ret;
}

/* The sensor reports 0xff in the frame counter once the output has stopped. */
static void s5k2x7_wait_stream_off(struct s5k2x7 *s5k2x7)
{
	unsigned int i;
	u64 val;

	for (i = 0; i < 5; i++) {
		if (!cci_read(s5k2x7->regmap, S5K2X7_REG_FRAME_COUNT, &val,
			      NULL) && val == S5K2X7_FRAME_COUNT_STOPPED)
			return;
		msleep(50);
	}

	dev_warn(s5k2x7->dev, "output did not stop\n");
}

static int s5k2x7_enable_streams(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state, u32 pad,
				 u64 streams_mask)
{
	struct s5k2x7 *s5k2x7 = to_s5k2x7(sd);
	const struct s5k2x7_mode *mode = s5k2x7->mode;
	const struct v4l2_rect *crop;
	int ret;

	ret = pm_runtime_resume_and_get(s5k2x7->dev);
	if (ret)
		return ret;

	cci_multi_reg_write(s5k2x7->regmap, s5k2x7_init_regs_a,
			    ARRAY_SIZE(s5k2x7_init_regs_a), &ret);
	if (ret)
		goto error;

	/* The vendor waits 3 ms between the first and the second part. */
	usleep_range(3 * USEC_PER_MSEC, 4 * USEC_PER_MSEC);

	cci_multi_reg_write(s5k2x7->regmap, s5k2x7_init_regs_b,
			    ARRAY_SIZE(s5k2x7_init_regs_b), &ret);
	cci_write(s5k2x7->regmap, S5K2X7_REG_MODE_SELECT, 0, &ret);
	if (ret)
		goto error;

	s5k2x7_wait_stream_off(s5k2x7);

	cci_multi_reg_write(s5k2x7->regmap, mode->regs, mode->num_regs, &ret);
	if (ret)
		goto error;

	crop = v4l2_subdev_state_get_crop(state, 0);
	cci_write(s5k2x7->regmap, S5K2X7_REG_X_START, crop->left, &ret);
	cci_write(s5k2x7->regmap, S5K2X7_REG_Y_START, crop->top, &ret);
	cci_write(s5k2x7->regmap, S5K2X7_REG_X_END,
		  crop->left + crop->width - 1, &ret);
	cci_write(s5k2x7->regmap, S5K2X7_REG_Y_END,
		  crop->top + crop->height - 1, &ret);
	cci_write(s5k2x7->regmap, S5K2X7_REG_X_OUTPUT,
		  crop->width / S5K2X7_BINNING, &ret);
	cci_write(s5k2x7->regmap, S5K2X7_REG_Y_OUTPUT,
		  crop->height / S5K2X7_BINNING, &ret);
	if (ret)
		goto error;

	ret = __v4l2_ctrl_handler_setup(s5k2x7->sd.ctrl_handler);
	if (ret)
		goto error;

	ret = cci_write(s5k2x7->regmap, S5K2X7_REG_MODE_SELECT,
			S5K2X7_MODE_STREAMING, NULL);
	if (ret)
		goto error;

	return 0;

error:
	dev_err(s5k2x7->dev, "failed to start streaming: %d\n", ret);
	pm_runtime_put_autosuspend(s5k2x7->dev);

	return ret;
}

static int s5k2x7_disable_streams(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state, u32 pad,
				  u64 streams_mask)
{
	struct s5k2x7 *s5k2x7 = to_s5k2x7(sd);
	int ret;

	ret = cci_write(s5k2x7->regmap, S5K2X7_REG_MODE_SELECT, 0, NULL);
	if (ret)
		dev_err(s5k2x7->dev, "failed to stop streaming: %d\n", ret);
	else
		s5k2x7_wait_stream_off(s5k2x7);

	pm_runtime_put_autosuspend(s5k2x7->dev);

	return ret;
}

static void s5k2x7_update_pad_format(const struct v4l2_rect *crop,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = S5K2X7_MBUS_CODE;
	fmt->width = crop->width / S5K2X7_BINNING;
	fmt->height = crop->height / S5K2X7_BINNING;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

/* The frame size changed: keep 30 fps, so the blanking follows the new size. */
static int s5k2x7_update_blanking(struct s5k2x7 *s5k2x7, u32 width, u32 height)
{
	const struct s5k2x7_mode *mode = s5k2x7->mode;
	s64 exposure_max = mode->vts - S5K2X7_EXPOSURE_MARGIN;

	s5k2x7->height = height;

	__v4l2_ctrl_modify_range(s5k2x7->hblank, mode->hts - width,
				 mode->hts - width, 1, mode->hts - width);
	__v4l2_ctrl_modify_range(s5k2x7->vblank, mode->vts - height,
				 S5K2X7_VTS_MAX - height, 1,
				 mode->vts - height);
	__v4l2_ctrl_s_ctrl(s5k2x7->vblank, mode->vts - height);

	__v4l2_ctrl_modify_range(s5k2x7->exposure, S5K2X7_EXPOSURE_MIN,
				 exposure_max, S5K2X7_EXPOSURE_STEP,
				 mode->exposure);
	__v4l2_ctrl_s_ctrl(s5k2x7->exposure, mode->exposure);

	return s5k2x7->sd.ctrl_handler->error;
}

static int s5k2x7_set_pad_format(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_format *fmt)
{
	struct s5k2x7 *s5k2x7 = to_s5k2x7(sd);
	const struct v4l2_rect *crop = v4l2_subdev_state_get_crop(state, 0);

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    v4l2_subdev_is_streaming(sd))
		return -EBUSY;

	/* There is no scaler: the size is the binned crop window. */
	s5k2x7_update_pad_format(crop, &fmt->format);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		return s5k2x7_update_blanking(s5k2x7, fmt->format.width,
					      fmt->format.height);

	return 0;
}

static int s5k2x7_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = S5K2X7_MBUS_CODE;

	return 0;
}

static int s5k2x7_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	const struct v4l2_mbus_framefmt *fmt;

	if (fse->index || fse->code != S5K2X7_MBUS_CODE)
		return -EINVAL;

	fmt = v4l2_subdev_state_get_format(state, 0);
	fse->min_width = fmt->width;
	fse->max_width = fmt->width;
	fse->min_height = fmt->height;
	fse->max_height = fmt->height;

	return 0;
}

static int s5k2x7_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
		sel->r = *v4l2_subdev_state_get_crop(state, 0);
		return 0;
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.left = 0;
		sel->r.top = 0;
		sel->r.width = S5K2X7_NATIVE_WIDTH;
		sel->r.height = S5K2X7_NATIVE_HEIGHT;
		return 0;
	}

	return -EINVAL;
}

static int s5k2x7_set_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	struct s5k2x7 *s5k2x7 = to_s5k2x7(sd);
	struct v4l2_rect *crop = v4l2_subdev_state_get_crop(state, 0);
	struct v4l2_mbus_framefmt *fmt = v4l2_subdev_state_get_format(state, 0);
	struct v4l2_rect r = sel->r;

	if (sel->target != V4L2_SEL_TGT_CROP)
		return -EINVAL;

	if (sel->which == V4L2_SUBDEV_FORMAT_ACTIVE &&
	    v4l2_subdev_is_streaming(sd))
		return -EBUSY;

	r.width = clamp(round_down(r.width, S5K2X7_CROP_ALIGN),
			S5K2X7_CROP_MIN_WIDTH, S5K2X7_NATIVE_WIDTH);
	r.height = clamp(round_down(r.height, S5K2X7_CROP_ALIGN),
			 S5K2X7_CROP_MIN_HEIGHT, S5K2X7_NATIVE_HEIGHT);
	r.left = clamp_t(int, round_down(max(r.left, 0), S5K2X7_CROP_ALIGN), 0,
			 S5K2X7_NATIVE_WIDTH - r.width);
	r.top = clamp_t(int, round_down(max(r.top, 0), S5K2X7_CROP_ALIGN), 0,
			S5K2X7_NATIVE_HEIGHT - r.height);

	*crop = r;
	sel->r = r;
	s5k2x7_update_pad_format(crop, fmt);

	if (sel->which == V4L2_SUBDEV_FORMAT_ACTIVE)
		return s5k2x7_update_blanking(s5k2x7, fmt->width, fmt->height);

	return 0;
}

static int s5k2x7_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct v4l2_rect *crop = v4l2_subdev_state_get_crop(state, 0);

	crop->left = 0;
	crop->top = 0;
	crop->width = S5K2X7_NATIVE_WIDTH;
	crop->height = S5K2X7_NATIVE_HEIGHT;
	s5k2x7_update_pad_format(crop, v4l2_subdev_state_get_format(state, 0));

	return 0;
}

static const struct v4l2_subdev_video_ops s5k2x7_video_ops = {
	.s_stream = v4l2_subdev_s_stream_helper,
};

static const struct v4l2_subdev_pad_ops s5k2x7_pad_ops = {
	.set_fmt = s5k2x7_set_pad_format,
	.get_fmt = v4l2_subdev_get_fmt,
	.get_selection = s5k2x7_get_selection,
	.set_selection = s5k2x7_set_selection,
	.enum_mbus_code = s5k2x7_enum_mbus_code,
	.enum_frame_size = s5k2x7_enum_frame_size,
	.enable_streams = s5k2x7_enable_streams,
	.disable_streams = s5k2x7_disable_streams,
};

static const struct v4l2_subdev_ops s5k2x7_subdev_ops = {
	.video = &s5k2x7_video_ops,
	.pad = &s5k2x7_pad_ops,
};

static const struct v4l2_subdev_internal_ops s5k2x7_internal_ops = {
	.init_state = s5k2x7_init_state,
};

static const struct media_entity_operations s5k2x7_subdev_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
};

static int s5k2x7_identify_sensor(struct s5k2x7 *s5k2x7)
{
	u64 val;
	int ret;

	ret = cci_read(s5k2x7->regmap, S5K2X7_REG_CHIP_ID, &val, NULL);
	if (ret)
		return dev_err_probe(s5k2x7->dev, ret,
				     "failed to read chip id\n");

	if (val != S5K2X7_CHIP_ID)
		return dev_err_probe(s5k2x7->dev, -ENODEV,
				     "chip id mismatch: 0x%04x != 0x%04llx\n",
				     S5K2X7_CHIP_ID, val);

	dev_info(s5k2x7->dev, "S5K2X7 chip id 0x%04llx\n", val);

	return 0;
}

static int s5k2x7_check_hwcfg(struct s5k2x7 *s5k2x7)
{
	struct fwnode_handle *fwnode = dev_fwnode(s5k2x7->dev), *ep;
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	unsigned long freq_bitmap;
	int ret;

	ep = fwnode_graph_get_next_endpoint(fwnode, NULL);
	if (!ep)
		return -EINVAL;

	ret = v4l2_fwnode_endpoint_alloc_parse(ep, &bus_cfg);
	fwnode_handle_put(ep);
	if (ret)
		return ret;

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != S5K2X7_DATA_LANES) {
		dev_err(s5k2x7->dev, "invalid number of data lanes: %u\n",
			bus_cfg.bus.mipi_csi2.num_data_lanes);
		ret = -EINVAL;
		goto endpoint_free;
	}

	ret = v4l2_link_freq_to_bitmap(s5k2x7->dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       s5k2x7_link_freq_menu,
				       ARRAY_SIZE(s5k2x7_link_freq_menu),
				       &freq_bitmap);

endpoint_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);

	return ret;
}

static int s5k2x7_power_on(struct device *dev)
{
	struct s5k2x7 *s5k2x7 = to_s5k2x7(dev_get_drvdata(dev));
	int i, ret;

	/* Vendor order: MCLK, powerdown and reset held, rails, powerdown, reset. */
	ret = clk_prepare_enable(s5k2x7->mclk);
	if (ret) {
		dev_err(dev, "failed to enable mclk: %d\n", ret);
		return ret;
	}

	gpiod_set_value_cansleep(s5k2x7->powerdown_gpio, 1);
	gpiod_set_value_cansleep(s5k2x7->reset_gpio, 1);

	for (i = 0; i < ARRAY_SIZE(s5k2x7_supply_name); i++) {
		ret = regulator_enable(s5k2x7->supplies[i].consumer);
		if (ret) {
			dev_err(dev, "failed to enable %s: %d\n",
				s5k2x7_supply_name[i], ret);
			goto err_rails;
		}
		fsleep(S5K2X7_POWER_STEP_US);
	}

	if (s5k2x7->afvdd) {
		ret = regulator_enable(s5k2x7->afvdd);
		if (ret) {
			dev_err(dev, "failed to enable afvdd: %d\n", ret);
			goto err_rails;
		}
		fsleep(S5K2X7_POWER_STEP_US);
	}

	gpiod_set_value_cansleep(s5k2x7->powerdown_gpio, 0);
	fsleep(USEC_PER_MSEC);
	gpiod_set_value_cansleep(s5k2x7->reset_gpio, 0);
	fsleep(USEC_PER_MSEC);

	return 0;

err_rails:
	while (--i >= 0)
		regulator_disable(s5k2x7->supplies[i].consumer);
	clk_disable_unprepare(s5k2x7->mclk);

	return ret;
}

static int s5k2x7_power_off(struct device *dev)
{
	struct s5k2x7 *s5k2x7 = to_s5k2x7(dev_get_drvdata(dev));
	int i;

	gpiod_set_value_cansleep(s5k2x7->reset_gpio, 1);
	gpiod_set_value_cansleep(s5k2x7->powerdown_gpio, 1);

	if (s5k2x7->afvdd)
		regulator_disable(s5k2x7->afvdd);

	for (i = ARRAY_SIZE(s5k2x7_supply_name) - 1; i >= 0; i--)
		regulator_disable(s5k2x7->supplies[i].consumer);

	clk_disable_unprepare(s5k2x7->mclk);

	return 0;
}

static int s5k2x7_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct s5k2x7 *s5k2x7;
	unsigned long freq;
	unsigned int i;
	int ret;

	s5k2x7 = devm_kzalloc(dev, sizeof(*s5k2x7), GFP_KERNEL);
	if (!s5k2x7)
		return -ENOMEM;

	s5k2x7->dev = dev;
	v4l2_i2c_subdev_init(&s5k2x7->sd, client, &s5k2x7_subdev_ops);

	s5k2x7->regmap = devm_cci_regmap_init_i2c(client, 16);
	if (IS_ERR(s5k2x7->regmap))
		return dev_err_probe(dev, PTR_ERR(s5k2x7->regmap),
				     "failed to init CCI\n");

	s5k2x7->mclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(s5k2x7->mclk))
		return dev_err_probe(dev, PTR_ERR(s5k2x7->mclk),
				     "failed to get MCLK clock\n");

	freq = clk_get_rate(s5k2x7->mclk);
	if (freq != S5K2X7_MCLK_FREQ)
		return dev_err_probe(dev, -EINVAL,
				     "MCLK must run at %lu Hz, is %lu Hz\n",
				     S5K2X7_MCLK_FREQ, freq);

	ret = s5k2x7_check_hwcfg(s5k2x7);
	if (ret)
		return dev_err_probe(dev, ret,
				     "failed to check HW configuration\n");

	s5k2x7->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(s5k2x7->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(s5k2x7->reset_gpio),
				     "cannot get reset GPIO\n");

	s5k2x7->powerdown_gpio = devm_gpiod_get(dev, "powerdown",
						GPIOD_OUT_HIGH);
	if (IS_ERR(s5k2x7->powerdown_gpio))
		return dev_err_probe(dev, PTR_ERR(s5k2x7->powerdown_gpio),
				     "cannot get powerdown GPIO\n");

	for (i = 0; i < ARRAY_SIZE(s5k2x7_supply_name); i++)
		s5k2x7->supplies[i].supply = s5k2x7_supply_name[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(s5k2x7->supplies),
				      s5k2x7->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	s5k2x7->afvdd = devm_regulator_get_optional(dev, "afvdd");
	if (IS_ERR(s5k2x7->afvdd)) {
		if (PTR_ERR(s5k2x7->afvdd) != -ENODEV)
			return dev_err_probe(dev, PTR_ERR(s5k2x7->afvdd),
					     "failed to get afvdd\n");
		s5k2x7->afvdd = NULL;
	}

	/* The sensor must be powered to read its chip id. */
	ret = s5k2x7_power_on(dev);
	if (ret)
		return ret;

	ret = s5k2x7_identify_sensor(s5k2x7);
	if (ret)
		goto power_off;

	s5k2x7->mode = &s5k2x7_supported_modes[0];
	s5k2x7->height = s5k2x7->mode->height;
	ret = s5k2x7_init_controls(s5k2x7);
	if (ret) {
		dev_err_probe(dev, ret, "failed to init controls\n");
		goto power_off;
	}

	s5k2x7->sd.state_lock = s5k2x7->ctrl_handler.lock;
	s5k2x7->sd.internal_ops = &s5k2x7_internal_ops;
	s5k2x7->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	s5k2x7->sd.entity.ops = &s5k2x7_subdev_entity_ops;
	s5k2x7->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	s5k2x7->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&s5k2x7->sd.entity, 1, &s5k2x7->pad);
	if (ret) {
		dev_err_probe(dev, ret, "failed to init media entity pads\n");
		goto v4l2_ctrl_handler_free;
	}

	ret = v4l2_subdev_init_finalize(&s5k2x7->sd);
	if (ret) {
		dev_err_probe(dev, ret, "failed to init subdev state\n");
		goto media_entity_cleanup;
	}

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);

	ret = v4l2_async_register_subdev_sensor(&s5k2x7->sd);
	if (ret) {
		dev_err_probe(dev, ret, "failed to register V4L2 subdev\n");
		goto subdev_cleanup;
	}

	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_idle(dev);

	return 0;

subdev_cleanup:
	v4l2_subdev_cleanup(&s5k2x7->sd);
	pm_runtime_disable(dev);
	pm_runtime_set_suspended(dev);

media_entity_cleanup:
	media_entity_cleanup(&s5k2x7->sd.entity);

v4l2_ctrl_handler_free:
	v4l2_ctrl_handler_free(s5k2x7->sd.ctrl_handler);

power_off:
	s5k2x7_power_off(dev);

	return ret;
}

static void s5k2x7_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct s5k2x7 *s5k2x7 = to_s5k2x7(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(sd->ctrl_handler);
	pm_runtime_disable(s5k2x7->dev);

	if (!pm_runtime_status_suspended(s5k2x7->dev)) {
		s5k2x7_power_off(s5k2x7->dev);
		pm_runtime_set_suspended(s5k2x7->dev);
	}
}

static const struct dev_pm_ops s5k2x7_pm_ops = {
	SET_RUNTIME_PM_OPS(s5k2x7_power_off, s5k2x7_power_on, NULL)
};

static const struct of_device_id s5k2x7_of_match[] = {
	{ .compatible = "samsung,s5k2x7" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, s5k2x7_of_match);

static struct i2c_driver s5k2x7_i2c_driver = {
	.driver = {
		.name = "s5k2x7",
		.pm = &s5k2x7_pm_ops,
		.of_match_table = s5k2x7_of_match,
	},
	.probe = s5k2x7_probe,
	.remove = s5k2x7_remove,
};
module_i2c_driver(s5k2x7_i2c_driver);

MODULE_AUTHOR("Fredrik Lindlöf <fredrik.lindlof@gmail.com>");
MODULE_DESCRIPTION("Samsung S5K2X7 image sensor driver");
MODULE_LICENSE("GPL");
