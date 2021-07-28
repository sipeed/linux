// SPDX-License-Identifier: GPL-2.0+
/*
 * This driver supports the analog controls for the internal codec
 * found in Allwinner's A64 SoC.
 *
 * Copyright (C) 2021 Sipeed
 *
 * TODO: Extra microphone inputs
 */

#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/regmap.h>

#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>

/* Codec analog control register offsets and bit fields */
#define SUN50I_R329_ADDA_ADC1		0x00
#define SUN50I_R329_ADDA_ADC2		0x04
#define SUN50I_R329_ADDA_ADC3		0x08
#define SUN50I_R329_ADDA_ADC4		0x0c
#define SUN50I_R329_ADDA_ADC5		0x30
#define SUN50I_R329_ADDA_ADC_PGA_GAIN		8
#define SUN50I_R329_ADDA_ADC_PGA_EN		30
#define SUN50I_R329_ADDA_ADC_EN			31

#define SUN50I_R329_ADDA_DAC		0x10
#define SUN50I_R329_ADDA_DAC_SPK_VOL		0
#define SUN50I_R329_ADDA_DAC_SPKR_DIFF		5
#define SUN50I_R329_ADDA_DAC_SPKL_DIFF		6
#define SUN50I_R329_ADDA_DAC_RSWITCH		9
#define SUN50I_R329_ADDA_DAC_SPKR_MUTE		10
#define SUN50I_R329_ADDA_DAC_SPKR_EN		11
#define SUN50I_R329_ADDA_DAC_SPKL_MUTE		12
#define SUN50I_R329_ADDA_DAC_SPKL_EN		13
#define SUN50I_R329_ADDA_DAC_DACR_EN		14
#define SUN50I_R329_ADDA_DAC_DACL_EN		15

#define SUN50I_R329_ADDA_MICBIAS	0x18
#define SUN50I_R329_ADDA_MICBIAS_MMICBIASEN	7

static const DECLARE_TLV_DB_RANGE(sun50i_r329_codec_spk_vol_scale,
	0, 1, TLV_DB_SCALE_ITEM(TLV_DB_GAIN_MUTE, 0, 1),
	2, 31, TLV_DB_SCALE_ITEM(-4350, 150, 0),
);

static const char * const sun50i_r329_codec_diff_enum_text[] = {
	"Single ended", "Differential",
};

static SOC_ENUM_DOUBLE_DECL(sun50i_r329_codec_spk_diff_enum,
			    SUN50I_R329_ADDA_DAC,
			    SUN50I_R329_ADDA_DAC_SPKL_DIFF,
			    SUN50I_R329_ADDA_DAC_SPKR_DIFF,
			    sun50i_r329_codec_diff_enum_text);

static const DECLARE_TLV_DB_RANGE(sun50i_r329_codec_adc_gain_scale,
	0, 0, TLV_DB_SCALE_ITEM(TLV_DB_GAIN_MUTE, 0, 1),
	1, 3, TLV_DB_SCALE_ITEM(600, 0, 0),
	4, 4, TLV_DB_SCALE_ITEM(900, 0, 0),
	5, 31, TLV_DB_SCALE_ITEM(1000, 100, 0),
);

static const struct snd_kcontrol_new sun50i_r329_codec_controls[] = {
	SOC_SINGLE_TLV("Speaker Playback Volume",
		       SUN50I_R329_ADDA_DAC,
		       SUN50I_R329_ADDA_DAC_SPK_VOL, 0x1f, 0,
		       sun50i_r329_codec_spk_vol_scale),
	SOC_ENUM("Speaker Playback Mode",
		 sun50i_r329_codec_spk_diff_enum),

	SOC_SINGLE_TLV("Left Mic Capture Volume",
		       SUN50I_R329_ADDA_ADC1,
		       SUN50I_R329_ADDA_ADC_PGA_GAIN, 0x1f, 0,
		       sun50i_r329_codec_adc_gain_scale),
	SOC_SINGLE_TLV("Right Mic Capture Volume",
		       SUN50I_R329_ADDA_ADC2,
		       SUN50I_R329_ADDA_ADC_PGA_GAIN, 0x1f, 0,
		       sun50i_r329_codec_adc_gain_scale),
};

static const struct snd_kcontrol_new sun50i_r329_codec_spk_switch =
	SOC_DAPM_SINGLE("Speaker Playback Switch",
			SUN50I_R329_ADDA_DAC,
			SUN50I_R329_ADDA_DAC_SPKL_EN, 1, 0);

