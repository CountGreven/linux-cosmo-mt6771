// SPDX-License-Identifier: GPL-2.0-only
/*
 * Novatek NT36672 DSI panel driver
 *
 * The one panel supported so far is the 1080x2160 LCD of the Planet Computers Cosmo Communicator
 * (bootloader name "aeon_nt36672_fhd_dsi_vdo_x800_datong"). Its sequences come from Planet's
 * vendor kernel, drivers/misc/mediatek/lcm/aeon_nt36672_fhd_dsi_vdo_x600_xinli/
 * aeon_nt36672_fhd_dsi_vdo_x600_xinli.c, cited below as "vendor:<line>".
 *
 * Copyright (c) 2026 Fredrik Lindlöf <fredrik.lindlof@gmail.com>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/mod_devicetable.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regulator/consumer.h>

#include <drm/drm_connector.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_modes.h>
#include <drm/drm_panel.h>
#include <drm/drm_probe_helper.h>

static const char * const nt36672_supply_names[] = {
	"vddi",
	"avdd",
	"avee",
};

struct nt36672_panel_desc {
	const struct drm_display_mode *mode;

	unsigned long mode_flags;
	enum mipi_dsi_pixel_format format;
	unsigned int lanes;

	/* everything from the first register write to display on, delays included */
	void (*init_cmds)(struct mipi_dsi_multi_context *ctx);
	/* display off and sleep in, delays included */
	void (*off_cmds)(struct mipi_dsi_multi_context *ctx);
};

struct nt36672_panel {
	struct drm_panel panel;
	struct mipi_dsi_device *dsi;
	const struct nt36672_panel_desc *desc;

	struct regulator_bulk_data supplies[ARRAY_SIZE(nt36672_supply_names)];
	struct gpio_desc *reset_gpio;
};

static inline struct nt36672_panel *to_nt36672_panel(struct drm_panel *panel)
{
	return container_of(panel, struct nt36672_panel, panel);
}

/*
 * The vendor's init_setting[] (vendor:108-320, the live "#if 1" branch) as its push_table() sends
 * it: commands below 0xb0 go out as DCS packets and the rest as generic ones (MediaTek's
 * DSI_set_cmdq_V2), which is what the two write helpers send for the same bytes. Sleep out (0x11)
 * and display on (0x29) carry a 0x00 parameter in the vendor table, so they stay raw writes rather
 * than the parameterless helpers.
 *
 * Generated from the vendor table, together with planet_cosmo_off_cmds(); do not edit by hand.
 */
