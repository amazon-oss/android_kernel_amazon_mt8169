// SPDX-License-Identifier: GPL-2.0
//
// mt8169-tungsten.c  --  mt8169 tungsten ALSA SoC machine driver
//
// Copyright (c) 2021 MediaTek Inc.
// Author: Jiaxin Yu <jiaxin.yu@mediatek.com>
//
// Reconstructed from the Fire OS 8 mt8169-tungsten.ko (see re/audio/NOTES.md),
// using the MT8169 raspite machine driver of the same release as template.

#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#include "mt8169-afe-common.h"
#include "mt8169-afe-clk.h"
#include "mt8169-afe-gpio.h"
#include "mt8169-afe-utils.h"
#include "../common/mtk-afe-platform-driver.h"
#if IS_ENABLED(CONFIG_SND_SOC_MT6358_ACCDET)
#include "../../codecs/mt6358-accdet.h"
#endif

/*
 * if need additional control for the ext spk amp and ext hp amp that
 * is connected after Lineout Buffer / HP Buffer on the codec, put the
 * control in mt8169_tungsten_spk_amp_event() and
 * mt8169_tungsten_hp_amp_event()
 */
#define EXT_SPK_AMP_W_NAME "Ext_Speaker_Amp"
#define EXT_HP_AMP_W_NAME "Ext_Headphone_Amp"

#define MT8169_AFE_TDMIN_MCLK_RATIO 256
#define MT8169_AFE_I2S_MCLK_RATIO 128

/* speaker PA variants, selected by the spk-pa-id gpio */
enum {
	EXT_AMP_TPA2011 = 0,
	EXT_AMP_AW8011,
};

struct mt8169_tungsten_priv {
	struct gpio_desc *ext_amp1_gpio;
	struct gpio_desc *ext_amp2_gpio;
	struct gpio_desc *spk_pa_5v_en_gpio;
	struct gpio_desc *spk_pa_id_gpio;
	struct gpio_desc *apa_sdn_gpio;
	int spk_pa_id;
	unsigned int ext_spk_amp_vdd_on_time_us;
};

/* hostless paths that are started from the kernel (kcontrol driven) */
struct mt8169_hostless_data {
	struct afe_hostless_pcm_config config;
	struct snd_pcm_substream *substream;
	int enable;
	int dir;
	const char *dai_link_name;
	const char *kctl_name;
};

static struct mt8169_hostless_data hostless_datas[] = {
	{
		.config = {
			.channels = 2,
			.rate = 48000,
			.format = 32,
			.period_size = 768,
			.period_count = 4,
		},
		.dir = SNDRV_PCM_STREAM_PLAYBACK,
		.dai_link_name = "Hostless_SRC_1",
		.kctl_name = "MT8169_GSRC1_PLAYBACK_Enable",
	},
	{
		.config = {
			.channels = 2,
			.rate = 48000,
			.format = 32,
			.period_size = 768,
			.period_count = 4,
		},
		.dir = SNDRV_PCM_STREAM_CAPTURE,
		.dai_link_name = "Hostless_SRC_1",
		.kctl_name = "MT8169_GSRC1_CAPTURE_Enable",
	},
};

static const char *const mt8169_spk_type_str[] = {MTK_SPK_NOT_SMARTPA_STR};
static const char *const mt8169_spk_i2s_type_str[] = {MTK_SPK_I2S_0_STR,
						      MTK_SPK_I2S_1_STR,
						      MTK_SPK_I2S_2_STR,
						      MTK_SPK_I2S_3_STR,
						      MTK_SPK_I2S_5_STR};

static const struct soc_enum mt8169_spk_type_enum[] = {
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(mt8169_spk_type_str),
			    mt8169_spk_type_str),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(mt8169_spk_i2s_type_str),
			    mt8169_spk_i2s_type_str),
};

static int mt8169_spk_type_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	int idx = MTK_SPK_NOT_SMARTPA;

	pr_debug("%s() = %d\n", __func__, idx);
	ucontrol->value.integer.value[0] = idx;
	return 0;
}

static int mt8169_spk_i2s_out_type_get(struct snd_kcontrol *kcontrol,
				       struct snd_ctl_elem_value *ucontrol)
{
	int idx = MTK_SPK_I2S_3;

	pr_debug("%s() = %d\n", __func__, idx);
	ucontrol->value.integer.value[0] = idx;
	return 0;
}

