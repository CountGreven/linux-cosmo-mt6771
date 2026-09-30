// SPDX-License-Identifier: GPL-2.0
/*
 * Fairchild FUSB301 Type-C port controller driver
 *
 * Copyright (C) 2023 Otto Pflüger
 *
 * Based on wusb3801.c, Copyright (C) 2022 Samuel Holland <samuel@sholland.org>
 */

#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>
#include <linux/usb/role.h>
#include <linux/usb/typec.h>
#include <linux/workqueue.h>
#include <kunit/visibility.h>

#include "fusb301.h"

#define FUSB301_REG_DEVICE_ID		0x01

#define FUSB301_REG_MODES		0x02
#define FUSB301_MODES_DRP_ACC		BIT(5)
#define FUSB301_MODES_DRP		BIT(4)
#define FUSB301_MODES_SINK_ACC		BIT(3)
#define FUSB301_MODES_SINK		BIT(2)
#define FUSB301_MODES_SOURCE_ACC	BIT(1)
#define FUSB301_MODES_SOURCE		BIT(0)

#define FUSB301_REG_CONTROL		0x03
#define FUSB301_CONTROL_HOST_CURRENT	GENMASK(2, 1)
#define FUSB301_HOST_CURRENT_3_A	(0x3 << 1)
#define FUSB301_HOST_CURRENT_1_5_A	(0x2 << 1)
#define FUSB301_HOST_CURRENT_DEFAULT	(0x1 << 1)
#define FUSB301_HOST_CURRENT_NONE	(0x0 << 1)
#define FUSB301_CONTROL_INT_MASK	BIT(0)
#define FUSB301_ENABLE_INTERRUPTS	0
#define FUSB301_DISABLE_INTERRUPTS	1

#define FUSB301_REG_MANUAL		0x04
#define FUSB301_MANUAL_UNATT_SINK	BIT(3)
#define FUSB301_MANUAL_UNATT_SOURCE	BIT(2)
#define FUSB301_MANUAL_DISABLED		BIT(1)
#define FUSB301_MANUAL_ERROR_RECOVERY	BIT(0)

#define FUSB301_REG_RESET		0x05
#define FUSB301_RESET_SW		BIT(0)

#define FUSB301_REG_MASK		0x10
#define FUSB301_MASK_ACC_CHANGE		BIT(3)
#define FUSB301_MASK_BC_LVL		BIT(2)
#define FUSB301_MASK_DETACH		BIT(1)
#define FUSB301_MASK_ATTACH		BIT(0)

#define FUSB301_REG_STATUS		0x11
#define FUSB301_STATUS_ORIENTATION	GENMASK(5, 4)
#define FUSB301_ORIENTATION_ERROR	(0x3 << 4)
#define FUSB301_ORIENTATION_CC2		(0x2 << 4)
#define FUSB301_ORIENTATION_CC1		(0x1 << 4)
#define FUSB301_ORIENTATION_NONE	(0x0 << 4)
#define FUSB301_STATUS_VBUSOK		BIT(3)
#define FUSB301_STATUS_BC_LVL		GENMASK(2, 1)
#define FUSB301_BC_LVL_3_A		(0x3 << 1)
#define FUSB301_BC_LVL_1_5_A		(0x2 << 1)
#define FUSB301_BC_LVL_DEFAULT		(0x1 << 1)
#define FUSB301_BC_LVL_NONE		(0x0 << 1)
#define FUSB301_STATUS_ATTACH		BIT(0)

#define FUSB301_REG_TYPE		0x12
#define FUSB301_TYPE_SINK		BIT(4)
#define FUSB301_TYPE_SOURCE		BIT(3)
#define FUSB301_TYPE_DEBUGACC		BIT(1)
#define FUSB301_TYPE_AUDIOACC		BIT(0)

#define FUSB301_REG_INTERRUPT		0x13
#define FUSB301_INTERRUPT_ACC_CHANGE	BIT(3)
#define FUSB301_INTERRUPT_BC_LVL	BIT(2)
#define FUSB301_INTERRUPT_DETACH	BIT(1)
#define FUSB301_INTERRUPT_ATTACH	BIT(0)

/* The vendor polls every 5 s; a source can face our VBUS until the next read */
#define FUSB301_POLL_MS			1000
/* Vendor wait between GPIO178 high and reading GPIO54 (usb_typec.c:329-334) */
#define FUSB301_HDMI_DETECT_MS		400

