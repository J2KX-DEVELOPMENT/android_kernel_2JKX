/*
 * universal7270-Unknown Audio Machine driver.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/clk.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/gpio.h>
#include <sound/tlv.h>

#include <sound/soc.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/initval.h>
#include <sound/exynos-audmixer.h>

#include "i2s.h"
#include "i2s-regs.h"
#include "../codecs/largo.h"

#define CODEC_BFS_48KHZ		32
#define CODEC_RFS_48KHZ		512
#define CODEC_SAMPLE_RATE_48KHZ	48000

#define CODEC_BFS_192KHZ		64
#define CODEC_RFS_192KHZ		128
#define CODEC_SAMPLE_RATE_192KHZ	192000

#define MCLK_RATE	24000000
#define SYSCLK_RATE	147456000

#ifdef CONFIG_SND_SOC_SAMSUNG_VERBOSE_DEBUG
#ifdef dev_dbg
#undef dev_dbg
#endif
#define dev_dbg dev_err
#endif

static struct snd_soc_card universal7270_largo_card;

static const struct snd_soc_component_driver universal7270_cmpnt = {
	.name = "universal7270-audio",
};

static int universal7270_aif1_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_dai *amixer_dai = rtd->codec_dais[0];
	struct snd_soc_dai *codec_dai = rtd->codec_dais[1];
	struct snd_soc_dai *cpu_dai = rtd->cpu_dai;
	int ret;
	int rfs, bfs;

	dev_info(card->dev, "aif1: %dch, %dHz, %dbytes\n",
		 params_channels(params), params_rate(params),
		 params_buffer_bytes(params));

	if (params_rate(params) == CODEC_SAMPLE_RATE_192KHZ) {
		rfs = CODEC_RFS_192KHZ;
		bfs = CODEC_BFS_192KHZ;
	} else {
		rfs = CODEC_RFS_48KHZ;
		bfs = CODEC_BFS_48KHZ;
	}

	ret = snd_soc_dai_set_fmt(codec_dai, SND_SOC_DAIFMT_I2S
						| SND_SOC_DAIFMT_NB_NF
						| SND_SOC_DAIFMT_CBS_CFS);
	if (ret < 0) {
		dev_err(card->dev, "aif1: Failed to set Codec DAIFMT\n");
		return ret;
	}

	ret = snd_soc_dai_set_fmt(cpu_dai, SND_SOC_DAIFMT_I2S
						| SND_SOC_DAIFMT_NB_NF
						| SND_SOC_DAIFMT_CBS_CFS);
	if (ret < 0) {
		dev_err(card->dev, "aif1: Failed to set CPU DAIFMT\n");
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(cpu_dai, SAMSUNG_I2S_CDCLK,
				rfs, SND_SOC_CLOCK_OUT);
	if (ret < 0) {
		dev_err(card->dev, "aif1: Failed to set SAMSUNG_I2S_CDCLK\n");
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(cpu_dai, SAMSUNG_I2S_OPCLK,
						0, MOD_OPCLK_PCLK);
	if (ret < 0) {
		dev_err(card->dev, "aif1: Failed to set SAMSUNG_I2S_OPCLK\n");
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(cpu_dai, SAMSUNG_I2S_RCLKSRC_1, 0, 0);
	if (ret < 0) {
		dev_err(card->dev,
				"aif1: Failed to set SAMSUNG_I2S_RCLKSRC_1\n");
		return ret;
	}

	ret = snd_soc_dai_set_clkdiv(cpu_dai,
			SAMSUNG_I2S_DIV_BCLK, bfs);
	if (ret < 0) {
		dev_err(card->dev, "aif1: Failed to set BFS\n");
		return ret;
	}

	ret = snd_soc_dai_set_clkdiv(cpu_dai,
			SAMSUNG_I2S_DIV_RCLK, rfs);
	if (ret < 0) {
		dev_err(card->dev, "aif1: Failed to set RFS\n");
		return ret;
	}

	ret = snd_soc_dai_set_bclk_ratio(amixer_dai, bfs);
	if (ret < 0) {
		dev_err(card->dev, "aif1: Failed to configure mixer\n");
		return ret;
	}

	return 0;
}

static int universal7270_aif2_hw_params(struct snd_pcm_substream *substream,
				 struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;
	struct snd_soc_dai *amixer_dai = rtd->codec_dais[0];
	int bfs, ret;

	dev_info(card->dev, "aif2: %dch, %dHz, %dbytes\n",
		 params_channels(params), params_rate(params),
		 params_buffer_bytes(params));

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_U24:
	case SNDRV_PCM_FORMAT_S24:
		bfs = 48;
		break;
	case SNDRV_PCM_FORMAT_U16_LE:
	case SNDRV_PCM_FORMAT_S16_LE:
		bfs = 32;
		break;
	default:
		dev_err(card->dev, "aif2: Unsupported PCM_FORMAT\n");
		return -EINVAL;
	}

	ret = snd_soc_dai_set_bclk_ratio(amixer_dai, bfs);
	if (ret < 0) {
		dev_err(card->dev, "aif2: Failed to configure mixer\n");
		return ret;
	}

	return 0;
}

static int universal7270_aif1_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;

	dev_dbg(card->dev, "aif1: (%s) %s called\n",
			substream->stream ? "C" : "P", __func__);

	return 0;
}

void universal7270_aif1_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;

	dev_dbg(card->dev, "aif1: (%s) %s called\n",
			substream->stream ? "C" : "P", __func__);
}

static int universal7270_aif2_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;

	dev_dbg(card->dev, "aif2: (%s) %s called\n",
			substream->stream ? "C" : "P", __func__);

	return 0;
}

void universal7270_aif2_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;

	dev_dbg(card->dev, "aif2: (%s) %s called\n",
			substream->stream ? "C" : "P", __func__);
}

static int universal7270_aif3_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;

	dev_dbg(card->dev, "aif3: (%s) %s called\n",
			substream->stream ? "C" : "P", __func__);

	return 0;
}

void universal7270_aif3_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;

	dev_dbg(card->dev, "aif3: (%s) %s called\n",
			substream->stream ? "C" : "P", __func__);
}

static int universal7270_aif4_startup(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;

	dev_dbg(card->dev, "aif4: (%s) %s called\n",
			substream->stream ? "C" : "P", __func__);

	return 0;
}

void universal7270_aif4_shutdown(struct snd_pcm_substream *substream)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	struct snd_soc_card *card = rtd->card;

	dev_dbg(card->dev, "aif4: (%s) %s called\n",
			substream->stream ? "C" : "P", __func__);
}

static int universal7270_set_bias_level(struct snd_soc_card *card,
				 struct snd_soc_dapm_context *dapm,
				 enum snd_soc_bias_level level)
{
	struct snd_soc_dai *codec_dai = card->rtd[0].codec_dais[1];
	struct snd_soc_codec *codec = codec_dai->codec;
	int ret;

	if (dapm->dev != codec_dai->dev)
		return 0;

	switch (level) {
	case SND_SOC_BIAS_PREPARE:
		if (dapm->bias_level != SND_SOC_BIAS_STANDBY)
			break;
		pr_no_info("pll in audio codec is enabled\n");
		ret = snd_soc_codec_set_pll(codec, LARGO_FLL1,
					    ARIZONA_FLL_SRC_MCLK2,
					    32768,
					    SYSCLK_RATE);
		if (ret < 0)
			pr_err("Failed to start FLL: %d\n", ret);
		break;
	default:
		break;
	}

	return 0;
}

static int universal7270_set_bias_level_post(struct snd_soc_card *card,
				       struct snd_soc_dapm_context *dapm,
				       enum snd_soc_bias_level level)
{
	struct snd_soc_dai *codec_dai = card->rtd[0].codec_dais[1];
	struct snd_soc_codec *codec = codec_dai->codec;
	int ret;

	if (dapm->dev != codec_dai->dev)
		return 0;

	switch (level) {
	case SND_SOC_BIAS_STANDBY:
		pr_no_info("pll in audio codec is disabled\n");
		ret = snd_soc_codec_set_pll(codec, LARGO_FLL1, 0, 0, 0);
		if (ret < 0) {
			pr_err("Failed to stop FLL: %d\n", ret);
			return ret;
		}
		break;
	default:
		break;
	}

	dapm->bias_level = level;

	return 0;
}

static void universal7270_ez2c_trigger(void)
{
	pr_no_warn("Ez2Control Triggered\n");
}

static int universal7270_late_probe(struct snd_soc_card *card)
{

	struct snd_soc_dai *codec_dai = card->rtd[0].codec_dais[1];
	struct snd_soc_codec *codec = codec_dai->codec;
	int ret;

	ret = snd_soc_codec_set_sysclk(codec, ARIZONA_CLK_SYSCLK,
				       ARIZONA_CLK_SRC_FLL1,
				       SYSCLK_RATE,
				       SND_SOC_CLOCK_IN);
	if (ret != 0) {
		dev_err(codec->dev, "Failed to set SYSCLK: %d\n", ret);
		return ret;
	}

	ret = snd_soc_dai_set_sysclk(codec_dai, ARIZONA_CLK_SYSCLK, 0, 0);
	if (ret != 0) {
		dev_err(codec_dai->dev, "Failed to set AIF1 clock: %d\n", ret);
		return ret;
	}

	ret = snd_soc_codec_set_pll(codec, LARGO_FLL1_REFCLK,
			ARIZONA_FLL_SRC_NONE, 0, 0);
	if (ret < 0) {
		pr_err("Failed to clear FLL refclk: %d\n", ret);
		return ret;
	}

	arizona_set_ez2ctrl_cb(codec, universal7270_ez2c_trigger);

	return 0;
}

static int audmixer_init(struct snd_soc_component *cmp)
{
	dev_dbg(cmp->dev, "%s called\n", __func__);

	return 0;
}

static const struct snd_kcontrol_new universal7270_controls[] = {
};

const struct snd_soc_dapm_widget universal7270_dapm_widgets[] = {
	SND_SOC_DAPM_MIC("DMIC1", NULL),
};

const struct snd_soc_dapm_route universal7270_dapm_routes[] = {
	{ "DMIC1", NULL, "MICBIAS1" },
	{ "IN1L", NULL, "DMIC1" },
	{ "IN1R", NULL, "DMIC1" },
};

static struct snd_soc_ops universal7270_aif1_ops = {
	.hw_params = universal7270_aif1_hw_params,
	.startup = universal7270_aif1_startup,
	.shutdown = universal7270_aif1_shutdown,
};

static struct snd_soc_ops universal7270_aif2_ops = {
	.hw_params = universal7270_aif2_hw_params,
	.startup = universal7270_aif2_startup,
	.shutdown = universal7270_aif2_shutdown,
};

static struct snd_soc_ops universal7270_aif3_ops = {
	.startup = universal7270_aif3_startup,
	.shutdown = universal7270_aif3_shutdown,
};

static struct snd_soc_ops universal7270_aif4_ops = {
	.startup = universal7270_aif4_startup,
	.shutdown = universal7270_aif4_shutdown,
};

static struct snd_soc_dai_driver universal7270_ext_dai[] = {
	{
		.name = "universal7270 voice call",
		.playback = {
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
			.rates = (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
					SNDRV_PCM_RATE_48000),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
			 SNDRV_PCM_FMTBIT_S24_LE)
		},
		.capture = {
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
			.rates = (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
					SNDRV_PCM_RATE_48000),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
			 SNDRV_PCM_FMTBIT_S24_LE)
		},
	},
	{
		.name = "universal7270 BT",
		.playback = {
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
			.rates = (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
					SNDRV_PCM_RATE_48000),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
			 SNDRV_PCM_FMTBIT_S24_LE)
		},
		.capture = {
			.channels_min = 1,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
			.rates = (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
					SNDRV_PCM_RATE_48000),
			.formats = (SNDRV_PCM_FMTBIT_S16_LE |
			 SNDRV_PCM_FMTBIT_S24_LE)
		},
	},
	{
		.name = "universal7270 FM",
		.playback = {
			.channels_min = 2,
			.channels_max = 2,
			.rate_min = 8000,
			.rate_max = 48000,
			.rates = (SNDRV_PCM_RATE_8000 | SNDRV_PCM_RATE_16000 |
					SNDRV_PCM_RATE_48000),
			.formats = SNDRV_PCM_FMTBIT_S16_LE
		},
	},
};

static struct snd_soc_dai_link_component codecs_ap0[] = {{
		.name = "14880000.s1403x",
		.dai_name = "AP0",
	}, {
		.dai_name = "largo-aif1",
	},
};

static struct snd_soc_dai_link_component codecs_cp0[] = {{
		.name = "14880000.s1403x",
		.dai_name = "CP0",
	}, {
		.dai_name = "largo-aif1",
	},
};

static struct snd_soc_dai_link_component codecs_bt[] = {{
		.name = "14880000.s1403x",
		.dai_name = "BT",
	}, {
		.dai_name = "dummy-aif2",
	},
};

static struct snd_soc_dai_link_component codecs_fm[] = {{
		.name = "14880000.s1403x",
		.dai_name = "FM",
	}, {
		.dai_name = "largo-aif2",
	},
};

static struct snd_soc_dai_link universal7270_largo_dai[] = {
	/* Playback and Recording */
	{
		.name = "universal7270-largo",
		.stream_name = "i2s0-pri",
		.codecs = codecs_ap0,
		.num_codecs = ARRAY_SIZE(codecs_ap0),
		.ops = &universal7270_aif1_ops,
	},
	/* Deep buffer playback */
	{
		.name = "universal7270-largo-sec",
		.cpu_dai_name = "samsung-i2s-sec",
		.stream_name = "i2s0-sec",
		.platform_name = "samsung-i2s-sec",
		.codecs = codecs_ap0,
		.num_codecs = ARRAY_SIZE(codecs_ap0),
		.ops = &universal7270_aif1_ops,
	},
	/* Voice Call */
	{
		.name = "cp",
		.stream_name = "voice call",
		.cpu_dai_name = "universal7270 voice call",
		.platform_name = "snd-soc-dummy",
		.codecs = codecs_cp0,
		.num_codecs = ARRAY_SIZE(codecs_cp0),
		.ops = &universal7270_aif2_ops,
		.ignore_suspend = 1,
	},
	/* BT */
	{
		.name = "bt",
		.stream_name = "bluetooth audio",
		.cpu_dai_name = "universal7270 BT",
		.platform_name = "snd-soc-dummy",
		.codecs = codecs_bt,
		.num_codecs = ARRAY_SIZE(codecs_bt),
		.ops = &universal7270_aif3_ops,
		.ignore_suspend = 1,
	},
	/* FM */
	{
		.name = "fm",
		.stream_name = "FM audio",
		.cpu_dai_name = "universal7270 FM",
		.platform_name = "snd-soc-dummy",
		.codecs = codecs_fm,
		.num_codecs = ARRAY_SIZE(codecs_fm),
		.ops = &universal7270_aif4_ops,
		.ignore_suspend = 1,
	},
};

