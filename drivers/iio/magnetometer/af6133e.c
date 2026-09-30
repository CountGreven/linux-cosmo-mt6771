// SPDX-License-Identifier: GPL-2.0-only
/*
 * Voltafield AF6133E 3-axis magnetometer
 *
 * The register settings, the self-test (BIST) calibration and the output
 * compensation follow the vendor MediaTek driver (af6133e.c, version 3.0.0).
 */

#include <linux/cleanup.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/math.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/regmap.h>
#include <linux/regulator/consumer.h>

#include <linux/iio/iio.h>

#include <kunit/visibility.h>

#include "af6133e.h"

#define AF6133E_REG_PCODE	0x00
#define AF6133E_PCODE		0x68
#define AF6133E_REG_DATA	0x03
#define AF6133E_REG_MEASURE	0x0a
#define AF6133E_REG_AVG		0x13
#define AF6133E_REG_SR_MODE	0x14
#define AF6133E_REG_AVG_2ND	0x15
#define AF6133E_REG_OSR		0x16
#define AF6133E_REG_OSC_FREQ	0x19
#define AF6133E_REG_XY_WAITING	0x32
#define AF6133E_REG_CHOPPER	0x35
#define AF6133E_REG_TEST2	0x37

#define AF6133E_CHOPPER_BIST	0xf1
#define AF6133E_CHOPPER_NORMAL	0xc1

#define AF6133E_BIST_SAMPLES	5
#define AF6133E_BIST_RETRIES	5
#define AF6133E_BIST_LIMIT	16383

#define AF6133E_GAIN_MIN	60
#define AF6133E_GAIN_MAX	600
#define AF6133E_COMP_XY_MAX	9
#define AF6133E_COMP_Z_MAX	70

/* 0.15 uT per LSB */
#define AF6133E_SCALE_NANO_GAUSS	1500000

/* Per-axis BIST sensitivity coefficients, in 1/100 */
static const s32 af6133e_bist_k[3] = { 79, 82, 79 };

/* TEST2 values that drive the self-test coil of each axis */
static const u8 af6133e_bist_pos[3] = { 0x04, 0x02, 0x01 };
static const u8 af6133e_bist_neg[3] = { 0x0c, 0x0a, 0x09 };

static const char * const af6133e_supply_names[] = { "vdd", "vddio" };

struct af6133e_data {
	struct regmap *regmap;
	struct regulator_bulk_data supplies[ARRAY_SIZE(af6133e_supply_names)];
	/* Serialises a measurement trigger and its result read */
	struct mutex lock;
	struct iio_mount_matrix orientation;
	struct af6133e_cal cal;
};

VISIBLE_IF_KUNIT void af6133e_cal_default(struct af6133e_cal *cal)
{
	int i;

	for (i = 0; i < 3; i++)
		cal->gain[i] = 100;
	for (i = 0; i < 4; i++)
		cal->comp[i] = 0;
}
EXPORT_SYMBOL_IF_KUNIT(af6133e_cal_default);

VISIBLE_IF_KUNIT bool af6133e_cal_from_bist(struct af6133e_cal *cal, const s16 bist_x[3],
					    const s16 bist_y[3], const s16 bist_z[3])
{
	const s16 *bist[3] = { bist_x, bist_y, bist_z };
	const s32 *k = af6133e_bist_k;
	s32 gain[3], comp[4], sz;
	s64 xz, yz, zz;
	int i;

	af6133e_cal_default(cal);

	for (i = 0; i < 3; i++)
		if (bist[i][i] <= 0)
			return false;

	for (i = 0; i < 3; i++)
		gain[i] = (6666660 / k[i]) / bist[i][i];

	comp[0] = -(bist_y[0] * k[1] * 100) / (bist_x[0] * k[0]);
	comp[1] = -(bist_x[1] * k[0] * 100) / (bist_y[1] * k[1]);

	xz = (s64)bist_x[2] * k[0];
	yz = (s64)bist_y[2] * k[1];
	zz = div_s64((s64)bist_z[2] * k[2], 100);
	sz = int_sqrt64(div_s64(xz * xz + yz * yz, 10000) + zz * zz);
	if (!sz)
		return false;

	comp[2] = -div_s64(xz, sz);
	comp[3] = -div_s64(yz, sz);

	for (i = 0; i < 3; i++)
		if (gain[i] < AF6133E_GAIN_MIN || gain[i] > AF6133E_GAIN_MAX)
			return false;
	if (abs(comp[0] + comp[1]) > AF6133E_COMP_XY_MAX ||
	    abs(comp[2]) > AF6133E_COMP_Z_MAX || abs(comp[3]) > AF6133E_COMP_Z_MAX)
		return false;

	for (i = 0; i < 3; i++)
		cal->gain[i] = gain[i];
	for (i = 0; i < 4; i++)
		cal->comp[i] = comp[i];

	return true;
}
EXPORT_SYMBOL_IF_KUNIT(af6133e_cal_from_bist);

