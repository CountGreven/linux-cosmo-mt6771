// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek CAMSV raw capture unit (MT8183 family, used on MT6771)
 *
 * One instance takes the stream of SENINF mux 0 and writes the raw frames to
 * memory. The driver owns the media and V4L2 devices; the receiver joins them
 * as an async sub-device and brings its sensors along.
 *
 * The register layout is only partly known, see mtk-camsv-reg.h and the
 * debugfs aid that pokes registers before the stream starts.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mc.h>

#include "mtk-camsv.h"

#define CAMSV_DEFAULT_WIDTH		1600
#define CAMSV_DEFAULT_HEIGHT		1200
#define CAMSV_MAX_WIDTH			8191
#define CAMSV_MAX_HEIGHT		8191

/* One error bit that keeps firing is masked after this many interrupts */
#define CAMSV_ERR_STORM			100
#define CAMSV_RESET_TIMEOUT_US		1000
/* One frame at 30 fps, the state stays at 2 until the reset on this unit */
#define CAMSV_IDLE_TIMEOUT_US		40000

const struct mtk_camsv_format mtk_camsv_formats[4] = {
	{ MEDIA_BUS_FMT_SBGGR10_1X10, V4L2_PIX_FMT_SBGGR14 },
	{ MEDIA_BUS_FMT_SGBRG10_1X10, V4L2_PIX_FMT_SGBRG14 },
	{ MEDIA_BUS_FMT_SGRBG10_1X10, V4L2_PIX_FMT_SGRBG14 },
	{ MEDIA_BUS_FMT_SRGGB10_1X10, V4L2_PIX_FMT_SRGGB14 },
};

const struct mtk_camsv_format *mtk_camsv_format_by_code(u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mtk_camsv_formats); i++)
		if (mtk_camsv_formats[i].code == code)
			return &mtk_camsv_formats[i];

	return NULL;
}

static struct mtk_camsv *sd_to_camsv(struct v4l2_subdev *sd)
{
	return container_of(sd, struct mtk_camsv, sd);
}

/* Hardware */

/* Reset sequence of the vendor driver, with the wait bounded */
int mtk_camsv_hw_reset(struct mtk_camsv *priv)
{
	u32 val;
	int ret;

	camsv_write(priv, CAMSV_SW_CTL, CAMSV_SW_CTL_SW_RST);
	camsv_write(priv, CAMSV_SW_CTL, 0);
	camsv_write(priv, CAMSV_SW_CTL, CAMSV_SW_CTL_IMGO_RST_TRIG);
	ret = readl_poll_timeout_atomic(priv->base + CAMSV_SW_CTL, val,
					val == (CAMSV_SW_CTL_IMGO_RST_TRIG |
						CAMSV_SW_CTL_IMGO_RST_ST),
					1, CAMSV_RESET_TIMEOUT_US);
	camsv_write(priv, CAMSV_SW_CTL, 0);
	if (ret)
		dev_warn(priv->dev, "reset did not complete, SW_CTL %#x\n", val);

	return ret;
}

static dma_addr_t mtk_camsv_buf_addr(struct mtk_camsv *priv, struct mtk_camsv_buffer *buf)
{
	return buf ? buf->addr : priv->dummy_dma;
}

static void mtk_camsv_apply_extra(struct mtk_camsv *priv)
{
	unsigned int i;

	for (i = 0; i < priv->num_extra; i++) {
		const struct mtk_camsv_extra *e = &priv->extra[i];

		camsv_update(priv, e->off, e->mask, e->val);
	}
}

/*
 * Called with the block powered and its interrupt disabled. The buffer the
 * first frame lands in is priv->active, or the dummy buffer.
 */
