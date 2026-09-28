// SPDX-License-Identifier: GPL-2.0
/*
 * Planet Cosmo Communicator (MT6771 + MT6358) ALSA SoC machine driver
 *
 * The speakers hang off the MT6358 headphone buffer through two one-wire
 * GPIO amplifiers, and the earpiece path has its own GPIO switch; both are
 * auxiliary devices given by the "aux-devs" property.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <sound/jack.h>
#include <sound/soc.h>

#include "../../codecs/mt6358-accdet.h"

struct cosmo_priv {
	bool phase_fix;
	bool speaker_on;
	struct snd_soc_component *accdet;
	struct snd_soc_jack headset;
	struct notifier_block jack_nb;
	bool speaker_was_enabled;
	bool headphone_in;
};

static struct snd_soc_jack_pin cosmo_jack_pins[] = {
	{ .pin = "Headphone", .mask = SND_JACK_HEADPHONE },
	{ .pin = "Headset Mic", .mask = SND_JACK_MICROPHONE },
};

/* The vendor switches the speaker amps off while anything is in the jack */
static int cosmo_jack_event(struct notifier_block *nb, unsigned long status, void *data)
{
	struct cosmo_priv *priv = container_of(nb, struct cosmo_priv, jack_nb);
	struct snd_soc_jack *jack = data;
	struct snd_soc_dapm_context *dapm = snd_soc_card_to_dapm(jack->card);
	bool in = status & SND_JACK_HEADPHONE;

	if (in == priv->headphone_in)
		return NOTIFY_OK;
	priv->headphone_in = in;

	if (in) {
		priv->speaker_was_enabled = snd_soc_dapm_get_pin_status(dapm, "Speaker");
		snd_soc_dapm_disable_pin(dapm, "Speaker");
	} else if (priv->speaker_was_enabled) {
		snd_soc_dapm_enable_pin(dapm, "Speaker");
	}
	snd_soc_dapm_sync(dapm);

	return NOTIFY_OK;
}

/*
 * One speaker is wired with inverted polarity (the headphone jack is not), so while the
 * speakers play the codec inverts its left DAC channel, unless headphones are enabled too.
 */
static void cosmo_apply_phase(struct snd_soc_card *card)
{
	struct cosmo_priv *priv = snd_soc_card_get_drvdata(card);
	struct snd_soc_dapm_context *dapm = snd_soc_card_to_dapm(card);
	struct snd_kcontrol *kctl;
	struct snd_ctl_elem_value *val;

	kctl = snd_soc_card_get_kcontrol(card, "DAC Left Invert Switch");
	if (!kctl)
		return;

	val = kzalloc_obj(*val);
	if (!val)
		return;

	val->value.integer.value[0] = priv->phase_fix && priv->speaker_on &&
				      !snd_soc_dapm_get_pin_status(dapm, "Headphone");
	kctl->put(kctl, val);
	kfree(val);
}

static int cosmo_speaker_event(struct snd_soc_dapm_widget *w,
			       struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_card *card = snd_soc_dapm_to_card(w->dapm);
	struct cosmo_priv *priv = snd_soc_card_get_drvdata(card);

	priv->speaker_on = SND_SOC_DAPM_EVENT_ON(event);
	cosmo_apply_phase(card);

	return 0;
}

static int cosmo_phase_fix_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct cosmo_priv *priv = snd_soc_card_get_drvdata(card);

	ucontrol->value.integer.value[0] = priv->phase_fix;
	return 0;
}

static int cosmo_phase_fix_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct cosmo_priv *priv = snd_soc_card_get_drvdata(card);
	bool fix = ucontrol->value.integer.value[0];

	if (priv->phase_fix == fix)
		return 0;

	priv->phase_fix = fix;
	cosmo_apply_phase(card);
	return 1;
}

static const struct snd_soc_dapm_widget cosmo_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_SPK("Speaker", cosmo_speaker_event),
	SND_SOC_DAPM_SPK("Earpiece", NULL),
	SND_SOC_DAPM_MIC("Main Mic", NULL),
	SND_SOC_DAPM_MIC("Second Mic", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
};

static const struct snd_soc_dapm_route cosmo_routes[] = {
	{ "Headphone", NULL, "Headphone L" },
	{ "Headphone", NULL, "Headphone R" },

	/*
	 * The amps sit on the stereo headphone buffer (vendor int_hp_buf); the codec's
	 * "Ext Spk Amp" mode powers only the left DAC and does not fit this board.
	 */
	{ "Amp1 IN", NULL, "Headphone L" },
	{ "Amp2 IN", NULL, "Headphone R" },
	{ "Speaker", NULL, "Amp1 OUT" },
	{ "Speaker", NULL, "Amp2 OUT" },

	{ "Earpiece Switch INL", NULL, "Receiver" },
	{ "Earpiece", NULL, "Earpiece Switch OUTL" },

	{ "AIN0", NULL, "Main Mic" },
	{ "AIN2", NULL, "Second Mic" },
	{ "AIN1", NULL, "Headset Mic" },
};

static const struct snd_kcontrol_new cosmo_controls[] = {
	SOC_DAPM_PIN_SWITCH("Headphone"),
	SOC_DAPM_PIN_SWITCH("Speaker"),
	SOC_DAPM_PIN_SWITCH("Earpiece"),
	SOC_DAPM_PIN_SWITCH("Main Mic"),
	SOC_DAPM_PIN_SWITCH("Second Mic"),
	SOC_DAPM_PIN_SWITCH("Headset Mic"),
	SOC_SINGLE_BOOL_EXT("Speaker Phase Fix Switch", 0,
			    cosmo_phase_fix_get, cosmo_phase_fix_put),
};

