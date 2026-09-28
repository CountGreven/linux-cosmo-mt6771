// SPDX-License-Identifier: GPL-2.0
/*
 * MediaTek MT6358 accessory detection (headset jack and buttons)
 *
 * Software-controlled mode as the vendor MT6771 driver runs it: EINT0 reports
 * the plug, the ACCDET comparator state (AB) tells a 3-pole from a 4-pole plug
 * and a pressed button, and the AUXADC voltage tells the buttons apart.
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/iio/consumer.h>
#include <linux/input.h>
#include <linux/interrupt.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>
#include <linux/workqueue.h>
#include <sound/jack.h>
#include <sound/soc.h>

#include <kunit/visibility.h>

#include "mt6358-accdet.h"

#define MT6358_AUD_TOP_RST_CON0		0x2220
#define MT6358_AUDENC_ANA_CON6		0x2394
#define MT6358_AUDENC_ANA_CON10		0x239c
#define MT6358_AUDENC_ANA_CON11		0x239e
#define ACCDET_CON(n)			(0x2508 + 2 * (n))

#define RG_ACCDET_RST			BIT(1)
#define RG_FSTDSCHRG_EN			(BIT(5) | BIT(6))
#define RG_MICBIAS1_LOWP_EN		BIT(2)
#define RG_MICBIAS1_VOL			GENMASK(6, 4)
#define RG_ACCDET_MODE1			0x0807

/* CON01 */
#define ACCDET_EN			BIT(0)
#define ACCDET_SEQ_INIT			BIT(1)
#define ACCDET_EINT0_EN			BIT(2)
/* CON02 */
#define ACCDET_SWCTRL_EN		GENMASK(2, 0)
#define ACCDET_EINT0_PWM_EN		BIT(3)
#define ACCDET_PWM_IDLE			GENMASK(10, 8)
#define ACCDET_EINT0_PWM_IDLE		BIT(11)
/* CON12 */
#define ACCDET_IRQ			BIT(0)
#define ACCDET_EINT0_IRQ		BIT(2)
#define ACCDET_EINT_IRQS		GENMASK(3, 2)
#define ACCDET_IRQ_CLR			BIT(8)
#define ACCDET_EINT0_IRQ_CLR		BIT(10)
#define ACCDET_EINT_IRQ_CLRS		GENMASK(11, 10)
#define ACCDET_EINT0_IRQ_POL		BIT(14)
/* CON14 */
#define ACCDET_AB_SHIFT			6
#define ACCDET_AB_MASK			0x3
/* CON15 */
#define ACCDET_EINT0_DEB		GENMASK(6, 3)
#define ACCDET_EINT0_DEB_IN_256MS	(0xe << 3)
#define ACCDET_EINT0_DEB_OUT_120US	(0x1 << 3)
#define ACCDET_EINT0_PWM		GENMASK(12, 8)
#define ACCDET_EINT0_PWM_16MS		((0x6 << 8) | (0x2 << 12))
/* CON24 */
#define ACCDET_HWMODE_SEL		BIT(2)
#define ACCDET_FAST_DISCHARGE		BIT(4)

#define ACCDET_AB_00			0
#define ACCDET_AB_01			1
#define ACCDET_AB_11			3

#define ACCDET_IRQ_CLEAR_TIMEOUT_US	400
#define ACCDET_MICBIAS_OFF_DELAY	(6 * HZ)
#define ACCDET_JACK_MASK		(SND_JACK_HEADSET | SND_JACK_BTN_0 | \
					 SND_JACK_BTN_1 | SND_JACK_BTN_2 | SND_JACK_BTN_3)

enum { PWM_WIDTH, PWM_THRESH, FALL_DELAY, RISE_DELAY, DEB0, DEB1, DEB3, DEB4, PWM_DEB_NUM };

struct mt6358_accdet {
	struct device *dev;
	struct regmap *regmap;
	struct iio_channel *adc;
	struct snd_soc_jack *jack;
	/* Serialises the interrupt handling against plug-out and the micbias timeout */
	struct mutex lock;
	struct delayed_work micbias_off;
	int irq, eint0_irq;

	u32 mic_vol;
	u32 eint_level_pol;
	u32 pwm_deb[PWM_DEB_NUM];
	struct mt6358_accdet_key_thr key_thr;

	bool plugged;
	enum mt6358_accdet_state state;
	int jack_type;
	int key;
};

