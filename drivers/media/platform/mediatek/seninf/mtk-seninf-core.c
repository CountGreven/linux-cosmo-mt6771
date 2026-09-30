// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek SENINF CSI-2 receiver (MT8183 family, used on MT6771)
 *
 * The register sequences follow the public MT8183 ISP driver. The driver
 * owns the media and V4L2 devices until a capture driver exists that can
 * take the receiver as a sub-device; the sensors bind to it through the
 * async notifier and are started from the receiver.
 */

#include <linux/bitfield.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/property.h>
#include <media/v4l2-fwnode.h>
#include <media/v4l2-mc.h>

#include "mtk-seninf.h"

#define MTK_SENINF_DEFAULT_CODE		MEDIA_BUS_FMT_SRGGB10_1X10
#define MTK_SENINF_DEFAULT_WIDTH	1600
#define MTK_SENINF_DEFAULT_HEIGHT	1200
#define MTK_SENINF_MAX_SIZE		8192

/* Values of the reference driver, see the tunables in struct mtk_seninf_tune */
#define MTK_SENINF_SETTLE_DEFAULT	0x15
#define MTK_SENINF_PORT_NONE		0xff
#define MTK_SENINF_LANE_MAP_NONE	0x100

const struct mtk_seninf_port_info mtk_seninf_port_info[SENINF_NUM_PORTS] = {
	[SENINF_PORT_CSI0]  = { "csi0", 0, 4, 0, false },
	[SENINF_PORT_CSI1]  = { "csi1", 2, 4, 1, false },
	[SENINF_PORT_CSI2]  = { "csi2", 4, 4, 2, false },
	[SENINF_PORT_CSI0A] = { "csi0a", 0, 2, 0, true },
	[SENINF_PORT_CSI0B] = { "csi0b", 1, 2, 0, true },
};

static const u32 mtk_seninf_codes[] = {
	MEDIA_BUS_FMT_SBGGR8_1X8,
	MEDIA_BUS_FMT_SGBRG8_1X8,
	MEDIA_BUS_FMT_SGRBG8_1X8,
	MEDIA_BUS_FMT_SRGGB8_1X8,
	MEDIA_BUS_FMT_SBGGR10_1X10,
	MEDIA_BUS_FMT_SGBRG10_1X10,
	MEDIA_BUS_FMT_SGRBG10_1X10,
	MEDIA_BUS_FMT_SRGGB10_1X10,
	MEDIA_BUS_FMT_SBGGR12_1X12,
	MEDIA_BUS_FMT_SGBRG12_1X12,
	MEDIA_BUS_FMT_SGRBG12_1X12,
	MEDIA_BUS_FMT_SRGGB12_1X12,
};

static struct mtk_seninf *sd_to_seninf(struct v4l2_subdev *sd)
{
	return container_of(sd, struct mtk_seninf, sd);
}

/* Register programming */

