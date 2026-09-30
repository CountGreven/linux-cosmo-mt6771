// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MIPI CSI v0.5 driver
 *
 * Copyright (c) 2023, MediaTek Inc.
 * Copyright (c) 2023, BayLibre Inc.
 */

#include <dt-bindings/phy/phy.h>
#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/slab.h>

#include "phy-mtk-io.h"
#include "phy-mtk-mipi-csi-0-5-rx-reg.h"

#define CSIXB_OFFSET		0x1000

enum mtk_csi_layout {
	MTK_CSI_LAYOUT_4D1C,
	MTK_CSI_LAYOUT_2D1C_A,
	MTK_CSI_LAYOUT_2D1C_B,
	MTK_CSI_NUM_LAYOUTS
};

#define MTK_CSI_HALF_A		BIT(0)
#define MTK_CSI_HALF_B		BIT(1)

struct mtk_mipi_cdphy_data {
	bool half_ports;
};

struct mtk_mipi_cdphy_port;

struct mtk_mipi_cdphy_phy {
	struct mtk_mipi_cdphy_port *port;
	struct phy *phy;
	/* Which halves of the receiver this PHY drives */
	unsigned int halves;
};

struct mtk_mipi_cdphy_port {
	struct device *dev;
	void __iomem *base;
	const struct mtk_mipi_cdphy_data *data;
	struct mtk_mipi_cdphy_phy phys[MTK_CSI_NUM_LAYOUTS];
	u32 type;
	u32 mode;
	u32 num_lanes;
};

enum PHY_TYPE {
	DPHY = 0,
	CPHY,
	CDPHY,
};

/* Clock/data pad selection of ANA00, one bit per field */
struct mtk_csi_ck_cfg {
	u8 l0_ckmode, l0_cksel;
	u8 l1_ckmode, l1_cksel;
	u8 l2_ckmode, l2_cksel;
};

/* 4D1C: the clock is on lane 2 of half A, half B carries data only */
static const struct mtk_csi_ck_cfg mtk_csi_ck_4d1c_a = { 0, 1, 0, 1, 1, 1 };
static const struct mtk_csi_ck_cfg mtk_csi_ck_4d1c_b = { 0, 1, 0, 1, 0, 1 };
/* 2D1C: one half alone, the clock is on its lane 1 */
static const struct mtk_csi_ck_cfg mtk_csi_ck_2d1c = { 0, 0, 1, 0, 0, 0 };

static void mtk_phy_csi_cdphy_ana_eq_tune(void __iomem *base)
{
	mtk_phy_update_field(base + MIPI_RX_ANA18_CSIXA, RG_CSI0A_L0_T0AB_EQ_IS, 1);
	mtk_phy_update_field(base + MIPI_RX_ANA18_CSIXA, RG_CSI0A_L0_T0AB_EQ_BW, 1);
	mtk_phy_update_field(base + MIPI_RX_ANA1C_CSIXA, RG_CSI0A_L1_T1AB_EQ_IS, 1);
	mtk_phy_update_field(base + MIPI_RX_ANA1C_CSIXA, RG_CSI0A_L1_T1AB_EQ_BW, 1);
	mtk_phy_update_field(base + MIPI_RX_ANA20_CSI0A, RG_CSI0A_L2_T1BC_EQ_IS, 1);
	mtk_phy_update_field(base + MIPI_RX_ANA20_CSI0A, RG_CSI0A_L2_T1BC_EQ_BW, 1);
}

static void mtk_phy_csi_dphy_ana_eq_tune(void __iomem *base)
{
	mtk_phy_update_field(base + MIPI_RX_ANA18_CSIXA, RG_CSI1A_L0_EQ_IS, 1);
	mtk_phy_update_field(base + MIPI_RX_ANA18_CSIXA, RG_CSI1A_L0_EQ_BW, 1);
	mtk_phy_update_field(base + MIPI_RX_ANA18_CSIXA, RG_CSI1A_L1_EQ_IS, 1);
	mtk_phy_update_field(base + MIPI_RX_ANA18_CSIXA, RG_CSI1A_L1_EQ_BW, 1);
	mtk_phy_update_field(base + MIPI_RX_ANA1C_CSIXA, RG_CSI1A_L2_EQ_IS, 1);
	mtk_phy_update_field(base + MIPI_RX_ANA1C_CSIXA, RG_CSI1A_L2_EQ_BW, 1);
}

