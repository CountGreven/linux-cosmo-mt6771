/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek SENINF CSI-2 receiver (MT8183 family)
 */

#ifndef __MTK_SENINF_H__
#define __MTK_SENINF_H__

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/mutex.h>
#include <linux/types.h>
#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>

#include "mtk-seninf-reg.h"

/* CSI ports, also the sink pads and the DT port numbers */
enum mtk_seninf_port_id {
	SENINF_PORT_CSI0,	/* 4D1C */
	SENINF_PORT_CSI1,	/* 4D1C */
	SENINF_PORT_CSI2,	/* 4D1C */
	SENINF_PORT_CSI0A,	/* 2D1C, half of CSI0 */
	SENINF_PORT_CSI0B,	/* 2D1C, half of CSI0 */
	SENINF_NUM_PORTS
};

/* One source pad for now: the output of MUX 0 */
#define SENINF_PAD_MUX0		SENINF_NUM_PORTS
#define SENINF_NUM_PADS		(SENINF_NUM_PORTS + 1)
#define SENINF_MUX		0

#define SENINF_NUM_CLKS		2

struct dentry;

struct mtk_seninf_port_info {
	const char *phy_name;
	u8 page;		/* SENINF page holding the CSI-2 decoder */
	u8 max_lanes;
	u8 top_ctl;		/* index of SENINF_TOP_PHY_CTL_CSI */
	bool split;		/* half of a CD-PHY receiver */
};

/* Values not settled by any source, changed at run time by the debugfs knobs */
struct mtk_seninf_tune {
	u32 settle;
	u32 hs_trail;
	/* Receiver port used instead of the one the sensor is wired to, 0xff = none */
	u32 port;
	/* Data lanes used instead of the endpoint's, 0 = none */
	u32 lanes;
	/* Value of the lane map byte instead of the derived one, above 0xff = none */
	u32 lane_map;
};

struct mtk_seninf_port {
	struct phy *phy;
	unsigned int lanes;	/* from the endpoint, 0 = not described */
};

struct mtk_seninf_asc {
	struct v4l2_async_connection asc;
	unsigned int port;
};

struct mtk_seninf {
	struct device *dev;
	void __iomem *base;
	struct clk_bulk_data clks[SENINF_NUM_CLKS];

	struct v4l2_subdev sd;
	struct media_pad pads[SENINF_NUM_PADS];
	struct v4l2_async_notifier notifier;

	struct mtk_seninf_port ports[SENINF_NUM_PORTS];

	/* Protects everything below and the register programming */
	struct mutex lock;
	struct mtk_seninf_tune tune;
	bool streaming;
	unsigned int stream_port;	/* sink pad the sensor is wired to */
	unsigned int stream_hw_port;	/* receiver port in use */
	struct v4l2_subdev *stream_sensor;
	unsigned int stream_sensor_pad;

	/* Bring-up aid, see mtk-seninf-debugfs.c */
	struct dentry *debugfs;
	bool dbg_power;
	bool tm_on;
	unsigned int dbg_page;
	u32 dbg_reg_val;
};

extern const struct mtk_seninf_port_info mtk_seninf_port_info[SENINF_NUM_PORTS];

static inline u32 seninf_read(struct mtk_seninf *priv, unsigned int page,
			      u32 reg)
{
	return readl(priv->base + page * SENINF_PAGE_SIZE + reg);
}

static inline void seninf_write(struct mtk_seninf *priv, unsigned int page,
				u32 reg, u32 val)
{
	writel(val, priv->base + page * SENINF_PAGE_SIZE + reg);
}

static inline void seninf_update(struct mtk_seninf *priv, unsigned int page,
				 u32 reg, u32 mask, u32 val)
{
	u32 tmp = seninf_read(priv, page, reg);

	tmp &= ~mask;
	tmp |= val & mask;
	seninf_write(priv, page, reg, tmp);
}

int mtk_seninf_start(struct mtk_seninf *priv, unsigned int sink_port);
void mtk_seninf_stop(struct mtk_seninf *priv);

#ifdef CONFIG_DEBUG_FS
void mtk_seninf_debugfs_init(struct mtk_seninf *priv);
void mtk_seninf_debugfs_exit(struct mtk_seninf *priv);
#else
static inline void mtk_seninf_debugfs_init(struct mtk_seninf *priv) { }
static inline void mtk_seninf_debugfs_exit(struct mtk_seninf *priv) { }
#endif

#endif
