// SPDX-License-Identifier: GPL-2.0
/*
 * Driver for the GalaxyCore GC5035 image sensor
 *
 * The register tables and the power sequence come from the GPL Android
 * driver of the Planet Cosmo Communicator (GalaxyCore reference settings).
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
#include <linux/limits.h>
#include <linux/math.h>
#include <linux/minmax.h>
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
#include <media/v4l2-fwnode.h>
#include <media/v4l2-subdev.h>

/* The registers are banked; every access outside 0xf0-0xff needs the page first. */
#define GC5035_REG_PAGE			CCI_REG8(0xfe)

#define GC5035_REG_CHIP_ID		CCI_REG16(0xf0)
#define GC5035_CHIP_ID			0x5035

/* Page 0 */
#define GC5035_REG_STREAM		CCI_REG8(0x3e)
#define GC5035_STREAM_ON		0x91
#define GC5035_STREAM_OFF		0x01

#define GC5035_REG_EXPOSURE		CCI_REG16(0x03)
#define GC5035_EXPOSURE_MIN		4
#define GC5035_EXPOSURE_STEP		4
#define GC5035_EXPOSURE_MARGIN		16

#define GC5035_REG_VTS			CCI_REG16(0x41)
#define GC5035_VTS_STEP			4
#define GC5035_VTS_MAX			0x3ffc

#define GC5035_REG_AGAIN		CCI_REG8(0xb6)
#define GC5035_REG_DGAIN		CCI_REG16(0xb1)
#define GC5035_DGAIN_MASK		0x0ffc
#define GC5035_GAIN_BASE		256
#define GC5035_GAIN_MIN			GC5035_GAIN_BASE
#define GC5035_GAIN_MAX			(16 * GC5035_GAIN_BASE)

/* Page 1 */
#define GC5035_REG_TEST_PATTERN		CCI_REG8(0x8c)
#define GC5035_TEST_PATTERN_OFF		0x10
#define GC5035_TEST_PATTERN_ON		0x11

#define GC5035_NATIVE_WIDTH		2592
#define GC5035_NATIVE_HEIGHT		1944

#define GC5035_XCLK_FREQ		(24 * HZ_PER_MHZ)
#define GC5035_DATA_LANES		2
#define GC5035_BITS_PER_PIXEL		10
/* Unflipped color order is taken from the vendor's RAW_R, measured with the test pattern. */
#define GC5035_MBUS_CODE		MEDIA_BUS_FMT_SRGGB10_1X10

#define GC5035_REG_ORIENTATION		CCI_REG8(0x17)
#define GC5035_ORIENTATION_BASE		0x80
#define GC5035_ORIENTATION_HFLIP	BIT(0)
#define GC5035_ORIENTATION_VFLIP	BIT(1)

#define GC5035_POWER_STEP_US		(10 * USEC_PER_MSEC)

static const char * const gc5035_test_pattern_menu[] = {
	"Disabled",
	"Enabled",
};

/*
 * UNKNOWN: the vendor driver never states the link rate (the receiver
 * uses a fixed settle count). These are pixel clock * 10 bit / 2 lanes / 2
 * (DDR), the lowest rate that carries the pixels, and are not measured.
 */
static const s64 gc5035_link_freq_menu[] = {
	438 * HZ_PER_MHZ,
	219 * HZ_PER_MHZ,
};

/* Rails in power-on order: DOVDD, AVDD, DVDD (off in reverse). */
static const char * const gc5035_supply_name[] = {
	"dovdd",
	"avdd",
	"dvdd",
};

/* Analog gain steps: gain in 1/256 units at or above which a code applies. */
static const struct {
	u16 gain;
	u8 code;
} gc5035_again_table[] = {
	{  256,  0 }, {  302,  1 }, {  358,  2 }, {  425,  3 },
	{  502,  8 }, {  599,  9 }, {  717, 10 }, {  845, 11 },
	{  998, 12 }, { 1203, 13 }, { 1434, 14 }, { 1710, 15 },
	{ 1997, 16 }, { 2355, 17 }, { 2816, 18 }, { 3318, 19 },
	{ 3994, 20 },
};