static void mtk_seninf_program_decoder(struct mtk_seninf *priv,
				       const struct mtk_seninf_port_info *info,
				       unsigned int lanes)
{
	unsigned int page = info->page;
	u32 lane_map;

	/* Timestamp step, in the top page like the reference driver */
	seninf_update(priv, 0, SENINF_CTRL, SENINF_CTRL_EN, SENINF_CTRL_EN);
	seninf_update(priv, 0, SENINF_CTRL_EXT, SENINF_CTRL_EXT_MASK,
		      SENINF_CTRL_EXT_VAL);
	seninf_write(priv, 0, SENINF_TG_TM_STP, SENINF_TG_TM_STP_NORMAL);

	if (info->split)
		seninf_update(priv, 0, SENINF_TOP_PHY_CTL_CSI(info->top_ctl),
			      SENINF_TOP_PHY_CTL_2D1C_MASK,
			      SENINF_TOP_PHY_CTL_2D1C_VAL);
	else
		seninf_update(priv, 0, SENINF_TOP_PHY_CTL_CSI(info->top_ctl),
			      SENINF_TOP_PHY_CTL_4D1C_MASK,
			      SENINF_TOP_PHY_CTL_4D1C_VAL);

	/* Enable the decoder, 10 bit pad, source select cleared */
	seninf_update(priv, page, SENINF_CTRL, SENINF_CTRL_EN, SENINF_CTRL_EN);
	seninf_update(priv, page, SENINF_CTRL,
		      SENINF_CTRL_PAD_MASK | SENINF_CTRL_SRC_MASK,
		      FIELD_PREP(SENINF_CTRL_PAD_MASK, SENINF_CTRL_PAD_10BIT));
	seninf_update(priv, page, SENINF_CTRL_EXT, SENINF_CTRL_EXT_MASK,
		      SENINF_CTRL_EXT_VAL);

	if (priv->tune.lane_map <= 0xff)
		lane_map = priv->tune.lane_map;
	else
		lane_map = info->split ? SENINF_LANE_MAP_2D1C : SENINF_LANE_MAP_4D1C;
	seninf_update(priv, page, SENINF_MIPI_RX_CON24, SENINF_LANE_MAP_MASK,
		      FIELD_PREP(SENINF_LANE_MAP_MASK, lane_map));

	seninf_write(priv, page, SENINF_CSI2_DPCM, SENINF_CSI2_DPCM_NONE);
	seninf_update(priv, page, SENINF_CSI2_LNRD_TIMING, SENINF_CSI2_LNRD_SETTLE,
		      FIELD_PREP(SENINF_CSI2_LNRD_SETTLE, priv->tune.settle));

	seninf_update(priv, page, SENINF_CSI2_CTL,
		      SENINF_CSI2_CTL_LANES | SENINF_CSI2_CTL_CLK_EN |
		      SENINF_CSI2_CTL_HDR_ORDER | SENINF_CSI2_CTL_BIT25 |
		      SENINF_CSI2_CTL_CLR_MASK,
		      FIELD_PREP(SENINF_CSI2_CTL_LANES, BIT(lanes) - 1) |
		      SENINF_CSI2_CTL_CLK_EN | SENINF_CSI2_CTL_HDR_ORDER |
		      SENINF_CSI2_CTL_BIT25);
	seninf_update(priv, page, SENINF_CSI2_RESYNC_MERGE_CTL,
		      SENINF_CSI2_RESYNC_MASK, SENINF_CSI2_RESYNC_VAL);
	seninf_update(priv, page, SENINF_CSI2_MODE, SENINF_CSI2_MODE_MASK, 0);
	seninf_write(priv, page, SENINF_CSI2_DPHY_SYNC, SENINF_CSI2_DPHY_SYNC_VAL);
	seninf_update(priv, page, SENINF_CSI2_SPARE0, BIT(0), 0);
	seninf_update(priv, page, SENINF_CSI2_HS_TRAIL, SENINF_CSI2_HS_TRAIL_MASK,
		      priv->tune.hs_trail);

	/* Packet counter on the debug port, then all interrupt bits armed */
	seninf_write(priv, page, SENINF_CSI2_DGB_SEL, SENINF_CSI2_DGB_SEL_PKT);
	seninf_write(priv, page, SENINF_CSI2_INT_EN, 0xffffffff);
	seninf_write(priv, page, SENINF_CSI2_INT_STATUS, 0xffffffff);
	seninf_write(priv, page, SENINF_CSI2_INT_EN_EXT, SENINF_CSI2_INT_EN_EXT_VAL);

	/* Soft reset pulse */
	seninf_update(priv, page, SENINF_CTRL, SENINF_CTRL_RESET, SENINF_CTRL_RESET);
	udelay(1);
	seninf_update(priv, page, SENINF_CTRL, SENINF_CTRL_RESET, 0);
}

static void mtk_seninf_program_mux(struct mtk_seninf *priv,
				   const struct mtk_seninf_port_info *info)
{
	unsigned int mux = SENINF_MUX;
	unsigned int decoder = info->page;

	seninf_update(priv, mux, SENINF_MUX_CTRL_EXT, SENINF_MUX_CTRL_EXT_MASK,
		      SENINF_MUX_CTRL_EXT_VAL);
	seninf_update(priv, mux, SENINF_MUX_CTRL,
		      SENINF_MUX_CTRL_EN | SENINF_MUX_CTRL_FMT |
		      SENINF_MUX_CTRL_SIZE | SENINF_MUX_CTRL_SEL |
		      SENINF_MUX_CTRL_MODE_MASK,
		      SENINF_MUX_CTRL_EN |
		      FIELD_PREP(SENINF_MUX_CTRL_FMT, SENINF_MUX_CTRL_FMT_RAW) |
		      FIELD_PREP(SENINF_MUX_CTRL_SIZE, SENINF_MUX_CTRL_SIZE_RAW) |
		      FIELD_PREP(SENINF_MUX_CTRL_SEL, SENINF_MUX_CTRL_SEL_VAL));

	/* MUX m takes the decoder named by nibble m; write it in the top page */
	seninf_update(priv, 0, SENINF_TOP_MUX_CTRL, GENMASK(3, 0) << (4 * mux),
		      decoder << (4 * mux));
	seninf_write(priv, 0, SENINF_TOP_CAM_MUX_CTRL, 0);
}