static const struct snd_soc_dapm_widget sun50i_r329_codec_widgets[] = {
	/* DAC */
	SND_SOC_DAPM_DAC("Left DAC", NULL, SUN50I_R329_ADDA_DAC,
			 SUN50I_R329_ADDA_DAC_DACL_EN, 0),
	SND_SOC_DAPM_DAC("Right DAC", NULL, SUN50I_R329_ADDA_DAC,
			 SUN50I_R329_ADDA_DAC_DACR_EN, 0),
	/* ADC */
	SND_SOC_DAPM_ADC("Left ADC", NULL, SUN50I_R329_ADDA_ADC1,
			 SUN50I_R329_ADDA_ADC_EN, 0),
	SND_SOC_DAPM_ADC("Right ADC", NULL, SUN50I_R329_ADDA_ADC2,
			 SUN50I_R329_ADDA_ADC_EN, 0),

	/*
	 * Due to this component and the codec belonging to separate DAPM
	 * contexts, we need to manually link the above widgets to their
	 * stream widgets at the card level.
	 */

	SND_SOC_DAPM_SWITCH("Left Speaker Switch", SUN50I_R329_ADDA_DAC,
			    SUN50I_R329_ADDA_DAC_SPKL_MUTE, 0,
			    &sun50i_r329_codec_spk_switch),
	SND_SOC_DAPM_SWITCH("Right Speaker Switch", SUN50I_R329_ADDA_DAC,
			    SUN50I_R329_ADDA_DAC_SPKL_MUTE, 0,
			    &sun50i_r329_codec_spk_switch),
	SND_SOC_DAPM_OUTPUT("SPKL"),
	SND_SOC_DAPM_OUTPUT("SPKR"),

	/* Microphone inputs */
	SND_SOC_DAPM_INPUT("MIC1"),
	SND_SOC_DAPM_INPUT("MIC2"),

	/* Microphone Bias */
	SND_SOC_DAPM_SUPPLY("MBIAS", SUN50I_R329_ADDA_MICBIAS,
			    SUN50I_R329_ADDA_MICBIAS_MMICBIASEN,
			    0, NULL, 0),

	/* Mic input path */
	SND_SOC_DAPM_PGA("Left Mic Amplifier", SUN50I_R329_ADDA_ADC1,
			 SUN50I_R329_ADDA_ADC_PGA_EN, 0, NULL, 0),
	SND_SOC_DAPM_PGA("Right Mic Amplifier", SUN50I_R329_ADDA_ADC2,
			 SUN50I_R329_ADDA_ADC_PGA_EN, 0, NULL, 0),
};

static const struct snd_soc_dapm_route sun50i_r329_codec_routes[] = {
	/* Speaker Routes */
	{ "Left Speaker Switch", "Speaker Playback Switch", "Left DAC" },
	{ "Right Speaker Switch", "Speaker Playback Switch", "Right DAC" },
	{ "SPKL", NULL, "Left Speaker Switch" },
	{ "SPKR", NULL, "Right Speaker Switch" },

	/* Microphone Routes */
	{ "Left ADC", NULL, "Left Mic Amplifier" },
	{ "Right ADC", NULL, "Right Mic Amplifier" },
	{ "Left Mic Amplifier", NULL, "MIC1"},
	{ "Right Mic Amplifier", NULL, "MIC2"},
};

static int sun50i_r329_codec_analog_cmpnt_probe(struct snd_soc_component *cmpnt)
{
	/*
	 * Override the RSWITCH bit value. This bit is not documented
	 * clearly enough, and it's hardcoded in BSP driver.
	 *
	 * So just follow the BSP driver behavior here.
	 */
	return snd_soc_component_update_bits(cmpnt, SUN50I_R329_ADDA_DAC,
					     BIT(SUN50I_R329_ADDA_DAC_RSWITCH),
					     BIT(SUN50I_R329_ADDA_DAC_RSWITCH));
}

static const struct snd_soc_component_driver sun50i_r329_codec_analog_cmpnt_drv = {
	.controls		= sun50i_r329_codec_controls,
	.num_controls		= ARRAY_SIZE(sun50i_r329_codec_controls),
	.dapm_widgets		= sun50i_r329_codec_widgets,
	.num_dapm_widgets	= ARRAY_SIZE(sun50i_r329_codec_widgets),
	.dapm_routes		= sun50i_r329_codec_routes,
	.num_dapm_routes	= ARRAY_SIZE(sun50i_r329_codec_routes),
	.probe			= sun50i_r329_codec_analog_cmpnt_probe,
};

static const struct of_device_id sun50i_r329_codec_analog_of_match[] = {
	{
		.compatible = "allwinner,sun50i-r329-codec-analog",
	},
	{}
};
MODULE_DEVICE_TABLE(of, sun50i_r329_codec_analog_of_match);

static const struct regmap_config sun50i_r329_codec_regmap_config = {
	.reg_bits	= 32,
	.reg_stride	= 4,
	.val_bits	= 32,
	.max_register	= SUN50I_R329_ADDA_ADC5,
};

static int sun50i_r329_codec_analog_probe(struct platform_device *pdev)
{
	struct regmap *regmap;
	void __iomem *base;

	base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(base)) {
		dev_err(&pdev->dev, "Failed to map the registers\n");
		return PTR_ERR(base);
	}

	regmap = devm_regmap_init_mmio(&pdev->dev, base,
				       &sun50i_r329_codec_regmap_config);
	if (IS_ERR(regmap)) {
		dev_err(&pdev->dev, "Failed to create regmap\n");
		return PTR_ERR(regmap);
	}

	return devm_snd_soc_register_component(&pdev->dev,
					       &sun50i_r329_codec_analog_cmpnt_drv,
					       NULL, 0);
}

static struct platform_driver sun50i_r329_codec_analog_driver = {
	.driver = {
		.name = "sun50i-r329-codec-analog",
		.of_match_table = sun50i_r329_codec_analog_of_match,
	},
	.probe = sun50i_r329_codec_analog_probe,
};
module_platform_driver(sun50i_r329_codec_analog_driver);

MODULE_DESCRIPTION("Allwinner internal codec analog controls driver for R329");
MODULE_AUTHOR("Icenowy Zheng <icenowy@sipeed.com>");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:sun50i-r329-codec-analog");