/* @base is the start of one half (CSIxA or CSIxB) of the receiver */
static void mtk_phy_csi_half_configure(void __iomem *base, bool cdphy,
				       const struct mtk_csi_ck_cfg *ck)
{
	/*
	 * The driver currently supports DPHY and CD-PHY phys,
	 * but the only mode supported is DPHY,
	 * so CD-PHY capable phys must be configured in DPHY mode
	 */
	if (cdphy)
		mtk_phy_update_field(base + MIPI_RX_ANA00_CSIXA, RG_CSI0A_CPHY_EN, 0);

	mtk_phy_update_field(base + MIPI_RX_ANA00_CSIXA, RG_CSIXA_DPHY_L0_CKMODE_EN,
			     ck->l0_ckmode);
	mtk_phy_update_field(base + MIPI_RX_ANA00_CSIXA, RG_CSIXA_DPHY_L0_CKSEL,
			     ck->l0_cksel);
	mtk_phy_update_field(base + MIPI_RX_ANA00_CSIXA, RG_CSIXA_DPHY_L1_CKMODE_EN,
			     ck->l1_ckmode);
	mtk_phy_update_field(base + MIPI_RX_ANA00_CSIXA, RG_CSIXA_DPHY_L1_CKSEL,
			     ck->l1_cksel);
	mtk_phy_update_field(base + MIPI_RX_ANA00_CSIXA, RG_CSIXA_DPHY_L2_CKMODE_EN,
			     ck->l2_ckmode);
	mtk_phy_update_field(base + MIPI_RX_ANA00_CSIXA, RG_CSIXA_DPHY_L2_CKSEL,
			     ck->l2_cksel);

	/* Byte clock invert */
	mtk_phy_update_field(base + MIPI_RX_ANAA8_CSIXA, RG_CSIXA_CDPHY_L0_T0_BYTECK_INVERT, 1);
	mtk_phy_update_field(base + MIPI_RX_ANAA8_CSIXA, RG_CSIXA_DPHY_L1_BYTECK_INVERT, 1);
	mtk_phy_update_field(base + MIPI_RX_ANAA8_CSIXA, RG_CSIXA_CDPHY_L2_T1_BYTECK_INVERT, 1);

	/* Start ANA EQ tuning */
	if (cdphy)
		mtk_phy_csi_cdphy_ana_eq_tune(base);
	else
		mtk_phy_csi_dphy_ana_eq_tune(base);
	/* End ANA EQ tuning */

	mtk_phy_update_field(base + MIPI_RX_ANA24_CSIXA, RG_CSIXA_RESERVE, 0x40);
	mtk_phy_update_field(base + MIPI_RX_WRAPPER80_CSIXA, CSR_CSI_RST_MODE, 0);
}

static int mtk_mipi_phy_power_on(struct phy *phy)
{
	struct mtk_mipi_cdphy_phy *cphy = phy_get_drvdata(phy);
	struct mtk_mipi_cdphy_port *port = cphy->port;
	void __iomem *base = port->base;
	bool cdphy = port->type == CDPHY;
	void __iomem *a = base;
	void __iomem *b = base + CSIXB_OFFSET;

	/*
	 * Lane configuration:
	 *
	 * 4 data + 1 clock is the following mapping:
	 *
	 * CSIXA_LNR0 --> D2
	 * CSIXA_LNR1 --> D0
	 * CSIXA_LNR2 --> C
	 * CSIXB_LNR0 --> D1
	 * CSIXB_LNR1 --> D3
	 *
	 * 2 data + 1 clock uses one half alone, the pads of which are not
	 * known to match the 4D1C roles.
	 */
	if (cphy->halves == (MTK_CSI_HALF_A | MTK_CSI_HALF_B)) {
		mtk_phy_csi_half_configure(a, cdphy, &mtk_csi_ck_4d1c_a);
		mtk_phy_csi_half_configure(b, cdphy, &mtk_csi_ck_4d1c_b);
	} else if (cphy->halves == MTK_CSI_HALF_A) {
		mtk_phy_csi_half_configure(a, cdphy, &mtk_csi_ck_2d1c);
	} else {
		mtk_phy_csi_half_configure(b, cdphy, &mtk_csi_ck_2d1c);
	}

	/* The vendor writes this once per port, into half A */
	mtk_phy_set_bits(base + MIPI_RX_ANA40_CSIXA, 0x90);

	/* ANA power on */
	if (cphy->halves & MTK_CSI_HALF_A)
		mtk_phy_update_field(a + MIPI_RX_ANA00_CSIXA, RG_CSIXA_BG_CORE_EN, 1);
	if (cphy->halves & MTK_CSI_HALF_B)
		mtk_phy_update_field(b + MIPI_RX_ANA00_CSIXA, RG_CSIXA_BG_CORE_EN, 1);
	usleep_range(20, 40);
	if (cphy->halves & MTK_CSI_HALF_A)
		mtk_phy_update_field(a + MIPI_RX_ANA00_CSIXA, RG_CSIXA_BG_LPF_EN, 1);
	if (cphy->halves & MTK_CSI_HALF_B)
		mtk_phy_update_field(b + MIPI_RX_ANA00_CSIXA, RG_CSIXA_BG_LPF_EN, 1);

	if (cphy->halves & MTK_CSI_HALF_A)
		dev_dbg(port->dev, "half A: ANA00 %#x ANAA8 %#x\n",
			readl(a + MIPI_RX_ANA00_CSIXA), readl(a + MIPI_RX_ANAA8_CSIXA));
	if (cphy->halves & MTK_CSI_HALF_B)
		dev_dbg(port->dev, "half B: ANA00 %#x ANAA8 %#x\n",
			readl(b + MIPI_RX_ANA00_CSIXA), readl(b + MIPI_RX_ANAA8_CSIXA));

	return 0;
}