static int mt8169_spk_i2s_in_type_get(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	int idx = MTK_SPK_I2S_0;

	pr_debug("%s() = %d\n", __func__, idx);
	ucontrol->value.integer.value[0] = idx;
	return 0;
}

static int mt8169_hostless_index(const char *kctl_name)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(hostless_datas); i++)
		if (!strcmp(kctl_name, hostless_datas[i].kctl_name))
			return i;

	return -EINVAL;
}

static int mt8169_hostless_enable(struct snd_soc_card *card,
				  struct mt8169_hostless_data *data,
				  int enable)
{
	struct snd_soc_pcm_runtime *rtd;
	int ret;

	rtd = snd_soc_get_pcm_runtime(card, data->dai_link_name);
	if (!rtd) {
		dev_info(card->dev, "%s() runtime is NULL\n", __func__);
		return -ENODEV;
	}

	if (data->enable == enable) {
		dev_info(card->dev, "already set %s enable:%d\n",
			 data->dai_link_name, enable);
		return 0;
	}

	if (enable) {
		dev_info(card->dev, "dai_link:%s, dir:%d, channel:%d, rate:%d\n",
			 data->dai_link_name, data->dir,
			 data->config.channels, data->config.rate);
		ret = afe_hostless_enable(rtd->pcm, &data->substream,
					  &data->config, data->dir);
		if (ret)
			return ret;
	} else {
		afe_hostless_disable(&data->substream);
	}

	data->enable = enable;
	dev_info(card->dev, "%s enable:%d\n", data->dai_link_name, enable);

	return 0;
}

static int mt8169_hostless_get(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	int idx = mt8169_hostless_index(ucontrol->id.name);

	if (idx < 0)
		return idx;

	ucontrol->value.integer.value[0] = hostless_datas[idx].enable;
	return 0;
}

static int mt8169_hostless_put(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	int enable = ucontrol->value.integer.value[0];
	int idx = mt8169_hostless_index(ucontrol->id.name);

	if (idx < 0)
		return idx;

	return mt8169_hostless_enable(card, &hostless_datas[idx], enable);
}

static int mt8169_afe_dl12_playback_state_get(struct snd_kcontrol *kcontrol,
					      struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct snd_soc_pcm_runtime *rtd = NULL;
	enum snd_soc_dpcm_state state;

	rtd = snd_soc_get_pcm_runtime(card, "Primary Codec");
	if (!rtd) {
		ucontrol->value.integer.value[0] = 0;
		dev_dbg(card->dev, "%s rtd get fail\n", __func__);
		return 0;
	}

	state = snd_soc_dpcm_be_get_state(rtd, SNDRV_PCM_STREAM_PLAYBACK);
	dev_dbg(card->dev, "[%s] dpcm state:%d\n", __func__, state);
	if (state >= SND_SOC_DPCM_STATE_NEW && state <= SND_SOC_DPCM_STATE_START)
		ucontrol->value.integer.value[0] = 1;
	else
		ucontrol->value.integer.value[0] = 0;

	dev_dbg(card->dev, "%s+ enable %ld\n", __func__,
		ucontrol->value.integer.value[0]);

	return 0;
}

static void mt8169_tungsten_ext_spk_amp_turn_on(struct snd_soc_card *card)
{
	struct mt8169_tungsten_priv *priv = snd_soc_card_get_drvdata(card);

	if (!IS_ERR(priv->spk_pa_5v_en_gpio) &&
	    (priv->spk_pa_id == EXT_AMP_TPA2011 ||
	     priv->spk_pa_id == EXT_AMP_AW8011)) {
		gpiod_set_value_cansleep(priv->spk_pa_5v_en_gpio, 1);
		usleep_range(priv->ext_spk_amp_vdd_on_time_us,
			     priv->ext_spk_amp_vdd_on_time_us + 1);

		if (IS_ERR(priv->ext_amp1_gpio) || IS_ERR(priv->ext_amp2_gpio)) {
			dev_err(card->dev, "audio ext amp1 and ext amp2 gpio invalid\n");
			goto exit;
		}

		gpiod_set_value_cansleep(priv->ext_amp1_gpio, 1);
		gpiod_set_value_cansleep(priv->ext_amp2_gpio, 1);
		if (priv->spk_pa_id == EXT_AMP_TPA2011)
			dev_info(card->dev, "audio tpa2011 spk amp enable\n");
		else
			dev_info(card->dev, "audio aw8011 spk amp enable\n");
	} else {
		dev_err(card->dev, "audio spk pa en gpio invalid\n");
	}

	return;
exit:
	gpiod_set_value_cansleep(priv->spk_pa_5v_en_gpio, 0);
	usleep_range(priv->ext_spk_amp_vdd_on_time_us,
		     priv->ext_spk_amp_vdd_on_time_us + 1);
}