static void planet_cosmo_init_cmds(struct mipi_dsi_multi_context *ctx)
{
	mipi_dsi_generic_write_seq_multi(ctx, 0xff, 0x20);
	mipi_dsi_generic_write_seq_multi(ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x01, 0x33);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x06, 0x99);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x07, 0x9e);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x0e, 0x30);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x0f, 0x2e);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x1d, 0x33);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x6d, 0x66);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x68, 0x03);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x69, 0x99);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x89, 0x0f);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x95, 0xcd);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x96, 0xcd);
	mipi_dsi_generic_write_seq_multi(ctx, 0xff, 0x24);
	mipi_dsi_generic_write_seq_multi(ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x00, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x01, 0x1c);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x02, 0x0b);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x03, 0x0c);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x04, 0x29);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x05, 0x0f);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x06, 0x0f);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x07, 0x03);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x08, 0x05);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x09, 0x22);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x0a, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x0b, 0x24);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x0c, 0x13);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x0d, 0x13);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x0e, 0x15);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x0f, 0x15);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x10, 0x17);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x11, 0x17);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x12, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x13, 0x1c);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x14, 0x0b);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x15, 0x0c);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x16, 0x29);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x17, 0x0f);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x18, 0x0f);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x19, 0x04);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x1a, 0x06);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x1b, 0x23);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x1c, 0x0f);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x1d, 0x24);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x1e, 0x13);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x1f, 0x13);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x20, 0x15);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x21, 0x15);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x22, 0x17);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x23, 0x17);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x2f, 0x04);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x30, 0x08);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x31, 0x04);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x32, 0x08);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x33, 0x04);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x34, 0x04);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x35, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x37, 0x09);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x38, 0x75);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x39, 0x75);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x3b, 0xc0);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x3f, 0x75);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x60, 0x10);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x61, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x68, 0xc2);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x78, 0x80);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x79, 0x23);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x7a, 0x10);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x7b, 0x9b);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x7c, 0x80);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x7d, 0x06);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x7e, 0x02);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x8e, 0xf0);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x92, 0x76);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x93, 0x0a);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x94, 0x0a);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x99, 0x33);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x9b, 0xff);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x9f, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0xa3, 0x91);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb3, 0x00);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb4, 0x00);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb5, 0x04);
	mipi_dsi_generic_write_seq_multi(ctx, 0xdc, 0x40);
	mipi_dsi_generic_write_seq_multi(ctx, 0xdd, 0x03);
	mipi_dsi_generic_write_seq_multi(ctx, 0xde, 0x01);
	mipi_dsi_generic_write_seq_multi(ctx, 0xdf, 0x3d);
	mipi_dsi_generic_write_seq_multi(ctx, 0xe0, 0x3d);
	mipi_dsi_generic_write_seq_multi(ctx, 0xe1, 0x22);
	mipi_dsi_generic_write_seq_multi(ctx, 0xe2, 0x24);
	mipi_dsi_generic_write_seq_multi(ctx, 0xe3, 0x0a);
	mipi_dsi_generic_write_seq_multi(ctx, 0xe4, 0x0a);
	mipi_dsi_generic_write_seq_multi(ctx, 0xe8, 0x01);
	mipi_dsi_generic_write_seq_multi(ctx, 0xe9, 0x10);
	mipi_dsi_generic_write_seq_multi(ctx, 0xed, 0x40);
	mipi_dsi_generic_write_seq_multi(ctx, 0xff, 0x25);
	mipi_dsi_generic_write_seq_multi(ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x0a, 0x81);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x0b, 0xcd);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x0c, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x17, 0x82);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x21, 0x1b);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x22, 0x1b);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x24, 0x76);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x25, 0x76);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x30, 0x2a);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x31, 0x2a);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x38, 0x2a);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x3f, 0x11);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x40, 0x3a);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x4b, 0x31);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x4c, 0x3a);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x58, 0x22);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x59, 0x05);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x5a, 0x0a);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x5b, 0x0a);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x5c, 0x25);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x5d, 0x80);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x5e, 0x80);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x5f, 0x28);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x62, 0x3f);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x63, 0x82);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x65, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x66, 0xdd);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x6c, 0x6d);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x71, 0x6d);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x78, 0x25);
	mipi_dsi_generic_write_seq_multi(ctx, 0xc3, 0x00);
	mipi_dsi_generic_write_seq_multi(ctx, 0xff, 0x26);
	mipi_dsi_generic_write_seq_multi(ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x06, 0xc8);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x12, 0x5a);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x19, 0x09);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x1a, 0x84);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x1c, 0xfa);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x1d, 0x09);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x1e, 0x0b);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x99, 0x20);
	mipi_dsi_generic_write_seq_multi(ctx, 0xff, 0x27);
	mipi_dsi_generic_write_seq_multi(ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x13, 0x08);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x14, 0x43);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x16, 0xb8);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x17, 0xb8);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x7a, 0x02);
	mipi_dsi_generic_write_seq_multi(ctx, 0xff, 0x20);
	mipi_dsi_generic_write_seq_multi(ctx, 0xfb, 0x01);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb0, 0x00, 0xaa, 0x00, 0xb6, 0x00, 0xc8, 0x00, 0xd9,
					 0x00, 0xea, 0x00, 0xf6, 0x01, 0x07, 0x01, 0x11);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb1, 0x01, 0x1c, 0x01, 0x40, 0x01, 0x60, 0x01, 0x90,
					 0x01, 0xb6, 0x01, 0xf1, 0x02, 0x1b, 0x02, 0x1d);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb2, 0x02, 0x46, 0x02, 0x75, 0x02, 0x93, 0x02, 0xbe,
					 0x02, 0xdc, 0x03, 0x08, 0x03, 0x16, 0x03, 0x24);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb3, 0x03, 0x35, 0x03, 0x49, 0x03, 0x62, 0x03, 0x84,
					 0x03, 0xb3, 0x03, 0xff);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb4, 0x01, 0x03, 0x01, 0x09, 0x01, 0x15, 0x01, 0x20,
					 0x01, 0x2a, 0x01, 0x34, 0x01, 0x3c, 0x01, 0x45);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb5, 0x01, 0x4d, 0x01, 0x6a, 0x01, 0x82, 0x01, 0xaa,
					 0x01, 0xca, 0x01, 0xfe, 0x02, 0x25, 0x02, 0x26);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb6, 0x02, 0x4d, 0x02, 0x7a, 0x02, 0x9a, 0x02, 0xc3,
					 0x02, 0xe1, 0x03, 0x0e, 0x03, 0x1c, 0x03, 0x2a);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb7, 0x03, 0x3a, 0x03, 0x4e, 0x03, 0x66, 0x03, 0x88,
					 0x03, 0xb5, 0x03, 0xff);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb8, 0x00, 0x00, 0x00, 0x34, 0x00, 0x6c, 0x00, 0x92,
					 0x00, 0xad, 0x00, 0xc4, 0x00, 0xdc, 0x00, 0xeb);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb9, 0x00, 0xfc, 0x01, 0x2d, 0x01, 0x53, 0x01, 0x8a,
					 0x01, 0xb3, 0x01, 0xf1, 0x02, 0x1c, 0x02, 0x1d);
	mipi_dsi_generic_write_seq_multi(ctx, 0xba, 0x02, 0x46, 0x02, 0x75, 0x02, 0x94, 0x02, 0xbf,
					 0x02, 0xdd, 0x03, 0x0a, 0x03, 0x16, 0x03, 0x25);
	mipi_dsi_generic_write_seq_multi(ctx, 0xbb, 0x03, 0x35, 0x03, 0x49, 0x03, 0x61, 0x03, 0x7d,
					 0x03, 0xb1, 0x03, 0xff);
	mipi_dsi_generic_write_seq_multi(ctx, 0xff, 0x21);
	mipi_dsi_generic_write_seq_multi(ctx, 0xfb, 0x01);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb0, 0x00, 0xaa, 0x00, 0xb6, 0x00, 0xc8, 0x00, 0xd9,
					 0x00, 0xea, 0x00, 0xf6, 0x01, 0x07, 0x01, 0x11);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb1, 0x01, 0x1c, 0x01, 0x40, 0x01, 0x60, 0x01, 0x90,
					 0x01, 0xb6, 0x01, 0xf1, 0x02, 0x1b, 0x02, 0x1d);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb2, 0x02, 0x46, 0x02, 0x75, 0x02, 0x93, 0x02, 0xbe,
					 0x02, 0xdc, 0x03, 0x08, 0x03, 0x16, 0x03, 0x24);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb3, 0x03, 0x35, 0x03, 0x49, 0x03, 0x62, 0x03, 0x84,
					 0x03, 0xb3, 0x03, 0xff);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb4, 0x01, 0x03, 0x01, 0x09, 0x01, 0x15, 0x01, 0x20,
					 0x01, 0x2a, 0x01, 0x34, 0x01, 0x3c, 0x01, 0x45);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb5, 0x01, 0x4d, 0x01, 0x6a, 0x01, 0x82, 0x01, 0xaa,
					 0x01, 0xca, 0x01, 0xfe, 0x02, 0x25, 0x02, 0x26);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb6, 0x02, 0x4d, 0x02, 0x7a, 0x02, 0x9a, 0x02, 0xc3,
					 0x02, 0xe1, 0x03, 0x0e, 0x03, 0x1c, 0x03, 0x2a);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb7, 0x03, 0x3a, 0x03, 0x4e, 0x03, 0x66, 0x03, 0x88,
					 0x03, 0xb5, 0x03, 0xff);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb8, 0x00, 0x00, 0x00, 0x34, 0x00, 0x6c, 0x00, 0x92,
					 0x00, 0xad, 0x00, 0xc4, 0x00, 0xdc, 0x00, 0xeb);
	mipi_dsi_generic_write_seq_multi(ctx, 0xb9, 0x00, 0xfc, 0x01, 0x2d, 0x01, 0x53, 0x01, 0x8a,
					 0x01, 0xb3, 0x01, 0xf1, 0x02, 0x1c, 0x02, 0x1d);
	mipi_dsi_generic_write_seq_multi(ctx, 0xba, 0x02, 0x46, 0x02, 0x75, 0x02, 0x94, 0x02, 0xbf,
					 0x02, 0xdd, 0x03, 0x0a, 0x03, 0x16, 0x03, 0x25);
	mipi_dsi_generic_write_seq_multi(ctx, 0xbb, 0x03, 0x35, 0x03, 0x49, 0x03, 0x61, 0x03, 0x7d,
					 0x03, 0xb1, 0x03, 0xff);
	mipi_dsi_generic_write_seq_multi(ctx, 0xff, 0x10);
	mipi_dsi_generic_write_seq_multi(ctx, 0xfb, 0x01);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x51, 0xff);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x53, 0x24);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x55, 0x00);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x36, 0x03);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x11, 0x00);
	mipi_dsi_msleep(ctx, 120);
	mipi_dsi_dcs_write_seq_multi(ctx, 0x29, 0x00);
	mipi_dsi_msleep(ctx, 10);
}