int mtk_camsv_hw_start(struct mtk_camsv *priv, const struct v4l2_mbus_framefmt *fmt,
		       u32 bytesperline)
{
	int ret;

	ret = mtk_camsv_hw_reset(priv);
	if (ret)
		return ret;

	camsv_write(priv, CAMSV_INT_EN, 0);
	memset(priv->err_storm, 0, sizeof(priv->err_storm));

	camsv_write(priv, CAMSV_TG_SEN_GRAB_PXL, FIELD_PREP(CAMSV_TG_GRAB_END, fmt->width));
	camsv_write(priv, CAMSV_TG_SEN_GRAB_LIN, FIELD_PREP(CAMSV_TG_GRAB_END, fmt->height));
	camsv_update(priv, CAMSV_CLK_EN, CAMSV_CLK_EN_TG, CAMSV_CLK_EN_TG);
	camsv_update(priv, CAMSV_TG_PATH_CFG, CAMSV_TG_PATH_CFG_DB_LOAD_DIS,
		     CAMSV_TG_PATH_CFG_DB_LOAD_DIS);
	camsv_update(priv, CAMSV_TG_SEN_MODE, CAMSV_TG_SEN_MODE_CMOS_EN,
		     CAMSV_TG_SEN_MODE_CMOS_EN);

	if (priv->dbg_dma) {
		camsv_write(priv, CAMSV_IMGO_XSIZE, bytesperline - 1);
		camsv_write(priv, CAMSV_IMGO_YSIZE, fmt->height - 1);
		camsv_write(priv, CAMSV_IMGO_STRIDE, bytesperline);
		camsv_write(priv, CAMSV_IMGO_BASE_ADDR,
			    mtk_camsv_buf_addr(priv, priv->active));
		camsv_update(priv, CAMSV_MODULE_EN,
			     CAMSV_MODULE_EN_IMGO | CAMSV_MODULE_EN_DATA,
			     CAMSV_MODULE_EN_IMGO | CAMSV_MODULE_EN_DATA);
	}

	mtk_camsv_apply_extra(priv);

	/* Drop what the reset and the setup left pending, then arm */
	camsv_read(priv, CAMSV_INT_STATUS);
	priv->stats.sequence = 0;
	camsv_write(priv, CAMSV_INT_EN, priv->dbg_int_en);
	enable_irq(priv->irq);

	priv->streaming = true;
	/* Waits for the next frame start: the sensor starts after this */
	camsv_update(priv, CAMSV_TG_VF_CON, CAMSV_TG_VF_CON_VFDATA_EN,
		     CAMSV_TG_VF_CON_VFDATA_EN);

	return 0;
}

/* Stop the frame flow while the sensor still runs, so the TG ends on a frame boundary */
void mtk_camsv_hw_vf_off(struct mtk_camsv *priv)
{
	u32 val;

	camsv_update(priv, CAMSV_TG_VF_CON, CAMSV_TG_VF_CON_VFDATA_EN, 0);
	if (readl_poll_timeout(priv->base + CAMSV_TG_INTER_ST, val,
			       FIELD_GET(CAMSV_TG_INTER_ST_STATE, val) ==
			       CAMSV_TG_INTER_ST_IDLE, 1000, CAMSV_IDLE_TIMEOUT_US))
		dev_dbg(priv->dev, "timing generator not idle, TG_INTER_ST %#x\n", val);
}

/* Rest of the vendor stop protocol: reset, then clear the enables */
void mtk_camsv_hw_stop(struct mtk_camsv *priv)
{
	camsv_write(priv, CAMSV_INT_EN, 0);
	disable_irq(priv->irq);
	priv->streaming = false;

	mtk_camsv_hw_reset(priv);
	camsv_update(priv, CAMSV_MODULE_EN, CAMSV_MODULE_EN_IMGO | CAMSV_MODULE_EN_DATA, 0);
	camsv_update(priv, CAMSV_TG_SEN_MODE, CAMSV_TG_SEN_MODE_CMOS_EN, 0);
}

/* Interrupts */

/*
 * The base address is latched at the start of a frame: the one written while
 * frame N runs is used by frame N + 1. So a buffer is complete at the start of
 * the next frame, and the address for the frame after that is written now.
 * Called with qlock held.
 */
static void mtk_camsv_frame_start(struct mtk_camsv *priv)
{
	struct mtk_camsv_stats *st = &priv->stats;
	u64 now = ktime_get_ns();

	if (st->sequence) {
		struct mtk_camsv_buffer *buf = priv->active;

		if (buf) {
			buf->vb.field = V4L2_FIELD_NONE;
			vb2_buffer_done(&buf->vb.vb2_buf, VB2_BUF_STATE_DONE);
			st->frames++;
		} else {
			st->drops++;
		}
		priv->active = priv->next;
	}

	if (priv->active) {
		priv->active->vb.sequence = st->sequence;
		priv->active->vb.vb2_buf.timestamp = now;
	}
	st->sequence++;
	st->sof_ts = now;

	priv->next = list_first_entry_or_null(&priv->buffers, struct mtk_camsv_buffer, list);
	if (priv->next)
		list_del(&priv->next->list);

	if (priv->dbg_dma)
		camsv_write(priv, CAMSV_IMGO_BASE_ADDR, mtk_camsv_buf_addr(priv, priv->next));
}