struct fusb301 {
	struct typec_capability	cap;
	struct device		*dev;
	struct typec_partner	*partner;
	struct typec_port	*port;
	struct regmap		*regmap;
	struct regulator	*vbus_supply;
	struct usb_role_switch	*role_sw;
	struct gpio_desc	*hdmi_gpio;
	struct gpio_desc	*notify_gpio;
	struct delayed_work	poll_work;
	struct mutex		lock;
	unsigned int		partner_type;
	enum typec_port_type	port_type;
	enum typec_pwr_opmode	pwr_opmode;
	bool			vbus_idle_on;
	bool			vbus_on;
	bool			hdmi;
};

static enum typec_role fusb301_get_default_role(struct fusb301 *fusb301)
{
	switch (fusb301->port_type) {
	case TYPEC_PORT_SRC:
		return TYPEC_SOURCE;
	case TYPEC_PORT_SNK:
		return TYPEC_SINK;
	case TYPEC_PORT_DRP:
	default:
		if (fusb301->cap.prefer_role == TYPEC_SOURCE)
			return TYPEC_SOURCE;
		return TYPEC_SINK;
	}
}

/* The vendor kernel writes plain DRP (0x10), without accessory detection */
VISIBLE_IF_KUNIT unsigned int fusb301_mode(enum typec_port_type type)
{
	switch (type) {
	case TYPEC_PORT_SRC:
		return FUSB301_MODES_SOURCE;
	case TYPEC_PORT_SNK:
		return FUSB301_MODES_SINK;
	case TYPEC_PORT_DRP:
	default:
		return FUSB301_MODES_DRP;
	}
}
EXPORT_SYMBOL_IF_KUNIT(fusb301_mode);

/* Source vs not from Type & 0x18 alone, as the vendor poll does (usb_typec.c:410-428) */
VISIBLE_IF_KUNIT enum fusb301_partner fusb301_partner_kind(unsigned int type)
{
	switch (type & (FUSB301_TYPE_SINK | FUSB301_TYPE_SOURCE)) {
	case FUSB301_TYPE_SOURCE:
		return FUSB301_PARTNER_SOURCE;
	case FUSB301_TYPE_SINK:
		return FUSB301_PARTNER_SINK;
	case 0:
		break;
	default:
		return FUSB301_PARTNER_NONE;
	}

	switch (type & (FUSB301_TYPE_DEBUGACC | FUSB301_TYPE_AUDIOACC)) {
	case FUSB301_TYPE_AUDIOACC:
		return FUSB301_PARTNER_AUDIO;
	case FUSB301_TYPE_DEBUGACC:
		return FUSB301_PARTNER_DEBUG;
	default:
		return FUSB301_PARTNER_NONE;
	}
}
EXPORT_SYMBOL_IF_KUNIT(fusb301_partner_kind);

/*
 * Only a sink partner makes us host, and not an HDMI adapter; the vendor never runs the gadget on
 * this port. vbus_idle_on keeps VBUS up unless a source is attached (vendor GPIO178 poll), and
 * source_notify is the vendor's GPIO52 to the STM32.
 */
VISIBLE_IF_KUNIT void fusb301_decode(unsigned int type, enum typec_role default_role,
				     bool vbus_idle_on, bool hdmi, struct fusb301_state *st)
{
	st->partner = fusb301_partner_kind(type);
	st->attached = st->partner != FUSB301_PARTNER_NONE;
	st->pwr_role = default_role;
	st->usb_role = USB_ROLE_NONE;
	st->accessory = TYPEC_ACCESSORY_NONE;

	switch (st->partner) {
	case FUSB301_PARTNER_SINK:
		st->pwr_role = TYPEC_SOURCE;
		if (!hdmi)
			st->usb_role = USB_ROLE_HOST;
		break;
	case FUSB301_PARTNER_SOURCE:
		st->pwr_role = TYPEC_SINK;
		break;
	case FUSB301_PARTNER_AUDIO:
		st->accessory = TYPEC_ACCESSORY_AUDIO;
		break;
	case FUSB301_PARTNER_DEBUG:
		st->accessory = TYPEC_ACCESSORY_DEBUG;
		break;
	case FUSB301_PARTNER_NONE:
		break;
	}

	st->source_notify = st->partner == FUSB301_PARTNER_SOURCE;
	st->vbus = st->partner == FUSB301_PARTNER_SINK ||
		   (vbus_idle_on && !st->source_notify);
}
EXPORT_SYMBOL_IF_KUNIT(fusb301_decode);