/* The vendor's lcm_suspend_setting[] (vendor:99-106). */
static void planet_cosmo_off_cmds(struct mipi_dsi_multi_context *ctx)
{
	mipi_dsi_dcs_set_display_off_multi(ctx);
	mipi_dsi_msleep(ctx, 50);
	mipi_dsi_dcs_enter_sleep_mode_multi(ctx);
	mipi_dsi_msleep(ctx, 120);
}

static int nt36672_power_on(struct nt36672_panel *ctx)
{
	int i, ret;

	/* VDDI before the bias rails; the bias rails in the vendor's order, positive first */
	for (i = 0; i < ARRAY_SIZE(ctx->supplies); i++) {
		ret = regulator_enable(ctx->supplies[i].consumer);
		if (ret) {
			dev_err(&ctx->dsi->dev, "failed to enable %s: %d\n",
				ctx->supplies[i].supply, ret);
			while (--i >= 0)
				regulator_disable(ctx->supplies[i].consumer);
			return ret;
		}
	}

	/* lcm_poweron() (vendor:603-624): low 20 ms, high 10 ms, low 10 ms, high 20 ms */
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	msleep(20);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 1);
	usleep_range(10000, 11000);
	gpiod_set_value_cansleep(ctx->reset_gpio, 0);
	msleep(20);

	return 0;
}