static irqreturn_t mtk_camsv_irq(int irq, void *data)
{
	struct mtk_camsv *priv = data;
	unsigned long flags;
	unsigned long pending, errors;
	unsigned int bit;
	u32 status;

	/* The block is gated when this is false: touching it would hang the SoC */
	if (!READ_ONCE(priv->powered))
		return IRQ_NONE;

	status = camsv_read(priv, CAMSV_INT_STATUS);
	if (!status)
		return IRQ_NONE;

	spin_lock_irqsave(&priv->qlock, flags);

	priv->stats.irqs++;
	pending = status;
	for_each_set_bit(bit, &pending, 32)
		priv->stats.bits[bit]++;

	errors = status & CAMSV_INT_ERRORS;
	if (errors) {
		u32 mask = 0;

		for_each_set_bit(bit, &errors, 32)
			if (++priv->err_storm[bit] == CAMSV_ERR_STORM)
				mask |= BIT(bit);
		if (mask) {
			camsv_update(priv, CAMSV_INT_EN, mask, 0);
			dev_warn(priv->dev, "interrupt bits %#x masked, they keep firing\n",
				 mask);
		}
		dev_warn_ratelimited(priv->dev, "error interrupt %#lx\n", errors);
	}

	if (status & CAMSV_INT_SOF)
		mtk_camsv_frame_start(priv);

	spin_unlock_irqrestore(&priv->qlock, flags);

	return IRQ_HANDLED;
}

/* Sub-device */

static void mtk_camsv_init_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	fmt->width = CAMSV_DEFAULT_WIDTH;
	fmt->height = CAMSV_DEFAULT_HEIGHT;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int mtk_camsv_init_state(struct v4l2_subdev *sd, struct v4l2_subdev_state *state)
{
	unsigned int i;

	for (i = 0; i < CAMSV_NUM_PADS; i++)
		mtk_camsv_init_format(v4l2_subdev_state_get_format(state, i));

	return 0;
}

static int mtk_camsv_enum_mbus_code(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
				    struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad == CAMSV_PAD_SINK) {
		if (code->index >= ARRAY_SIZE(mtk_camsv_formats))
			return -EINVAL;
		code->code = mtk_camsv_formats[code->index].code;
		return 0;
	}

	if (code->index)
		return -EINVAL;

	code->code = v4l2_subdev_state_get_format(state, CAMSV_PAD_SINK)->code;

	return 0;
}

static int mtk_camsv_set_fmt(struct v4l2_subdev *sd, struct v4l2_subdev_state *state,
			     struct v4l2_subdev_format *fmt)
{
	struct mtk_camsv *priv = sd_to_camsv(sd);
	struct v4l2_mbus_framefmt *format = &fmt->format;

	/* The source follows the sink */
	if (fmt->pad == CAMSV_PAD_SOURCE)
		return v4l2_subdev_get_fmt(sd, state, fmt);

	if (priv->streaming)
		return -EBUSY;

	if (!mtk_camsv_format_by_code(format->code))
		format->code = MEDIA_BUS_FMT_SRGGB10_1X10;
	format->width = clamp_t(u32, format->width, 1, CAMSV_MAX_WIDTH);
	format->height = clamp_t(u32, format->height, 1, CAMSV_MAX_HEIGHT);
	format->field = V4L2_FIELD_NONE;

	*v4l2_subdev_state_get_format(state, CAMSV_PAD_SINK) = *format;
	*v4l2_subdev_state_get_format(state, CAMSV_PAD_SOURCE) = *format;

	return 0;
}

static const struct v4l2_subdev_pad_ops mtk_camsv_pad_ops = {
	.enum_mbus_code = mtk_camsv_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = mtk_camsv_set_fmt,
	.link_validate = v4l2_subdev_link_validate_default,
};

static const struct v4l2_subdev_ops mtk_camsv_subdev_ops = {
	.pad = &mtk_camsv_pad_ops,
};

static const struct v4l2_subdev_internal_ops mtk_camsv_internal_ops = {
	.init_state = mtk_camsv_init_state,
};

static const struct media_entity_operations mtk_camsv_entity_ops = {
	.link_validate = v4l2_subdev_link_validate,
	.get_fwnode_pad = v4l2_subdev_get_fwnode_pad_1_to_1,
};

