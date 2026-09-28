// SPDX-License-Identifier: GPL-2.0-only
/*
 * MediaTek MT6358 fuel gauge
 *
 * Reads the PMIC coulomb counter (left running by the boot loader), the
 * instantaneous current and the battery voltage, and estimates the state of
 * charge: an OCV table lookup at probe, then coulomb counting, re-anchored at
 * 100 % when the charger reports full. Register handshakes and scaling follow
 * the vendor driver (mt6358_gauge.c); the capacity algorithm of the vendor's
 * closed fuel gauge daemon is not reproduced.
 */

#include <linux/delay.h>
#include <linux/devm-helpers.h>
#include <linux/iio/consumer.h>
#include <linux/math64.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/reboot.h>
#include <linux/regmap.h>
#include <linux/thermal.h>
#include <linux/workqueue.h>

#include <kunit/visibility.h>

#include "mt6358-battery.h"

#define MT6358_FGADC_CON1		0x0d0a
#define MT6358_FG_LATCH_CMD		GENMASK(3, 0)
#define MT6358_FG_LATCH_CAR_MASK	0x1f05
#define MT6358_FG_LATCHDATA_ST		BIT(15)
#define MT6358_FGADC_CAR_CON0		0x0d14
#define MT6358_FGADC_CAR_CON1		0x0d16
#define MT6358_FGADC_CUR_CON0		0x0d8a
#define MT6358_SYSTEM_INFO_CON0		0x0d9a
#define MT6358_SYSTEM_INFO_BAT_PLUG	BIT(3)

/* Units per LSB times 1000, and the board CAR_TUNE of 0.93 (vendor battery node) */
#define MT6358_FG_CURRENT_UNIT		381470
#define MT6358_FG_CAR_UNIT		108507
#define MT6358_FG_CAR_TUNE		93

#define MT6358_BAT_POLL			(10 * HZ)
#define MT6358_BAT_LOW_SAMPLES		10
#define MT6358_BAT_LOW_UV		3400000
#define MT6358_BAT_LOW_COLD_UV		3200000
#define MT6358_BAT_COLD_DECI_C		50

struct mt6358_battery {
	struct device *dev;
	struct regmap *regmap;
	struct iio_channel *vbat;
	struct power_supply *psy;
	struct power_supply_battery_info *info;
	struct delayed_work work;
	/* Serialises the latch handshakes and the state of charge bookkeeping */
	struct mutex lock;

	int r_uohm;
	int soc0;		/* permille at the anchor */
	s64 car0;		/* counter at the anchor, uAh */
	int soc;		/* permille */
	unsigned int low_count;
};

VISIBLE_IF_KUNIT int mt6358_bat_current_ua(u16 raw)
{
	s64 ua;

	/* The vendor treats 0x8000..0xffff as 65535 - raw of discharge */
	if (raw > 32767)
		ua = -(s64)(65535 - raw);
	else
		ua = raw;

	ua *= (s64)MT6358_FG_CURRENT_UNIT * MT6358_FG_CAR_TUNE;
	return div_s64(ua, 100000);
}
EXPORT_SYMBOL_IF_KUNIT(mt6358_bat_current_ua);

VISIBLE_IF_KUNIT s64 mt6358_bat_car_uah(u16 car_lo, u16 car_hi)
{
	u32 v = (car_lo >> 11) | ((u32)(car_hi & 0x7fff) << 5);
	s64 mag;

	if (v == 0 || v == 0xfffff)
		return 0;

	/* Discharge is stored as a count down from 0xfffff (vendor fgauge_get_coulomb) */
	mag = (car_hi & BIT(15)) ? 0xfffff - v : v;
	mag = div_s64(mag * MT6358_FG_CAR_UNIT * MT6358_FG_CAR_TUNE, 100000);

	return (car_hi & BIT(15)) ? -mag : mag;
}
EXPORT_SYMBOL_IF_KUNIT(mt6358_bat_car_uah);