static const struct cci_reg_sequence gc5035_init_regs[] = {
	/* SYSTEM */
	{ CCI_REG8(0xfc), 0x01 },
	{ CCI_REG8(0xf4), 0x40 },
	{ CCI_REG8(0xf5), 0xe9 },
	{ CCI_REG8(0xf6), 0x14 },
	{ CCI_REG8(0xf8), 0x49 },
	{ CCI_REG8(0xf9), 0x82 },
	{ CCI_REG8(0xfa), 0x00 },
	{ CCI_REG8(0xfc), 0x81 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0x36), 0x01 },
	{ CCI_REG8(0xd3), 0x87 },
	{ CCI_REG8(0x36), 0x00 },
	{ CCI_REG8(0x33), 0x00 },
	{ CCI_REG8(0xfe), 0x03 },
	{ CCI_REG8(0x01), 0xe7 },
	{ CCI_REG8(0xf7), 0x01 },
	{ CCI_REG8(0xfc), 0x8f },
	{ CCI_REG8(0xfc), 0x8f },
	{ CCI_REG8(0xfc), 0x8e },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xee), 0x30 },
	{ CCI_REG8(0x87), 0x18 },
	{ CCI_REG8(0xfe), 0x01 },
	{ CCI_REG8(0x8c), 0x90 },
	{ CCI_REG8(0xfe), 0x00 },
	/* Analog & CISCTL */
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0x05), 0x02 },
	{ CCI_REG8(0x06), 0xda },
	{ CCI_REG8(0x9d), 0x0c },
	{ CCI_REG8(0x09), 0x00 },
	{ CCI_REG8(0x0a), 0x04 },
	{ CCI_REG8(0x0b), 0x00 },
	{ CCI_REG8(0x0c), 0x03 },
	{ CCI_REG8(0x0d), 0x07 },
	{ CCI_REG8(0x0e), 0xa8 },
	{ CCI_REG8(0x0f), 0x0a },
	{ CCI_REG8(0x10), 0x30 },
	{ CCI_REG8(0x11), 0x02 },
	{ CCI_REG8(0x17), 0x80 },
	{ CCI_REG8(0x19), 0x05 },
	{ CCI_REG8(0xfe), 0x02 },
	{ CCI_REG8(0x30), 0x03 },
	{ CCI_REG8(0x31), 0x03 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xd9), 0xc0 },
	{ CCI_REG8(0x1b), 0x20 },
	{ CCI_REG8(0x21), 0x48 },
	{ CCI_REG8(0x28), 0x22 },
	{ CCI_REG8(0x29), 0x58 },
	{ CCI_REG8(0x44), 0x20 },
	{ CCI_REG8(0x4b), 0x10 },
	{ CCI_REG8(0x4e), 0x1a },
	{ CCI_REG8(0x50), 0x11 },
	{ CCI_REG8(0x52), 0x33 },
	{ CCI_REG8(0x53), 0x44 },
	{ CCI_REG8(0x55), 0x10 },
	{ CCI_REG8(0x5b), 0x11 },
	{ CCI_REG8(0xc5), 0x02 },
	{ CCI_REG8(0x8c), 0x1a },
	{ CCI_REG8(0xfe), 0x02 },
	{ CCI_REG8(0x33), 0x05 },
	{ CCI_REG8(0x32), 0x38 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0x91), 0x80 },
	{ CCI_REG8(0x92), 0x28 },
	{ CCI_REG8(0x93), 0x20 },
	{ CCI_REG8(0x95), 0xa0 },
	{ CCI_REG8(0x96), 0xe0 },
	{ CCI_REG8(0xd5), 0xfc },
	{ CCI_REG8(0x97), 0x28 },
	{ CCI_REG8(0x16), 0x0c },
	{ CCI_REG8(0x1a), 0x1a },
	{ CCI_REG8(0x1f), 0x11 },
	{ CCI_REG8(0x20), 0x10 },
	{ CCI_REG8(0x46), 0xe3 },
	{ CCI_REG8(0x4a), 0x04 },
	{ CCI_REG8(0x54), 0x02 },
	{ CCI_REG8(0x62), 0x00 },
	{ CCI_REG8(0x72), 0xcf },
	{ CCI_REG8(0x73), 0xc9 },
	{ CCI_REG8(0x7a), 0x05 },
	{ CCI_REG8(0x7d), 0xcc },
	{ CCI_REG8(0x90), 0x00 },
	{ CCI_REG8(0xce), 0x98 },
	{ CCI_REG8(0xd0), 0xb2 },
	{ CCI_REG8(0xd2), 0x40 },
	{ CCI_REG8(0xe6), 0xe0 },
	{ CCI_REG8(0xfe), 0x02 },
	{ CCI_REG8(0x12), 0x01 },
	{ CCI_REG8(0x13), 0x01 },
	{ CCI_REG8(0x14), 0x01 },
	{ CCI_REG8(0x15), 0x02 },
	{ CCI_REG8(0x22), 0x7c },
	{ CCI_REG8(0x91), 0x00 },
	{ CCI_REG8(0x92), 0x00 },
	{ CCI_REG8(0x93), 0x00 },
	{ CCI_REG8(0x94), 0x00 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfc), 0x88 },
	{ CCI_REG8(0xfe), 0x10 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfc), 0x8e },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfc), 0x88 },
	{ CCI_REG8(0xfe), 0x10 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfc), 0x8e },
	/* Gain */
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xb0), 0x6e },
	{ CCI_REG8(0xb1), 0x01 },
	{ CCI_REG8(0xb2), 0x00 },
	{ CCI_REG8(0xb3), 0x00 },
	{ CCI_REG8(0xb4), 0x00 },
	{ CCI_REG8(0xb6), 0x00 },
	/* ISP */
	{ CCI_REG8(0xfe), 0x01 },
	{ CCI_REG8(0x53), 0x00 },
	{ CCI_REG8(0x89), 0x03 },
	{ CCI_REG8(0x60), 0x40 },
	/* BLK */
	{ CCI_REG8(0xfe), 0x01 },
	{ CCI_REG8(0x42), 0x21 },
	{ CCI_REG8(0x49), 0x03 },
	{ CCI_REG8(0x4a), 0xff },
	{ CCI_REG8(0x4b), 0xc0 },
	{ CCI_REG8(0x55), 0x00 },
	/* Anti_blooming */
	{ CCI_REG8(0xfe), 0x01 },
	{ CCI_REG8(0x41), 0x28 },
	{ CCI_REG8(0x4c), 0x00 },
	{ CCI_REG8(0x4d), 0x00 },
	{ CCI_REG8(0x4e), 0x3c },
	{ CCI_REG8(0x44), 0x08 },
	{ CCI_REG8(0x48), 0x01 },
	/* Crop */
	{ CCI_REG8(0xfe), 0x01 },
	{ CCI_REG8(0x91), 0x00 },
	{ CCI_REG8(0x92), 0x08 },
	{ CCI_REG8(0x93), 0x00 },
	{ CCI_REG8(0x94), 0x07 },
	{ CCI_REG8(0x95), 0x07 },
	{ CCI_REG8(0x96), 0x98 },
	{ CCI_REG8(0x97), 0x0a },
	{ CCI_REG8(0x98), 0x20 },
	{ CCI_REG8(0x99), 0x00 },
	/* MIPI */
	{ CCI_REG8(0xfe), 0x03 },
	{ CCI_REG8(0x02), 0x57 },
	{ CCI_REG8(0x03), 0xb7 },
	{ CCI_REG8(0x15), 0x14 },
	{ CCI_REG8(0x18), 0x0f },
	{ CCI_REG8(0x21), 0x22 },
	{ CCI_REG8(0x22), 0x06 },
	{ CCI_REG8(0x23), 0x48 },
	{ CCI_REG8(0x24), 0x12 },
	{ CCI_REG8(0x25), 0x28 },
	{ CCI_REG8(0x26), 0x08 },
	{ CCI_REG8(0x29), 0x06 },
	{ CCI_REG8(0x2a), 0x58 },
	{ CCI_REG8(0x2b), 0x08 },
	{ CCI_REG8(0xfe), 0x01 },
	{ CCI_REG8(0x8c), 0x10 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0x3e), 0x01 },
};