static int mtk_mipi_phy_power_off(struct phy *phy)
{
	struct mtk_mipi_cdphy_phy *cphy = phy_get_drvdata(phy);
	void __iomem *base = cphy->port->base;

	/* Disable MIPI BG. */
	if (cphy->halves & MTK_CSI_HALF_A) {
		mtk_phy_update_field(base + MIPI_RX_ANA00_CSIXA, RG_CSIXA_BG_CORE_EN, 0);
		mtk_phy_update_field(base + MIPI_RX_ANA00_CSIXA, RG_CSIXA_BG_LPF_EN, 0);
	}
	if (cphy->halves & MTK_CSI_HALF_B) {
		mtk_phy_update_field(base + CSIXB_OFFSET + MIPI_RX_ANA00_CSIXA,
				     RG_CSIXA_BG_CORE_EN, 0);
		mtk_phy_update_field(base + CSIXB_OFFSET + MIPI_RX_ANA00_CSIXA,
				     RG_CSIXA_BG_LPF_EN, 0);
	}

	return 0;
}

static struct phy *mtk_mipi_cdphy_xlate(struct device *dev,
					const struct of_phandle_args *args)
{
	struct mtk_mipi_cdphy_port *priv = dev_get_drvdata(dev);
	unsigned int layout = MTK_CSI_LAYOUT_4D1C;

	/*
	 * If PHY is CD-PHY then we need to get the operating mode
	 * For now only D-PHY mode is supported
	 */
	if (priv->type == CDPHY) {
		if (args->args_count != (priv->data->half_ports ? 2 : 1)) {
			dev_err(dev, "invalid number of arguments\n");
			return ERR_PTR(-EINVAL);
		}
		switch (args->args[0]) {
		case PHY_TYPE_DPHY:
			priv->mode = DPHY;
			break;
		default:
			dev_err(dev, "Unsupported PHY type: %i\n", args->args[0]);
			return ERR_PTR(-EINVAL);
		}
		/* Second cell: 0 both halves (4D1C), 1 half A, 2 half B (2D1C) */
		if (priv->data->half_ports)
			layout = args->args[1];
		if (layout >= MTK_CSI_NUM_LAYOUTS) {
			dev_err(dev, "invalid port layout: %u\n", layout);
			return ERR_PTR(-EINVAL);
		}
		if (layout == MTK_CSI_LAYOUT_4D1C && priv->num_lanes != 4) {
			dev_err(dev, "Only 4D1C mode is supported for now!\n");
			return ERR_PTR(-EINVAL);
		}
	} else {
		if (args->args_count) {
			dev_err(dev, "invalid number of arguments\n");
			return ERR_PTR(-EINVAL);
		}
		priv->mode = DPHY;
	}

	return priv->phys[layout].phy;
}

static const struct phy_ops mtk_cdphy_ops = {
	.power_on	= mtk_mipi_phy_power_on,
	.power_off	= mtk_mipi_phy_power_off,
	.owner		= THIS_MODULE,
};