VISIBLE_IF_KUNIT int mt6358_bat_soc_permille(int soc0_permille, s64 car0_uah, s64 car_uah,
					     int full_uah)
{
	s64 soc = soc0_permille + div_s64((car_uah - car0_uah) * 1000, full_uah);

	return clamp_t(s64, soc, 0, 1000);
}
EXPORT_SYMBOL_IF_KUNIT(mt6358_bat_soc_permille);

VISIBLE_IF_KUNIT bool mt6358_bat_temp_inhibit(bool inhibited, int deci_c)
{
	return inhibited;
}
EXPORT_SYMBOL_IF_KUNIT(mt6358_bat_temp_inhibit);

/* Vendor latch handshake: request, wait for LATCHDATA_ST, read, clear, wait, restore */
static int mt6358_bat_latch(struct mt6358_battery *bat, unsigned int mask, unsigned int reg0,
			    unsigned int *val0, unsigned int reg1, unsigned int *val1)
{
	unsigned int st;
	int ret;

	ret = regmap_update_bits(bat->regmap, MT6358_FGADC_CON1, mask, 1);
	if (ret)
		return ret;
	ret = regmap_read_poll_timeout(bat->regmap, MT6358_FGADC_CON1, st,
				       st & MT6358_FG_LATCHDATA_ST, 0, 10000);
	if (ret)
		goto restore;

	ret = regmap_read(bat->regmap, reg0, val0);
	if (!ret && val1)
		ret = regmap_read(bat->regmap, reg1, val1);

	regmap_update_bits(bat->regmap, MT6358_FGADC_CON1, MT6358_FG_LATCH_CMD, 8);
	regmap_read_poll_timeout(bat->regmap, MT6358_FGADC_CON1, st,
				 !(st & MT6358_FG_LATCHDATA_ST), 0, 10000);
restore:
	regmap_update_bits(bat->regmap, MT6358_FGADC_CON1, MT6358_FG_LATCH_CMD, 0);
	return ret;
}

static int mt6358_bat_read_current(struct mt6358_battery *bat, int *ua)
{
	unsigned int raw;
	int ret;

	ret = mt6358_bat_latch(bat, MT6358_FG_LATCH_CMD, MT6358_FGADC_CUR_CON0, &raw, 0, NULL);
	if (!ret)
		*ua = mt6358_bat_current_ua(raw);
	return ret;
}

static int mt6358_bat_read_car(struct mt6358_battery *bat, s64 *uah)
{
	unsigned int lo, hi;
	int ret;

	ret = mt6358_bat_latch(bat, MT6358_FG_LATCH_CAR_MASK, MT6358_FGADC_CAR_CON0, &lo,
			       MT6358_FGADC_CAR_CON1, &hi);
	if (!ret)
		*uah = mt6358_bat_car_uah(lo, hi);
	return ret;
}

static int mt6358_bat_read_voltage(struct mt6358_battery *bat, int *uv)
{
	int mv, ret;

	ret = iio_read_channel_processed(bat->vbat, &mv);
	if (!ret)
		*uv = mv * 1000;
	return ret;
}

static int mt6358_bat_read_temp(int *deci_c)
{
	struct thermal_zone_device *tz;
	int mc, ret;

	tz = thermal_zone_get_zone_by_name("battery-thermal");
	if (IS_ERR(tz))
		return PTR_ERR(tz);

	ret = thermal_zone_get_temp(tz, &mc);
	if (!ret)
		*deci_c = mc / 100;
	return ret;
}

static int mt6358_bat_charger_status(void)
{
	union power_supply_propval val = { .intval = POWER_SUPPLY_STATUS_UNKNOWN };
	struct power_supply *chg = power_supply_get_by_name("mt6370-charger");

	if (!chg)
		return POWER_SUPPLY_STATUS_UNKNOWN;
	power_supply_get_property(chg, POWER_SUPPLY_PROP_STATUS, &val);
	power_supply_put(chg);
	return val.intval;
}