static const struct cci_reg_sequence gc5035_binned_regs[] = {
	/* System */
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0x3e), 0x01 },
	{ CCI_REG8(0xfc), 0x01 },
	{ CCI_REG8(0xf4), 0x40 },
	{ CCI_REG8(0xf5), 0xe4 },
	{ CCI_REG8(0xf6), 0x14 },
	{ CCI_REG8(0xf8), 0x49 },
	{ CCI_REG8(0xf9), 0x12 },
	{ CCI_REG8(0xfa), 0x01 },
	{ CCI_REG8(0xfc), 0x81 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0x36), 0x01 },
	{ CCI_REG8(0xd3), 0x87 },
	{ CCI_REG8(0x36), 0x00 },
	{ CCI_REG8(0x33), 0x20 },
	{ CCI_REG8(0xfe), 0x03 },
	{ CCI_REG8(0x01), 0x87 },
	{ CCI_REG8(0xf7), 0x11 },
	{ CCI_REG8(0xfc), 0x8f },
	{ CCI_REG8(0xfc), 0x8f },
	{ CCI_REG8(0xfc), 0x8e },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xee), 0x30 },
	{ CCI_REG8(0x87), 0x18 },
	{ CCI_REG8(0xfe), 0x01 },
	{ CCI_REG8(0x8c), 0x90 },
	{ CCI_REG8(0xfe), 0x00 },
	/* Analog & CISCTL */
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0x05), 0x02 },
	{ CCI_REG8(0x06), 0xda },
	{ CCI_REG8(0x9d), 0x0c },
	{ CCI_REG8(0x09), 0x00 },
	{ CCI_REG8(0x0a), 0x04 },
	{ CCI_REG8(0x0b), 0x00 },
	{ CCI_REG8(0x0c), 0x03 },
	{ CCI_REG8(0x0d), 0x07 },
	{ CCI_REG8(0x0e), 0xa8 },
	{ CCI_REG8(0x0f), 0x0a },
	{ CCI_REG8(0x10), 0x30 },
	{ CCI_REG8(0x21), 0x60 },
	{ CCI_REG8(0x29), 0x30 },
	{ CCI_REG8(0x44), 0x18 },
	{ CCI_REG8(0x4e), 0x20 },
	{ CCI_REG8(0x8c), 0x20 },
	{ CCI_REG8(0x91), 0x15 },
	{ CCI_REG8(0x92), 0x3a },
	{ CCI_REG8(0x93), 0x20 },
	{ CCI_REG8(0x95), 0x45 },
	{ CCI_REG8(0x96), 0x35 },
	{ CCI_REG8(0xd5), 0xf0 },
	{ CCI_REG8(0x97), 0x20 },
	{ CCI_REG8(0x1f), 0x19 },
	{ CCI_REG8(0xce), 0x9b },
	{ CCI_REG8(0xd0), 0xb3 },
	{ CCI_REG8(0xfe), 0x02 },
	{ CCI_REG8(0x14), 0x02 },
	{ CCI_REG8(0x15), 0x00 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfc), 0x88 },
	{ CCI_REG8(0xfe), 0x10 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfc), 0x8e },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfc), 0x88 },
	{ CCI_REG8(0xfe), 0x10 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfc), 0x8e },
	/* BLK */
	{ CCI_REG8(0xfe), 0x01 },
	{ CCI_REG8(0x49), 0x00 },
	{ CCI_REG8(0x4a), 0x01 },
	{ CCI_REG8(0x4b), 0xf8 },
	/* Anti_blooming */
	{ CCI_REG8(0xfe), 0x01 },
	{ CCI_REG8(0x4e), 0x06 },
	{ CCI_REG8(0x44), 0x02 },
	/* Crop */
	{ CCI_REG8(0xfe), 0x01 },
	{ CCI_REG8(0x91), 0x00 },
	{ CCI_REG8(0x92), 0x04 },
	{ CCI_REG8(0x93), 0x00 },
	{ CCI_REG8(0x94), 0x03 },
	{ CCI_REG8(0x95), 0x03 },
	{ CCI_REG8(0x96), 0xcc },
	{ CCI_REG8(0x97), 0x05 },
	{ CCI_REG8(0x98), 0x10 },
	{ CCI_REG8(0x99), 0x00 },
	/* MIPI */
	{ CCI_REG8(0xfe), 0x03 },
	{ CCI_REG8(0x02), 0x58 },
	{ CCI_REG8(0x22), 0x03 },
	{ CCI_REG8(0x26), 0x06 },
	{ CCI_REG8(0x29), 0x03 },
	{ CCI_REG8(0x2b), 0x06 },
	{ CCI_REG8(0xfe), 0x01 },
	{ CCI_REG8(0x8c), 0x10 },
	{ CCI_REG8(0xfe), 0x00 },
};