static int fusb301_map_pwr_opmode(enum typec_pwr_opmode mode)
{
	switch (mode) {
	case TYPEC_PWR_MODE_USB:
	default:
		return FUSB301_HOST_CURRENT_DEFAULT;
	case TYPEC_PWR_MODE_1_5A:
		return FUSB301_HOST_CURRENT_1_5_A;
	case TYPEC_PWR_MODE_3_0A:
		return FUSB301_HOST_CURRENT_3_A;
	}
}

static unsigned int fusb301_map_try_role(int role)
{
	switch (role) {
	case TYPEC_NO_PREFERRED_ROLE:
	default:
		return 0;
	case TYPEC_SINK:
		return FUSB301_MANUAL_UNATT_SINK;
	case TYPEC_SOURCE:
		return FUSB301_MANUAL_UNATT_SOURCE;
	}
}

static enum typec_orientation fusb301_unmap_orientation(unsigned int status)
{
	switch (status & FUSB301_STATUS_ORIENTATION) {
	case FUSB301_ORIENTATION_NONE:
	case FUSB301_ORIENTATION_ERROR:
	default:
		return TYPEC_ORIENTATION_NONE;
	case FUSB301_ORIENTATION_CC1:
		return TYPEC_ORIENTATION_NORMAL;
	case FUSB301_ORIENTATION_CC2:
		return TYPEC_ORIENTATION_REVERSE;
	}
}

static enum typec_pwr_opmode fusb301_unmap_pwr_opmode(unsigned int status)
{
	switch (status & FUSB301_STATUS_BC_LVL) {
	case FUSB301_BC_LVL_NONE:
	case FUSB301_BC_LVL_DEFAULT:
	default:
		return TYPEC_PWR_MODE_USB;
	case FUSB301_BC_LVL_1_5_A:
		return TYPEC_PWR_MODE_1_5A;
	case FUSB301_BC_LVL_3_A:
		return TYPEC_PWR_MODE_3_0A;
	}
}

static int fusb301_try_role(struct typec_port *port, int role)
{
	struct fusb301 *fusb301 = typec_get_drvdata(port);

	return regmap_write(fusb301->regmap, FUSB301_REG_MANUAL,
			    fusb301_map_try_role(role));
}

static int fusb301_port_type_set(struct typec_port *port,
				  enum typec_port_type type)
{
	struct fusb301 *fusb301 = typec_get_drvdata(port);
	int ret;

	ret = regmap_write(fusb301->regmap, FUSB301_REG_MODES,
			   fusb301_mode(type));
	if (ret)
		return ret;

	fusb301->port_type = type;

	return 0;
}

static const struct typec_operations fusb301_typec_ops = {
	.try_role	= fusb301_try_role,
	.port_type_set	= fusb301_port_type_set,
};

static int fusb301_hw_init(struct fusb301 *fusb301)
{
	int ret;
	ret = regmap_write(fusb301->regmap, FUSB301_REG_RESET,
			   FUSB301_RESET_SW);
	if (ret < 0)
		return ret;
	ret = regmap_write_bits(fusb301->regmap, FUSB301_REG_CONTROL,
				FUSB301_CONTROL_HOST_CURRENT |
				FUSB301_CONTROL_INT_MASK,
				fusb301_map_pwr_opmode(fusb301->pwr_opmode) |
				FUSB301_ENABLE_INTERRUPTS);
	if (ret < 0)
		return ret;
	ret = regmap_write(fusb301->regmap, FUSB301_REG_MODES,
			   fusb301_mode(fusb301->port_type));
	if (ret < 0)
		return ret;
	ret = regmap_write(fusb301->regmap, FUSB301_REG_MANUAL,
			   fusb301_map_try_role(fusb301->cap.prefer_role));
	if (ret < 0)
		return ret;
	return 0;
}

