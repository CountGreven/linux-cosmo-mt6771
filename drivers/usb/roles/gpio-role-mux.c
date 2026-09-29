// SPDX-License-Identifier: GPL-2.0-only
/*
 * USB role switch multiplexer: one dual-role controller, two connectors, and a GPIO that routes
 * the USB 2.0 data lines to one of them. Each connector's port controller sees its own role switch;
 * the mux gives the controller to one of them at a time.
 */

#include <linux/cleanup.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/mod_devicetable.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/property.h>
#include <linux/sysfs.h>
#include <linux/usb/role.h>
#include <kunit/visibility.h>

#include "gpio-role-mux.h"

struct gpio_role_mux;

struct gpio_role_mux_input {
	struct gpio_role_mux *mux;
	struct usb_role_switch *sw;
	const char *label;
	enum usb_role req;
};

struct gpio_role_mux {
	struct device *dev;
	struct mutex lock;
	struct gpio_desc *select;
	struct usb_role_switch *out;
	enum usb_role out_role;
	int active;
	int forced;
	struct gpio_role_mux_input in[GPIO_ROLE_MUX_INPUTS];
};

/*
 * A host request takes the lines from a device request (a peripheral on one side while a charger
 * or PC is on the other); otherwise the side that has them keeps them, and on a tie input 0 wins.
 */
VISIBLE_IF_KUNIT int gpio_role_mux_select(const enum usb_role *req, int active, int forced)
{
	int i;

	if (forced != GPIO_ROLE_MUX_NONE)
		return forced;

	if (active != GPIO_ROLE_MUX_NONE && req[active] != USB_ROLE_NONE) {
		if (req[active] == USB_ROLE_DEVICE && req[!active] == USB_ROLE_HOST)
			return !active;
		return active;
	}

	for (i = 0; i < GPIO_ROLE_MUX_INPUTS; i++)
		if (req[i] == USB_ROLE_HOST)
			return i;
	for (i = 0; i < GPIO_ROLE_MUX_INPUTS; i++)
		if (req[i] == USB_ROLE_DEVICE)
			return i;

	return GPIO_ROLE_MUX_NONE;
}
EXPORT_SYMBOL_IF_KUNIT(gpio_role_mux_select);

VISIBLE_IF_KUNIT void gpio_role_mux_plan(const enum usb_role *req, int active, int forced,
					 enum usb_role out_role, struct gpio_role_mux_plan *p)
{
	p->sel = gpio_role_mux_select(req, active, forced);
	p->role = p->sel == GPIO_ROLE_MUX_NONE ? USB_ROLE_NONE : req[p->sel];
	/* the controller's role is dropped before the data lines move */
	p->drop = p->sel != active && out_role != USB_ROLE_NONE;
}
EXPORT_SYMBOL_IF_KUNIT(gpio_role_mux_plan);

static int gpio_role_mux_apply(struct gpio_role_mux *mux)
{
	enum usb_role req[GPIO_ROLE_MUX_INPUTS];
	struct gpio_role_mux_plan p;
	enum usb_role role;
	int i, sel, ret;

	for (i = 0; i < GPIO_ROLE_MUX_INPUTS; i++)
		req[i] = mux->in[i].req;

	gpio_role_mux_plan(req, mux->active, mux->forced, mux->out_role, &p);
	sel = p.sel;
	role = p.role;

	if (p.drop) {
		ret = usb_role_switch_set_role(mux->out, USB_ROLE_NONE);
		if (ret)
			return ret;
		mux->out_role = USB_ROLE_NONE;
	}

	if (sel != mux->active) {
		gpiod_set_value_cansleep(mux->select, sel == GPIO_ROLE_MUX_NONE ? 0 : sel);
		dev_dbg(mux->dev, "data lines to %s\n",
			sel == GPIO_ROLE_MUX_NONE ? "none" : mux->in[sel].label);
		mux->active = sel;
	}

	if (role != mux->out_role) {
		ret = usb_role_switch_set_role(mux->out, role);
		if (ret)
			return ret;
		mux->out_role = role;
	}

	return 0;
}

static int gpio_role_mux_set(struct usb_role_switch *sw, enum usb_role role)
{
	struct gpio_role_mux_input *in = usb_role_switch_get_drvdata(sw);
	struct gpio_role_mux *mux = in->mux;

	guard(mutex)(&mux->lock);
	in->req = role;

	return gpio_role_mux_apply(mux);
}

static enum usb_role gpio_role_mux_get(struct usb_role_switch *sw)
{
	struct gpio_role_mux_input *in = usb_role_switch_get_drvdata(sw);

	guard(mutex)(&in->mux->lock);

	return in->req;
}

static const char *gpio_role_mux_label(struct gpio_role_mux *mux, int index)
{
	return index == GPIO_ROLE_MUX_NONE ? "none" : mux->in[index].label;
}

static ssize_t active_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gpio_role_mux *mux = dev_get_drvdata(dev);

	guard(mutex)(&mux->lock);

	return sysfs_emit(buf, "%s\n", gpio_role_mux_label(mux, mux->active));
}
static DEVICE_ATTR_RO(active);