static int mtk_camsv_register_subdev(struct mtk_camsv *priv)
{
	struct v4l2_subdev *sd = &priv->sd;
	int ret;

	v4l2_subdev_init(sd, &mtk_camsv_subdev_ops);
	sd->internal_ops = &mtk_camsv_internal_ops;
	sd->owner = THIS_MODULE;
	sd->dev = priv->dev;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	strscpy(sd->name, "mtk-camsv0", sizeof(sd->name));

	sd->entity.function = MEDIA_ENT_F_PROC_VIDEO_PIXEL_FORMATTER;
	sd->entity.ops = &mtk_camsv_entity_ops;
	priv->pads[CAMSV_PAD_SINK].flags = MEDIA_PAD_FL_SINK | MEDIA_PAD_FL_MUST_CONNECT;
	priv->pads[CAMSV_PAD_SOURCE].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sd->entity, CAMSV_NUM_PADS, priv->pads);
	if (ret)
		return ret;

	ret = v4l2_subdev_init_finalize(sd);
	if (ret)
		goto err_entity;

	ret = v4l2_device_register_subdev(&priv->v4l2_dev, sd);
	if (ret)
		goto err_cleanup;

	return 0;

err_cleanup:
	v4l2_subdev_cleanup(sd);
err_entity:
	media_entity_cleanup(&sd->entity);
	return ret;
}

static void mtk_camsv_unregister_subdev(struct mtk_camsv *priv)
{
	v4l2_device_unregister_subdev(&priv->sd);
	v4l2_subdev_cleanup(&priv->sd);
	media_entity_cleanup(&priv->sd.entity);
}

/* Receiver */

static int mtk_camsv_notify_bound(struct v4l2_async_notifier *notifier,
				  struct v4l2_subdev *sd, struct v4l2_async_connection *asc)
{
	struct mtk_camsv *priv = container_of(notifier, struct mtk_camsv, notifier);

	return v4l2_create_fwnode_links_to_pad(sd, &priv->pads[CAMSV_PAD_SINK],
					       MEDIA_LNK_FL_ENABLED | MEDIA_LNK_FL_IMMUTABLE);
}

static int mtk_camsv_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct mtk_camsv *priv = container_of(notifier, struct mtk_camsv, notifier);
	int ret;

	ret = v4l2_device_register_subdev_nodes(&priv->v4l2_dev);
	if (ret)
		return ret;

	return media_device_register(&priv->mdev);
}

static const struct v4l2_async_notifier_operations mtk_camsv_notify_ops = {
	.bound = mtk_camsv_notify_bound,
	.complete = mtk_camsv_notify_complete,
};

static int mtk_camsv_parse_endpoint(struct mtk_camsv *priv)
{
	struct v4l2_async_connection *asc;

	struct fwnode_handle *ep __free(fwnode_handle) =
		fwnode_graph_get_endpoint_by_id(dev_fwnode(priv->dev), 0, 0, 0);
	if (!ep)
		return dev_err_probe(priv->dev, -ENOTCONN, "no receiver endpoint\n");

	asc = v4l2_async_nf_add_fwnode_remote(&priv->notifier, ep, struct v4l2_async_connection);
	if (IS_ERR(asc))
		return dev_err_probe(priv->dev, PTR_ERR(asc), "cannot add the receiver\n");

	return 0;
}

/* Power */

static int mtk_camsv_runtime_suspend(struct device *dev)
{
	struct mtk_camsv *priv = dev_get_drvdata(dev);

	WRITE_ONCE(priv->powered, false);
	synchronize_irq(priv->irq);
	clk_bulk_disable_unprepare(CAMSV_NUM_CLKS, priv->clks);

	return 0;
}

static int mtk_camsv_runtime_resume(struct device *dev)
{
	struct mtk_camsv *priv = dev_get_drvdata(dev);
	int ret;

	ret = clk_bulk_prepare_enable(CAMSV_NUM_CLKS, priv->clks);
	if (ret)
		return ret;

	WRITE_ONCE(priv->powered, true);

	return 0;
}

static DEFINE_RUNTIME_DEV_PM_OPS(mtk_camsv_pm_ops, mtk_camsv_runtime_suspend,
				 mtk_camsv_runtime_resume, NULL);

/*
 * The larbs carry the clocks of the DMA port. Until the port is known and the
 * IOMMU takes over, the larbs are only kept powered through device links.
 */
static void mtk_camsv_larb_unlink(struct mtk_camsv *priv)
{
	while (priv->num_larbs)
		device_link_del(priv->larb_links[--priv->num_larbs]);
}

static int mtk_camsv_larb_link(struct mtk_camsv *priv)
{
	struct device_node *np = priv->dev->of_node;
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(priv->larb_links); i++) {
		struct device_node *larb_np __free(device_node) =
			of_parse_phandle(np, "mediatek,larb", i);
		struct platform_device *pdev;
		struct device_link *link;

		if (!larb_np)
			break;

		pdev = of_find_device_by_node(larb_np);
		if (!pdev)
			return dev_err_probe(priv->dev, -EPROBE_DEFER, "larb %u is not ready\n", i);

		link = device_link_add(priv->dev, &pdev->dev,
				       DL_FLAG_PM_RUNTIME | DL_FLAG_STATELESS);
		put_device(&pdev->dev);
		if (!link) {
			mtk_camsv_larb_unlink(priv);
			return -EINVAL;
		}

		priv->larb_links[priv->num_larbs++] = link;
	}

	return 0;
}