static void fusb301_set_vbus(struct fusb301 *fusb301, bool on)
{
	int ret;

	if (!fusb301->vbus_supply || on == fusb301->vbus_on)
		return;

	if (on) {
		ret = regulator_enable(fusb301->vbus_supply);
		if (ret) {
			dev_warn(fusb301->dev, "Failed to enable VBUS: %d\n", ret);
			return;
		}
	} else {
		regulator_disable(fusb301->vbus_supply);
	}
	fusb301->vbus_on = on;
}

/* Vendor order: GPIO52 up before VBUS goes, VBUS up before GPIO52 drops */
static void fusb301_apply_power(struct fusb301 *fusb301, const struct fusb301_state *st)
{
	if (st->source_notify)
		gpiod_set_value_cansleep(fusb301->notify_gpio, 1);
	fusb301_set_vbus(fusb301, st->vbus);
	if (!st->source_notify)
		gpiod_set_value_cansleep(fusb301->notify_gpio, 0);
}

static void fusb301_hw_update(struct fusb301 *fusb301)
{
	struct typec_port *port = fusb301->port;
	struct device *dev = fusb301->dev;
	unsigned int partner_type, status;
	enum typec_role default_role;
	struct fusb301_state st;
	bool hdmi, changed;
	int ret;

	guard(mutex)(&fusb301->lock);

	ret = regmap_read(fusb301->regmap, FUSB301_REG_STATUS, &status);
	if (ret) {
		dev_warn(dev, "Failed to read port status: %d\n", ret);
		status = 0;
	}
	dev_dbg(dev, "status = 0x%02x\n", status);

	ret = regmap_read(fusb301->regmap, FUSB301_REG_TYPE, &partner_type);
	if (ret) {
		dev_warn(dev, "Failed to read partner type: %d\n", ret);
		partner_type = 0;
	}
	dev_dbg(dev, "partner_type = 0x%02x\n", partner_type);

	partner_type &= FUSB301_TYPE_SINK |
			FUSB301_TYPE_SOURCE |
			FUSB301_TYPE_AUDIOACC |
			FUSB301_TYPE_DEBUGACC;
	changed = partner_type != fusb301->partner_type;
	default_role = fusb301_get_default_role(fusb301);

	hdmi = changed ? false : fusb301->hdmi;
	fusb301_decode(partner_type, default_role, fusb301->vbus_idle_on, hdmi, &st);
	fusb301_apply_power(fusb301, &st);

	/* An HDMI adapter needs VBUS before it can drive its detect line */
	if (changed && st.partner == FUSB301_PARTNER_SINK && fusb301->hdmi_gpio) {
		msleep(FUSB301_HDMI_DETECT_MS);
		ret = gpiod_get_value_cansleep(fusb301->hdmi_gpio);
		hdmi = ret > 0;
		if (hdmi)
			dev_info(dev, "HDMI adapter, no USB role\n");
		fusb301_decode(partner_type, default_role, fusb301->vbus_idle_on, hdmi, &st);
	}
	fusb301->hdmi = hdmi;

	if (changed) {
		struct typec_partner_desc desc = { .accessory = st.accessory };

		if (fusb301->partner) {
			typec_unregister_partner(fusb301->partner);
			fusb301->partner = NULL;
		}

		if (st.attached) {
			fusb301->partner = typec_register_partner(port, &desc);
			if (IS_ERR(fusb301->partner))
				dev_err(dev, "Failed to register partner: %ld\n",
					PTR_ERR(fusb301->partner));
		}

		typec_set_data_role(port, st.pwr_role == TYPEC_SOURCE ?
				    TYPEC_HOST : TYPEC_DEVICE);
		typec_set_pwr_role(port, st.pwr_role);
		typec_set_vconn_role(port, st.pwr_role);

		if (fusb301->role_sw) {
			ret = usb_role_switch_set_role(fusb301->role_sw, st.usb_role);
			if (ret)
				dev_warn(dev, "Failed to set USB role: %d\n", ret);
		}
	}

	typec_set_pwr_opmode(fusb301->port,
			     st.partner == FUSB301_PARTNER_SOURCE
				? fusb301_unmap_pwr_opmode(status)
				: fusb301->pwr_opmode);
	typec_set_orientation(fusb301->port,
			      fusb301_unmap_orientation(status));

	fusb301->partner_type = partner_type;
}