static const struct cci_reg_sequence gc5035_full_regs[] = {
	/* System */
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0x3e), 0x01 },
	{ CCI_REG8(0xfc), 0x01 },
	{ CCI_REG8(0xf4), 0x40 },
	{ CCI_REG8(0xf5), 0xe9 },
	{ CCI_REG8(0xf6), 0x14 },
	{ CCI_REG8(0xf8), 0x49 },
	{ CCI_REG8(0xf9), 0x82 },
	{ CCI_REG8(0xfa), 0x00 },
	{ CCI_REG8(0xfc), 0x81 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0x36), 0x01 },
	{ CCI_REG8(0xd3), 0x87 },
	{ CCI_REG8(0x36), 0x00 },
	{ CCI_REG8(0x33), 0x00 },
	{ CCI_REG8(0xfe), 0x03 },
	{ CCI_REG8(0x01), 0xe7 },
	{ CCI_REG8(0xf7), 0x01 },
	{ CCI_REG8(0xfc), 0x8f },
	{ CCI_REG8(0xfc), 0x8f },
	{ CCI_REG8(0xfc), 0x8e },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xee), 0x30 },
	{ CCI_REG8(0x87), 0x18 },
	{ CCI_REG8(0xfe), 0x01 },
	{ CCI_REG8(0x8c), 0x90 },
	{ CCI_REG8(0xfe), 0x00 },
	/* Analog & CISCTL */
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0x05), 0x02 },
	{ CCI_REG8(0x06), 0xda },
	{ CCI_REG8(0x9d), 0x0c },
	{ CCI_REG8(0x09), 0x00 },
	{ CCI_REG8(0x0a), 0x04 },
	{ CCI_REG8(0x0b), 0x00 },
	{ CCI_REG8(0x0c), 0x03 },
	{ CCI_REG8(0x0d), 0x07 },
	{ CCI_REG8(0x0e), 0xa8 },
	{ CCI_REG8(0x0f), 0x0a },
	{ CCI_REG8(0x10), 0x30 },
	{ CCI_REG8(0x21), 0x48 },
	{ CCI_REG8(0x29), 0x58 },
	{ CCI_REG8(0x44), 0x20 },
	{ CCI_REG8(0x4e), 0x1a },
	{ CCI_REG8(0x8c), 0x1a },
	{ CCI_REG8(0x91), 0x80 },
	{ CCI_REG8(0x92), 0x28 },
	{ CCI_REG8(0x93), 0x20 },
	{ CCI_REG8(0x95), 0xa0 },
	{ CCI_REG8(0x96), 0xe0 },
	{ CCI_REG8(0xd5), 0xfc },
	{ CCI_REG8(0x97), 0x28 },
	{ CCI_REG8(0x1f), 0x11 },
	{ CCI_REG8(0xce), 0x98 },
	{ CCI_REG8(0xd0), 0xb2 },
	{ CCI_REG8(0xfe), 0x02 },
	{ CCI_REG8(0x14), 0x01 },
	{ CCI_REG8(0x15), 0x02 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfc), 0x88 },
	{ CCI_REG8(0xfe), 0x10 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfc), 0x8e },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfc), 0x88 },
	{ CCI_REG8(0xfe), 0x10 },
	{ CCI_REG8(0xfe), 0x00 },
	{ CCI_REG8(0xfc), 0x8e },
	/* BLK */
	{ CCI_REG8(0xfe), 0x01 },
	{ CCI_REG8(0x49), 0x03 },
	{ CCI_REG8(0x4a), 0xff },
	{ CCI_REG8(0x4b), 0xc0 },
	/* Anti_blooming */
	{ CCI_REG8(0xfe), 0x01 },
	{ CCI_REG8(0x4e), 0x3c },
	{ CCI_REG8(0x44), 0x08 },
	/* Crop */
	{ CCI_REG8(0xfe), 0x01 },
	{ CCI_REG8(0x91), 0x00 },
	{ CCI_REG8(0x92), 0x08 },
	{ CCI_REG8(0x93), 0x00 },
	{ CCI_REG8(0x94), 0x07 },
	{ CCI_REG8(0x95), 0x07 },
	{ CCI_REG8(0x96), 0x98 },
	{ CCI_REG8(0x97), 0x0a },
	{ CCI_REG8(0x98), 0x20 },
	{ CCI_REG8(0x99), 0x00 },
	/* MIPI */
	{ CCI_REG8(0xfe), 0x03 },
	{ CCI_REG8(0x02), 0x57 },
	{ CCI_REG8(0x22), 0x06 },
	{ CCI_REG8(0x26), 0x08 },
	{ CCI_REG8(0x29), 0x06 },
	{ CCI_REG8(0x2b), 0x08 },
	{ CCI_REG8(0xfe), 0x01 },
	{ CCI_REG8(0x8c), 0x10 },
	{ CCI_REG8(0xfe), 0x00 },
};


struct gc5035_mode {
	u32 width;
	u32 height;
	const struct cci_reg_sequence *regs;
	unsigned int num_regs;
	u32 hts;
	u32 vts;
	u32 link_freq_idx;
	u64 pixel_rate;
};

/* Ordered from the biggest to the smallest frame. */
static const struct gc5035_mode gc5035_modes[] = {
	{
		/* 2592x1944 at 30 fps, pixel clock 175.2 MHz */
		.width = 2592,
		.height = 1944,
		.regs = gc5035_full_regs,
		.num_regs = ARRAY_SIZE(gc5035_full_regs),
		.hts = 2920,
		.vts = 2008,
		.link_freq_idx = 0,
		.pixel_rate = 175200000,
	},
	{
		/* 1296x972 (2x2 binned) at 30 fps, pixel clock 87.6 MHz */
		.width = 1296,
		.height = 972,
		.regs = gc5035_binned_regs,
		.num_regs = ARRAY_SIZE(gc5035_binned_regs),
		.hts = 1460,
		.vts = 2008,
		.link_freq_idx = 1,
		.pixel_rate = 87600000,
	},
};

struct gc5035 {
	struct device *dev;
	struct v4l2_subdev sd;
	struct media_pad pad;

	struct clk *xclk;
	struct regulator_bulk_data supplies[ARRAY_SIZE(gc5035_supply_name)];
	struct gpio_desc *reset_gpio;
	struct gpio_desc *powerdown_gpio;

	struct v4l2_ctrl_handler ctrls;
	struct v4l2_ctrl *pixel_rate;
	struct v4l2_ctrl *link_freq;
	struct v4l2_ctrl *exposure;
	struct v4l2_ctrl *vblank;
	struct v4l2_ctrl *hblank;
	struct v4l2_ctrl *hflip;
	struct v4l2_ctrl *vflip;

	struct regmap *regmap;
	unsigned long link_freq_bitmap;
	const struct gc5035_mode *cur_mode;
};

static inline struct gc5035 *to_gc5035(struct v4l2_subdev *sd)
{
	return container_of(sd, struct gc5035, sd);
}

static int gc5035_power_on(struct device *dev)
{
	struct gc5035 *gc5035 = to_gc5035(dev_get_drvdata(dev));
	int i, ret;

	/* Vendor order: MCLK, reset and powerdown held, rails, reset, powerdown. */
	ret = clk_prepare_enable(gc5035->xclk);
	if (ret) {
		dev_err(dev, "failed to enable xclk: %d\n", ret);
		return ret;
	}

	gpiod_set_value_cansleep(gc5035->reset_gpio, 1);
	gpiod_set_value_cansleep(gc5035->powerdown_gpio, 1);

	for (i = 0; i < ARRAY_SIZE(gc5035_supply_name); i++) {
		ret = regulator_enable(gc5035->supplies[i].consumer);
		if (ret) {
			dev_err(dev, "failed to enable %s: %d\n",
				gc5035_supply_name[i], ret);
			goto err_rails;
		}
		fsleep(GC5035_POWER_STEP_US);
	}

	gpiod_set_value_cansleep(gc5035->reset_gpio, 0);
	fsleep(GC5035_POWER_STEP_US);
	gpiod_set_value_cansleep(gc5035->powerdown_gpio, 0);
	fsleep(GC5035_POWER_STEP_US);

	return 0;

err_rails:
	while (--i >= 0)
		regulator_disable(gc5035->supplies[i].consumer);
	clk_disable_unprepare(gc5035->xclk);

	return ret;
}

