// SPDX-License-Identifier: GPL-2.0-only
/*
 * Awinic AW9523B/AW9524 constant-current LED driver
 *
 * The I/O expander pins described as LED child nodes are switched to LED mode
 * and dimmed through their 8-bit current registers; the other pins are left
 * alone. Register use follows the vendor Planet Cosmo driver (aw9524_key.c).
 */

#include <linux/bits.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/i2c.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/regmap.h>

#include <kunit/visibility.h>

#include "leds-aw9524.h"

#define AW9524_REG_CONFIG_P0	0x04
#define AW9524_REG_CONFIG_P1	0x05
#define AW9524_REG_ID		0x10
#define AW9524_ID		0x23
#define AW9524_REG_CTL		0x11
#define AW9524_CTL_ISEL		GENMASK(1, 0)
#define AW9524_REG_MODE_P0	0x12
#define AW9524_REG_MODE_P1	0x13
#define AW9524_REG_RESET	0x7f

#define AW9524_NUM_PINS		16

/* ISEL selects the full-scale current: Imax, 3/4, 1/2 or 1/4 of 37 mA */
static const u32 aw9524_imax_ua[] = { 37000, 27750, 18500, 9250 };

struct aw9524_led {
	struct led_classdev cdev;
	struct regmap *regmap;
	unsigned int pin;
};

VISIBLE_IF_KUNIT int aw9524_dim_reg(unsigned int pin)
{
	if (pin < 8)
		return 0x24 + pin;
	if (pin < 12)
		return 0x20 + pin - 8;
	if (pin < AW9524_NUM_PINS)
		return 0x2c + pin - 12;
	return -EINVAL;
}
EXPORT_SYMBOL_IF_KUNIT(aw9524_dim_reg);

static int aw9524_brightness_set(struct led_classdev *cdev, enum led_brightness brightness)
{
	struct aw9524_led *led = container_of(cdev, struct aw9524_led, cdev);

	return regmap_write(led->regmap, aw9524_dim_reg(led->pin), brightness);
}

static const struct regmap_config aw9524_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AW9524_REG_RESET,
};

static int aw9524_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct gpio_desc *reset;
	struct regmap *regmap;
	unsigned int id, isel;
	u16 led_pins = 0;
	u32 imax;
	int ret;

	regmap = devm_regmap_init_i2c(client, &aw9524_regmap_config);
	if (IS_ERR(regmap))
		return PTR_ERR(regmap);

	/* Released from reset here and kept released */
	reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(reset))
		return dev_err_probe(dev, PTR_ERR(reset), "failed to get reset GPIO\n");
	if (reset)
		usleep_range(1000, 2000);

	ret = regmap_read(regmap, AW9524_REG_ID, &id);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read ID\n");
	if (id != AW9524_ID)
		return dev_err_probe(dev, -ENODEV, "unknown ID 0x%02x\n", id);

	ret = regmap_write(regmap, AW9524_REG_RESET, 0);
	if (ret)
		return ret;

	imax = aw9524_imax_ua[0];
	of_property_read_u32(dev->of_node, "awinic,led-max-microamp", &imax);
	for (isel = 0; isel < ARRAY_SIZE(aw9524_imax_ua); isel++)
		if (aw9524_imax_ua[isel] == imax)
			break;
	if (isel == ARRAY_SIZE(aw9524_imax_ua))
		return dev_err_probe(dev, -EINVAL, "unsupported current %u uA\n", imax);

	ret = regmap_update_bits(regmap, AW9524_REG_CTL, AW9524_CTL_ISEL, isel);
	if (ret)
		return ret;

	for_each_available_child_of_node_scoped(dev->of_node, child) {
		struct led_init_data init_data = { .fwnode = of_fwnode_handle(child) };
		struct aw9524_led *led;
		u32 pin;

		ret = of_property_read_u32(child, "reg", &pin);
		if (ret || pin >= AW9524_NUM_PINS)
			return dev_err_probe(dev, -EINVAL, "%pOF: bad reg\n", child);

		led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
		if (!led)
			return -ENOMEM;

		led->regmap = regmap;
		led->pin = pin;
		led->cdev.max_brightness = LED_FULL;
		led->cdev.brightness_set_blocking = aw9524_brightness_set;

		ret = regmap_write(regmap, aw9524_dim_reg(pin), 0);
		if (ret)
			return ret;

		ret = devm_led_classdev_register_ext(dev, &led->cdev, &init_data);
		if (ret)
			return dev_err_probe(dev, ret, "%pOF: failed to register\n", child);

		led_pins |= BIT(pin);
	}

	/* LED mode (0) and output direction (0) for the described pins only */
	ret = regmap_update_bits(regmap, AW9524_REG_MODE_P0, led_pins & 0xff, 0);
	if (!ret)
		ret = regmap_update_bits(regmap, AW9524_REG_MODE_P1, led_pins >> 8, 0);
	if (!ret)
		ret = regmap_update_bits(regmap, AW9524_REG_CONFIG_P0, led_pins & 0xff, 0);
	if (!ret)
		ret = regmap_update_bits(regmap, AW9524_REG_CONFIG_P1, led_pins >> 8, 0);

	return ret;
}

static const struct of_device_id aw9524_of_match[] = {
	{ .compatible = "awinic,aw9524" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw9524_of_match);

static struct i2c_driver aw9524_driver = {
	.driver = {
		.name = "leds-aw9524",
		.of_match_table = aw9524_of_match,
	},
	.probe = aw9524_probe,
};
module_i2c_driver(aw9524_driver);

MODULE_DESCRIPTION("Awinic AW9523B/AW9524 LED driver");
MODULE_LICENSE("GPL");
