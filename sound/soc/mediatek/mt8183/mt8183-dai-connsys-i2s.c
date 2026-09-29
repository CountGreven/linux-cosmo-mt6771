// SPDX-License-Identifier: GPL-2.0
//
// MediaTek ALSA SoC Audio DAI CONNSYS I2S Control
//
// The connectivity subsystem (FM radio) drives this I2S input as master at
// 32 kHz; the AFE takes it in slave mode through an ASRC to the stream rate.

#include <linux/bitops.h>
#include <linux/regmap.h>
#include <sound/pcm_params.h>
#include "mt8183-afe-common.h"
#include "mt8183-reg.h"

#define AUDIO_TOP_CON0_PDN_I2S		BIT(6)

static const struct snd_soc_dapm_widget mtk_dai_connsys_i2s_widgets[] = {
	SND_SOC_DAPM_INPUT("CONNSYS"),
};

static const struct snd_soc_dapm_route mtk_dai_connsys_i2s_routes[] = {
	{"Connsys I2S", NULL, "CONNSYS"},
};

static int mtk_dai_connsys_i2s_startup(struct snd_pcm_substream *substream,
				       struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);

	/* Not a CCF gate; the boot loader may leave it set */
	return regmap_update_bits(afe->regmap, AUDIO_TOP_CON0,
				  AUDIO_TOP_CON0_PDN_I2S, 0);
}

static int mtk_dai_connsys_i2s_hw_params(struct snd_pcm_substream *substream,
					 struct snd_pcm_hw_params *params,
					 struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	unsigned int rate = params_rate(params);
	unsigned int rate_reg = mt8183_rate_transform(afe->dev, rate, dai->id);
	unsigned int i2s_con;

	/* i2s format, slave, 16 bit, from connsys, asrc not bypassed */
	i2s_con = (1 << I2S_FMT_SFT) | (1 << I2S_SRC_SFT) |
		  (rate_reg << I2S_MODE_SFT);
	regmap_update_bits(afe->regmap, AFE_CONNSYS_I2S_CON,
			   ~I2S_EN_MASK_SFT, i2s_con);

	regmap_write(afe->regmap, AFE_ASRC_2CH_CON3,
		     rate == 44100 ? 0x001b9000 : 0x001e0000);
	regmap_write(afe->regmap, AFE_ASRC_2CH_CON4, 0x00140000);
	regmap_write(afe->regmap, AFE_ASRC_2CH_CON5, 0x00ff5987);
	regmap_write(afe->regmap, AFE_ASRC_2CH_CON6, 0x00007ef4);
	regmap_write(afe->regmap, AFE_ASRC_2CH_CON5, 0x00ff5986);
	regmap_write(afe->regmap, AFE_ASRC_2CH_CON5, 0x00ff5987);
	regmap_update_bits(afe->regmap, AFE_ASRC_2CH_CON2,
			   CHSET_IS_MONO_MASK_SFT, 0);
	regmap_write(afe->regmap, AFE_ASRC_2CH_CON9, 0x00036000);
	regmap_write(afe->regmap, AFE_ASRC_2CH_CON10, 0x0002fc00);

	return 0;
}

static int mtk_dai_connsys_i2s_trigger(struct snd_pcm_substream *substream,
				       int cmd, struct snd_soc_dai *dai)
{
	struct mtk_base_afe *afe = snd_soc_dai_get_drvdata(dai);
	unsigned int asrc_on = CON0_CHSET_EN_MASK_SFT | CON0_ASM_ON_MASK_SFT;

	switch (cmd) {
	case SNDRV_PCM_TRIGGER_START:
	case SNDRV_PCM_TRIGGER_RESUME:
		regmap_update_bits(afe->regmap, AFE_ASRC_2CH_CON0,
				   asrc_on, asrc_on);
		regmap_update_bits(afe->regmap, AFE_CONNSYS_I2S_CON,
				   I2S_EN_MASK_SFT, I2S_EN_MASK_SFT);
		return 0;
	case SNDRV_PCM_TRIGGER_STOP:
	case SNDRV_PCM_TRIGGER_SUSPEND:
		regmap_update_bits(afe->regmap, AFE_ASRC_2CH_CON0, asrc_on, 0);
		regmap_update_bits(afe->regmap, AFE_CONNSYS_I2S_CON,
				   I2S_BYPSRC_MASK_SFT, I2S_BYPSRC_MASK_SFT);
		regmap_update_bits(afe->regmap, AFE_CONNSYS_I2S_CON,
				   I2S_EN_MASK_SFT, 0);
		return 0;
	default:
		return -EINVAL;
	}
}

static const struct snd_soc_dai_ops mtk_dai_connsys_i2s_ops = {
	.startup = mtk_dai_connsys_i2s_startup,
	.hw_params = mtk_dai_connsys_i2s_hw_params,
	.trigger = mtk_dai_connsys_i2s_trigger,
};

static struct snd_soc_dai_driver mtk_dai_connsys_i2s_driver[] = {
	{
		.name = "CONNSYS_I2S",
		.id = MT8183_DAI_CONNSYS_I2S,
		.capture = {
			.stream_name = "Connsys I2S",
			.channels_min = 1,
			.channels_max = 2,
			.rates = SNDRV_PCM_RATE_44100 | SNDRV_PCM_RATE_48000,
			.formats = SNDRV_PCM_FMTBIT_S16_LE |
				   SNDRV_PCM_FMTBIT_S24_LE |
				   SNDRV_PCM_FMTBIT_S32_LE,
		},
		.ops = &mtk_dai_connsys_i2s_ops,
	},
};

int mt8183_dai_connsys_i2s_register(struct mtk_base_afe *afe)
{
	struct mtk_base_afe_dai *dai;

	dai = devm_kzalloc(afe->dev, sizeof(*dai), GFP_KERNEL);
	if (!dai)
		return -ENOMEM;

	list_add(&dai->list, &afe->sub_dais);

	dai->dai_drivers = mtk_dai_connsys_i2s_driver;
	dai->num_dai_drivers = ARRAY_SIZE(mtk_dai_connsys_i2s_driver);
	dai->dapm_widgets = mtk_dai_connsys_i2s_widgets;
	dai->num_dapm_widgets = ARRAY_SIZE(mtk_dai_connsys_i2s_widgets);
	dai->dapm_routes = mtk_dai_connsys_i2s_routes;
	dai->num_dapm_routes = ARRAY_SIZE(mtk_dai_connsys_i2s_routes);

	return 0;
}