static void mtk_seninf_unprogram(struct mtk_seninf *priv,
				 const struct mtk_seninf_port_info *info)
{
	seninf_update(priv, SENINF_MUX, SENINF_MUX_CTRL, SENINF_MUX_CTRL_EN, 0);
	seninf_update(priv, info->page, SENINF_CSI2_CTL, SENINF_CSI2_CTL_EN_MASK, 0);
}

/* Streaming */

int mtk_seninf_start(struct mtk_seninf *priv, unsigned int sink_port)
{
	const struct mtk_seninf_port_info *info;
	struct media_pad *remote;
	unsigned int hw_port, lanes;
	struct phy *phy;
	int ret;

	mutex_lock(&priv->lock);

	if (priv->streaming) {
		ret = -EBUSY;
		goto out;
	}

	if (sink_port >= SENINF_NUM_PORTS || !priv->ports[sink_port].lanes) {
		ret = -EINVAL;
		goto out;
	}

	remote = media_pad_remote_pad_first(&priv->pads[sink_port]);
	if (!remote || !is_media_entity_v4l2_subdev(remote->entity)) {
		ret = -ENOLINK;
		goto out;
	}

	if (priv->tune.settle > 0xff || priv->tune.hs_trail > 0xff) {
		ret = -EINVAL;
		goto out;
	}

	hw_port = priv->tune.port < SENINF_NUM_PORTS ? priv->tune.port : sink_port;
	info = &mtk_seninf_port_info[hw_port];
	lanes = priv->tune.lanes ?: priv->ports[sink_port].lanes;
	if (lanes > info->max_lanes) {
		dev_err(priv->dev, "%u lanes do not fit port %s\n", lanes,
			info->phy_name);
		ret = -EINVAL;
		goto out;
	}

	phy = priv->ports[hw_port].phy;
	if (!phy) {
		dev_err(priv->dev, "port %s has no PHY\n", info->phy_name);
		ret = -ENODEV;
		goto out;
	}

	/* Preconditions in order: power domain and clocks, analog, then registers */
	ret = pm_runtime_resume_and_get(priv->dev);
	if (ret < 0)
		goto out;

	ret = phy_init(phy);
	if (ret)
		goto err_put;

	ret = phy_power_on(phy);
	if (ret)
		goto err_exit;

	mtk_seninf_program_decoder(priv, info, lanes);
	mtk_seninf_program_mux(priv, info);

	/* Armed before the first HS burst: only now start the sensor */
	priv->stream_sensor = media_entity_to_v4l2_subdev(remote->entity);
	priv->stream_sensor_pad = remote->index;
	ret = v4l2_subdev_enable_streams(priv->stream_sensor,
					 priv->stream_sensor_pad, BIT_ULL(0));
	if (ret)
		goto err_unprogram;

	priv->stream_port = sink_port;
	priv->stream_hw_port = hw_port;
	priv->streaming = true;
	dev_dbg(priv->dev, "streaming %s on port %s, %u lanes\n",
		priv->stream_sensor->name, info->phy_name, lanes);
	goto out;

err_unprogram:
	mtk_seninf_unprogram(priv, info);
	phy_power_off(phy);
err_exit:
	phy_exit(phy);
err_put:
	pm_runtime_put(priv->dev);
out:
	mutex_unlock(&priv->lock);
	return ret;
}

void mtk_seninf_stop(struct mtk_seninf *priv)
{
	const struct mtk_seninf_port_info *info;
	struct phy *phy;

	mutex_lock(&priv->lock);

	if (!priv->streaming)
		goto out;

	info = &mtk_seninf_port_info[priv->stream_hw_port];
	phy = priv->ports[priv->stream_hw_port].phy;

	v4l2_subdev_disable_streams(priv->stream_sensor, priv->stream_sensor_pad,
				    BIT_ULL(0));
	mtk_seninf_unprogram(priv, info);
	phy_power_off(phy);
	phy_exit(phy);
	pm_runtime_put(priv->dev);
	priv->streaming = false;
out:
	mutex_unlock(&priv->lock);
}

/* V4L2 sub-device */