static void mt8169_tungsten_ext_spk_amp_turn_off(struct snd_soc_card *card)
{
	struct mt8169_tungsten_priv *priv = snd_soc_card_get_drvdata(card);

	if (!IS_ERR(priv->spk_pa_5v_en_gpio) &&
	    (priv->spk_pa_id == EXT_AMP_TPA2011 ||
	     priv->spk_pa_id == EXT_AMP_AW8011)) {
		if (!IS_ERR(priv->ext_amp1_gpio) && !IS_ERR(priv->ext_amp2_gpio)) {
			gpiod_set_value_cansleep(priv->ext_amp1_gpio, 0);
			gpiod_set_value_cansleep(priv->ext_amp2_gpio, 0);
		} else {
			dev_err(card->dev, "audio ext amp1 and ext amp2 gpio invalid\n");
		}
		gpiod_set_value_cansleep(priv->spk_pa_5v_en_gpio, 0);
	} else {
		dev_err(card->dev, "audio spk pa en gpio invalid\n");
	}
}

static int mt8169_tungsten_spk_amp_event(struct snd_soc_dapm_widget *w,
					 struct snd_kcontrol *kcontrol,
					 int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;

	dev_info(card->dev, "audio event %d\n", event);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		/* spk amp on control */
		mt8169_tungsten_ext_spk_amp_turn_on(card);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		/* spk amp off control */
		mt8169_tungsten_ext_spk_amp_turn_off(card);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

/* HP Amp */
static int mt8169_tungsten_hp_amp_event(struct snd_soc_dapm_widget *w,
					struct snd_kcontrol *kcontrol,
					int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;
	struct mt8169_tungsten_priv *priv = snd_soc_card_get_drvdata(card);

	dev_info(card->dev, "audio event %d\n", event);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		if (!IS_ERR(priv->apa_sdn_gpio))
			gpiod_set_value_cansleep(priv->apa_sdn_gpio, 1);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		if (!IS_ERR(priv->apa_sdn_gpio))
			gpiod_set_value_cansleep(priv->apa_sdn_gpio, 0);
		break;
	default:
		break;
	}

	return 0;
}

static const struct snd_soc_dapm_widget mt8169_tungsten_widgets[] = {
	SND_SOC_DAPM_SPK(EXT_SPK_AMP_W_NAME, mt8169_tungsten_spk_amp_event),
	SND_SOC_DAPM_HP(EXT_HP_AMP_W_NAME, mt8169_tungsten_hp_amp_event),
};

static const struct snd_soc_dapm_route mt8169_tungsten_routes[] = {
	{EXT_SPK_AMP_W_NAME, NULL, "LINEOUT L"},
	{EXT_SPK_AMP_W_NAME, NULL, "LINEOUT L HSSPK"},
	{EXT_SPK_AMP_W_NAME, NULL, "Headphone L Ext Spk Amp"},
	{EXT_SPK_AMP_W_NAME, NULL, "Headphone R Ext Spk Amp"},
	{EXT_HP_AMP_W_NAME, NULL, "Headphone L"},
	{EXT_HP_AMP_W_NAME, NULL, "Headphone R"},
};

static const struct snd_kcontrol_new mt8169_tungsten_controls[] = {
	SOC_DAPM_PIN_SWITCH(EXT_SPK_AMP_W_NAME),
	SOC_DAPM_PIN_SWITCH(EXT_HP_AMP_W_NAME),
	SOC_ENUM_EXT("MTK_SPK_TYPE_GET", mt8169_spk_type_enum[0],
		     mt8169_spk_type_get, NULL),
	SOC_ENUM_EXT("MTK_SPK_I2S_OUT_TYPE_GET", mt8169_spk_type_enum[1],
		     mt8169_spk_i2s_out_type_get, NULL),
	SOC_ENUM_EXT("MTK_SPK_I2S_IN_TYPE_GET", mt8169_spk_type_enum[1],
		     mt8169_spk_i2s_in_type_get, NULL),
	SOC_SINGLE_BOOL_EXT("MT8169_GSRC1_PLAYBACK_Enable", 0,
			    mt8169_hostless_get, mt8169_hostless_put),
	SOC_SINGLE_BOOL_EXT("MT8169_GSRC1_CAPTURE_Enable", 0,
			    mt8169_hostless_get, mt8169_hostless_put),
	SOC_SINGLE_BOOL_EXT("DL_Playback_State", 0,
			    mt8169_afe_dl12_playback_state_get, NULL),
};