/* A source partner does not move the interrupt line on the Cosmo (vendor polls Type instead) */
static void fusb301_poll(struct work_struct *work)
{
	struct fusb301 *fusb301 = container_of(to_delayed_work(work),
					       struct fusb301, poll_work);

	fusb301_hw_update(fusb301);
	queue_delayed_work(system_freezable_wq, &fusb301->poll_work,
			   msecs_to_jiffies(FUSB301_POLL_MS));
}

static bool fusb301_needs_poll(struct fusb301 *fusb301)
{
	return fusb301->vbus_supply || fusb301->notify_gpio;
}

static irqreturn_t fusb301_irq(int irq, void *data)
{
	struct fusb301 *fusb301 = data;
	unsigned int interrupt;

	/*
	 * The interrupt register must be read in order to clear the IRQ,
	 * but all of the useful information is in the status register.
	 */
	regmap_read(fusb301->regmap, FUSB301_REG_INTERRUPT, &interrupt);
	dev_dbg(fusb301->dev, "IRQ (interrupt = 0x%02x)\n", interrupt);

	fusb301_hw_update(fusb301);

	return IRQ_HANDLED;
}

static const struct regmap_config config = {
	.reg_bits	= 8,
	.val_bits	= 8,
	.max_register	= FUSB301_REG_INTERRUPT,
};

static int fusb301_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct fwnode_handle *connector;
	struct fusb301 *fusb301;
	const char *cap_str;
	int ret;

	fusb301 = devm_kzalloc(dev, sizeof(*fusb301), GFP_KERNEL);
	if (!fusb301)
		return -ENOMEM;

	i2c_set_clientdata(client, fusb301);

	fusb301->dev = dev;

	fusb301->regmap = devm_regmap_init_i2c(client, &config);
	if (IS_ERR(fusb301->regmap))
		return PTR_ERR(fusb301->regmap);

	ret = devm_mutex_init(dev, &fusb301->lock);
	if (ret)
		return ret;
	INIT_DEFERRABLE_WORK(&fusb301->poll_work, fusb301_poll);

	fusb301->vbus_supply = devm_regulator_get_optional(dev, "vbus");
	if (IS_ERR(fusb301->vbus_supply)) {
		if (PTR_ERR(fusb301->vbus_supply) != -ENODEV)
			return PTR_ERR(fusb301->vbus_supply);
		fusb301->vbus_supply = NULL;
	}
	fusb301->vbus_idle_on = fusb301->vbus_supply &&
		device_property_read_bool(dev, "fcs,vbus-on-unless-source");

	fusb301->hdmi_gpio = devm_gpiod_get_optional(dev, "hdmi-detect", GPIOD_IN);
	if (IS_ERR(fusb301->hdmi_gpio))
		return dev_err_probe(dev, PTR_ERR(fusb301->hdmi_gpio),
				     "Failed to get HDMI detect GPIO\n");

	fusb301->notify_gpio = devm_gpiod_get_optional(dev, "source-notify", GPIOD_OUT_LOW);
	if (IS_ERR(fusb301->notify_gpio))
		return dev_err_probe(dev, PTR_ERR(fusb301->notify_gpio),
				     "Failed to get source notify GPIO\n");

	connector = device_get_named_child_node(dev, "connector");
	if (!connector) {
		dev_err(dev, "Failed to get connector node\n");
		return -ENODEV;
	}

	fusb301->role_sw = fwnode_usb_role_switch_get(connector);
	if (IS_ERR(fusb301->role_sw)) {
		ret = dev_err_probe(dev, PTR_ERR(fusb301->role_sw),
				    "Failed to get role switch\n");
		goto err_put_connector;
	}

	ret = typec_get_fw_cap(&fusb301->cap, connector);
	if (ret) {
		dev_err(dev, "Failed to read capabilities from node\n");
		goto err_put_connector;
	}
	fusb301->port_type = fusb301->cap.type;

	ret = fwnode_property_read_string(connector, "typec-power-opmode", &cap_str);
	if (ret) {
		dev_err(dev, "Failed to read typec-power-opmode property\n");
		goto err_put_connector;
	}

	ret = typec_find_pwr_opmode(cap_str);
	if (ret < 0 || ret == TYPEC_PWR_MODE_PD) {
		dev_err(dev, "Invalid typec-power-opmode specified\n");
		goto err_put_connector;
	}
	fusb301->pwr_opmode = ret;

	/* An idle-on rail (shared with other loads) stays up across the reset and the first read */
	if (fusb301->vbus_idle_on)
		fusb301_set_vbus(fusb301, true);

	/* Initialize the hardware with the devicetree settings. */
	ret = fusb301_hw_init(fusb301);
	if (ret) {
		dev_err(dev, "Failed to initialize hardware\n");
		goto err_put_connector;
	}

	fusb301->cap.revision		= USB_TYPEC_REV_1_2;
	fusb301->cap.accessory[0]	= TYPEC_ACCESSORY_AUDIO;
	fusb301->cap.accessory[1]	= TYPEC_ACCESSORY_DEBUG;
	fusb301->cap.orientation_aware	= true;
	fusb301->cap.driver_data	= fusb301;
	fusb301->cap.ops		= &fusb301_typec_ops;

	fusb301->port = typec_register_port(dev, &fusb301->cap);
	if (IS_ERR(fusb301->port)) {
		ret = PTR_ERR(fusb301->port);
		goto err_put_connector;
	}

	ret = request_threaded_irq(client->irq, NULL, fusb301_irq,
				   IRQF_ONESHOT, dev_name(dev), fusb301);
	if (ret)
		goto err_unregister_port;

	/* After the IRQ is requested, so that an edge in between is not lost */
	disable_irq(client->irq);
	fusb301_hw_update(fusb301);
	enable_irq(client->irq);

	if (fusb301_needs_poll(fusb301))
		queue_delayed_work(system_freezable_wq, &fusb301->poll_work,
				   msecs_to_jiffies(FUSB301_POLL_MS));

	fwnode_handle_put(connector);

	return 0;

