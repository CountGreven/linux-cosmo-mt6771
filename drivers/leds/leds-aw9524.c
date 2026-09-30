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
#include <linux/cleanup.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/pm.h>
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

struct aw9524 {
	struct regmap *regmap;
	struct gpio_desc *reset;
	/* Serialises the dimming registers against reset in suspend */
	struct mutex lock;
	u16 led_pins;
	u8 isel;
	u8 dim[AW9524_NUM_PINS];
	bool in_reset;
};

struct aw9524_led {
	struct led_classdev cdev;
	struct aw9524 *chip;
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

VISIBLE_IF_KUNIT bool aw9524_any_lit(const u8 *dim)
{
	int i;

	for (i = 0; i < AW9524_NUM_PINS; i++)
		if (dim[i])
			return true;

	return false;
}
EXPORT_SYMBOL_IF_KUNIT(aw9524_any_lit);

static int aw9524_brightness_set(struct led_classdev *cdev, enum led_brightness brightness)
{
	struct aw9524_led *led = container_of(cdev, struct aw9524_led, cdev);
	struct aw9524 *chip = led->chip;

	guard(mutex)(&chip->lock);

	chip->dim[led->pin] = brightness;
	if (chip->in_reset)
		return 0;

	return regmap_write(chip->regmap, aw9524_dim_reg(led->pin), brightness);
}

static const struct regmap_config aw9524_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AW9524_REG_RESET,
};

/* LED mode (0) and output direction (0) for the described pins only */
static int aw9524_setup(struct aw9524 *chip)
{
	struct regmap *regmap = chip->regmap;
	unsigned int id;
	int ret, pin;

	ret = regmap_read(regmap, AW9524_REG_ID, &id);
	if (ret)
		return ret;
	if (id != AW9524_ID)
		return -ENODEV;

	ret = regmap_write(regmap, AW9524_REG_RESET, 0);
	if (ret)
		return ret;

	ret = regmap_update_bits(regmap, AW9524_REG_CTL, AW9524_CTL_ISEL, chip->isel);
	if (ret)
		return ret;

	for (pin = 0; pin < AW9524_NUM_PINS; pin++) {
		if (!(chip->led_pins & BIT(pin)))
			continue;
		ret = regmap_write(regmap, aw9524_dim_reg(pin), chip->dim[pin]);
		if (ret)
			return ret;
	}

	ret = regmap_update_bits(regmap, AW9524_REG_MODE_P0, chip->led_pins & 0xff, 0);
	if (!ret)
		ret = regmap_update_bits(regmap, AW9524_REG_MODE_P1, chip->led_pins >> 8, 0);
	if (!ret)
		ret = regmap_update_bits(regmap, AW9524_REG_CONFIG_P0, chip->led_pins & 0xff, 0);
	if (!ret)
		ret = regmap_update_bits(regmap, AW9524_REG_CONFIG_P1, chip->led_pins >> 8, 0);

	return ret;
}

static int aw9524_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct aw9524 *chip;
	unsigned int isel;
	u32 imax;
	int ret;

	chip = devm_kzalloc(dev, sizeof(*chip), GFP_KERNEL);
	if (!chip)
		return -ENOMEM;

	chip->regmap = devm_regmap_init_i2c(client, &aw9524_regmap_config);
	if (IS_ERR(chip->regmap))
		return PTR_ERR(chip->regmap);

	ret = devm_mutex_init(dev, &chip->lock);
	if (ret)
		return ret;

	/* Released from reset here; held in reset across a suspend with every LED dark */
	chip->reset = devm_gpiod_get_optional(dev, "reset", GPIOD_OUT_LOW);
	if (IS_ERR(chip->reset))
		return dev_err_probe(dev, PTR_ERR(chip->reset), "failed to get reset GPIO\n");
	if (chip->reset)
		usleep_range(1000, 2000);

	imax = aw9524_imax_ua[0];
	of_property_read_u32(dev->of_node, "awinic,led-max-microamp", &imax);
	for (isel = 0; isel < ARRAY_SIZE(aw9524_imax_ua); isel++)
		if (aw9524_imax_ua[isel] == imax)
			break;
	if (isel == ARRAY_SIZE(aw9524_imax_ua))
		return dev_err_probe(dev, -EINVAL, "unsupported current %u uA\n", imax);
	chip->isel = isel;

	for_each_available_child_of_node_scoped(dev->of_node, child) {
		u32 pin;

		ret = of_property_read_u32(child, "reg", &pin);
		if (ret || pin >= AW9524_NUM_PINS)
			return dev_err_probe(dev, -EINVAL, "%pOF: bad reg\n", child);

		chip->led_pins |= BIT(pin);
	}

	ret = aw9524_setup(chip);
	if (ret)
		return dev_err_probe(dev, ret, "failed to set up the chip\n");

	i2c_set_clientdata(client, chip);

	for_each_available_child_of_node_scoped(dev->of_node, child) {
		struct led_init_data init_data = { .fwnode = of_fwnode_handle(child) };
		struct aw9524_led *led;
		u32 pin;

		of_property_read_u32(child, "reg", &pin);

		led = devm_kzalloc(dev, sizeof(*led), GFP_KERNEL);
		if (!led)
			return -ENOMEM;

		led->chip = chip;
		led->pin = pin;
		led->cdev.max_brightness = LED_FULL;
		led->cdev.brightness_set_blocking = aw9524_brightness_set;

		ret = devm_led_classdev_register_ext(dev, &led->cdev, &init_data);
		if (ret)
			return dev_err_probe(dev, ret, "%pOF: failed to register\n", child);
	}

	return 0;
}

/*
 * The vendor driver leaves the chip running across suspend. With every LED dark, RSTN low
 * costs nothing and saves the chip's own supply current; a lit LED (caps lock) stays lit.
 */
static int aw9524_suspend(struct device *dev)
{
	struct aw9524 *chip = dev_get_drvdata(dev);

	guard(mutex)(&chip->lock);

	if (!chip->reset || aw9524_any_lit(chip->dim))
		return 0;

	gpiod_set_value_cansleep(chip->reset, 1);
	chip->in_reset = true;

	return 0;
}

static int aw9524_resume(struct device *dev)
{
	struct aw9524 *chip = dev_get_drvdata(dev);
	int ret;

	guard(mutex)(&chip->lock);

	if (!chip->in_reset)
		return 0;

	gpiod_set_value_cansleep(chip->reset, 0);
	usleep_range(1000, 2000);

	ret = aw9524_setup(chip);
	if (ret)
		dev_err(dev, "failed to restore the chip: %d\n", ret);
	chip->in_reset = false;

	return ret;
}

static DEFINE_SIMPLE_DEV_PM_OPS(aw9524_pm_ops, aw9524_suspend, aw9524_resume);

static const struct of_device_id aw9524_of_match[] = {
	{ .compatible = "awinic,aw9524" },
	{ }
};
MODULE_DEVICE_TABLE(of, aw9524_of_match);

static struct i2c_driver aw9524_driver = {
	.driver = {
		.name = "leds-aw9524",
		.of_match_table = aw9524_of_match,
		.pm = pm_sleep_ptr(&aw9524_pm_ops),
	},
	.probe = aw9524_probe,
};
module_i2c_driver(aw9524_driver);

MODULE_DESCRIPTION("Awinic AW9523B/AW9524 LED driver");
MODULE_LICENSE("GPL");