static int gc5035_power_off(struct device *dev)
{
	struct gc5035 *gc5035 = to_gc5035(dev_get_drvdata(dev));
	int i;

	gpiod_set_value_cansleep(gc5035->powerdown_gpio, 1);
	gpiod_set_value_cansleep(gc5035->reset_gpio, 1);

	for (i = ARRAY_SIZE(gc5035_supply_name) - 1; i >= 0; i--)
		regulator_disable(gc5035->supplies[i].consumer);

	clk_disable_unprepare(gc5035->xclk);

	return 0;
}

/* The flips move the first pixel of the array, which changes the Bayer order. */
static u32 gc5035_mbus_code(const struct gc5035 *gc5035)
{
	static const u32 codes[2][2] = {
		{ MEDIA_BUS_FMT_SRGGB10_1X10, MEDIA_BUS_FMT_SGRBG10_1X10 },
		{ MEDIA_BUS_FMT_SGBRG10_1X10, MEDIA_BUS_FMT_SBGGR10_1X10 },
	};

	if (!gc5035->hflip || !gc5035->vflip)
		return GC5035_MBUS_CODE;

	return codes[gc5035->vflip->val][gc5035->hflip->val];
}

static int gc5035_enum_mbus_code(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state,
				 struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->index > 0)
		return -EINVAL;

	code->code = gc5035_mbus_code(to_gc5035(sd));

	return 0;
}

static bool gc5035_mode_usable(const struct gc5035 *gc5035,
			       const struct gc5035_mode *mode)
{
	return gc5035->link_freq_bitmap & BIT(mode->link_freq_idx);
}

static int gc5035_enum_frame_size(struct v4l2_subdev *sd,
				  struct v4l2_subdev_state *state,
				  struct v4l2_subdev_frame_size_enum *fse)
{
	struct gc5035 *gc5035 = to_gc5035(sd);
	unsigned int i, n = 0;

	if (fse->code != gc5035_mbus_code(gc5035))
		return -EINVAL;

	for (i = 0; i < ARRAY_SIZE(gc5035_modes); i++) {
		if (!gc5035_mode_usable(gc5035, &gc5035_modes[i]))
			continue;
		if (n++ != fse->index)
			continue;

		fse->min_width = gc5035_modes[i].width;
		fse->max_width = gc5035_modes[i].width;
		fse->min_height = gc5035_modes[i].height;
		fse->max_height = gc5035_modes[i].height;

		return 0;
	}

	return -EINVAL;
}

static const struct gc5035_mode *
gc5035_find_mode(const struct gc5035 *gc5035, u32 width, u32 height)
{
	const struct gc5035_mode *best = NULL;
	u32 best_dist = U32_MAX;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(gc5035_modes); i++) {
		const struct gc5035_mode *mode = &gc5035_modes[i];
		u32 dist;

		if (!gc5035_mode_usable(gc5035, mode))
			continue;

		dist = abs((int)mode->width - (int)width) +
		       abs((int)mode->height - (int)height);
		if (dist < best_dist) {
			best_dist = dist;
			best = mode;
		}
	}

	return best;
}

static int gc5035_update_mode_controls(struct gc5035 *gc5035,
				       const struct gc5035_mode *mode)
{
	s64 hblank = mode->hts - mode->width;
	s64 exposure_max = mode->vts - GC5035_EXPOSURE_MARGIN;
	int ret;

	ret = __v4l2_ctrl_s_ctrl(gc5035->link_freq, mode->link_freq_idx);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(gc5035->pixel_rate, mode->pixel_rate,
				       mode->pixel_rate, 1, mode->pixel_rate);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(gc5035->hblank, hblank, hblank, 1,
				       hblank);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(gc5035->vblank,
				       mode->vts - mode->height,
				       GC5035_VTS_MAX - mode->height,
				       GC5035_VTS_STEP,
				       mode->vts - mode->height);
	if (ret)
		return ret;

	ret = __v4l2_ctrl_modify_range(gc5035->exposure, GC5035_EXPOSURE_MIN,
				       exposure_max, GC5035_EXPOSURE_STEP,
				       exposure_max);
	if (ret)
		return ret;

	return __v4l2_ctrl_s_ctrl(gc5035->vblank, mode->vts - mode->height);
}

static void gc5035_update_pad_format(const struct gc5035 *gc5035,
				     const struct gc5035_mode *mode,
				     struct v4l2_mbus_framefmt *fmt)
{
	fmt->width = mode->width;
	fmt->height = mode->height;
	fmt->code = gc5035_mbus_code(gc5035);
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_MAP_YCBCR_ENC_DEFAULT(fmt->colorspace);
	fmt->quantization = V4L2_QUANTIZATION_FULL_RANGE;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int gc5035_set_format(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *fmt)
{
	struct gc5035 *gc5035 = to_gc5035(sd);
	const struct gc5035_mode *mode;

	mode = gc5035_find_mode(gc5035, fmt->format.width, fmt->format.height);
	if (!mode)
		return -EINVAL;

	gc5035_update_pad_format(gc5035, mode, &fmt->format);
	*v4l2_subdev_state_get_format(state, 0) = fmt->format;

	if (fmt->which == V4L2_SUBDEV_FORMAT_TRY)
		return 0;

	if (gc5035->cur_mode == mode)
		return 0;

	gc5035->cur_mode = mode;

	return gc5035_update_mode_controls(gc5035, mode);
}

static int gc5035_get_selection(struct v4l2_subdev *sd,
				struct v4l2_subdev_state *state,
				struct v4l2_subdev_selection *sel)
{
	switch (sel->target) {
	case V4L2_SEL_TGT_CROP:
	case V4L2_SEL_TGT_CROP_DEFAULT:
	case V4L2_SEL_TGT_CROP_BOUNDS:
	case V4L2_SEL_TGT_NATIVE_SIZE:
		sel->r.top = 0;
		sel->r.left = 0;
		sel->r.width = GC5035_NATIVE_WIDTH;
		sel->r.height = GC5035_NATIVE_HEIGHT;
		return 0;
	}