static struct snd_soc_aux_dev audmixer_aux_dev[] = {
	{
		.init = audmixer_init,
	},
};

static struct snd_soc_codec_conf audmixer_codec_conf[] = {
	{
		.name_prefix = "AudioMixer",
	},
};

static struct snd_soc_card universal7270_largo_card = {
	.name = "universal7270-I2S",
	.owner = THIS_MODULE,

	.dai_link = universal7270_largo_dai,
	.num_links = ARRAY_SIZE(universal7270_largo_dai),

	.controls = universal7270_controls,
	.num_controls = ARRAY_SIZE(universal7270_controls),
	.dapm_widgets = universal7270_dapm_widgets,
	.num_dapm_widgets = ARRAY_SIZE(universal7270_dapm_widgets),
	.dapm_routes = universal7270_dapm_routes,
	.num_dapm_routes = ARRAY_SIZE(universal7270_dapm_routes),

	.late_probe = universal7270_late_probe,
	.set_bias_level = universal7270_set_bias_level,
	.set_bias_level_post = universal7270_set_bias_level_post,
	.aux_dev = audmixer_aux_dev,
	.num_aux_devs = ARRAY_SIZE(audmixer_aux_dev),
	.codec_conf = audmixer_codec_conf,
	.num_configs = ARRAY_SIZE(audmixer_codec_conf),
};