VISIBLE_IF_KUNIT int mt6358_accdet_key(const struct mt6358_accdet_key_thr *thr, int mv)
{
	if (mv < thr->mid)
		return SND_JACK_BTN_0;
	if (mv < thr->up)
		return SND_JACK_BTN_2;
	if (mv < thr->down)
		return SND_JACK_BTN_1;
	return 0;
}
EXPORT_SYMBOL_IF_KUNIT(mt6358_accdet_key);

VISIBLE_IF_KUNIT void mt6358_accdet_next(enum mt6358_accdet_state state, int jack,
					 unsigned int ab, struct mt6358_accdet_step *next)
{
	next->state = state;
	next->jack = jack;
	next->key = MT6358_ACCDET_KEY_NONE;

	switch (state) {
	case MT6358_ACCDET_PLUG_OUT:
		if (ab == ACCDET_AB_00) {
			next->state = MT6358_ACCDET_HOOK_SWITCH;
			next->jack = SND_JACK_HEADPHONE;
		} else if (ab == ACCDET_AB_01) {
			next->state = MT6358_ACCDET_MIC_BIAS;
			next->jack = SND_JACK_HEADSET;
		} else if (ab == ACCDET_AB_11) {
			next->jack = 0;
		}
		break;
	case MT6358_ACCDET_MIC_BIAS:
		if (ab == ACCDET_AB_00) {
			next->state = MT6358_ACCDET_HOOK_SWITCH;
			next->key = MT6358_ACCDET_KEY_PRESS;
		} else if (ab == ACCDET_AB_01) {
			next->jack = SND_JACK_HEADSET;
		} else if (ab == ACCDET_AB_11) {
			next->state = MT6358_ACCDET_PLUG_OUT;
		}
		break;
	case MT6358_ACCDET_HOOK_SWITCH:
		if (ab == ACCDET_AB_01) {
			next->state = MT6358_ACCDET_MIC_BIAS;
			next->jack = SND_JACK_HEADSET;
			next->key = MT6358_ACCDET_KEY_RELEASE;
		} else if (ab == ACCDET_AB_11) {
			next->state = MT6358_ACCDET_PLUG_OUT;
		}
		break;
	}
}
EXPORT_SYMBOL_IF_KUNIT(mt6358_accdet_next);

static void accdet_set(struct mt6358_accdet *a, unsigned int reg, unsigned int bits)
{
	regmap_set_bits(a->regmap, reg, bits);
}

static void accdet_clear(struct mt6358_accdet *a, unsigned int reg, unsigned int bits)
{
	regmap_clear_bits(a->regmap, reg, bits);
}

static void accdet_wait_clear(struct mt6358_accdet *a, unsigned int bit)
{
	unsigned int val;

	regmap_read_poll_timeout(a->regmap, ACCDET_CON(12), val, !(val & bit), 10,
				 ACCDET_IRQ_CLEAR_TIMEOUT_US);
}

static void accdet_report(struct mt6358_accdet *a)
{
	if (a->jack)
		snd_soc_jack_report(a->jack, a->jack_type | a->key, ACCDET_JACK_MASK);
}

/* Vendor accdet_init(): restart the sequencer and load the state debounce times */
static void accdet_restart(struct mt6358_accdet *a)
{
	accdet_set(a, ACCDET_CON(1), ACCDET_SEQ_INIT);
	usleep_range(2000, 2500);
	accdet_clear(a, ACCDET_CON(1), ACCDET_SEQ_INIT);
	usleep_range(1000, 1500);

	regmap_write(a->regmap, ACCDET_CON(6), a->pwm_deb[DEB0]);
	regmap_write(a->regmap, ACCDET_CON(7), a->pwm_deb[DEB1]);
	regmap_write(a->regmap, ACCDET_CON(9), a->pwm_deb[DEB3]);
	regmap_write(a->regmap, ACCDET_CON(10), a->pwm_deb[DEB4]);
}

static void accdet_enable(struct mt6358_accdet *a)
{
	accdet_set(a, MT6358_AUDENC_ANA_CON10, RG_MICBIAS1_LOWP_EN);
	accdet_set(a, ACCDET_CON(2), ACCDET_EINT0_PWM_IDLE | ACCDET_SWCTRL_EN);
	accdet_set(a, ACCDET_CON(1), ACCDET_EN);
}

