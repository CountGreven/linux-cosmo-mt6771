/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MediaTek CAMSV raw capture (MT8183 family)
 */

#ifndef __MTK_CAMSV_H__
#define __MTK_CAMSV_H__

#include <linux/clk.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/spinlock.h>
#include <media/media-device.h>
#include <media/v4l2-async.h>
#include <media/v4l2-device.h>
#include <media/v4l2-subdev.h>
#include <media/videobuf2-v4l2.h>

#include "mtk-camsv-reg.h"

#define CAMSV_PAD_SINK		0
#define CAMSV_PAD_SOURCE	1
#define CAMSV_NUM_PADS		2
#define CAMSV_NUM_CLKS		3
#define CAMSV_MAX_EXTRA		16

struct dentry;
struct device_link;

struct mtk_camsv_format {
	u32 code;
	u32 fourcc;		/* 10 bit value in bits 13:4 of a 16 bit word */
};

struct mtk_camsv_buffer {
	struct vb2_v4l2_buffer vb;
	struct list_head list;
	dma_addr_t addr;
};

struct mtk_camsv_stats {
	u32 irqs;
	u32 bits[32];
	u32 frames;
	u32 drops;
	u32 sequence;
	u64 sof_ts;
};

/* Register writes applied before the timing generator starts, see debugfs */
struct mtk_camsv_extra {
	u32 off;
	u32 mask;
	u32 val;
};

struct mtk_camsv {
	struct device *dev;
	void __iomem *base;
	int irq;
	struct clk_bulk_data clks[CAMSV_NUM_CLKS];
	struct device_link *larb_links[2];
	unsigned int num_larbs;

	/* Set while the clocks are on, read by the interrupt handler */
	bool powered;

	struct media_device mdev;
	struct v4l2_device v4l2_dev;
	struct v4l2_subdev sd;
	struct media_pad pads[CAMSV_NUM_PADS];
	struct v4l2_async_notifier notifier;

	struct video_device vdev;
	struct media_pad vdev_pad;
	struct vb2_queue queue;
	/* Serializes the ioctls and the vb2 queue */
	struct mutex lock;

	/*
	 * Buffers: the queued list, the buffer the frame in flight lands in
	 * (active), the one programmed for the next frame (next), and a dummy
	 * for frames that find no buffer.
	 */
	spinlock_t qlock;
	struct list_head buffers;
	struct mtk_camsv_buffer *active;
	struct mtk_camsv_buffer *next;
	void *dummy_cpu;
	dma_addr_t dummy_dma;
	size_t dummy_size;
	bool streaming;
	u32 err_storm[32];

	struct mtk_camsv_stats stats;

	/* Bring-up aid, see mtk-camsv-debugfs.c */
	struct dentry *debugfs;
	bool dbg_power;
	bool dbg_dma;
	u32 dbg_int_en;
	u32 dbg_poison;
	u32 dbg_reg_val;
	struct mtk_camsv_extra extra[CAMSV_MAX_EXTRA];
	unsigned int num_extra;
};

static inline u32 camsv_read(struct mtk_camsv *priv, u32 reg)
{
	return readl(priv->base + reg);
}

static inline void camsv_write(struct mtk_camsv *priv, u32 reg, u32 val)
{
	writel(val, priv->base + reg);
}

static inline void camsv_update(struct mtk_camsv *priv, u32 reg, u32 mask, u32 val)
{
	u32 tmp = camsv_read(priv, reg);

	tmp &= ~mask;
	tmp |= val & mask;
	camsv_write(priv, reg, tmp);
}

extern const struct mtk_camsv_format mtk_camsv_formats[4];

const struct mtk_camsv_format *mtk_camsv_format_by_code(u32 code);
int mtk_camsv_hw_reset(struct mtk_camsv *priv);
int mtk_camsv_hw_start(struct mtk_camsv *priv, const struct v4l2_mbus_framefmt *fmt,
		       u32 bytesperline);
void mtk_camsv_hw_vf_off(struct mtk_camsv *priv);
void mtk_camsv_hw_stop(struct mtk_camsv *priv);

int mtk_camsv_video_register(struct mtk_camsv *priv);
void mtk_camsv_video_unregister(struct mtk_camsv *priv);

#ifdef CONFIG_DEBUG_FS
void mtk_camsv_debugfs_init(struct mtk_camsv *priv);
void mtk_camsv_debugfs_exit(struct mtk_camsv *priv);
#else
static inline void mtk_camsv_debugfs_init(struct mtk_camsv *priv) { }
static inline void mtk_camsv_debugfs_exit(struct mtk_camsv *priv) { }
#endif

#endif