	return -EINVAL;
}

static int gc5035_init_state(struct v4l2_subdev *sd,
			     struct v4l2_subdev_state *state)
{
	struct gc5035 *gc5035 = to_gc5035(sd);
	struct v4l2_subdev_format fmt = {
		.which = V4L2_SUBDEV_FORMAT_TRY,
		.format = {
			.code = GC5035_MBUS_CODE,
			.width = gc5035_modes[0].width,
			.height = gc5035_modes[0].height,
		},
	};

	if (!gc5035_mode_usable(gc5035, &gc5035_modes[0])) {
		fmt.format.width = gc5035_modes[1].width;
		fmt.format.height = gc5035_modes[1].height;
	}

	return gc5035_set_format(sd, state, &fmt);
}

/* Split a gain in 1/256 units into the analog step and the digital remainder. */
static void gc5035_gain_to_regs(u32 gain, u8 *again, u16 *dgain)
{
	int i;

	gain = clamp(gain, GC5035_GAIN_MIN, GC5035_GAIN_MAX);

	for (i = ARRAY_SIZE(gc5035_again_table) - 1; i > 0; i--)
		if (gain >= gc5035_again_table[i].gain)
			break;

	*again = gc5035_again_table[i].code;
	*dgain = (gain * 256 / gc5035_again_table[i].gain) & GC5035_DGAIN_MASK;
}

static int gc5035_set_gain(struct gc5035 *gc5035, u32 gain)
{
	int ret = 0;
	u16 dgain;
	u8 again;

	gc5035_gain_to_regs(gain, &again, &dgain);

	cci_write(gc5035->regmap, GC5035_REG_PAGE, 0, &ret);
	cci_write(gc5035->regmap, GC5035_REG_AGAIN, again, &ret);
	cci_write(gc5035->regmap, GC5035_REG_DGAIN, dgain, &ret);

	return ret;
}

static int gc5035_set_test_pattern(struct gc5035 *gc5035, u32 val)
{
	int ret = 0;

	cci_write(gc5035->regmap, GC5035_REG_PAGE, 1, &ret);
	cci_write(gc5035->regmap, GC5035_REG_TEST_PATTERN,
		  val ? GC5035_TEST_PATTERN_ON : GC5035_TEST_PATTERN_OFF, &ret);
	cci_write(gc5035->regmap, GC5035_REG_PAGE, 0, &ret);

	return ret;
}

static int gc5035_set_ctrl(struct v4l2_ctrl *ctrl)
{
	struct gc5035 *gc5035 = container_of(ctrl->handler, struct gc5035,
					     ctrls);
	int ret = 0;

	if (ctrl->id == V4L2_CID_VBLANK) {
		s64 exposure_max = gc5035->cur_mode->height + ctrl->val -
				   GC5035_EXPOSURE_MARGIN;

		ret = __v4l2_ctrl_modify_range(gc5035->exposure,
					       gc5035->exposure->minimum,
					       exposure_max,
					       gc5035->exposure->step,
					       exposure_max);
		if (ret)
			return ret;
	}

	/* Controls reach the sensor only while it is powered. */
	if (!pm_runtime_get_if_active(gc5035->dev))
		return 0;

	switch (ctrl->id) {
	case V4L2_CID_EXPOSURE:
		cci_write(gc5035->regmap, GC5035_REG_PAGE, 0, &ret);
		cci_write(gc5035->regmap, GC5035_REG_EXPOSURE, ctrl->val, &ret);
		break;
	case V4L2_CID_ANALOGUE_GAIN:
		ret = gc5035_set_gain(gc5035, ctrl->val);
		break;
	case V4L2_CID_VBLANK:
		cci_write(gc5035->regmap, GC5035_REG_PAGE, 0, &ret);
		cci_write(gc5035->regmap, GC5035_REG_VTS,
			  gc5035->cur_mode->height + ctrl->val, &ret);
		break;
	case V4L2_CID_TEST_PATTERN:
		ret = gc5035_set_test_pattern(gc5035, ctrl->val);
		break;
	case V4L2_CID_HFLIP:
	case V4L2_CID_VFLIP:
		cci_write(gc5035->regmap, GC5035_REG_PAGE, 0, &ret);
		cci_write(gc5035->regmap, GC5035_REG_ORIENTATION,
			  GC5035_ORIENTATION_BASE |
			  (gc5035->hflip->val ? GC5035_ORIENTATION_HFLIP : 0) |
			  (gc5035->vflip->val ? GC5035_ORIENTATION_VFLIP : 0),
			  &ret);
		break;
	default:
		break;
	}

	pm_runtime_put(gc5035->dev);

	return ret;
}

static const struct v4l2_ctrl_ops gc5035_ctrl_ops = {
	.s_ctrl = gc5035_set_ctrl,
};

static int gc5035_start_streaming(struct gc5035 *gc5035)
{
	const struct gc5035_mode *mode = gc5035->cur_mode;
	int ret;

	ret = pm_runtime_resume_and_get(gc5035->dev);
	if (ret < 0)
		return ret;

	ret = cci_multi_reg_write(gc5035->regmap, gc5035_init_regs,
				  ARRAY_SIZE(gc5035_init_regs), NULL);
	if (ret)
		goto err_rpm_put;

	ret = cci_multi_reg_write(gc5035->regmap, mode->regs, mode->num_regs,
				  NULL);
	if (ret)
		goto err_rpm_put;

	ret = __v4l2_ctrl_handler_setup(&gc5035->ctrls);
	if (ret)
		goto err_rpm_put;

	ret = cci_write(gc5035->regmap, GC5035_REG_STREAM, GC5035_STREAM_ON,
			NULL);
	if (ret)
		goto err_rpm_put;

	return 0;

err_rpm_put:
	dev_err(gc5035->dev, "failed to start streaming: %d\n", ret);
	pm_runtime_put(gc5035->dev);

	return ret;
}

static void gc5035_stop_streaming(struct gc5035 *gc5035)
{
	int ret;

	ret = cci_write(gc5035->regmap, GC5035_REG_STREAM, GC5035_STREAM_OFF,
			NULL);
	if (ret)
		dev_err(gc5035->dev, "failed to stop streaming: %d\n", ret);

	pm_runtime_put(gc5035->dev);
}

static int gc5035_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct gc5035 *gc5035 = to_gc5035(sd);
	struct v4l2_subdev_state *state;
	int ret = 0;