/* OCV estimate: the terminal voltage corrected by the current through the cell resistance */
static int mt6358_bat_ocv_soc(struct mt6358_battery *bat, int *ocv_uv)
{
	int uv, ua, temp = 250, ret;

	ret = mt6358_bat_read_voltage(bat, &uv);
	if (ret)
		return ret;
	ret = mt6358_bat_read_current(bat, &ua);
	if (ret)
		return ret;
	mt6358_bat_read_temp(&temp);

	*ocv_uv = uv - (int)div_s64((s64)ua * bat->r_uohm, 1000000);
	return power_supply_batinfo_ocv2cap(bat->info, *ocv_uv, temp / 10) * 10;
}

static void mt6358_bat_low_voltage_check(struct mt6358_battery *bat, int uv, int ua)
{
	int temp = 250, floor;

	mt6358_bat_read_temp(&temp);
	floor = temp < MT6358_BAT_COLD_DECI_C ? MT6358_BAT_LOW_COLD_UV : MT6358_BAT_LOW_UV;

	if (ua < 0 && uv < floor)
		bat->low_count++;
	else
		bat->low_count = 0;

	if (bat->low_count == MT6358_BAT_LOW_SAMPLES) {
		dev_crit(bat->dev, "battery at %d uV for %u samples, powering off\n", uv,
			 bat->low_count);
		orderly_poweroff(true);
	}
}

static void mt6358_bat_update(struct mt6358_battery *bat)
{
	int old, uv, ua;
	s64 car;

	mutex_lock(&bat->lock);
	old = bat->soc;

	if (!mt6358_bat_read_car(bat, &car)) {
		if (mt6358_bat_charger_status() == POWER_SUPPLY_STATUS_FULL) {
			bat->soc0 = 1000;
			bat->car0 = car;
		}
		bat->soc = mt6358_bat_soc_permille(bat->soc0, bat->car0, car,
						   bat->info->charge_full_design_uah);
	}

	if (!mt6358_bat_read_voltage(bat, &uv) && !mt6358_bat_read_current(bat, &ua))
		mt6358_bat_low_voltage_check(bat, uv, ua);
	mutex_unlock(&bat->lock);

	if (bat->soc / 10 != old / 10)
		power_supply_changed(bat->psy);
}

static void mt6358_bat_work(struct work_struct *work)
{
	struct mt6358_battery *bat = container_of(work, struct mt6358_battery, work.work);

	mt6358_bat_update(bat);
	schedule_delayed_work(&bat->work, MT6358_BAT_POLL);
}

static const enum power_supply_property mt6358_bat_props[] = {
	POWER_SUPPLY_PROP_STATUS,
	POWER_SUPPLY_PROP_PRESENT,
	POWER_SUPPLY_PROP_TECHNOLOGY,
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
	POWER_SUPPLY_PROP_CAPACITY,
	POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN,
	POWER_SUPPLY_PROP_CHARGE_NOW,
	POWER_SUPPLY_PROP_TEMP,
	POWER_SUPPLY_PROP_SCOPE,
};

static int mt6358_bat_get_property(struct power_supply *psy, enum power_supply_property psp,
				   union power_supply_propval *val)
{
	struct mt6358_battery *bat = power_supply_get_drvdata(psy);
	unsigned int info;
	int ret = 0;