SND_SOC_DAILINK_DEFS(playback1,
	DAILINK_COMP_ARRAY(COMP_CPU("DL1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture1,
	DAILINK_COMP_ARRAY(COMP_CPU("UL1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(primary_codec,
	DAILINK_COMP_ARRAY(COMP_CPU("ADDA")),
	DAILINK_COMP_ARRAY(COMP_CODEC("mt6358-sound", "mt6358-snd-codec-aif1")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

static struct snd_soc_dai_link cosmo_dai_links[] = {
	/* FE */
	{
		.name = "Playback_1",
		.stream_name = "Playback_1",
		.trigger = { SND_SOC_DPCM_TRIGGER_PRE, SND_SOC_DPCM_TRIGGER_PRE },
		.dynamic = 1,
		.playback_only = 1,
		SND_SOC_DAILINK_REG(playback1),
	},
	{
		.name = "Capture_1",
		.stream_name = "Capture_1",
		.trigger = { SND_SOC_DPCM_TRIGGER_PRE, SND_SOC_DPCM_TRIGGER_PRE },
		.dynamic = 1,
		.capture_only = 1,
		SND_SOC_DAILINK_REG(capture1),
	},
	/* BE */
	{
		.name = "Primary Codec",
		.no_pcm = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(primary_codec),
	},
};

static int cosmo_late_probe(struct snd_soc_card *card)
{
	struct snd_soc_dapm_context *dapm = snd_soc_card_to_dapm(card);

	/* The earpiece switch is a stereo simple amplifier with one side wired */
	struct cosmo_priv *priv = snd_soc_card_get_drvdata(card);
	int ret;

	snd_soc_dapm_disable_pin(dapm, "Earpiece Switch INR");
	snd_soc_dapm_disable_pin(dapm, "Earpiece Switch OUTR");

	if (priv->accdet) {
		ret = snd_soc_card_jack_new_pins(card, "Headset Jack", SND_JACK_HEADSET |
						 SND_JACK_BTN_0 | SND_JACK_BTN_1 |
						 SND_JACK_BTN_2 | SND_JACK_BTN_3,
						 &priv->headset, cosmo_jack_pins,
						 ARRAY_SIZE(cosmo_jack_pins));
		if (ret)
			return ret;

		priv->jack_nb.notifier_call = cosmo_jack_event;
		snd_soc_jack_notifier_register(&priv->headset, &priv->jack_nb);

		/* After the codec probe: it forces ACCDET_CON13, detection clears it */
		ret = mt6358_accdet_enable_jack_detect(priv->accdet, &priv->headset);
		if (ret)
			return ret;
	}

	return snd_soc_dapm_sync(dapm);
}

static struct snd_soc_card cosmo_card = {
	.name = "cosmo",
	.owner = THIS_MODULE,
	.dai_link = cosmo_dai_links,
	.num_links = ARRAY_SIZE(cosmo_dai_links),
	.controls = cosmo_controls,
	.num_controls = ARRAY_SIZE(cosmo_controls),
	.dapm_widgets = cosmo_widgets,
	.num_dapm_widgets = ARRAY_SIZE(cosmo_widgets),
	.dapm_routes = cosmo_routes,
	.num_dapm_routes = ARRAY_SIZE(cosmo_routes),
	.late_probe = cosmo_late_probe,
};

static int cosmo_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct snd_soc_card *card = &cosmo_card;
	struct snd_soc_dai_link *link;
	struct cosmo_priv *priv;
	struct device_node *platform, *np;
	int i, n, ret;

	platform = of_parse_phandle(dev->of_node, "mediatek,platform", 0);
	if (!platform)
		return dev_err_probe(dev, -EINVAL, "missing mediatek,platform\n");

	for_each_card_prelinks(card, i, link)
		if (!link->platforms->name)
			link->platforms->of_node = platform;

	n = of_count_phandle_with_args(dev->of_node, "aux-devs", NULL);
	if (n > 0) {
		card->aux_dev = devm_kcalloc(dev, n, sizeof(*card->aux_dev), GFP_KERNEL);
		if (!card->aux_dev) {
			ret = -ENOMEM;
			goto out;
		}
		for (i = 0; i < n; i++)
			card->aux_dev[i].dlc.of_node = of_parse_phandle(dev->of_node,
									"aux-devs", i);
		card->num_aux_devs = n;
	}

	priv = devm_kzalloc(dev, sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto out;
	}
	priv->phase_fix = true;
	snd_soc_card_set_drvdata(card, priv);

	np = of_parse_phandle(dev->of_node, "mediatek,accdet", 0);
	if (np) {
		struct platform_device *accdet_pdev = of_find_device_by_node(np);

		of_node_put(np);
		if (accdet_pdev) {
			priv->accdet = snd_soc_lookup_component(&accdet_pdev->dev, NULL);
			put_device(&accdet_pdev->dev);
		}
		if (!priv->accdet) {
			ret = dev_err_probe(dev, -EPROBE_DEFER, "accdet not ready\n");
			goto out;
		}
	}

	card->dev = dev;
	ret = devm_snd_soc_register_card(dev, card);

out:
	of_node_put(platform);
	return ret;
}

static const struct of_device_id cosmo_of_match[] = {
	{ .compatible = "planet,cosmo-sound" },
	{ }
};
MODULE_DEVICE_TABLE(of, cosmo_of_match);

static struct platform_driver cosmo_driver = {
	.driver = {
		.name = "mt6771-cosmo",
		.of_match_table = cosmo_of_match,
		.pm = &snd_soc_pm_ops,
	},
	.probe = cosmo_probe,
};
module_platform_driver(cosmo_driver);

MODULE_DESCRIPTION("Planet Cosmo Communicator ALSA SoC machine driver");
MODULE_LICENSE("GPL");