	state = v4l2_subdev_lock_and_get_active_state(sd);

	if (enable)
		ret = gc5035_start_streaming(gc5035);
	else
		gc5035_stop_streaming(gc5035);

	v4l2_subdev_unlock_state(state);

	return ret;
}

static const struct v4l2_subdev_video_ops gc5035_video_ops = {
	.s_stream = gc5035_s_stream,
};

static const struct v4l2_subdev_pad_ops gc5035_pad_ops = {
	.enum_mbus_code = gc5035_enum_mbus_code,
	.enum_frame_size = gc5035_enum_frame_size,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = gc5035_set_format,
	.get_selection = gc5035_get_selection,
};

static const struct v4l2_subdev_ops gc5035_subdev_ops = {
	.video = &gc5035_video_ops,
	.pad = &gc5035_pad_ops,
};

static const struct v4l2_subdev_internal_ops gc5035_internal_ops = {
	.init_state = gc5035_init_state,
};

static int gc5035_parse_fwnode(struct gc5035 *gc5035)
{
	struct v4l2_fwnode_endpoint bus_cfg = {
		.bus_type = V4L2_MBUS_CSI2_DPHY,
	};
	struct device *dev = gc5035->dev;
	struct fwnode_handle *endpoint;
	int ret;

	/* by_id skips an endpoint without a remote; the sensor must probe before a receiver exists */
	endpoint = fwnode_graph_get_next_endpoint(dev_fwnode(dev), NULL);
	if (!endpoint)
		return dev_err_probe(dev, -EINVAL, "endpoint node not found\n");

	ret = v4l2_fwnode_endpoint_alloc_parse(endpoint, &bus_cfg);
	if (ret) {
		dev_err_probe(dev, ret, "parsing endpoint node failed\n");
		goto out_put;
	}

	if (bus_cfg.bus.mipi_csi2.num_data_lanes != GC5035_DATA_LANES) {
		ret = dev_err_probe(dev, -EINVAL,
				    "only %u data lanes are supported\n",
				    GC5035_DATA_LANES);
		goto out_free;
	}

	ret = v4l2_link_freq_to_bitmap(dev, bus_cfg.link_frequencies,
				       bus_cfg.nr_of_link_frequencies,
				       gc5035_link_freq_menu,
				       ARRAY_SIZE(gc5035_link_freq_menu),
				       &gc5035->link_freq_bitmap);

out_free:
	v4l2_fwnode_endpoint_free(&bus_cfg);
out_put:
	fwnode_handle_put(endpoint);

	return ret;
}

static int gc5035_init_controls(struct gc5035 *gc5035)
{
	const struct gc5035_mode *mode = gc5035->cur_mode;
	struct v4l2_fwnode_device_properties props;
	struct v4l2_ctrl_handler *hdlr = &gc5035->ctrls;
	s64 hblank = mode->hts - mode->width;
	s64 exposure_max = mode->vts - GC5035_EXPOSURE_MARGIN;
	int ret;

	v4l2_ctrl_handler_init(hdlr, 11);

	gc5035->link_freq = v4l2_ctrl_new_int_menu(hdlr, &gc5035_ctrl_ops,
			V4L2_CID_LINK_FREQ,
			ARRAY_SIZE(gc5035_link_freq_menu) - 1,
			mode->link_freq_idx, gc5035_link_freq_menu);
	if (gc5035->link_freq)
		gc5035->link_freq->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	gc5035->pixel_rate = v4l2_ctrl_new_std(hdlr, &gc5035_ctrl_ops,
			V4L2_CID_PIXEL_RATE, mode->pixel_rate,
			mode->pixel_rate, 1, mode->pixel_rate);
	if (gc5035->pixel_rate)
		gc5035->pixel_rate->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	gc5035->hblank = v4l2_ctrl_new_std(hdlr, &gc5035_ctrl_ops,
			V4L2_CID_HBLANK, hblank, hblank, 1, hblank);
	if (gc5035->hblank)
		gc5035->hblank->flags |= V4L2_CTRL_FLAG_READ_ONLY;

	gc5035->vblank = v4l2_ctrl_new_std(hdlr, &gc5035_ctrl_ops,
			V4L2_CID_VBLANK, mode->vts - mode->height,
			GC5035_VTS_MAX - mode->height, GC5035_VTS_STEP,
			mode->vts - mode->height);

	gc5035->exposure = v4l2_ctrl_new_std(hdlr, &gc5035_ctrl_ops,
			V4L2_CID_EXPOSURE, GC5035_EXPOSURE_MIN, exposure_max,
			GC5035_EXPOSURE_STEP, exposure_max);

	v4l2_ctrl_new_std(hdlr, &gc5035_ctrl_ops, V4L2_CID_ANALOGUE_GAIN,
			  GC5035_GAIN_MIN, GC5035_GAIN_MAX, 1, GC5035_GAIN_MIN);

	gc5035->hflip = v4l2_ctrl_new_std(hdlr, &gc5035_ctrl_ops,
					  V4L2_CID_HFLIP, 0, 1, 1, 0);
	gc5035->vflip = v4l2_ctrl_new_std(hdlr, &gc5035_ctrl_ops,
					  V4L2_CID_VFLIP, 0, 1, 1, 0);
	if (gc5035->hflip && gc5035->vflip) {
		gc5035->hflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
		gc5035->vflip->flags |= V4L2_CTRL_FLAG_MODIFY_LAYOUT;
		v4l2_ctrl_cluster(2, &gc5035->hflip);
	}

	v4l2_ctrl_new_std_menu_items(hdlr, &gc5035_ctrl_ops,
				     V4L2_CID_TEST_PATTERN,
				     ARRAY_SIZE(gc5035_test_pattern_menu) - 1,
				     0, 0, gc5035_test_pattern_menu);

	if (hdlr->error) {
		ret = hdlr->error;
		goto err_free;
	}

	ret = v4l2_fwnode_device_parse(gc5035->dev, &props);
	if (ret)
		goto err_free;

	ret = v4l2_ctrl_new_fwnode_properties(hdlr, &gc5035_ctrl_ops, &props);
	if (ret)
		goto err_free;

	/* Modes whose link frequency the board does not list are not offered. */
	gc5035->link_freq->menu_skip_mask = ~gc5035->link_freq_bitmap;
	gc5035->sd.ctrl_handler = hdlr;

	return 0;

err_free:
	v4l2_ctrl_handler_free(hdlr);

	return ret;
}