VISIBLE_IF_KUNIT void af6133e_compensate(const struct af6133e_cal *cal, const s16 raw[3],
					 s32 out[3])
{
	s32 v[3];
	int i;

	for (i = 0; i < 3; i++)
		v[i] = raw[i] * cal->gain[i] / 100;

	out[0] = v[0] + v[1] * cal->comp[0] / 100;
	out[1] = v[1] + v[0] * cal->comp[1] / 100;
	out[2] = v[2] + v[0] * cal->comp[2] / 100 + v[1] * cal->comp[3] / 100;
}
EXPORT_SYMBOL_IF_KUNIT(af6133e_compensate);

static int af6133e_measure(struct af6133e_data *data, s16 val[3])
{
	__le16 buf[3];
	int ret, i;

	ret = regmap_write(data->regmap, AF6133E_REG_MEASURE, 1);
	if (ret)
		return ret;

	usleep_range(8000, 9000);

	ret = regmap_bulk_read(data->regmap, AF6133E_REG_DATA, buf, sizeof(buf));
	if (ret)
		return ret;

	for (i = 0; i < 3; i++)
		val[i] = le16_to_cpu(buf[i]);

	return 0;
}

static int af6133e_bist_mean(struct af6133e_data *data, u8 test2, s32 mean[3])
{
	int n = 0, retries = AF6133E_BIST_RETRIES;
	s32 sum[3] = { };
	s16 v[3];
	int ret, i;

	ret = regmap_write(data->regmap, AF6133E_REG_TEST2, test2);
	if (ret)
		return ret;

	usleep_range(1000, 2000);

	while (n < AF6133E_BIST_SAMPLES && retries) {
		ret = af6133e_measure(data, v);
		if (ret)
			return ret;

		if (abs(v[0]) >= AF6133E_BIST_LIMIT || abs(v[1]) >= AF6133E_BIST_LIMIT ||
		    abs(v[2]) >= AF6133E_BIST_LIMIT) {
			retries--;
			continue;
		}

		for (i = 0; i < 3; i++)
			sum[i] += v[i];
		n++;
	}

	if (!n)
		return -EIO;

	for (i = 0; i < 3; i++)
		mean[i] = sum[i] / n;

	return 0;
}

static int af6133e_bist(struct af6133e_data *data, s16 bist[3][3])
{
	s32 pos[3], neg[3];
	int ret, restore, axis, i;

	ret = regmap_write(data->regmap, AF6133E_REG_CHOPPER, AF6133E_CHOPPER_BIST);
	if (ret)
		return ret;

	for (axis = 0; axis < 3; axis++) {
		ret = af6133e_bist_mean(data, af6133e_bist_pos[axis], pos);
		if (ret)
			break;
		ret = af6133e_bist_mean(data, af6133e_bist_neg[axis], neg);
		if (ret)
			break;
		for (i = 0; i < 3; i++)
			bist[axis][i] = neg[i] - pos[i];
	}

	restore = regmap_write(data->regmap, AF6133E_REG_CHOPPER, AF6133E_CHOPPER_NORMAL);
	if (!restore)
		restore = regmap_write(data->regmap, AF6133E_REG_TEST2, 0);

	return ret ?: restore;
}