static void accdet_disable(struct mt6358_accdet *a)
{
	unsigned int val;
	int i;

	accdet_set(a, ACCDET_CON(12), ACCDET_IRQ_CLR);
	udelay(200);
	for (i = 0; i < 10; i++) {
		regmap_read(a->regmap, ACCDET_CON(12), &val);
		if (!(val & ACCDET_IRQ))
			break;
		msleep(20);
	}
	accdet_clear(a, ACCDET_CON(12), ACCDET_IRQ_CLR);

	accdet_clear(a, ACCDET_CON(1), ACCDET_EN);
	accdet_clear(a, ACCDET_CON(2), ACCDET_SWCTRL_EN);
}

static int accdet_read_key(struct mt6358_accdet *a)
{
	int raw, ret;

	ret = iio_read_channel_raw(a->adc, &raw);
	if (ret < 0) {
		dev_warn(a->dev, "AUXADC read failed: %d\n", ret);
		return 0;
	}

	/* 12-bit AUXADC with a 1.8 V reference */
	return mt6358_accdet_key(&a->key_thr, raw * 1800 / 4096);
}

static void accdet_check_cable(struct mt6358_accdet *a)
{
	struct mt6358_accdet_step next;
	unsigned int val, ab;

	regmap_read(a->regmap, ACCDET_CON(14), &val);
	ab = (val >> ACCDET_AB_SHIFT) & ACCDET_AB_MASK;

	mt6358_accdet_next(a->state, a->jack_type, ab, &next);

	/* Debounce and PWM adjustments exactly as the vendor state machine makes them */
	if (a->state == MT6358_ACCDET_PLUG_OUT && ab == ACCDET_AB_01) {
		regmap_write(a->regmap, ACCDET_CON(9), a->pwm_deb[DEB3] * 30);
		regmap_write(a->regmap, ACCDET_CON(6), a->pwm_deb[DEB0] >> 1);
	} else if (a->state == MT6358_ACCDET_MIC_BIAS && ab == ACCDET_AB_00) {
		regmap_write(a->regmap, ACCDET_CON(6), a->pwm_deb[DEB0]);
	} else if (a->state == MT6358_ACCDET_HOOK_SWITCH && ab == ACCDET_AB_01) {
		regmap_write(a->regmap, ACCDET_CON(6), a->pwm_deb[DEB0] >> 1);
	}

	if (next.key != MT6358_ACCDET_KEY_NONE) {
		int key = next.key == MT6358_ACCDET_KEY_PRESS ? accdet_read_key(a) : 0;

		usleep_range(10000, 11000);
		regmap_read(a->regmap, ACCDET_CON(12), &val);
		/* An EINT pending here means the plug is leaving: not a button */
		if ((val & ACCDET_EINT_IRQS) != ACCDET_EINT_IRQS)
			a->key = key;
		else
			a->key = 0;

		if (next.key == MT6358_ACCDET_KEY_PRESS) {
			regmap_write(a->regmap, ACCDET_CON(4), a->pwm_deb[PWM_THRESH] - 1);
			regmap_write(a->regmap, ACCDET_CON(3), a->pwm_deb[PWM_WIDTH] - 1);
		}
	}

	a->state = next.state;
	a->jack_type = next.jack;
	accdet_report(a);
}

static void accdet_plug_in(struct mt6358_accdet *a)
{
	accdet_restart(a);
	accdet_set(a, ACCDET_CON(2), ACCDET_PWM_IDLE);
	accdet_enable(a);
}

static void accdet_plug_out(struct mt6358_accdet *a)
{
	cancel_delayed_work(&a->micbias_off);
	accdet_clear(a, ACCDET_CON(2), ACCDET_PWM_IDLE);
	accdet_disable(a);

	a->state = MT6358_ACCDET_PLUG_OUT;
	a->jack_type = 0;
	a->key = 0;
	accdet_report(a);

	accdet_clear(a, ACCDET_CON(12), ACCDET_EINT_IRQ_CLRS);
}

static void accdet_eint(struct mt6358_accdet *a, unsigned int con12)
{
	/* Arm the opposite level for the next edge (the EINT line is level low when plugged) */
	if (a->plugged == (a->eint_level_pol == IRQ_TYPE_LEVEL_LOW))
		regmap_write(a->regmap, ACCDET_CON(12), con12 & ~ACCDET_EINT0_IRQ_POL);
	else
		regmap_write(a->regmap, ACCDET_CON(12), con12 | ACCDET_EINT0_IRQ_POL);

	accdet_set(a, ACCDET_CON(12), ACCDET_EINT0_IRQ_CLR);
	accdet_wait_clear(a, ACCDET_EINT0_IRQ);
	accdet_clear(a, ACCDET_CON(12), ACCDET_EINT0_IRQ_CLR);

	accdet_clear(a, ACCDET_CON(15), ACCDET_EINT0_DEB);
	if (a->plugged) {
		accdet_set(a, ACCDET_CON(15), ACCDET_EINT0_DEB_IN_256MS);
		regmap_write(a->regmap, ACCDET_CON(9), a->pwm_deb[DEB3]);
		a->plugged = false;
		accdet_plug_out(a);
	} else {
		accdet_set(a, ACCDET_CON(15), ACCDET_EINT0_DEB_OUT_120US);
		a->plugged = true;
		schedule_delayed_work(&a->micbias_off, ACCDET_MICBIAS_OFF_DELAY);
		accdet_plug_in(a);
	}
}