static int gc5035_identify_module(struct gc5035 *gc5035)
{
	u64 id;
	int ret;

	ret = cci_read(gc5035->regmap, GC5035_REG_CHIP_ID, &id, NULL);
	if (ret)
		return dev_err_probe(gc5035->dev, ret,
				     "failed to read chip id\n");

	if (id != GC5035_CHIP_ID)
		return dev_err_probe(gc5035->dev, -ENXIO,
				     "chip id mismatch: 0x%04x != 0x%04llx\n",
				     GC5035_CHIP_ID, id);

	dev_info(gc5035->dev, "GC5035 chip id 0x%04llx\n", id);

	return 0;
}

static int gc5035_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gc5035 *gc5035;
	unsigned int i;
	int ret;

	gc5035 = devm_kzalloc(dev, sizeof(*gc5035), GFP_KERNEL);
	if (!gc5035)
		return -ENOMEM;

	gc5035->dev = dev;

	ret = gc5035_parse_fwnode(gc5035);
	if (ret)
		return ret;

	gc5035->regmap = devm_cci_regmap_init_i2c(client, 8);
	if (IS_ERR(gc5035->regmap))
		return dev_err_probe(dev, PTR_ERR(gc5035->regmap),
				     "failed to init CCI\n");

	gc5035->xclk = devm_v4l2_sensor_clk_get(dev, NULL);
	if (IS_ERR(gc5035->xclk))
		return dev_err_probe(dev, PTR_ERR(gc5035->xclk),
				     "failed to get xclk\n");

	if (clk_get_rate(gc5035->xclk) != GC5035_XCLK_FREQ)
		return dev_err_probe(dev, -EINVAL,
				     "xclk must run at %lu Hz, is %lu Hz\n",
				     GC5035_XCLK_FREQ,
				     clk_get_rate(gc5035->xclk));

	for (i = 0; i < ARRAY_SIZE(gc5035_supply_name); i++)
		gc5035->supplies[i].supply = gc5035_supply_name[i];

	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(gc5035->supplies),
				      gc5035->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	gc5035->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_HIGH);
	if (IS_ERR(gc5035->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(gc5035->reset_gpio),
				     "failed to get reset gpio\n");

	gc5035->powerdown_gpio = devm_gpiod_get(dev, "powerdown",
						GPIOD_OUT_HIGH);
	if (IS_ERR(gc5035->powerdown_gpio))
		return dev_err_probe(dev, PTR_ERR(gc5035->powerdown_gpio),
				     "failed to get powerdown gpio\n");

	v4l2_i2c_subdev_init(&gc5035->sd, client, &gc5035_subdev_ops);
	gc5035->sd.internal_ops = &gc5035_internal_ops;
	gc5035->cur_mode = gc5035_find_mode(gc5035, gc5035_modes[0].width,
					    gc5035_modes[0].height);
	if (!gc5035->cur_mode)
		return -EINVAL;

	ret = gc5035_power_on(dev);
	if (ret)
		return ret;

	ret = gc5035_identify_module(gc5035);
	if (ret)
		goto err_power_off;

	ret = gc5035_init_controls(gc5035);
	if (ret)
		goto err_power_off;

	gc5035->sd.flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	gc5035->sd.entity.function = MEDIA_ENT_F_CAM_SENSOR;
	gc5035->pad.flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&gc5035->sd.entity, 1, &gc5035->pad);
	if (ret)
		goto err_ctrl_free;

	gc5035->sd.state_lock = gc5035->ctrls.lock;
	ret = v4l2_subdev_init_finalize(&gc5035->sd);
	if (ret)
		goto err_entity_cleanup;

	pm_runtime_set_active(dev);
	pm_runtime_enable(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_use_autosuspend(dev);
	pm_runtime_idle(dev);

	ret = v4l2_async_register_subdev_sensor(&gc5035->sd);
	if (ret) {
		dev_err(dev, "failed to register subdev: %d\n", ret);
		goto err_rpm;
	}

	return 0;

err_rpm:
	pm_runtime_disable(dev);
	/* pm_runtime_idle() may already have powered the sensor off. */
	if (!pm_runtime_status_suspended(dev))
		gc5035_power_off(dev);
	pm_runtime_set_suspended(dev);
	v4l2_subdev_cleanup(&gc5035->sd);
	media_entity_cleanup(&gc5035->sd.entity);
	v4l2_ctrl_handler_free(&gc5035->ctrls);

	return ret;

err_entity_cleanup:
	media_entity_cleanup(&gc5035->sd.entity);
err_ctrl_free:
	v4l2_ctrl_handler_free(&gc5035->ctrls);
err_power_off:
	gc5035_power_off(dev);

	return ret;
}

static void gc5035_remove(struct i2c_client *client)
{
	struct v4l2_subdev *sd = i2c_get_clientdata(client);
	struct gc5035 *gc5035 = to_gc5035(sd);

	v4l2_async_unregister_subdev(sd);
	v4l2_subdev_cleanup(sd);
	media_entity_cleanup(&sd->entity);
	v4l2_ctrl_handler_free(&gc5035->ctrls);

	pm_runtime_disable(&client->dev);
	if (!pm_runtime_status_suspended(&client->dev))
		gc5035_power_off(&client->dev);
	pm_runtime_set_suspended(&client->dev);
}

static const struct of_device_id gc5035_of_match[] = {
	{ .compatible = "galaxycore,gc5035" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, gc5035_of_match);

static DEFINE_RUNTIME_DEV_PM_OPS(gc5035_pm_ops, gc5035_power_off,
				 gc5035_power_on, NULL);

static struct i2c_driver gc5035_i2c_driver = {
	.driver = {
		.name = "gc5035",
		.of_match_table = gc5035_of_match,
		.pm = pm_ptr(&gc5035_pm_ops),
	},
	.probe = gc5035_probe,
	.remove = gc5035_remove,
};
module_i2c_driver(gc5035_i2c_driver);

MODULE_DESCRIPTION("GalaxyCore GC5035 camera sensor driver");
MODULE_AUTHOR("Fredrik Lindlöf <fredrik.lindlof@gmail.com>");
MODULE_LICENSE("GPL");
