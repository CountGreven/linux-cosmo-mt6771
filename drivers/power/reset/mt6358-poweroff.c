// SPDX-License-Identifier: GPL-2.0
/*
 * Power off for the MediaTek MT6358 PMIC
 *
 * The sequence follows the vendor's hal_rtc_bbpu_pwdn() for this PMIC: unlike the MT6323 and
 * MT6397, clearing BBPU alone does not switch the MT6358 off, the power hold bit does.
 */

#include <linux/delay.h>
#include <linux/err.h>
#include <linux/mfd/mt6397/core.h>
#include <linux/mfd/mt6397/rtc.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/regmap.h>

#define MT6358_TOP_TMA_KEY		0x03a8
#define MT6358_TOP_TMA_KEY_UNLOCK	0x9ca7
#define MT6358_RTC_BASE_ADDR		0x0588
#define MT6358_PPCCTL0			0x0a08
#define MT6358_RG_PWRHOLD		BIT(0)
#define MT6358_PSEQ_ELR11		0x0a62
#define MT6358_RG_SDN_DLY_ENB		BIT(10)

#define MT6358_BBPU_PWREN		BIT(0)
#define MT6358_BBPU_CLR			BIT(1)
#define MT6358_BBPU_RELOAD		BIT(5)
#define MT6358_AL_SEC_AUTO_PDN_SEL	BIT(6)
#define MT6358_AL_SEC_2SEC_EN		BIT(8)

struct mt6358_pwrc {
	struct device *dev;
	struct regmap *regmap;
};

static void mt6358_rtc_write_trigger(struct mt6358_pwrc *pwrc)
{
	u32 val;

	regmap_write(pwrc->regmap, MT6358_RTC_BASE_ADDR + RTC_WRTGR_MT6358, 1);
	if (regmap_read_poll_timeout(pwrc->regmap, MT6358_RTC_BASE_ADDR + RTC_BBPU, val,
				     !(val & RTC_BBPU_CBUSY), MTK_RTC_POLL_DELAY_US,
				     MTK_RTC_POLL_TIMEOUT))
		dev_err(pwrc->dev, "RTC write did not complete\n");
}

static int mt6358_power_off(struct sys_off_data *data)
{
	struct mt6358_pwrc *pwrc = data->cb_data;
	struct regmap *map = pwrc->regmap;
	u32 val;

	/* no restart two seconds after the power is gone */
	regmap_update_bits(map, MT6358_RTC_BASE_ADDR + RTC_AL_SEC,
			   MT6358_AL_SEC_2SEC_EN | MT6358_AL_SEC_AUTO_PDN_SEL, 0);
	mt6358_rtc_write_trigger(pwrc);

	regmap_write(map, MT6358_RTC_BASE_ADDR + RTC_BBPU,
		     RTC_BBPU_KEY | MT6358_BBPU_CLR | MT6358_BBPU_PWREN);
	regmap_write(map, MT6358_RTC_BASE_ADDR + RTC_AL_MASK, RTC_AL_MASK_DOW);
	mt6358_rtc_write_trigger(pwrc);
	regmap_read_poll_timeout(map, MT6358_RTC_BASE_ADDR + RTC_BBPU, val,
				 !(val & MT6358_BBPU_CLR), MTK_RTC_POLL_DELAY_US,
				 MTK_RTC_POLL_TIMEOUT);

	regmap_write(map, MT6358_TOP_TMA_KEY, MT6358_TOP_TMA_KEY_UNLOCK);
	regmap_update_bits(map, MT6358_PSEQ_ELR11, MT6358_RG_SDN_DLY_ENB, 0);
	regmap_write(map, MT6358_TOP_TMA_KEY, 0);

	regmap_read(map, MT6358_RTC_BASE_ADDR + RTC_BBPU, &val);
	regmap_write(map, MT6358_RTC_BASE_ADDR + RTC_BBPU,
		     val | RTC_BBPU_KEY | MT6358_BBPU_RELOAD);
	mt6358_rtc_write_trigger(pwrc);

	regmap_update_bits(map, MT6358_PPCCTL0, MT6358_RG_PWRHOLD, 0);

	/* the rails drop within this; with a charger attached the PMIC starts again by itself */
	mdelay(1000);

	return NOTIFY_DONE;
}

static int mt6358_pwrc_probe(struct platform_device *pdev)
{
	struct mt6397_chip *chip = dev_get_drvdata(pdev->dev.parent);
	struct mt6358_pwrc *pwrc;

	pwrc = devm_kzalloc(&pdev->dev, sizeof(*pwrc), GFP_KERNEL);
	if (!pwrc)
		return -ENOMEM;

	pwrc->dev = &pdev->dev;
	pwrc->regmap = chip->regmap;

	/* ahead of PSCI SYSTEM_OFF, which does not return on firmware that leaves this to the PMIC */
	return devm_register_sys_off_handler(&pdev->dev, SYS_OFF_MODE_POWER_OFF,
					     SYS_OFF_PRIO_FIRMWARE + 1, mt6358_power_off, pwrc);
}

static struct platform_driver mt6358_pwrc_driver = {
	.probe = mt6358_pwrc_probe,
	.driver = {
		.name = "mt6358-pwrc",
	},
};
module_platform_driver(mt6358_pwrc_driver);

MODULE_DESCRIPTION("Power off driver for the MediaTek MT6358 PMIC");
MODULE_ALIAS("platform:mt6358-pwrc");
MODULE_LICENSE("GPL");