static void nt36672_power_off(struct nt36672_panel *ctx)
{
	int i;

	gpiod_set_value_cansleep(ctx->reset_gpio, 1);

	for (i = ARRAY_SIZE(ctx->supplies) - 1; i >= 0; i--)
		regulator_disable(ctx->supplies[i].consumer);
}

static int nt36672_prepare(struct drm_panel *panel)
{
	struct nt36672_panel *ctx = to_nt36672_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };
	int ret;

	ret = nt36672_power_on(ctx);
	if (ret)
		return ret;

	ctx->desc->init_cmds(&dsi_ctx);
	if (dsi_ctx.accum_err) {
		nt36672_power_off(ctx);
		return dsi_ctx.accum_err;
	}

	return 0;
}

static int nt36672_unprepare(struct drm_panel *panel)
{
	struct nt36672_panel *ctx = to_nt36672_panel(panel);
	struct mipi_dsi_multi_context dsi_ctx = { .dsi = ctx->dsi };

	ctx->desc->off_cmds(&dsi_ctx);
	/* lcm_suspend() waits another 10 ms after the table (vendor:638) */
	usleep_range(10000, 11000);

	nt36672_power_off(ctx);

	return 0;
}

static int nt36672_get_modes(struct drm_panel *panel, struct drm_connector *connector)
{
	struct nt36672_panel *ctx = to_nt36672_panel(panel);

	return drm_connector_helper_get_modes_fixed(connector, ctx->desc->mode);
}

static const struct drm_panel_funcs nt36672_panel_funcs = {
	.prepare = nt36672_prepare,
	.unprepare = nt36672_unprepare,
	.get_modes = nt36672_get_modes,
};