static int mtk_mipi_cdphy_create_phy(struct mtk_mipi_cdphy_port *port,
				     unsigned int layout, unsigned int halves)
{
	struct mtk_mipi_cdphy_phy *cphy = &port->phys[layout];
	struct phy *phy;

	cphy->port = port;
	cphy->halves = halves;

	phy = devm_phy_create(port->dev, NULL, &mtk_cdphy_ops);
	if (IS_ERR(phy)) {
		dev_err(port->dev, "Failed to create PHY: %ld\n", PTR_ERR(phy));
		return PTR_ERR(phy);
	}

	cphy->phy = phy;
	phy_set_drvdata(phy, cphy);

	return 0;
}

static int mtk_mipi_cdphy_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct phy_provider *phy_provider;
	struct mtk_mipi_cdphy_port *port;
	int ret;
	u32 phy_type;

	port = devm_kzalloc(dev, sizeof(*port), GFP_KERNEL);
	if (!port)
		return -ENOMEM;

	dev_set_drvdata(dev, port);

	port->dev = dev;
	port->data = device_get_match_data(dev);

	port->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(port->base))
		return PTR_ERR(port->base);

	ret = of_property_read_u32(dev->of_node, "num-lanes", &port->num_lanes);
	if (ret) {
		dev_err(dev, "Failed to read num-lanes property: %i\n", ret);
		return ret;
	}

	/*
	 * phy-type is optional, if not present, PHY is considered to be CD-PHY
	 */
	if (device_property_present(dev, "phy-type")) {
		ret = of_property_read_u32(dev->of_node, "phy-type", &phy_type);
		if (ret) {
			dev_err(dev, "Failed to read phy-type property: %i\n", ret);
			return ret;
		}
		switch (phy_type) {
		case PHY_TYPE_DPHY:
			port->type = DPHY;
			break;
		default:
			dev_err(dev, "Unsupported PHY type: %i\n", phy_type);
			return -EINVAL;
		}
	} else {
		port->type = CDPHY;
	}

	ret = mtk_mipi_cdphy_create_phy(port, MTK_CSI_LAYOUT_4D1C,
					MTK_CSI_HALF_A | MTK_CSI_HALF_B);
	if (ret)
		return ret;

	/* A CD-PHY receiver can also be split into two 2D1C halves */
	if (port->type == CDPHY && port->data->half_ports) {
		ret = mtk_mipi_cdphy_create_phy(port, MTK_CSI_LAYOUT_2D1C_A,
						MTK_CSI_HALF_A);
		if (ret)
			return ret;

		ret = mtk_mipi_cdphy_create_phy(port, MTK_CSI_LAYOUT_2D1C_B,
						MTK_CSI_HALF_B);
		if (ret)
			return ret;
	}

	phy_provider = devm_of_phy_provider_register(dev, mtk_mipi_cdphy_xlate);
	if (IS_ERR(phy_provider)) {
		dev_err(dev, "Failed to register PHY provider: %ld\n",
			PTR_ERR(phy_provider));
		return PTR_ERR(phy_provider);
	}

	return 0;
}

static const struct mtk_mipi_cdphy_data mtk_mipi_cdphy_mt8365_data = {
	.half_ports = false,
};

static const struct mtk_mipi_cdphy_data mtk_mipi_cdphy_mt8183_data = {
	.half_ports = true,
};

static const struct of_device_id mtk_mipi_cdphy_of_match[] = {
	{ .compatible = "mediatek,mt8365-csi-rx", .data = &mtk_mipi_cdphy_mt8365_data },
	{ .compatible = "mediatek,mt8183-csi-rx", .data = &mtk_mipi_cdphy_mt8183_data },
	{ /* sentinel */},
};
MODULE_DEVICE_TABLE(of, mtk_mipi_cdphy_of_match);

static struct platform_driver mipi_cdphy_pdrv = {
	.probe = mtk_mipi_cdphy_probe,
	.driver	= {
		.name	= "mtk-mipi-csi-0-5",
		.of_match_table = mtk_mipi_cdphy_of_match,
	},
};
module_platform_driver(mipi_cdphy_pdrv);

MODULE_DESCRIPTION("MediaTek MIPI CSI CD-PHY v0.5 Driver");
MODULE_AUTHOR("Louis Kuo <louis.kuo@mediatek.com>");
MODULE_LICENSE("GPL");