static void mtk_seninf_init_format(struct v4l2_mbus_framefmt *fmt)
{
	fmt->code = MTK_SENINF_DEFAULT_CODE;
	fmt->width = MTK_SENINF_DEFAULT_WIDTH;
	fmt->height = MTK_SENINF_DEFAULT_HEIGHT;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = V4L2_COLORSPACE_RAW;
	fmt->ycbcr_enc = V4L2_YCBCR_ENC_DEFAULT;
	fmt->quantization = V4L2_QUANTIZATION_DEFAULT;
	fmt->xfer_func = V4L2_XFER_FUNC_NONE;
}

static int mtk_seninf_init_state(struct v4l2_subdev *sd,
				 struct v4l2_subdev_state *state)
{
	unsigned int i;

	for (i = 0; i < SENINF_NUM_PADS; i++)
		mtk_seninf_init_format(v4l2_subdev_state_get_format(state, i));

	return 0;
}

static int mtk_seninf_enum_mbus_code(struct v4l2_subdev *sd,
				     struct v4l2_subdev_state *state,
				     struct v4l2_subdev_mbus_code_enum *code)
{
	if (code->pad < SENINF_NUM_PORTS) {
		if (code->index >= ARRAY_SIZE(mtk_seninf_codes))
			return -EINVAL;
		code->code = mtk_seninf_codes[code->index];
		return 0;
	}

	if (code->index)
		return -EINVAL;

	code->code = v4l2_subdev_state_get_format(state, code->pad)->code;

	return 0;
}

static int mtk_seninf_set_fmt(struct v4l2_subdev *sd,
			      struct v4l2_subdev_state *state,
			      struct v4l2_subdev_format *fmt)
{
	struct v4l2_mbus_framefmt *format = &fmt->format;
	unsigned int i;

	/* The source follows the sink */
	if (fmt->pad >= SENINF_NUM_PORTS)
		return v4l2_subdev_get_fmt(sd, state, fmt);

	for (i = 0; i < ARRAY_SIZE(mtk_seninf_codes); i++)
		if (mtk_seninf_codes[i] == format->code)
			break;
	if (i == ARRAY_SIZE(mtk_seninf_codes))
		format->code = MTK_SENINF_DEFAULT_CODE;

	format->width = clamp_t(u32, format->width, 1, MTK_SENINF_MAX_SIZE);
	format->height = clamp_t(u32, format->height, 1, MTK_SENINF_MAX_SIZE);
	format->field = V4L2_FIELD_NONE;

	*v4l2_subdev_state_get_format(state, fmt->pad) = *format;
	*v4l2_subdev_state_get_format(state, SENINF_PAD_MUX0) = *format;

	return 0;
}

static int mtk_seninf_s_stream(struct v4l2_subdev *sd, int enable)
{
	struct mtk_seninf *priv = sd_to_seninf(sd);
	unsigned int port;

	if (!enable) {
		mtk_seninf_stop(priv);
		return 0;
	}

	for (port = 0; port < SENINF_NUM_PORTS; port++)
		if (media_pad_remote_pad_first(&priv->pads[port]))
			return mtk_seninf_start(priv, port);

	return -ENOLINK;
}

static const struct v4l2_subdev_video_ops mtk_seninf_video_ops = {
	.s_stream = mtk_seninf_s_stream,
};

static const struct v4l2_subdev_pad_ops mtk_seninf_pad_ops = {
	.enum_mbus_code = mtk_seninf_enum_mbus_code,
	.get_fmt = v4l2_subdev_get_fmt,
	.set_fmt = mtk_seninf_set_fmt,
	.link_validate = v4l2_subdev_link_validate_default,
};

static const struct v4l2_subdev_ops mtk_seninf_subdev_ops = {
	.video = &mtk_seninf_video_ops,
	.pad = &mtk_seninf_pad_ops,
};

static const struct v4l2_subdev_internal_ops mtk_seninf_internal_ops = {
	.init_state = mtk_seninf_init_state,
};

static int mtk_seninf_link_setup(struct media_entity *entity,
				 const struct media_pad *local,
				 const struct media_pad *remote, u32 flags)
{
	struct mtk_seninf *priv = sd_to_seninf(media_entity_to_v4l2_subdev(entity));

	return priv->streaming ? -EBUSY : 0;
}

static const struct media_entity_operations mtk_seninf_entity_ops = {
	.link_setup = mtk_seninf_link_setup,
	.link_validate = v4l2_subdev_link_validate,
	.get_fwnode_pad = v4l2_subdev_get_fwnode_pad_1_to_1,
};

/* Sensors */