/*
 * lcm_get_params() (vendor:521-584): HFP 80, HSA 16, HBP 40, VFP 4, VSA 2, VBP 33, 68 x 136 mm.
 * The vendor gives no pixel clock, only a 550 MHz DSI PLL; its bootloader reports this timing
 * running at 64.44 Hz ("mt_disp_get_lcd_time, fps=6444"), so the clock is htotal * vtotal * 64.44.
 */
static const struct drm_display_mode planet_cosmo_mode = {
	.clock = 172312,
	.hdisplay = 1080,
	.hsync_start = 1080 + 80,
	.hsync_end = 1080 + 80 + 16,
	.htotal = 1080 + 80 + 16 + 40,
	.vdisplay = 2160,
	.vsync_start = 2160 + 4,
	.vsync_end = 2160 + 4 + 2,
	.vtotal = 2160 + 4 + 2 + 33,
	.width_mm = 68,
	.height_mm = 136,
	.type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED,
};

static const struct nt36672_panel_desc planet_cosmo_desc = {
	.mode = &planet_cosmo_mode,
	/* burst video mode; the clock lane drops to LP every line (clk_lp_per_line_enable) */
	.mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_BURST |
		      MIPI_DSI_CLOCK_NON_CONTINUOUS | MIPI_DSI_MODE_LPM,
	.format = MIPI_DSI_FMT_RGB888,
	.lanes = 4,
	.init_cmds = planet_cosmo_init_cmds,
	.off_cmds = planet_cosmo_off_cmds,
};

static int nt36672_probe(struct mipi_dsi_device *dsi)
{
	struct device *dev = &dsi->dev;
	struct nt36672_panel *ctx;
	int i, ret;

	ctx = devm_drm_panel_alloc(dev, struct nt36672_panel, panel, &nt36672_panel_funcs,
				   DRM_MODE_CONNECTOR_DSI);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	ctx->desc = of_device_get_match_data(dev);
	ctx->dsi = dsi;
	mipi_dsi_set_drvdata(dsi, ctx);

	for (i = 0; i < ARRAY_SIZE(ctx->supplies); i++)
		ctx->supplies[i].supply = nt36672_supply_names[i];
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(ctx->supplies), ctx->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get regulators\n");

	/* not asserted: the bootloader leaves the panel running, and prepare resets it anyway */
	ctx->reset_gpio = devm_gpiod_get(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(ctx->reset_gpio))
		return dev_err_probe(dev, PTR_ERR(ctx->reset_gpio), "failed to get reset gpio\n");

	dsi->lanes = ctx->desc->lanes;
	dsi->format = ctx->desc->format;
	dsi->mode_flags = ctx->desc->mode_flags;

	ret = drm_panel_of_backlight(&ctx->panel);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get backlight\n");

	/* the init sequence is sent from prepare, so the DSI host has to be up first */
	ctx->panel.prepare_prev_first = true;

	drm_panel_add(&ctx->panel);

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		drm_panel_remove(&ctx->panel);
		return dev_err_probe(dev, ret, "failed to attach to DSI host\n");
	}

	return 0;
}

static void nt36672_remove(struct mipi_dsi_device *dsi)
{
	struct nt36672_panel *ctx = mipi_dsi_get_drvdata(dsi);
	int ret;

	ret = mipi_dsi_detach(dsi);
	if (ret)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	drm_panel_remove(&ctx->panel);
}

static const struct of_device_id nt36672_of_match[] = {
	{ .compatible = "planet,cosmocom-nt36672", .data = &planet_cosmo_desc },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, nt36672_of_match);

static struct mipi_dsi_driver nt36672_driver = {
	.driver = {
		.name = "panel-novatek-nt36672",
		.of_match_table = nt36672_of_match,
	},
	.probe = nt36672_probe,
	.remove = nt36672_remove,
};
module_mipi_dsi_driver(nt36672_driver);

MODULE_AUTHOR("Fredrik Lindlöf <fredrik.lindlof@gmail.com>");
MODULE_DESCRIPTION("Novatek NT36672 DSI panel driver");
MODULE_LICENSE("GPL");