static int af6133e_init(struct af6133e_data *data)
{
	static const struct reg_sequence golden[] = {
		{ AF6133E_REG_AVG, 0x47 },
		{ AF6133E_REG_SR_MODE, 0x0d },
		{ AF6133E_REG_OSR, 0x3d },
		{ AF6133E_REG_OSC_FREQ, 0x3f },
		{ AF6133E_REG_XY_WAITING, 0x07 },
		{ AF6133E_REG_AVG_2ND, 0x50 },
	};
	int ret;

	ret = regmap_multi_reg_write(data->regmap, golden, ARRAY_SIZE(golden));
	if (ret)
		return ret;

	usleep_range(5000, 6000);

	return 0;
}

static int af6133e_read_raw(struct iio_dev *indio_dev,
			    struct iio_chan_spec const *chan,
			    int *val, int *val2, long mask)
{
	struct af6133e_data *data = iio_priv(indio_dev);
	s16 raw[3];
	s32 out[3];
	int ret;

	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		mutex_lock(&data->lock);
		ret = af6133e_measure(data, raw);
		mutex_unlock(&data->lock);
		if (ret)
			return ret;

		af6133e_compensate(&data->cal, raw, out);
		*val = out[chan->address];
		return IIO_VAL_INT;
	case IIO_CHAN_INFO_SCALE:
		*val = 0;
		*val2 = AF6133E_SCALE_NANO_GAUSS;
		return IIO_VAL_INT_PLUS_NANO;
	default:
		return -EINVAL;
	}
}

static const struct iio_mount_matrix *
af6133e_get_mount_matrix(const struct iio_dev *indio_dev,
			 const struct iio_chan_spec *chan)
{
	struct af6133e_data *data = iio_priv(indio_dev);

	return &data->orientation;
}

static const struct iio_chan_spec_ext_info af6133e_ext_info[] = {
	IIO_MOUNT_MATRIX(IIO_SHARED_BY_DIR, af6133e_get_mount_matrix),
	{ }
};

#define AF6133E_CHANNEL(_axis, _addr) {				\
	.type = IIO_MAGN,					\
	.modified = 1,						\
	.channel2 = IIO_MOD_##_axis,				\
	.address = _addr,					\
	.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),		\
	.info_mask_shared_by_type = BIT(IIO_CHAN_INFO_SCALE),	\
	.ext_info = af6133e_ext_info,				\
}

static const struct iio_chan_spec af6133e_channels[] = {
	AF6133E_CHANNEL(X, 0),
	AF6133E_CHANNEL(Y, 1),
	AF6133E_CHANNEL(Z, 2),
};

static const struct iio_info af6133e_info = {
	.read_raw = af6133e_read_raw,
};

static const struct regmap_config af6133e_regmap_config = {
	.reg_bits = 8,
	.val_bits = 8,
	.max_register = AF6133E_REG_TEST2 + 1,
};

static void af6133e_disable_supplies(void *arg)
{
	struct af6133e_data *data = arg;

	regulator_bulk_disable(ARRAY_SIZE(data->supplies), data->supplies);
}