err_unregister_port:
	typec_unregister_port(fusb301->port);
err_put_connector:
	usb_role_switch_put(fusb301->role_sw);
	fwnode_handle_put(connector);
	fusb301_set_vbus(fusb301, false);

	return ret;
}

static void fusb301_remove(struct i2c_client *client)
{
	struct fusb301 *fusb301 = i2c_get_clientdata(client);

	free_irq(client->irq, fusb301);
	cancel_delayed_work_sync(&fusb301->poll_work);

	if (fusb301->role_sw) {
		usb_role_switch_set_role(fusb301->role_sw, USB_ROLE_NONE);
		usb_role_switch_put(fusb301->role_sw);
	}

	if (fusb301->partner)
		typec_unregister_partner(fusb301->partner);
	typec_unregister_port(fusb301->port);

	if (fusb301->vbus_on)
		regulator_disable(fusb301->vbus_supply);
}

/*
 * The vendor leaves the chip untouched across suspend and the interrupt is no wake source; the
 * chip keeps detecting on its own. VBUS stays as it is. Only the poll stops, and one update on
 * resume picks up whatever happened meanwhile.
 */
static int fusb301_suspend(struct device *dev)
{
	struct fusb301 *fusb301 = dev_get_drvdata(dev);

	cancel_delayed_work_sync(&fusb301->poll_work);

	return 0;
}

static int fusb301_resume(struct device *dev)
{
	struct fusb301 *fusb301 = dev_get_drvdata(dev);

	if (fusb301_needs_poll(fusb301))
		queue_delayed_work(system_freezable_wq, &fusb301->poll_work, 0);
	else
		fusb301_hw_update(fusb301);

	return 0;
}

static DEFINE_SIMPLE_DEV_PM_OPS(fusb301_pm_ops, fusb301_suspend, fusb301_resume);

static const struct of_device_id fusb301_of_match[] = {
	{ .compatible = "fcs,fusb301" },
	{}
};
MODULE_DEVICE_TABLE(of, fusb301_of_match);

static struct i2c_driver fusb301_driver = {
	.probe = fusb301_probe,
	.remove		= fusb301_remove,
	.driver		= {
		.name		= "fusb301",
		.of_match_table	= fusb301_of_match,
		.pm		= pm_sleep_ptr(&fusb301_pm_ops),
	},
};

module_i2c_driver(fusb301_driver);

MODULE_AUTHOR("Otto Pflüger <otto.pflueger@abscue.de>");
MODULE_DESCRIPTION("Fairchild FUSB301 Type-C port controller driver");
MODULE_LICENSE("GPL");