static int universal7270_audio_probe(struct platform_device *pdev)
{
	int n, ret;
	struct device_node *np = pdev->dev.of_node;
	struct device_node *cpu_np, *codec_np, *auxdev_np;
	struct snd_soc_card *card = &universal7270_largo_card;

	if (!np) {
		dev_err(&pdev->dev, "Failed to get device node\n");
		return -EINVAL;
	}

	card->dev = &pdev->dev;
	card->num_links = 0;

	ret = snd_soc_register_component(card->dev, &universal7270_cmpnt,
			universal7270_ext_dai,
			ARRAY_SIZE(universal7270_ext_dai));
	if (ret) {
		dev_err(&pdev->dev, "Failed to register component: %d\n", ret);
		return ret;
	}

	for (n = 0; n < ARRAY_SIZE(universal7270_largo_dai); n++) {
		/* Skip parsing DT for fully formed dai links */
		if (universal7270_largo_dai[n].platform_name &&
				universal7270_largo_dai[n].codec_name) {
			dev_dbg(card->dev,
			"Skipping dt for populated dai link %s\n",
			universal7270_largo_dai[n].name);
			card->num_links++;
			continue;
		}

		cpu_np = of_parse_phandle(np, "samsung,audio-cpu", n);
		if (!cpu_np) {
			dev_err(&pdev->dev,
				"Property 'samsung,audio-cpu' missing\n");
			break;
		}

		codec_np = of_parse_phandle(np, "samsung,audio-codec", n);
		if (!codec_np) {
			dev_err(&pdev->dev,
				"Property 'samsung,audio-codec' missing\n");
			break;
		}

		universal7270_largo_dai[n].codecs[1].of_node = codec_np;
		if (!universal7270_largo_dai[n].cpu_dai_name)
			universal7270_largo_dai[n].cpu_of_node = cpu_np;
		if (!universal7270_largo_dai[n].platform_name)
			universal7270_largo_dai[n].platform_of_node = cpu_np;

		card->num_links++;
	}

	for (n = 0; n < ARRAY_SIZE(audmixer_aux_dev); n++) {
		auxdev_np = of_parse_phandle(np, "samsung,auxdev", n);
		if (!auxdev_np) {
			dev_err(&pdev->dev,
				"Property 'samsung,auxdev' missing\n");
			return -EINVAL;
		}

		audmixer_aux_dev[n].codec_of_node = auxdev_np;
		audmixer_codec_conf[n].of_node = auxdev_np;
	}

	ret = snd_soc_register_card(card);
	if (ret)
		dev_err(&pdev->dev, "Failed to register card:%d\n", ret);

	return ret;
}

static int universal7270_audio_remove(struct platform_device *pdev)
{
	struct snd_soc_card *card = platform_get_drvdata(pdev);

	snd_soc_unregister_card(card);

	return 0;
}

static const struct of_device_id universal7270_largo_of_match[] = {
	{.compatible = "samsung,universal7270-wm1831",},
};
MODULE_DEVICE_TABLE(of, universal7270_largo_of_match);

static struct platform_driver universal7270_audio_driver = {
	.driver = {
		.name = "universal7270-audio",
		.owner = THIS_MODULE,
		.pm = &snd_soc_pm_ops,
		.of_match_table = of_match_ptr(universal7270_largo_of_match),
	},
	.probe = universal7270_audio_probe,
	.remove = universal7270_audio_remove,
};

module_platform_driver(universal7270_audio_driver);

MODULE_DESCRIPTION("ALSA SoC universal7270 largo");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:universal7270-audio");