static ssize_t force_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct gpio_role_mux *mux = dev_get_drvdata(dev);

	guard(mutex)(&mux->lock);

	return sysfs_emit(buf, "%s\n", mux->forced == GPIO_ROLE_MUX_NONE ? "auto" :
			  mux->in[mux->forced].label);
}

static ssize_t force_store(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count)
{
	struct gpio_role_mux *mux = dev_get_drvdata(dev);
	int i, forced = GPIO_ROLE_MUX_NONE;
	int ret;

	if (!sysfs_streq(buf, "auto")) {
		for (i = 0; i < GPIO_ROLE_MUX_INPUTS; i++)
			if (sysfs_streq(buf, mux->in[i].label))
				break;
		if (i == GPIO_ROLE_MUX_INPUTS)
			return -EINVAL;
		forced = i;
	}

	guard(mutex)(&mux->lock);
	mux->forced = forced;
	ret = gpio_role_mux_apply(mux);

	return ret ?: count;
}
static DEVICE_ATTR_RW(force);

static struct attribute *gpio_role_mux_attrs[] = {
	&dev_attr_active.attr,
	&dev_attr_force.attr,
	NULL
};
ATTRIBUTE_GROUPS(gpio_role_mux);

static void gpio_role_mux_put_out(void *data)
{
	usb_role_switch_put(data);
}

static void gpio_role_mux_unregister(void *data)
{
	usb_role_switch_unregister(data);
}

static int gpio_role_mux_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct gpio_role_mux *mux;
	int ret;

	mux = devm_kzalloc(dev, sizeof(*mux), GFP_KERNEL);
	if (!mux)
		return -ENOMEM;

	mux->dev = dev;
	mux->active = GPIO_ROLE_MUX_NONE;
	mux->forced = GPIO_ROLE_MUX_NONE;
	ret = devm_mutex_init(dev, &mux->lock);
	if (ret)
		return ret;
	platform_set_drvdata(pdev, mux);

	mux->select = devm_gpiod_get(dev, "select", GPIOD_OUT_LOW);
	if (IS_ERR(mux->select))
		return dev_err_probe(dev, PTR_ERR(mux->select), "no select GPIO\n");

	mux->out = fwnode_usb_role_switch_get(dev_fwnode(dev));
	if (IS_ERR_OR_NULL(mux->out))
		return dev_err_probe(dev, mux->out ? PTR_ERR(mux->out) : -EPROBE_DEFER,
				     "no controller role switch\n");
	ret = devm_add_action_or_reset(dev, gpio_role_mux_put_out, mux->out);
	if (ret)
		return ret;
	mux->out_role = usb_role_switch_get_role(mux->out);

	device_for_each_child_node_scoped(dev, child) {
		struct usb_role_switch_desc desc = {};
		struct gpio_role_mux_input *in;
		u32 reg;

		if (!fwnode_property_present(child, "usb-role-switch"))
			continue;

		ret = fwnode_property_read_u32(child, "reg", &reg);
		if (ret || reg >= GPIO_ROLE_MUX_INPUTS)
			return dev_err_probe(dev, -EINVAL, "bad input reg\n");

		in = &mux->in[reg];
		if (in->sw)
			return dev_err_probe(dev, -EINVAL, "input %u twice\n", reg);

		in->mux = mux;
		in->req = USB_ROLE_NONE;
		if (fwnode_property_read_string(child, "label", &in->label))
			in->label = reg ? "1" : "0";

		desc.fwnode = child;
		desc.set = gpio_role_mux_set;
		desc.get = gpio_role_mux_get;
		desc.driver_data = in;
		desc.name = devm_kasprintf(dev, GFP_KERNEL, "%s-%s", dev_name(dev), in->label);
		if (!desc.name)
			return -ENOMEM;

		in->sw = usb_role_switch_register(dev, &desc);
		if (IS_ERR(in->sw))
			return dev_err_probe(dev, PTR_ERR(in->sw), "cannot register %s\n",
					     in->label);
		ret = devm_add_action_or_reset(dev, gpio_role_mux_unregister, in->sw);
		if (ret)
			return ret;
	}

	if (!mux->in[0].sw || !mux->in[1].sw)
		return dev_err_probe(dev, -EINVAL, "needs two inputs\n");

	return 0;
}

static const struct of_device_id gpio_role_mux_of_match[] = {
	{ .compatible = "gpio-usb-role-mux" },
	{}
};
MODULE_DEVICE_TABLE(of, gpio_role_mux_of_match);

static struct platform_driver gpio_role_mux_driver = {
	.probe = gpio_role_mux_probe,
	.driver = {
		.name = "gpio-usb-role-mux",
		.of_match_table = gpio_role_mux_of_match,
		.dev_groups = gpio_role_mux_groups,
	},
};
module_platform_driver(gpio_role_mux_driver);

MODULE_DESCRIPTION("GPIO USB role switch multiplexer");
MODULE_LICENSE("GPL");