/* Platform driver */

static int mtk_camsv_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_camsv *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	priv->dbg_dma = true;
	priv->dbg_int_en = CAMSV_INT_ENABLED;
	mutex_init(&priv->lock);
	spin_lock_init(&priv->qlock);
	INIT_LIST_HEAD(&priv->buffers);
	platform_set_drvdata(pdev, priv);

	ret = dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));
	if (ret)
		return ret;

	/* Nothing touches the registers before the first runtime resume */
	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->clks[0].id = "camsv";
	priv->clks[1].id = "cam";
	priv->clks[2].id = "camtg";
	ret = devm_clk_bulk_get(dev, CAMSV_NUM_CLKS, priv->clks);
	if (ret)
		return dev_err_probe(dev, ret, "cannot get the clocks\n");

	priv->irq = platform_get_irq(pdev, 0);
	if (priv->irq < 0)
		return priv->irq;

	/* Enabled by the stream start, once the interrupt mask is programmed */
	ret = devm_request_irq(dev, priv->irq, mtk_camsv_irq, IRQF_NO_AUTOEN,
			       dev_name(dev), priv);
	if (ret)
		return dev_err_probe(dev, ret, "cannot request the interrupt\n");

	if (of_property_present(dev->of_node, "memory-region")) {
		ret = of_reserved_mem_device_init(dev);
		if (ret)
			return dev_err_probe(dev, ret, "cannot use the memory region\n");
	}

	ret = mtk_camsv_larb_link(priv);
	if (ret)
		goto err_mem;

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		goto err_larb;

	priv->mdev.dev = dev;
	strscpy(priv->mdev.model, "mtk-camsv", sizeof(priv->mdev.model));
	media_device_init(&priv->mdev);
	priv->v4l2_dev.mdev = &priv->mdev;

	ret = v4l2_device_register(dev, &priv->v4l2_dev);
	if (ret)
		goto err_mdev;

	ret = mtk_camsv_register_subdev(priv);
	if (ret)
		goto err_v4l2;

	ret = mtk_camsv_video_register(priv);
	if (ret)
		goto err_subdev;

	v4l2_async_nf_init(&priv->notifier, &priv->v4l2_dev);
	priv->notifier.ops = &mtk_camsv_notify_ops;

	ret = mtk_camsv_parse_endpoint(priv);
	if (ret)
		goto err_nf;

	ret = v4l2_async_nf_register(&priv->notifier);
	if (ret)
		goto err_nf;

	mtk_camsv_debugfs_init(priv);

	return 0;

err_nf:
	v4l2_async_nf_cleanup(&priv->notifier);
	mtk_camsv_video_unregister(priv);
err_subdev:
	mtk_camsv_unregister_subdev(priv);
err_v4l2:
	v4l2_device_unregister(&priv->v4l2_dev);
err_mdev:
	media_device_cleanup(&priv->mdev);
err_larb:
	mtk_camsv_larb_unlink(priv);
err_mem:
	of_reserved_mem_device_release(dev);
	return ret;
}

static void mtk_camsv_remove(struct platform_device *pdev)
{
	struct mtk_camsv *priv = platform_get_drvdata(pdev);

	mtk_camsv_debugfs_exit(priv);

	v4l2_async_nf_unregister(&priv->notifier);
	v4l2_async_nf_cleanup(&priv->notifier);
	media_device_unregister(&priv->mdev);
	mtk_camsv_video_unregister(priv);
	mtk_camsv_unregister_subdev(priv);
	v4l2_device_unregister(&priv->v4l2_dev);
	media_device_cleanup(&priv->mdev);
	mtk_camsv_larb_unlink(priv);
	of_reserved_mem_device_release(priv->dev);
}

static const struct of_device_id mtk_camsv_of_match[] = {
	{ .compatible = "mediatek,mt8183-camsv" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_camsv_of_match);

static struct platform_driver mtk_camsv_driver = {
	.probe = mtk_camsv_probe,
	.remove = mtk_camsv_remove,
	.driver = {
		.name = "mtk-camsv",
		.of_match_table = mtk_camsv_of_match,
		.pm = pm_ptr(&mtk_camsv_pm_ops),
	},
};
module_platform_driver(mtk_camsv_driver);

MODULE_DESCRIPTION("MediaTek CAMSV raw capture");
MODULE_LICENSE("GPL");