	switch (psp) {
	case POWER_SUPPLY_PROP_STATUS:
		val->intval = mt6358_bat_charger_status();
		if (val->intval == POWER_SUPPLY_STATUS_UNKNOWN)
			val->intval = POWER_SUPPLY_STATUS_DISCHARGING;
		break;
	case POWER_SUPPLY_PROP_PRESENT:
		ret = regmap_read(bat->regmap, MT6358_SYSTEM_INFO_CON0, &info);
		val->intval = !!(info & MT6358_SYSTEM_INFO_BAT_PLUG);
		break;
	case POWER_SUPPLY_PROP_TECHNOLOGY:
		val->intval = POWER_SUPPLY_TECHNOLOGY_LIPO;
		break;
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		ret = mt6358_bat_read_voltage(bat, &val->intval);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		mutex_lock(&bat->lock);
		ret = mt6358_bat_read_current(bat, &val->intval);
		mutex_unlock(&bat->lock);
		break;
	case POWER_SUPPLY_PROP_CAPACITY:
		val->intval = DIV_ROUND_CLOSEST(bat->soc, 10);
		break;
	case POWER_SUPPLY_PROP_CHARGE_FULL_DESIGN:
		val->intval = bat->info->charge_full_design_uah;
		break;
	case POWER_SUPPLY_PROP_CHARGE_NOW:
		val->intval = div_s64((s64)bat->soc * bat->info->charge_full_design_uah, 1000);
		break;
	case POWER_SUPPLY_PROP_TEMP:
		ret = mt6358_bat_read_temp(&val->intval);
		break;
	case POWER_SUPPLY_PROP_SCOPE:
		val->intval = POWER_SUPPLY_SCOPE_SYSTEM;
		break;
	default:
		return -EINVAL;
	}

	return ret;
}

static const struct power_supply_desc mt6358_bat_desc = {
	.name = "battery",
	.type = POWER_SUPPLY_TYPE_BATTERY,
	.properties = mt6358_bat_props,
	.num_properties = ARRAY_SIZE(mt6358_bat_props),
	.get_property = mt6358_bat_get_property,
};

static int mt6358_bat_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6397_chip *mt6397 = dev_get_drvdata(dev->parent);
	struct power_supply_config cfg = { .fwnode = dev_fwnode(dev) };
	struct mt6358_battery *bat;
	int ocv, soc, ret;

	bat = devm_kzalloc(dev, sizeof(*bat), GFP_KERNEL);
	if (!bat)
		return -ENOMEM;

	bat->dev = dev;
	bat->regmap = mt6397->regmap;
	ret = devm_mutex_init(dev, &bat->lock);
	if (ret)
		return ret;

	bat->vbat = devm_iio_channel_get(dev, "vbat");
	if (IS_ERR(bat->vbat))
		return dev_err_probe(dev, PTR_ERR(bat->vbat), "no VBAT channel\n");

	cfg.drv_data = bat;
	bat->psy = devm_power_supply_register(dev, &mt6358_bat_desc, &cfg);
	if (IS_ERR(bat->psy))
		return dev_err_probe(dev, PTR_ERR(bat->psy), "failed to register\n");

	ret = power_supply_get_battery_info(bat->psy, &bat->info);
	if (ret)
		return dev_err_probe(dev, ret, "no monitored-battery\n");
	if (bat->info->charge_full_design_uah <= 0 || !bat->info->ocv_table_size[0])
		return dev_err_probe(dev, -EINVAL, "battery needs a capacity and an OCV table\n");
	bat->r_uohm = bat->info->factory_internal_resistance_uohm > 0 ?
		      bat->info->factory_internal_resistance_uohm : 0;

	soc = mt6358_bat_ocv_soc(bat, &ocv);
	if (soc < 0)
		return dev_err_probe(dev, soc, "failed to estimate the charge\n");
	ret = mt6358_bat_read_car(bat, &bat->car0);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read the coulomb counter\n");
	bat->soc0 = soc;
	bat->soc = soc;
	dev_info(dev, "OCV estimate %d uV, %d.%d %%\n", ocv, soc / 10, soc % 10);

	ret = devm_delayed_work_autocancel(dev, &bat->work, mt6358_bat_work);
	if (ret)
		return ret;
	schedule_delayed_work(&bat->work, MT6358_BAT_POLL);

	return 0;
}

static const struct of_device_id mt6358_bat_of_match[] = {
	{ .compatible = "mediatek,mt6358-battery" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6358_bat_of_match);

static struct platform_driver mt6358_bat_driver = {
	.driver = {
		.name = "mt6358-battery",
		.of_match_table = mt6358_bat_of_match,
	},
	.probe = mt6358_bat_probe,
};
module_platform_driver(mt6358_bat_driver);

MODULE_DESCRIPTION("MediaTek MT6358 fuel gauge");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS("IIO_CONSUMER");