static irqreturn_t mt6358_accdet_irq(int irq, void *data)
{
	struct mt6358_accdet *a = data;
	unsigned int con12;

	mutex_lock(&a->lock);
	regmap_read(a->regmap, ACCDET_CON(12), &con12);

	if (con12 & ACCDET_IRQ) {
		accdet_set(a, ACCDET_CON(12), ACCDET_IRQ_CLR);
		accdet_wait_clear(a, ACCDET_IRQ);
		accdet_clear(a, ACCDET_CON(12), ACCDET_IRQ_CLR);
		if (a->plugged)
			accdet_check_cable(a);
	} else if (con12 & ACCDET_EINT0_IRQ) {
		accdet_eint(a, con12);
	}

	mutex_unlock(&a->lock);
	return IRQ_HANDLED;
}

/* A 3-pole plug needs no mic bias: stop the detection after 6 s as the vendor does */
static void mt6358_accdet_micbias_off(struct work_struct *work)
{
	struct mt6358_accdet *a = container_of(work, struct mt6358_accdet, micbias_off.work);

	mutex_lock(&a->lock);
	if (a->plugged && a->jack_type == SND_JACK_HEADPHONE)
		accdet_disable(a);
	mutex_unlock(&a->lock);
}

/* Vendor accdet_late_init(): accdet_init, INIT0 and INIT1, in that order */
static void accdet_hw_init(struct mt6358_accdet *a)
{
	accdet_restart(a);

	regmap_write(a->regmap, MT6358_AUD_TOP_RST_CON0, RG_ACCDET_RST);
	accdet_clear(a, MT6358_AUD_TOP_RST_CON0, RG_ACCDET_RST);

	regmap_write(a->regmap, ACCDET_CON(4), a->pwm_deb[PWM_THRESH] - 1);
	regmap_write(a->regmap, ACCDET_CON(3), a->pwm_deb[PWM_WIDTH] - 1);
	regmap_write(a->regmap, ACCDET_CON(5),
		     a->pwm_deb[FALL_DELAY] << 15 | a->pwm_deb[RISE_DELAY]);

	regmap_update_bits(a->regmap, ACCDET_CON(24), ACCDET_HWMODE_SEL | ACCDET_FAST_DISCHARGE,
			   ACCDET_FAST_DISCHARGE);
	if (a->eint_level_pol == IRQ_TYPE_LEVEL_LOW)
		accdet_set(a, MT6358_AUDENC_ANA_CON6, RG_FSTDSCHRG_EN);
	else
		regmap_update_bits(a->regmap, MT6358_AUDENC_ANA_CON6, 0xff1f, 0);

	/* The codec forces the comparators under software control; detection needs the sequencer */
	regmap_write(a->regmap, ACCDET_CON(13), 0);

	regmap_update_bits(a->regmap, MT6358_AUDENC_ANA_CON10, 0x75,
			   FIELD_PREP(RG_MICBIAS1_VOL, a->mic_vol) | RG_MICBIAS1_LOWP_EN);
	regmap_update_bits(a->regmap, MT6358_AUDENC_ANA_CON11, 0x80 | RG_ACCDET_MODE1,
			   RG_ACCDET_MODE1);

	regmap_update_bits(a->regmap, ACCDET_CON(15), ACCDET_EINT0_PWM, ACCDET_EINT0_PWM_16MS);
	accdet_set(a, ACCDET_CON(2), ACCDET_EINT0_PWM_EN | ACCDET_EINT0_PWM_IDLE);
	accdet_set(a, ACCDET_CON(1), ACCDET_EINT0_EN);

	/* Level-low EINT: plug-in pulls the line low */
	accdet_clear(a, ACCDET_CON(12), ACCDET_EINT0_IRQ_POL);
	regmap_update_bits(a->regmap, ACCDET_CON(15), ACCDET_EINT0_DEB, ACCDET_EINT0_DEB_IN_256MS);
}