static int af6133e_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct af6133e_data *data;
	struct iio_dev *indio_dev;
	s16 bist[3][3] = { };
	unsigned int pcode;
	int ret, i;

	indio_dev = devm_iio_device_alloc(dev, sizeof(*data));
	if (!indio_dev)
		return -ENOMEM;

	data = iio_priv(indio_dev);
	ret = devm_mutex_init(dev, &data->lock);
	if (ret)
		return ret;

	data->regmap = devm_regmap_init_i2c(client, &af6133e_regmap_config);
	if (IS_ERR(data->regmap))
		return dev_err_probe(dev, PTR_ERR(data->regmap), "regmap init failed\n");

	ret = iio_read_mount_matrix(dev, &data->orientation);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read mount matrix\n");

	for (i = 0; i < ARRAY_SIZE(af6133e_supply_names); i++)
		data->supplies[i].supply = af6133e_supply_names[i];
	ret = devm_regulator_bulk_get(dev, ARRAY_SIZE(data->supplies), data->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to get supplies\n");

	ret = regulator_bulk_enable(ARRAY_SIZE(data->supplies), data->supplies);
	if (ret)
		return dev_err_probe(dev, ret, "failed to enable supplies\n");

	ret = devm_add_action_or_reset(dev, af6133e_disable_supplies, data);
	if (ret)
		return ret;

	i2c_set_clientdata(client, indio_dev);

	ret = regmap_read(data->regmap, AF6133E_REG_PCODE, &pcode);
	if (ret)
		return dev_err_probe(dev, ret, "failed to read product code\n");
	if (pcode != AF6133E_PCODE)
		return dev_err_probe(dev, -ENODEV, "unknown product code 0x%02x\n", pcode);

	ret = af6133e_init(data);
	if (ret)
		return dev_err_probe(dev, ret, "init failed\n");

	ret = af6133e_bist(data, bist);
	if (ret)
		dev_warn(dev, "self test failed: %d\n", ret);

	if (!af6133e_cal_from_bist(&data->cal, bist[0], bist[1], bist[2]))
		dev_warn(dev, "self test out of range, uncompensated output\n");

	dev_dbg(dev, "gain %d %d %d, comp %d %d %d %d\n",
		data->cal.gain[0], data->cal.gain[1], data->cal.gain[2],
		data->cal.comp[0], data->cal.comp[1], data->cal.comp[2], data->cal.comp[3]);

	indio_dev->name = "af6133e";
	indio_dev->info = &af6133e_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	indio_dev->channels = af6133e_channels;
	indio_dev->num_channels = ARRAY_SIZE(af6133e_channels);

	return devm_iio_device_register(dev, indio_dev);
}

/*
 * The vendor driver has no suspend hook: the chip measures once per trigger and idles between
 * reads, so there is no chip state to change. With switchable supplies they are dropped, and
 * the registers are written again on resume; the self-test calibration is kept.
 */
static int af6133e_suspend(struct device *dev)
{
	struct af6133e_data *data = iio_priv(dev_get_drvdata(dev));

	guard(mutex)(&data->lock);

	return regulator_bulk_disable(ARRAY_SIZE(data->supplies), data->supplies);
}

static int af6133e_resume(struct device *dev)
{
	struct af6133e_data *data = iio_priv(dev_get_drvdata(dev));
	unsigned int pcode;
	int ret;

	guard(mutex)(&data->lock);

	ret = regulator_bulk_enable(ARRAY_SIZE(data->supplies), data->supplies);
	if (ret)
		return ret;

	ret = regmap_read(data->regmap, AF6133E_REG_PCODE, &pcode);
	if (ret)
		return ret;
	if (pcode != AF6133E_PCODE)
		dev_warn(dev, "unexpected product code 0x%02x after resume\n", pcode);

	return af6133e_init(data);
}

static DEFINE_SIMPLE_DEV_PM_OPS(af6133e_pm_ops, af6133e_suspend, af6133e_resume);

static const struct of_device_id af6133e_of_match[] = {
	{ .compatible = "voltafield,af6133e" },
	{ }
};
MODULE_DEVICE_TABLE(of, af6133e_of_match);

static const struct i2c_device_id af6133e_id[] = {
	{ "af6133e" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, af6133e_id);

static struct i2c_driver af6133e_driver = {
	.driver = {
		.name = "af6133e",
		.of_match_table = af6133e_of_match,
		.pm = pm_sleep_ptr(&af6133e_pm_ops),
	},
	.probe = af6133e_probe,
	.id_table = af6133e_id,
};
module_i2c_driver(af6133e_driver);

MODULE_DESCRIPTION("Voltafield AF6133E magnetometer driver");
MODULE_LICENSE("GPL");