/* BE i2s need assign snd_soc_ops = mt8169_tungsten_i2s_ops */
static int mt8169_tungsten_i2s_hw_params(struct snd_pcm_substream *substream,
					 struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	unsigned int rate = params_rate(params);
	unsigned int mclk_fs_ratio = MT8169_AFE_I2S_MCLK_RATIO;
	unsigned int mclk_fs = rate * mclk_fs_ratio;

	return snd_soc_dai_set_sysclk(rtd->cpu_dai,
				      0, mclk_fs, SND_SOC_CLOCK_OUT);
}

static const struct snd_soc_ops mt8169_tungsten_i2s_ops = {
	.hw_params = mt8169_tungsten_i2s_hw_params,
};

/* BE tdm need assign snd_soc_ops = mt8169_tungsten_tdm_ops */
static int mt8169_tungsten_tdm_hw_params(struct snd_pcm_substream *substream,
					 struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	unsigned int rate = params_rate(params);
	unsigned int mclk_fs_ratio = MT8169_AFE_TDMIN_MCLK_RATIO;
	unsigned int mclk_fs = rate * mclk_fs_ratio;

	return snd_soc_dai_set_sysclk(rtd->cpu_dai,
				      0, mclk_fs, SND_SOC_CLOCK_OUT);
}

static const struct snd_soc_ops mt8169_tungsten_tdm_ops = {
	.hw_params = mt8169_tungsten_tdm_hw_params,
};

static int mt8169_tungsten_init(struct snd_soc_pcm_runtime *rtd)
{
	struct snd_soc_dapm_context *dapm = &rtd->card->dapm;
	int ret;

	/* disable ext amp connection */
	ret = snd_soc_dapm_disable_pin(dapm, EXT_SPK_AMP_W_NAME);
	if (ret) {
		dev_info(rtd->dev, "failed to disable %s: %d\n",
			 EXT_SPK_AMP_W_NAME, ret);
		return ret;
	}

	/* disable ext headphone amp connection */
	ret = snd_soc_dapm_disable_pin(dapm, EXT_HP_AMP_W_NAME);
	if (ret) {
		dev_info(rtd->dev, "failed to disable %s: %d\n",
			 EXT_HP_AMP_W_NAME, ret);
		return ret;
	}

	ret = snd_soc_dapm_sync(dapm);
	if (ret) {
		dev_info(rtd->dev, "failed to snd_soc_dapm_sync\n");
		return ret;
	}

	return 0;
}

static int mt8169_i2s_hw_params_fixup(struct snd_soc_pcm_runtime *rtd,
				      struct snd_pcm_hw_params *params)
{
	struct snd_interval *channels = hw_param_interval(params,
		SNDRV_PCM_HW_PARAM_CHANNELS);
	dev_info(rtd->dev, "%s(), fix format to 32bit\n", __func__);

	/* fix BE i2s channel to 2 channel */
	channels->min = channels->max = 2;

	/* fix BE i2s format to 32bit, clean param mask first */
	snd_mask_reset_range(hw_param_mask(params, SNDRV_PCM_HW_PARAM_FORMAT),
			     0, SNDRV_PCM_FORMAT_LAST);

	params_set_format(params, SNDRV_PCM_FORMAT_S32_LE);

	return 0;
}