int mt6358_accdet_enable_jack_detect(struct snd_soc_component *component,
				     struct snd_soc_jack *jack)
{
	struct mt6358_accdet *a = snd_soc_component_get_drvdata(component);

	snd_jack_set_key(jack->jack, SND_JACK_BTN_0, KEY_PLAYPAUSE);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_1, KEY_VOLUMEDOWN);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_2, KEY_VOLUMEUP);
	snd_jack_set_key(jack->jack, SND_JACK_BTN_3, KEY_VOICECOMMAND);

	mutex_lock(&a->lock);
	a->jack = jack;
	accdet_hw_init(a);
	accdet_report(a);
	mutex_unlock(&a->lock);

	enable_irq(a->irq);
	enable_irq(a->eint0_irq);

	return 0;
}
EXPORT_SYMBOL_GPL(mt6358_accdet_enable_jack_detect);

static const struct snd_soc_component_driver mt6358_accdet_component = {
	.name = "mt6358-accdet",
};

static int mt6358_accdet_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct mt6397_chip *mt6397 = dev_get_drvdata(dev->parent);
	struct device_node *np = dev->of_node;
	struct mt6358_accdet *a;
	u32 thr[4];
	int ret;

	a = devm_kzalloc(dev, sizeof(*a), GFP_KERNEL);
	if (!a)
		return -ENOMEM;

	a->dev = dev;
	a->regmap = mt6397->regmap;
	ret = devm_mutex_init(dev, &a->lock);
	if (ret)
		return ret;
	INIT_DELAYED_WORK(&a->micbias_off, mt6358_accdet_micbias_off);

	a->adc = devm_iio_channel_get(dev, "accdet");
	if (IS_ERR(a->adc))
		return dev_err_probe(dev, PTR_ERR(a->adc), "no AUXADC channel\n");

	a->mic_vol = 6;
	of_property_read_u32(np, "mediatek,mic-vol", &a->mic_vol);
	a->eint_level_pol = IRQ_TYPE_LEVEL_LOW;
	of_property_read_u32(np, "mediatek,eint-level-pol", &a->eint_level_pol);
	ret = of_property_read_u32_array(np, "mediatek,pwm-deb-setting", a->pwm_deb,
					 PWM_DEB_NUM);
	if (ret)
		return dev_err_probe(dev, ret, "missing mediatek,pwm-deb-setting\n");
	ret = of_property_read_u32_array(np, "mediatek,three-key-thr", thr, ARRAY_SIZE(thr));
	if (ret)
		return dev_err_probe(dev, ret, "missing mediatek,three-key-thr\n");
	a->key_thr.mid = thr[1];
	a->key_thr.up = thr[2];
	a->key_thr.down = thr[3];

	a->irq = platform_get_irq_byname(pdev, "accdet");
	if (a->irq < 0)
		return a->irq;
	a->eint0_irq = platform_get_irq_byname(pdev, "eint0");
	if (a->eint0_irq < 0)
		return a->eint0_irq;

	ret = devm_request_threaded_irq(dev, a->irq, NULL, mt6358_accdet_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN, "accdet", a);
	if (ret)
		return ret;
	ret = devm_request_threaded_irq(dev, a->eint0_irq, NULL, mt6358_accdet_irq,
					IRQF_ONESHOT | IRQF_NO_AUTOEN, "accdet-eint0", a);
	if (ret)
		return ret;

	platform_set_drvdata(pdev, a);

	ret = devm_snd_soc_register_component(dev, &mt6358_accdet_component, NULL, 0);
	if (ret)
		return ret;

	return 0;
}

static void mt6358_accdet_remove(struct platform_device *pdev)
{
	struct mt6358_accdet *a = platform_get_drvdata(pdev);

	cancel_delayed_work_sync(&a->micbias_off);
}

static const struct of_device_id mt6358_accdet_of_match[] = {
	{ .compatible = "mediatek,mt6358-accdet" },
	{ }
};
MODULE_DEVICE_TABLE(of, mt6358_accdet_of_match);

static struct platform_driver mt6358_accdet_driver = {
	.driver = {
		.name = "mt6358-accdet",
		.of_match_table = mt6358_accdet_of_match,
	},
	.probe = mt6358_accdet_probe,
	.remove = mt6358_accdet_remove,
};
module_platform_driver(mt6358_accdet_driver);

MODULE_DESCRIPTION("MediaTek MT6358 accessory detection driver");
MODULE_IMPORT_NS("IIO_CONSUMER");
MODULE_LICENSE("GPL");
