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
#include <linux/platform_device.h>
#include <sound/soc.h>

static const struct snd_soc_dapm_widget cosmo_widgets[] = {
	SND_SOC_DAPM_HP("Headphone", NULL),
	SND_SOC_DAPM_SPK("Speaker", NULL),
	SND_SOC_DAPM_SPK("Earpiece", NULL),
	SND_SOC_DAPM_MIC("Main Mic", NULL),
	SND_SOC_DAPM_MIC("Second Mic", NULL),
	SND_SOC_DAPM_MIC("Headset Mic", NULL),
};

static const struct snd_soc_dapm_route cosmo_routes[] = {
	{ "Headphone", NULL, "Headphone L" },
	{ "Headphone", NULL, "Headphone R" },

	{ "Amp1 IN", NULL, "Headphone L Ext Spk Amp" },
	{ "Amp2 IN", NULL, "Headphone R Ext Spk Amp" },
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
};

static int cosmo_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct snd_soc_card *card = &cosmo_card;
	struct snd_soc_dai_link *link;
	struct device_node *platform;
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