/* FE */
SND_SOC_DAILINK_DEFS(playback1,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL1")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback12,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL12")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback2,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL2")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback3,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL3")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback4,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL4")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback5,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL5")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback6,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL6")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback7,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL7")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(playback8,
		     DAILINK_COMP_ARRAY(COMP_CPU("DL8")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture1,
		     DAILINK_COMP_ARRAY(COMP_CPU("UL1")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture2,
		     DAILINK_COMP_ARRAY(COMP_CPU("UL2")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture3,
		     DAILINK_COMP_ARRAY(COMP_CPU("UL3")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture4,
		     DAILINK_COMP_ARRAY(COMP_CPU("UL4")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture5,
		     DAILINK_COMP_ARRAY(COMP_CPU("UL5")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture6,
		     DAILINK_COMP_ARRAY(COMP_CPU("UL6")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

SND_SOC_DAILINK_DEFS(capture7,
		     DAILINK_COMP_ARRAY(COMP_CPU("UL7")),
		     DAILINK_COMP_ARRAY(COMP_DUMMY()),
		     DAILINK_COMP_ARRAY(COMP_EMPTY()));

/* hostless */
SND_SOC_DAILINK_DEFS(hostless_lpbk,
	DAILINK_COMP_ARRAY(COMP_CPU("Hostless LPBK DAI")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(hostless_fm,
	DAILINK_COMP_ARRAY(COMP_CPU("Hostless FM DAI")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(hostless_src1,
	DAILINK_COMP_ARRAY(COMP_CPU("Hostless_SRC_1_DAI")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(hostless_src_bargein,
	DAILINK_COMP_ARRAY(COMP_CPU("Hostless_SRC_Bargein_DAI")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

/* BE */
SND_SOC_DAILINK_DEFS(adda,
	DAILINK_COMP_ARRAY(COMP_CPU("ADDA")),
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(i2s0,
	DAILINK_COMP_ARRAY(COMP_CPU("I2S0")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(i2s1,
	DAILINK_COMP_ARRAY(COMP_CPU("I2S1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(i2s2,
	DAILINK_COMP_ARRAY(COMP_CPU("I2S2")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(i2s3,
	DAILINK_COMP_ARRAY(COMP_CPU("I2S3")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(hw_gain1,
	DAILINK_COMP_ARRAY(COMP_CPU("HW Gain 1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(hw_gain2,
	DAILINK_COMP_ARRAY(COMP_CPU("HW Gain 2")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(hw_src1,
	DAILINK_COMP_ARRAY(COMP_CPU("HW_SRC_1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(hw_src2,
	DAILINK_COMP_ARRAY(COMP_CPU("HW_SRC_2")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(connsys_i2s,
	DAILINK_COMP_ARRAY(COMP_CPU("CONNSYS_I2S")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(pcm1,
	DAILINK_COMP_ARRAY(COMP_CPU("PCM 1")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(tdm_in,
	DAILINK_COMP_ARRAY(COMP_CPU("TDM IN")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));

/* hostless */
SND_SOC_DAILINK_DEFS(hostless_ul1,
	DAILINK_COMP_ARRAY(COMP_CPU("Hostless_UL1 DAI")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(hostless_ul2,
	DAILINK_COMP_ARRAY(COMP_CPU("Hostless_UL2 DAI")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(hostless_ul3,
	DAILINK_COMP_ARRAY(COMP_CPU("Hostless_UL3 DAI")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(hostless_ul5,
	DAILINK_COMP_ARRAY(COMP_CPU("Hostless_UL5 DAI")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(hostless_ul6,
	DAILINK_COMP_ARRAY(COMP_CPU("Hostless_UL6 DAI")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(hostless_hw_gain_aaudio,
	DAILINK_COMP_ARRAY(COMP_CPU("Hostless HW Gain AAudio DAI")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(hostless_src_aaudio,
	DAILINK_COMP_ARRAY(COMP_CPU("Hostless SRC AAudio DAI")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
#if IS_ENABLED(CONFIG_SND_SOC_MTK_BTCVSD)
SND_SOC_DAILINK_DEFS(btcvsd,
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_PLATFORM("18050000.mtk-btcvsd-snd")));
#endif
static struct snd_soc_dai_link mt8169_tungsten_dai_links[] = {
	/* Front End DAI links */
	{
		.name = "Playback_1",
		.stream_name = "Playback_1",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(playback1),
	},
	{
		.name = "Playback_12",
		.stream_name = "Playback_12",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(playback12),
	},
	{
		.name = "Playback_2",
		.stream_name = "Playback_2",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(playback2),
	},
	{
		.name = "Playback_3",
		.stream_name = "Playback_3",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(playback3),
	},
	{
		.name = "Playback_4",
		.stream_name = "Playback_4",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(playback4),
	},
	{
		.name = "Playback_5",
		.stream_name = "Playback_5",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(playback5),
	},
	{
		.name = "Playback_6",
		.stream_name = "Playback_6",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(playback6),
	},
	{
		.name = "Playback_7",
		.stream_name = "Playback_7",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(playback7),
	},
	{
		.name = "Playback_8",
		.stream_name = "Playback_8",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(playback8),
	},
	{
		.name = "Capture_1",
		.stream_name = "Capture_1",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(capture1),
	},
	{
		.name = "Capture_2",
		.stream_name = "Capture_2",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(capture2),
	},
	{
		.name = "Capture_3",
		.stream_name = "Capture_3",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(capture3),
	},
	{
		.name = "Capture_4",
		.stream_name = "Capture_4",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(capture4),
	},
	{
		.name = "Capture_5",
		.stream_name = "Capture_5",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(capture5),
	},
	{
		.name = "Capture_6",
		.stream_name = "Capture_6",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(capture6),
	},
	{
		.name = "Capture_7",
		.stream_name = "Capture_7",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(capture7),
	},
	{
		.name = "Hostless_LPBK",
		.stream_name = "Hostless_LPBK",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hostless_lpbk),
	},
	{
		.name = "Hostless_FM",
		.stream_name = "Hostless_FM",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hostless_fm),
	},
	{
		.name = "Hostless_SRC_1",
		.stream_name = "Hostless_SRC_1",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		/* opened from the kernel by mt8169_hostless_enable() */
		.no_host_mode = SND_SOC_DAI_LINK_NO_HOST,
		SND_SOC_DAILINK_REG(hostless_src1),
	},
	{
		.name = "Hostless_SRC_Bargein",
		.stream_name = "Hostless_SRC_Bargein",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hostless_src_bargein),
	},
	/* Back End DAI links */
	{
		.name = "Primary Codec",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		.init = mt8169_tungsten_init,
		SND_SOC_DAILINK_REG(adda),
	},
	{
		.name = "I2S3",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.ignore_suspend = 1,
		.be_hw_params_fixup = mt8169_i2s_hw_params_fixup,
		.ops = &mt8169_tungsten_i2s_ops,
		SND_SOC_DAILINK_REG(i2s3),
	},
	{
		.name = "I2S0",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		.be_hw_params_fixup = mt8169_i2s_hw_params_fixup,
		.ops = &mt8169_tungsten_i2s_ops,
		SND_SOC_DAILINK_REG(i2s0),
	},
	{
		.name = "I2S1",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.ignore_suspend = 1,
		.be_hw_params_fixup = mt8169_i2s_hw_params_fixup,
		.ops = &mt8169_tungsten_i2s_ops,
		SND_SOC_DAILINK_REG(i2s1),
	},
	{
		.name = "I2S2",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		.be_hw_params_fixup = mt8169_i2s_hw_params_fixup,
		.ops = &mt8169_tungsten_i2s_ops,
		SND_SOC_DAILINK_REG(i2s2),
	},
	{
		.name = "HW Gain 1",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hw_gain1),
	},
	{
		.name = "HW Gain 2",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hw_gain2),
	},
	{
		.name = "HW_SRC_1",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hw_src1),
	},
	{
		.name = "HW_SRC_2",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hw_src2),
	},
	{
		.name = "CONNSYS_I2S",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(connsys_i2s),
	},
	{
		.name = "PCM 1",
		.dai_fmt = SND_SOC_DAIFMT_I2S |
			   SND_SOC_DAIFMT_IB_IF,
		.no_pcm = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(pcm1),
	},
	{
		.name = "TDM IN",
		.dai_fmt = SND_SOC_DAIFMT_I2S |
			   SND_SOC_DAIFMT_IB_NF |
			   SND_SOC_DAIFMT_CBS_CFS,
		.no_pcm = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		.ops = &mt8169_tungsten_tdm_ops,
		SND_SOC_DAILINK_REG(tdm_in),
	},
	/* dummy BE for ul memif to record from dl memif */
	{
		.name = "Hostless_UL1",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hostless_ul1),
	},
	{
		.name = "Hostless_UL2",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hostless_ul2),
	},
	{
		.name = "Hostless_UL3",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hostless_ul3),
	},
	{
		.name = "Hostless_UL5",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hostless_ul5),
	},
	{
		.name = "Hostless_UL6",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hostless_ul6),
	},
	{
		.name = "Hostless_HW_Gain_AAudio",
		.stream_name = "Hostless_HW_Gain_AAudio",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hostless_hw_gain_aaudio),
	},
	{
		.name = "Hostless_SRC_AAudio",
		.stream_name = "Hostless_SRC_AAudio",
		.trigger = {SND_SOC_DPCM_TRIGGER_POST,
			    SND_SOC_DPCM_TRIGGER_POST},
		.dynamic = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		SND_SOC_DAILINK_REG(hostless_src_aaudio),
	},
	/* BTCVSD */
#if IS_ENABLED(CONFIG_SND_SOC_MTK_BTCVSD)
	{
		.name = "BTCVSD",
		.stream_name = "BTCVSD",
		SND_SOC_DAILINK_REG(btcvsd),
	},
#endif
};

static void mt8169_tungsten_gpio_probe(struct snd_soc_card *card)
{
	struct mt8169_tungsten_priv *priv = snd_soc_card_get_drvdata(card);
	int ret;

	priv->ext_amp1_gpio = devm_gpiod_get(card->dev, "ext_amp1",
					     GPIOD_OUT_LOW);
	if (IS_ERR(priv->ext_amp1_gpio)) {
		ret = PTR_ERR(priv->ext_amp1_gpio);
		dev_info(card->dev, "failed to get ext_amp1 gpio: %d\n", ret);
	}

	priv->ext_amp2_gpio = devm_gpiod_get(card->dev, "ext_amp2",
					     GPIOD_OUT_LOW);
	if (IS_ERR(priv->ext_amp2_gpio)) {
		ret = PTR_ERR(priv->ext_amp2_gpio);
		dev_info(card->dev, "failed to get ext_amp2 gpio: %d\n", ret);
	}

	priv->spk_pa_5v_en_gpio = devm_gpiod_get(card->dev, "spk-pa-5v-en",
						 GPIOD_OUT_LOW);
	if (IS_ERR(priv->spk_pa_5v_en_gpio)) {
		ret = PTR_ERR(priv->spk_pa_5v_en_gpio);
		dev_info(card->dev, "failed to get spk_pa_5v_en_gpio: %d\n",
			 ret);
	}

	priv->apa_sdn_gpio = devm_gpiod_get(card->dev, "apa-sdn",
					    GPIOD_OUT_LOW);
	if (IS_ERR(priv->apa_sdn_gpio)) {
		ret = PTR_ERR(priv->apa_sdn_gpio);
		dev_info(card->dev, "failed to get apa_sdn_gpio: %d\n", ret);
	}

	priv->spk_pa_id_gpio = devm_gpiod_get(card->dev, "spk-pa-id",
					      GPIOD_IN);
	if (IS_ERR(priv->spk_pa_id_gpio)) {
		ret = PTR_ERR(priv->spk_pa_id_gpio);
		dev_info(card->dev, "failed to get spk_pa_id_gpio: %d\n", ret);
		priv->spk_pa_id = EXT_AMP_TPA2011;
	} else if (gpiod_get_value_cansleep(priv->spk_pa_id_gpio)) {
		priv->spk_pa_id = EXT_AMP_AW8011;
	} else {
		priv->spk_pa_id = EXT_AMP_TPA2011;
	}
}

static void mt8169_tungsten_parse_of(struct snd_soc_card *card,
				     struct device_node *np)
{
	struct mt8169_tungsten_priv *priv = snd_soc_card_get_drvdata(card);
	int ret;

	ret = of_property_read_u32(np, "mediatek,ext-spk-amp-vdd-on-time-us",
				   &priv->ext_spk_amp_vdd_on_time_us);
	if (ret < 0)
		dev_info(card->dev,
			 "failed to get ext_spk_amp_vdd_on_time_us: %d\n", ret);
}

static struct snd_soc_card mt8169_tungsten_soc_card = {
	.name = "mt8169-tungsten",
	.owner = THIS_MODULE,
	.dai_link = mt8169_tungsten_dai_links,
	.num_links = ARRAY_SIZE(mt8169_tungsten_dai_links),

	.controls = mt8169_tungsten_controls,
	.num_controls = ARRAY_SIZE(mt8169_tungsten_controls),
	.dapm_widgets = mt8169_tungsten_widgets,
	.num_dapm_widgets = ARRAY_SIZE(mt8169_tungsten_widgets),
	.dapm_routes = mt8169_tungsten_routes,
	.num_dapm_routes = ARRAY_SIZE(mt8169_tungsten_routes),
};

#if IS_ENABLED(CONFIG_SND_SOC_MT6358_ACCDET)
static int mt8169_mt6358_headset_init(struct snd_soc_component *component)
{
	struct snd_soc_card *card = &mt8169_tungsten_soc_card;

	return mt6358_accdet_init(component, card);
}

static struct snd_soc_aux_dev mt8169_mt6358_headset_dev = {
	.dlc = COMP_EMPTY(),
	.init = mt8169_mt6358_headset_init,
};
#endif

static int mt8169_tungsten_dev_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &mt8169_tungsten_soc_card;
	struct device_node *platform_node, *codec_node, *dsp_node;
	struct snd_soc_dai_link *dai_link;
	struct mt8169_tungsten_priv *priv;
	int ret, i;

	dev_info(&pdev->dev, "audio mt8169_dev_probe\n");

	platform_node = of_parse_phandle(pdev->dev.of_node,
					 "mediatek,platform", 0);
	if (!platform_node) {
		dev_info(&pdev->dev, "Property 'platform' missing or invalid\n");
		return -EINVAL;
	}

	dsp_node = of_parse_phandle(pdev->dev.of_node,
				    "mediatek,adsp-platform", 0);
	if (!dsp_node)
		dev_info(&pdev->dev, "Property 'adsp-platform' missing or invalid\n");

	codec_node = of_get_child_by_name(pdev->dev.of_node,
					  "mediatek,audio-codec");
	if (!codec_node) {
		dev_info(&pdev->dev,
			 "codec_node of_get_child_by_name fail\n");
		/* do not block machine driver register */
	}

	dev_info(&pdev->dev, "audio update audio-codec dai\n");
	for_each_card_prelinks(card, i, dai_link) {
		if (dai_link->codecs && dai_link->codecs->name)
			continue;

		if (codec_node) {
			ret = snd_soc_of_get_dai_link_codecs(&pdev->dev,
							     codec_node,
							     dai_link);
			if (ret < 0) {
				dev_info(&pdev->dev,
					 "Speaker Codec get_dai_link fail\n");
				return -EINVAL;
			}
		}
	}

	dev_info(&pdev->dev, "audio update platform dai\n");
	for_each_card_prelinks(card, i, dai_link) {
		if (dai_link->platforms->name)
			continue;
		if (dsp_node &&
		    !strncmp(dai_link->name, "ADSP", strlen("ADSP"))) {
			dai_link->platforms->of_node = dsp_node;
			continue;
		}
		dai_link->platforms->of_node = platform_node;
	}

	card->dev = &pdev->dev;
#if IS_ENABLED(CONFIG_SND_SOC_MT6358_ACCDET)
	mt8169_mt6358_headset_dev.dlc.of_node =
		of_parse_phandle(pdev->dev.of_node,
				 "mediatek,headset-codec", 0);
	if (mt8169_mt6358_headset_dev.dlc.of_node) {
		card->aux_dev = &mt8169_mt6358_headset_dev;
		card->num_aux_devs = 1;
	} else
		dev_info(&pdev->dev,
			 "Property 'mediatek,headset-codec' missing/invalid\n");
	mt6358_accdet_set_drvdata(card);
#endif

	priv = devm_kzalloc(&pdev->dev, sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	snd_soc_card_set_drvdata(card, priv);

	mt8169_tungsten_gpio_probe(card);

	mt8169_tungsten_parse_of(card, pdev->dev.of_node);

	/* init gpio */
	ret = mt8169_afe_gpio_init(&pdev->dev);
	if (ret)
		dev_info(&pdev->dev, "init gpio error\n");

	dev_info(&pdev->dev, "audio devm_snd_soc_register_card\n");
	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret)
		dev_info(&pdev->dev, "audio snd_soc_register_card fail %d\n",
			 ret);

	return ret;
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id mt8169_tungsten_dt_match[] = {
	{.compatible = "mediatek,mt8169-tungsten-sound",},
	{}
};
#endif

static const struct dev_pm_ops mt8169_tungsten_pm_ops = {
	.poweroff = snd_soc_poweroff,
	.restore = snd_soc_resume,
};

static struct platform_driver mt8169_tungsten_driver = {
	.driver = {
		.name = "mt8169-tungsten",
#if IS_ENABLED(CONFIG_OF)
		.of_match_table = mt8169_tungsten_dt_match,
#endif
		.pm = &mt8169_tungsten_pm_ops,
	},
	.probe = mt8169_tungsten_dev_probe,
};

module_platform_driver(mt8169_tungsten_driver);

/* Module information */
MODULE_DESCRIPTION("MT8169 tungsten ALSA SoC machine driver");
MODULE_AUTHOR("Jiaxin Yu <jiaxin.yu@mediatek.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("mt8169 tungsten soc card");