static int mtk_seninf_notify_bound(struct v4l2_async_notifier *notifier,
				   struct v4l2_subdev *sd,
				   struct v4l2_async_connection *asc)
{
	struct mtk_seninf *priv = container_of(notifier, struct mtk_seninf, notifier);
	struct mtk_seninf_asc *sasc = container_of(asc, struct mtk_seninf_asc, asc);

	return v4l2_create_fwnode_links_to_pad(sd, &priv->pads[sasc->port],
					       MEDIA_LNK_FL_ENABLED);
}

static int mtk_seninf_notify_complete(struct v4l2_async_notifier *notifier)
{
	struct mtk_seninf *priv = container_of(notifier, struct mtk_seninf, notifier);
	int ret;

	ret = v4l2_device_register_subdev_nodes(&priv->v4l2_dev);
	if (ret)
		return ret;

	return media_device_register(&priv->mdev);
}

static const struct v4l2_async_notifier_operations mtk_seninf_notify_ops = {
	.bound = mtk_seninf_notify_bound,
	.complete = mtk_seninf_notify_complete,
};

static int mtk_seninf_parse_endpoints(struct mtk_seninf *priv)
{
	unsigned int port, found = 0;
	int ret;

	for (port = 0; port < SENINF_NUM_PORTS; port++) {
		struct v4l2_fwnode_endpoint vep = {
			.bus_type = V4L2_MBUS_CSI2_DPHY,
		};
		struct mtk_seninf_asc *sasc;
		unsigned int i;

		struct fwnode_handle *ep __free(fwnode_handle) =
			fwnode_graph_get_endpoint_by_id(dev_fwnode(priv->dev),
							port, 0, 0);
		if (!ep)
			continue;

		ret = v4l2_fwnode_endpoint_parse(ep, &vep);
		if (ret)
			return dev_err_probe(priv->dev, ret,
					     "bad endpoint on port %u\n", port);

		if (!vep.bus.mipi_csi2.num_data_lanes ||
		    vep.bus.mipi_csi2.num_data_lanes >
		    mtk_seninf_port_info[port].max_lanes)
			return dev_err_probe(priv->dev, -EINVAL,
					     "port %u: bad lane count\n", port);

		for (i = 0; i < vep.bus.mipi_csi2.num_data_lanes; i++)
			if (vep.bus.mipi_csi2.data_lanes[i] != i + 1)
				return dev_err_probe(priv->dev, -EINVAL,
						     "lane reordering is not supported\n");

		sasc = v4l2_async_nf_add_fwnode_remote(&priv->notifier, ep,
						       struct mtk_seninf_asc);
		if (IS_ERR(sasc))
			return dev_err_probe(priv->dev, PTR_ERR(sasc),
					     "port %u: cannot add the sensor\n", port);

		sasc->port = port;
		priv->ports[port].lanes = vep.bus.mipi_csi2.num_data_lanes;
		found++;
	}

	if (!found)
		return dev_err_probe(priv->dev, -ENOTCONN, "no sensor endpoint\n");

	return 0;
}

/* Power */

static int mtk_seninf_runtime_suspend(struct device *dev)
{
	struct mtk_seninf *priv = dev_get_drvdata(dev);

	clk_bulk_disable_unprepare(SENINF_NUM_CLKS, priv->clks);

	return 0;
}

static int mtk_seninf_runtime_resume(struct device *dev)
{
	struct mtk_seninf *priv = dev_get_drvdata(dev);

	return clk_bulk_prepare_enable(SENINF_NUM_CLKS, priv->clks);
}

static DEFINE_RUNTIME_DEV_PM_OPS(mtk_seninf_pm_ops, mtk_seninf_runtime_suspend,
				 mtk_seninf_runtime_resume, NULL);

/* Platform driver */

static int mtk_seninf_get_phys(struct mtk_seninf *priv)
{
	unsigned int i;

	for (i = 0; i < SENINF_NUM_PORTS; i++) {
		struct phy *phy;

		phy = devm_phy_optional_get(priv->dev,
					    mtk_seninf_port_info[i].phy_name);
		if (IS_ERR(phy))
			return dev_err_probe(priv->dev, PTR_ERR(phy),
					     "cannot get the %s PHY\n",
					     mtk_seninf_port_info[i].phy_name);
		priv->ports[i].phy = phy;
	}

	return 0;
}

static int mtk_seninf_register_subdev(struct mtk_seninf *priv)
{
	struct v4l2_subdev *sd = &priv->sd;
	unsigned int i;
	int ret;

	v4l2_subdev_init(sd, &mtk_seninf_subdev_ops);
	sd->internal_ops = &mtk_seninf_internal_ops;
	sd->owner = THIS_MODULE;
	sd->dev = priv->dev;
	sd->flags |= V4L2_SUBDEV_FL_HAS_DEVNODE;
	strscpy(sd->name, "mtk-seninf", sizeof(sd->name));

	sd->entity.function = MEDIA_ENT_F_VID_IF_BRIDGE;
	sd->entity.ops = &mtk_seninf_entity_ops;

	for (i = 0; i < SENINF_NUM_PORTS; i++)
		priv->pads[i].flags = MEDIA_PAD_FL_SINK;
	priv->pads[SENINF_PAD_MUX0].flags = MEDIA_PAD_FL_SOURCE;

	ret = media_entity_pads_init(&sd->entity, SENINF_NUM_PADS, priv->pads);
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

static int mtk_seninf_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mtk_seninf *priv;
	int ret;

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->dev = dev;
	mutex_init(&priv->lock);
	priv->tune.settle = MTK_SENINF_SETTLE_DEFAULT;
	priv->tune.hs_trail = SENINF_CSI2_HS_TRAIL_VAL;
	priv->tune.port = MTK_SENINF_PORT_NONE;
	priv->tune.lane_map = MTK_SENINF_LANE_MAP_NONE;
	platform_set_drvdata(pdev, priv);

	/* Nothing touches the registers before the first runtime resume */
	priv->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(priv->base))
		return PTR_ERR(priv->base);

	priv->clks[0].id = "seninf";
	priv->clks[1].id = "seninf_mux";
	ret = devm_clk_bulk_get(dev, SENINF_NUM_CLKS, priv->clks);
	if (ret)
		return dev_err_probe(dev, ret, "cannot get the clocks\n");

	ret = mtk_seninf_get_phys(priv);
	if (ret)
		return ret;

	ret = devm_pm_runtime_enable(dev);
	if (ret)
		return ret;

	priv->mdev.dev = dev;
	strscpy(priv->mdev.model, "mtk-seninf", sizeof(priv->mdev.model));
	media_device_init(&priv->mdev);
	priv->v4l2_dev.mdev = &priv->mdev;

	ret = v4l2_device_register(dev, &priv->v4l2_dev);
	if (ret)
		goto err_mdev;

	ret = mtk_seninf_register_subdev(priv);
	if (ret)
		goto err_v4l2;

	v4l2_async_nf_init(&priv->notifier, &priv->v4l2_dev);
	priv->notifier.ops = &mtk_seninf_notify_ops;

	ret = mtk_seninf_parse_endpoints(priv);
	if (ret)
		goto err_nf;

	ret = v4l2_async_nf_register(&priv->notifier);
	if (ret)
		goto err_nf;

	return 0;

err_nf:
	v4l2_async_nf_cleanup(&priv->notifier);
	v4l2_device_unregister_subdev(&priv->sd);
	v4l2_subdev_cleanup(&priv->sd);
	media_entity_cleanup(&priv->sd.entity);
err_v4l2:
	v4l2_device_unregister(&priv->v4l2_dev);
err_mdev:
	media_device_cleanup(&priv->mdev);
	return ret;
}

static void mtk_seninf_remove(struct platform_device *pdev)
{
	struct mtk_seninf *priv = platform_get_drvdata(pdev);

	mtk_seninf_stop(priv);

	v4l2_async_nf_unregister(&priv->notifier);
	v4l2_async_nf_cleanup(&priv->notifier);
	media_device_unregister(&priv->mdev);
	v4l2_device_unregister_subdev(&priv->sd);
	v4l2_subdev_cleanup(&priv->sd);
	media_entity_cleanup(&priv->sd.entity);
	v4l2_device_unregister(&priv->v4l2_dev);
	media_device_cleanup(&priv->mdev);
}

static const struct of_device_id mtk_seninf_of_match[] = {
	{ .compatible = "mediatek,mt8183-seninf" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, mtk_seninf_of_match);

static struct platform_driver mtk_seninf_driver = {
	.probe = mtk_seninf_probe,
	.remove = mtk_seninf_remove,
	.driver = {
		.name = "mtk-seninf",
		.of_match_table = mtk_seninf_of_match,
		.pm = pm_ptr(&mtk_seninf_pm_ops),
	},
};
module_platform_driver(mtk_seninf_driver);

MODULE_DESCRIPTION("MediaTek SENINF CSI-2 receiver");
MODULE_LICENSE("GPL");
