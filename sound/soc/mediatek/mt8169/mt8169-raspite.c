// SPDX-License-Identifier: GPL-2.0
//
// mt8169-raspite.c  --  mt8169 raspite ALSA SoC machine driver
//
// Copyright (c) 2021 MediaTek Inc.
// Author: Jiaxin Yu <jiaxin.yu@mediatek.com>

#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>

#if IS_ENABLED(CONFIG_SND_SOC_DC_DETECTION_SUPPORT)
#include <linux/of_gpio.h>
#include <linux/regulator/consumer.h>
#endif

#include "mt8169-afe-common.h"
#include "mt8169-afe-clk.h"
#include "mt8169-afe-gpio.h"
#include "../common/mtk-afe-platform-driver.h"
#if IS_ENABLED(CONFIG_SND_SOC_MT6366)
#include "../../codecs/mt6358.h"
#elif IS_ENABLED(CONFIG_SND_SOC_MT6357)
#include "../../codecs/mt6357.h"
#endif

#if IS_ENABLED(CONFIG_SND_SOC_MT6357_ACCDET)
#include "../../codecs/mt6357-accdet.h"
#endif
#if IS_ENABLED(CONFIG_SND_SOC_MT6358_ACCDET)
#include "../../codecs/mt6358-accdet.h"
#endif

#if IS_ENABLED(CONFIG_AMAZON_MINERVA_METRICS_LOG)
#include <linux/metricslog.h>
#define METRICS_LOG_MAX_SIZE (512)
#endif

/*
 * if need additional control for the ext spk amp and ext hp amp that
 * is connected after Lineout Buffer / HP Buffer on the codec, put the
 * control in mt8169_ras_spk_amp_event() and mt8169_ras_hp_amp_event()
 */
#define EXT_SPK_AMP_W_NAME "Ext_Speaker_Amp"
#define EXT_HP_AMP_W_NAME "Ext_Headphone_Amp"

#define MT8169_AFE_TDMIN_MCLK_RATIO 256
#define MT8169_AFE_I2S_MCLK_RATIO 128

#if IS_ENABLED(CONFIG_SND_SOC_DC_DETECTION_SUPPORT)
static struct platform_driver mt8169_raspite_driver;
extern struct pinctrl *aud_pinctrl;
#endif

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

static unsigned int spkr_gain;

static const char * const EXT_SPK_PGA_GAIN[] = {"0dB", "6dB", "9.5dB", "18dB", "21.6dB", "24dB"};

static const struct soc_enum mt8169_raspite_enum[] = {
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(EXT_SPK_PGA_GAIN), EXT_SPK_PGA_GAIN),
};

extern int aw87xxx_set_profile(int dev_index, char *profile);
static char *aw_profile[] = {"Music", "Off"};
enum aw87xxx_dev_index {
	AW_DEV_0 = 0,
	AW_DEV_1,
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

static int mt8169_ras_ext_speaker_pga_get(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	ucontrol->value.integer.value[0] = spkr_gain;
	return 0;
}

static int mt8169_ras_ext_speaker_pga_put(struct snd_kcontrol *kcontrol,
	struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct mt8169_raspite_priv *priv = snd_soc_card_get_drvdata(card);

	if (priv->spk_pa_id == EXT_AMP_TPA2011 ||
		priv->spk_pa_id == EXT_AMP_AD51562) {
		dev_info(card->dev, "%s, speaker amp not support setting\n", __func__);
		return -EINVAL;
	}

	if (ucontrol->value.enumerated.item[0] >= ARRAY_SIZE(EXT_SPK_PGA_GAIN)) {
		dev_err(card->dev, "%s value set error\n", __func__);
		return -EINVAL;
	}

	spkr_gain = ucontrol->value.integer.value[0];

	dev_dbg(card->dev, "%s spkr_gain %d\n", __func__, spkr_gain);

	return 0;
}

static int mt8169_afe_dl1_playback_state_get(struct snd_kcontrol *kcontrol,
					     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_card *card = snd_kcontrol_chip(kcontrol);
	struct snd_soc_pcm_runtime *rtd = NULL;
	enum snd_soc_dpcm_state state;

	rtd = snd_soc_get_pcm_runtime(card,"Primary Codec");
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

static void mt8169_ras_ext_hp_amp_spk_turn_on(struct snd_soc_card *card)
{
	struct mt8169_raspite_priv *priv = snd_soc_card_get_drvdata(card);

	mutex_lock(&priv->pa_power_lock);
#if IS_ENABLED(CONFIG_SND_SOC_DC_DETECTION_SUPPORT)
	if (priv->disable_pa == true) {
		dev_err(card->dev, "%s, As dc detection occured multi times, disable open pa.\n",
			__func__);
		mutex_unlock(&priv->pa_power_lock);
		return;
	}
#endif

	if (!IS_ERR(priv->spk_pa_5v_en_gpio) && (priv->spk_pa_id == EXT_AMP_TPA2011 ||
			priv->spk_pa_id == EXT_AMP_AD51562)) {
		gpiod_set_value_cansleep(priv->spk_pa_5v_en_gpio, 1);
		usleep_range(priv->ext_spk_amp_vdd_on_time_us,
			priv->ext_spk_amp_vdd_on_time_us + 1);

		if (priv->spk_pa_id == EXT_AMP_TPA2011) {
			/* enable tpa2011 spk amp */
			if ((!IS_ERR(priv->ext_amp1_gpio)) && (!IS_ERR(priv->ext_amp2_gpio))) {
				gpiod_set_value_cansleep(priv->ext_amp1_gpio, 1);
				gpiod_set_value_cansleep(priv->ext_amp2_gpio, 1);
				dev_info(card->dev, "%s, tpa2011 spk amp enable\n", __func__);
			} else {
				dev_err(card->dev, "%s, ext amp1 and ext amp2 gpio invalid\n", __func__);
				goto exit1;
			}
		} else {
			/* enable ad51562 spk amp */
			if ((!IS_ERR(priv->ext_amp1_gpio)) && (!IS_ERR(priv->ext_amp2_gpio))) {
				gpiod_set_value_cansleep(priv->ext_amp1_gpio, 1);
				gpiod_set_value_cansleep(priv->ext_amp2_gpio, 1);
				dev_info(card->dev, "%s, ad51562 spk amp enable\n", __func__);
			} else {
				dev_err(card->dev, "%s, ext amp1 and ext amp2 gpio invalid\n", __func__);
				goto exit1;
			}
		}
	} else if (priv->spk_pa_id == EXT_AMP_AW87390) {
		int ret;

		ret = aw87xxx_set_profile(AW_DEV_0, aw_profile[0]);
		if (ret < 0) {
			dev_err(card->dev, "[Awinic] %s: set profile[%s] failed", __func__, aw_profile[0]);
			mutex_unlock(&priv->pa_power_lock);
			return;
		}

		ret = aw87xxx_set_profile(AW_DEV_1, aw_profile[0]);
		if (ret < 0) {
			dev_err(card->dev, "[Awinic] %s: set profile[%s] failed", __func__, aw_profile[0]);
			mutex_unlock(&priv->pa_power_lock);
			return;
		}
	} else
		dev_err(card->dev, "%s, spk pa and kpa 5v en gpio invalid\n", __func__);
	mutex_unlock(&priv->pa_power_lock);

	return;
exit1:
	gpiod_set_value_cansleep(priv->spk_pa_5v_en_gpio, 0);
	usleep_range(priv->ext_spk_amp_vdd_on_time_us,
		priv->ext_spk_amp_vdd_on_time_us + 1);
	mutex_unlock(&priv->pa_power_lock);
}

static void mt8169_ras_ext_hp_amp_spk_turn_off(struct snd_soc_card *card)
{
	struct mt8169_raspite_priv *priv = snd_soc_card_get_drvdata(card);

	mutex_lock(&priv->pa_power_lock);
	if (!IS_ERR(priv->spk_pa_5v_en_gpio) && (priv->spk_pa_id == EXT_AMP_TPA2011 ||
	    priv->spk_pa_id == EXT_AMP_AD51562)) {
		if ((!IS_ERR(priv->ext_amp1_gpio)) && (!IS_ERR(priv->ext_amp2_gpio))) {
			gpiod_set_value_cansleep(priv->ext_amp1_gpio, 0);
			gpiod_set_value_cansleep(priv->ext_amp2_gpio, 0);
		} else
			dev_err(card->dev, "%s, ext amp1 and ext amp2 gpio invalid\n", __func__);
		gpiod_set_value_cansleep(priv->spk_pa_5v_en_gpio, 0);
	} else if (priv->spk_pa_id == EXT_AMP_AW87390) {
		int ret;

		ret = aw87xxx_set_profile(AW_DEV_0, aw_profile[1]);
		if (ret < 0) {
			dev_err(card->dev, "[Awinic] %s: set profile[%s] failed", __func__, aw_profile[0]);
			mutex_unlock(&priv->pa_power_lock);
			return;
		}

		ret = aw87xxx_set_profile(AW_DEV_1, aw_profile[1]);
		if (ret < 0) {
			dev_err(card->dev, "[Awinic] %s: set profile[%s] failed", __func__, aw_profile[0]);
			mutex_unlock(&priv->pa_power_lock);
			return;
		}
	} else {
		dev_err(card->dev, "%s, spk pa and kpa 5v en gpio invalid\n", __func__);
		mutex_unlock(&priv->pa_power_lock);
		return;
	}
	mutex_unlock(&priv->pa_power_lock);
}

static int mt8169_ras_spk_amp_event(struct snd_soc_dapm_widget *w,
				       struct snd_kcontrol *kcontrol,
				       int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;

	dev_info(card->dev, "%s(), event %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		/* spk amp on control */
		mt8169_ras_ext_hp_amp_spk_turn_on(card);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		/* spk amp off control */
		mt8169_ras_ext_hp_amp_spk_turn_off(card);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static void mt8169_ras_ext_hp_amp_turn_on(struct snd_soc_card *card)
{
	struct mt8169_raspite_priv *priv = snd_soc_card_get_drvdata(card);

	if (!IS_ERR(priv->apa_sdn_gpio))
		gpiod_set_value_cansleep(priv->apa_sdn_gpio, 1);
}

static void mt8169_ras_ext_hp_amp_turn_off(struct snd_soc_card *card)
{
	struct mt8169_raspite_priv *priv = snd_soc_card_get_drvdata(card);

	if (!IS_ERR(priv->apa_sdn_gpio))
		gpiod_set_value_cansleep(priv->apa_sdn_gpio, 0);
}

/* HP Amp */
static int mt8169_ras_hp_amp_event(struct snd_soc_dapm_widget *w,
	struct snd_kcontrol *kcontrol, int event)
{
	struct snd_soc_dapm_context *dapm = w->dapm;
	struct snd_soc_card *card = dapm->card;

	dev_info(card->dev, "%s, event %d\n", __func__, event);

	switch (event) {
	case SND_SOC_DAPM_POST_PMU:
		mt8169_ras_ext_hp_amp_turn_on(card);
		break;
	case SND_SOC_DAPM_PRE_PMD:
		mt8169_ras_ext_hp_amp_turn_off(card);
		break;
	default:
		break;
	}

	return 0;
}

static const struct snd_soc_dapm_widget mt8169_raspite_widgets[] = {
	SND_SOC_DAPM_SPK(EXT_SPK_AMP_W_NAME, mt8169_ras_spk_amp_event),
	SND_SOC_DAPM_HP(EXT_HP_AMP_W_NAME, mt8169_ras_hp_amp_event),
};

#if !IS_ENABLED(CONFIG_SND_SOC_FPGA)
static const struct snd_soc_dapm_route mt8169_raspite_routes[] = {
	{EXT_SPK_AMP_W_NAME, NULL, "LINEOUT L"},
	{EXT_SPK_AMP_W_NAME, NULL, "LINEOUT L HSSPK"},
	{EXT_SPK_AMP_W_NAME, NULL, "Headphone L Ext Spk Amp"},
	{EXT_SPK_AMP_W_NAME, NULL, "Headphone R Ext Spk Amp"},
	{EXT_HP_AMP_W_NAME, NULL, "Headphone L"},
	{EXT_HP_AMP_W_NAME, NULL, "Headphone R"},
};
#endif

static const struct snd_kcontrol_new mt8169_raspite_controls[] = {
	SOC_DAPM_PIN_SWITCH(EXT_SPK_AMP_W_NAME),
	SOC_DAPM_PIN_SWITCH(EXT_HP_AMP_W_NAME),
	SOC_ENUM_EXT("MTK_SPK_TYPE_GET", mt8169_spk_type_enum[0],
		     mt8169_spk_type_get, NULL),
	SOC_ENUM_EXT("MTK_SPK_I2S_OUT_TYPE_GET", mt8169_spk_type_enum[1],
		     mt8169_spk_i2s_out_type_get, NULL),
	SOC_ENUM_EXT("MTK_SPK_I2S_IN_TYPE_GET", mt8169_spk_type_enum[1],
		     mt8169_spk_i2s_in_type_get, NULL),
	SOC_ENUM_EXT("Ext_Speaker_PGA_Gain", mt8169_raspite_enum[0],
		     mt8169_ras_ext_speaker_pga_get,
		     mt8169_ras_ext_speaker_pga_put),
	SOC_SINGLE_BOOL_EXT("DL_Playback_State", 0,
		     mt8169_afe_dl1_playback_state_get,NULL),
};

/* BE i2s need assign snd_soc_ops = mt8169_ras_i2s_ops */
static int mt8169_ras_i2s_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	unsigned int rate = params_rate(params);
	unsigned int mclk_fs_ratio = MT8169_AFE_I2S_MCLK_RATIO;
	unsigned int mclk_fs = rate * mclk_fs_ratio;

	return snd_soc_dai_set_sysclk(rtd->cpu_dai,
				      0, mclk_fs, SND_SOC_CLOCK_OUT);
}

static const struct snd_soc_ops mt8169_raspite_i2s_ops = {
	.hw_params = mt8169_ras_i2s_hw_params,
};

/* BE tdm need assign snd_soc_ops = mt8169_ras_tdm_ops */
static int mt8169_ras_tdm_hw_params(struct snd_pcm_substream *substream,
				       struct snd_pcm_hw_params *params)
{
	struct snd_soc_pcm_runtime *rtd = substream->private_data;
	unsigned int rate = params_rate(params);
	unsigned int mclk_fs_ratio = MT8169_AFE_TDMIN_MCLK_RATIO;
	unsigned int mclk_fs = rate * mclk_fs_ratio;

	return snd_soc_dai_set_sysclk(rtd->cpu_dai,
				      0, mclk_fs, SND_SOC_CLOCK_OUT);
}

static const struct snd_soc_ops mt8169_raspite_tdm_ops = {
	.hw_params = mt8169_ras_tdm_hw_params,
};

static int mt8169_ras_init(struct snd_soc_pcm_runtime *rtd)
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
#if IS_ENABLED(CONFIG_SND_SOC_FPGA)
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
#else
	DAILINK_COMP_ARRAY(COMP_EMPTY()),
#endif
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
#if IS_ENABLED(CONFIG_MTK_HIFIXDSP_SUPPORT)
SND_SOC_DAILINK_DEFS(adsp_hostles_va,
	DAILINK_COMP_ARRAY(COMP_CPU("FE_HOSTLESS_VA")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(adsp_va_capture,
	DAILINK_COMP_ARRAY(COMP_CPU("FE_VA")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(adsp_va_playback,
	DAILINK_COMP_ARRAY(COMP_CPU("FE_PCM_PLAYBACK")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(adsp_ul5_in,
	DAILINK_COMP_ARRAY(COMP_CPU("BE_UL5_IN")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(adsp_ul1_in,
	DAILINK_COMP_ARRAY(COMP_CPU("BE_UL1_IN")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
SND_SOC_DAILINK_DEFS(adsp_dl3_out,
	DAILINK_COMP_ARRAY(COMP_CPU("BE_DL3_OUT")),
	DAILINK_COMP_ARRAY(COMP_DUMMY()),
	DAILINK_COMP_ARRAY(COMP_EMPTY()));
#endif
static struct snd_soc_dai_link mt8169_raspite_dai_links[] = {
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
#if IS_ENABLED(CONFIG_MTK_HIFIXDSP_SUPPORT)
	{
		.name = "ADSP HOSTLESS_VA",
		.stream_name = "HOSTLESS_VA",
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dynamic = 1,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(adsp_hostles_va),
	},
	{
		.name = "ADSP VA_FE",
		.stream_name = "VA_Capture",
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dynamic = 1,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(adsp_va_capture),
	},
	{
		.name = "ADSP PCM_PLAYBACK",
		.stream_name = "ADSP_PCM_PLAYBACK",
		.trigger = {
			SND_SOC_DPCM_TRIGGER_POST,
			SND_SOC_DPCM_TRIGGER_POST
		},
		.dynamic = 1,
		.dpcm_playback = 1,
		SND_SOC_DAILINK_REG(adsp_va_playback),
	},
#endif
	/* Back End DAI links */
	{
		.name = "Primary Codec",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		.init = mt8169_ras_init,
		SND_SOC_DAILINK_REG(adda),
	},
	{
		.name = "I2S3",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.ignore_suspend = 1,
		.be_hw_params_fixup = mt8169_i2s_hw_params_fixup,
		.ops = &mt8169_raspite_i2s_ops,
		SND_SOC_DAILINK_REG(i2s3),
	},
	{
		.name = "I2S0",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		.be_hw_params_fixup = mt8169_i2s_hw_params_fixup,
		.ops = &mt8169_raspite_i2s_ops,
		SND_SOC_DAILINK_REG(i2s0),
	},
	{
		.name = "I2S1",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.ignore_suspend = 1,
		.be_hw_params_fixup = mt8169_i2s_hw_params_fixup,
		.ops = &mt8169_raspite_i2s_ops,
		SND_SOC_DAILINK_REG(i2s1),
	},
	{
		.name = "I2S2",
		.no_pcm = 1,
		.dpcm_capture = 1,
		.ignore_suspend = 1,
		.be_hw_params_fixup = mt8169_i2s_hw_params_fixup,
		.ops = &mt8169_raspite_i2s_ops,
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
		.ops = &mt8169_raspite_tdm_ops,
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
#if IS_ENABLED(CONFIG_MTK_HIFIXDSP_SUPPORT)
	{
		.name = "ADSP_UL5_IN BE",
		.no_pcm = 1,
		.dpcm_playback = 0,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(adsp_ul5_in),
	},
	{
		.name = "ADSP_UL1_IN BE",
		.no_pcm = 1,
		.dpcm_playback = 0,
		.dpcm_capture = 1,
		SND_SOC_DAILINK_REG(adsp_ul1_in),
	},
	{
		.name = "ADSP_DL3_OUT BE",
		.no_pcm = 1,
		.dpcm_playback = 1,
		.dpcm_capture = 0,
		SND_SOC_DAILINK_REG(adsp_dl3_out),
	},

#endif
};

static int mt8169_ras_gpio_probe(struct snd_soc_card *card)
{
	struct mt8169_raspite_priv *priv = snd_soc_card_get_drvdata(card);
	int ret = 0;

	priv->ext_amp1_gpio = devm_gpiod_get(card->dev,
						      "ext_amp1",
						      GPIOD_OUT_LOW);

	if (IS_ERR(priv->ext_amp1_gpio)) {
		ret = PTR_ERR(priv->ext_amp1_gpio);
		dev_info(card->dev, "failed to get ext_amp1 gpio: %d\n", ret);
	}

	priv->ext_amp2_gpio = devm_gpiod_get(card->dev,
						      "ext_amp2",
						      GPIOD_OUT_LOW);

	if (IS_ERR(priv->ext_amp2_gpio)) {
		ret = PTR_ERR(priv->ext_amp2_gpio);
		dev_info(card->dev, "failed to get ext_amp2 gpio: %d\n", ret);
	}

	priv->spk_pa_5v_en_gpio = devm_gpiod_get(card->dev,
						      "spk-pa-5v-en",
						      GPIOD_OUT_LOW);

	if (IS_ERR(priv->spk_pa_5v_en_gpio)) {
		ret = PTR_ERR(priv->spk_pa_5v_en_gpio);
		dev_info(card->dev, "failed to get spk_pa_5v_en_gpio: %d\n", ret);
	}

	priv->apa_sdn_gpio = devm_gpiod_get(card->dev,
						      "apa-sdn",
						      GPIOD_OUT_LOW);

	if (IS_ERR(priv->apa_sdn_gpio)) {
		ret = PTR_ERR(priv->apa_sdn_gpio);
		dev_info(card->dev, "failed to get apa_sdn_gpio: %d\n", ret);
	}

	priv->spk_pa_id1_gpio = devm_gpiod_get(card->dev,
						      "spk-pa-id1", GPIOD_IN);

	if (IS_ERR(priv->spk_pa_id1_gpio)) {
		ret = PTR_ERR(priv->spk_pa_id1_gpio);
		dev_info(card->dev, "failed to get spk_pa_id1_gpio: %d\n", ret);
	} else {
		priv->spk_pa_id = gpiod_get_value_cansleep(priv->spk_pa_id2_gpio);
	}

	priv->spk_pa_id2_gpio = devm_gpiod_get(card->dev,
						      "spk-pa-id2", GPIOD_IN);

	if (IS_ERR(priv->spk_pa_id2_gpio)) {
		ret = PTR_ERR(priv->spk_pa_id2_gpio);
		dev_info(card->dev, "failed to get spk_pa_id2_gpio: %d\n", ret);
	} else {
		priv->spk_pa_id |= gpiod_get_value_cansleep(priv->spk_pa_id1_gpio) << 1;
	}

	if (priv->spk_pa_id == EXT_AMP_AW87390) {
		ret = of_property_read_u32(card->dev->of_node, "k_class_pa_codec_gain_l",
				&priv->spk_codec_gain_left);
		if (ret < 0) {
			dev_info(card->dev, "%s() get d pa left codec gain fail", __func__);
			priv->spk_codec_gain_left = -1;
		}

		ret = of_property_read_u32(card->dev->of_node, "k_class_pa_codec_gain_r",
				&priv->spk_codec_gain_right);
		if (ret < 0) {
			dev_info(card->dev, "%s() get d pa right codec gain fail", __func__);
			priv->spk_codec_gain_right = -1;
		}
	} else {
		ret = of_property_read_u32(card->dev->of_node, "d_class_pa_codec_gain_l",
				&priv->spk_codec_gain_left);
		if (ret < 0) {
			dev_info(card->dev, "%s() get d pa left codec gain fail", __func__);
			priv->spk_codec_gain_left = -1;
		}

		ret = of_property_read_u32(card->dev->of_node, "d_class_pa_codec_gain_r",
				&priv->spk_codec_gain_right);
		if (ret < 0) {
			dev_info(card->dev, "%s() get d pa right codec gain fail", __func__);
			priv->spk_codec_gain_right = -1;
		}
	}

	return ret;
}

static void mt8169_ras_parse_of(struct snd_soc_card *card,
	struct device_node *np)
{
	struct mt8169_raspite_priv *priv = snd_soc_card_get_drvdata(card);

	of_property_read_u32(np,
		"mediatek,fs1512n-start-work-time-us",
		&priv->fs1512n_start_work_time_us);

	of_property_read_u32(np,
		"mediatek,fs1512n-mode-setting-time-us",
		&priv->fs1512n_mode_setting_time_us);

	of_property_read_u32(np,
		"mediatek,fs1512n-power-down-time-us",
		&priv->fs1512n_power_down_time_us);

	of_property_read_u32(np,
		"mediatek,ext-spk-amp-vdd-on-time-us",
		&priv->ext_spk_amp_vdd_on_time_us);
}

static struct snd_soc_card mt8169_raspite_soc_card = {
	.name = "mt8169-raspite",
	.owner = THIS_MODULE,
	.dai_link = mt8169_raspite_dai_links,
	.num_links = ARRAY_SIZE(mt8169_raspite_dai_links),

	.controls = mt8169_raspite_controls,
	.num_controls = ARRAY_SIZE(mt8169_raspite_controls),
	.dapm_widgets = mt8169_raspite_widgets,
	.num_dapm_widgets = ARRAY_SIZE(mt8169_raspite_widgets),
#if !IS_ENABLED(CONFIG_SND_SOC_FPGA)
	.dapm_routes = mt8169_raspite_routes,
	.num_dapm_routes = ARRAY_SIZE(mt8169_raspite_routes),
#endif
};

#if IS_ENABLED(CONFIG_SND_SOC_MT6357_ACCDET)
static int mt8169_mt6357_headset_init(struct snd_soc_component *component)
{
	struct snd_soc_card *card = &mt8169_raspite_soc_card;

	return mt6357_accdet_init(component, card);
}

static struct snd_soc_aux_dev mt8169_mt6357_headset_dev = {
	.dlc = COMP_EMPTY(),
	.init = mt8169_mt6357_headset_init,
};
#elif IS_ENABLED(CONFIG_SND_SOC_MT6358_ACCDET)
static int mt8169_mt6358_headset_init(struct snd_soc_component *component)
{
	struct snd_soc_card *card = &mt8169_raspite_soc_card;

	return mt6358_accdet_init(component, card);
}

static struct snd_soc_aux_dev mt8169_mt6358_headset_dev = {
	.dlc = COMP_EMPTY(),
	.init = mt8169_mt6358_headset_init,
};
#endif

#if IS_ENABLED(CONFIG_SND_SOC_DC_DETECTION_SUPPORT)
static ssize_t dc_det_state_show(struct device_driver *ddri, char *buf)
{
	struct snd_soc_card *card = &mt8169_raspite_soc_card;
	struct mt8169_raspite_priv *priv = snd_soc_card_get_drvdata(card);
	int ret = 0;

	if (buf == NULL) {
		pr_notice("%s() *buf is NULL\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&priv->dc_det_res_lock);
	ret = snprintf(buf, PAGE_SIZE, "%d\n", priv->dc_det_tocal_count);
	if (ret < 0)
		pr_notice("%s(): snprintf failed\n", __func__);
	mutex_unlock(&priv->dc_det_res_lock);

	return strlen(buf);
}
static DRIVER_ATTR_RO(dc_det_state);

static struct driver_attribute *dc_det_attr_list[] = {
	&driver_attr_dc_det_state,
};

static int dc_det_create_attr(struct device_driver *driver)
{
	int idx, err = 0;
	int num = ARRAY_SIZE(dc_det_attr_list);

	if (driver == NULL)
		return -EINVAL;

	for (idx = 0; idx < num; idx++) {
		err = driver_create_file(driver, dc_det_attr_list[idx]);
		if (err) {
			pr_notice("%s() driver_create_file %s err:%d\n",
				__func__, dc_det_attr_list[idx]->attr.name, err);
			break;
		}
	}
	return err;
}

static int dc_detection_process(struct snd_soc_card *card, int dc_det_ch)
{
	struct mt8169_raspite_priv *priv = snd_soc_card_get_drvdata(card);
	int index = 0;
#if IS_ENABLED(CONFIG_AMAZON_MINERVA_METRICS_LOG)
	char metrics_log_buf[METRICS_LOG_MAX_SIZE];
	long dc_det_debounce_real_count = 0;
#endif

	for (index = 0; index <= priv->dc_det_debounce_count_max; index++) {
#if IS_ENABLED(CONFIG_AMAZON_MINERVA_METRICS_LOG)
		dc_det_debounce_real_count = index;
#endif
		priv->dc_det[dc_det_ch].gpio_state = gpio_get_value(priv->dc_det[dc_det_ch].gpio);
		if (priv->dc_det[dc_det_ch].gpio_state == DC_DET_HIGH)
			break;

		mdelay(priv->dc_det_debounce_interval_time_ms);
	}

	if (index >= priv->dc_det_debounce_count_max && priv->dc_det[dc_det_ch].gpio_state == DC_DET_LOW) {
		priv->dc_det[dc_det_ch].dc_result = true;
		priv->dc_det[dc_det_ch].dc_count++;

		mutex_lock(&priv->dc_det_res_lock);
		priv->dc_det_tocal_count++;

		if ((priv->dc_det_disable_pa_threshold != DC_DET_IGNORE_DISABLE_PA)
				&& (priv->dc_det[dc_det_ch].dc_count >= priv->dc_det_disable_pa_threshold)) {
			priv->disable_pa = true;
		}
		mutex_unlock(&priv->dc_det_res_lock);
	}

	// do power off pa
	if (priv->dc_det[dc_det_ch].dc_result == true)
		mt8169_ras_ext_hp_amp_spk_turn_off(card);

#if IS_ENABLED(CONFIG_AMAZON_MINERVA_METRICS_LOG)
	minerva_metrics_log(metrics_log_buf, METRICS_LOG_MAX_SIZE,
		"%s:%s:100:%s,%s,%s,DcOffsetDetected=%s;BO,DebounceCount=%ld;IN:us-east-1",
		METRICS_DC_OFFSET_DETECTED_GROUP_ID, METRICS_DC_OFFSET_DETECTED_SCHEMA_ID,
		PREDEFINED_ESSENTIAL_KEY, PREDEFINED_DEVICE_ID_KEY, PREDEFINED_OS_KEY,
		(priv->dc_det[dc_det_ch].dc_result == true) ? "true" : "false",
		dc_det_debounce_real_count);
#endif

	dev_info(card->dev, "%s(),dc_det_ch=%d, dc_result=%d, dc_count=%d, dc_det_tocal_count=%ld\n",
		__func__, dc_det_ch, priv->dc_det[dc_det_ch].dc_result, priv->dc_det[dc_det_ch].dc_count, priv->dc_det_tocal_count);

	return 0;
}

static void dc_det_r_work_func(struct work_struct *work)
{
	struct snd_soc_card *card = &mt8169_raspite_soc_card;
	struct mt8169_raspite_priv *priv = snd_soc_card_get_drvdata(card);

	dc_detection_process(card, DC_DET_R);
	enable_irq(priv->dc_det[DC_DET_R].irq);
}

static void dc_det_l_work_func(struct work_struct *work)
{
	struct snd_soc_card *card = &mt8169_raspite_soc_card;
	struct mt8169_raspite_priv *priv = snd_soc_card_get_drvdata(card);

	dc_detection_process(card, DC_DET_L);
	enable_irq(priv->dc_det[DC_DET_L].irq);
}

static irqreturn_t dc_detect_irq_handler(int irq, void *data)
{
	struct snd_soc_card *card = &mt8169_raspite_soc_card;
	struct mt8169_raspite_priv *priv = (struct mt8169_raspite_priv *)data;

	if (irq == priv->dc_det[DC_DET_R].irq) {
		disable_irq_nosync(priv->dc_det[DC_DET_R].irq);
		priv->dc_det[DC_DET_R].dc_result = false;

		schedule_delayed_work(&priv->dc_det[DC_DET_R].work, 0);
	} else if (irq == priv->dc_det[DC_DET_L].irq) {
		disable_irq_nosync(priv->dc_det[DC_DET_L].irq);
		priv->dc_det[DC_DET_L].dc_result = false;

		schedule_delayed_work(&priv->dc_det[DC_DET_L].work, 0);
	} else {
		dev_err(card->dev, "%s(), abornal irq, not handle\n", __func__);
		return IRQ_NONE;
	}

	return IRQ_HANDLED;
}

static int dc_det_register_irq(struct snd_soc_card *card)
{
	struct mt8169_raspite_priv *priv = snd_soc_card_get_drvdata(card);
	int index = 0;
	int ret = 0;

	for (index = 0; index < priv->dc_det_irq_num; index++) {
		priv->dc_det[index].irq = gpio_to_irq(priv->dc_det[index].gpio);
		disable_irq_nosync(priv->dc_det[index].irq);
		ret = devm_request_threaded_irq(card->dev, priv->dc_det[index].irq, NULL,
				dc_detect_irq_handler, IRQF_ONESHOT|IRQF_TRIGGER_FALLING,
				priv->irq_names[index], priv);
		if (ret) {
			dev_err(card->dev, "%s(), fail to register irq%d - \n",
					__func__, index, priv->dc_det[index].irq);
			return ret;
		}
		priv->dc_det[index].irq_registerred = true;
	}
	return 0;
}

static int dc_det_parse_dc_det_data(struct snd_soc_card *card)
{
	struct mt8169_raspite_priv *priv = snd_soc_card_get_drvdata(card);
	int index = 0;
	int ret = 0;
	int gpio_names_num = 0, irq_names_num = 0;

	if ((priv->dc_det_irq_num != DC_DET_IRQ_NORMAL)
			&& (priv->dc_det_irq_num != DC_DET_IRQ_MAX)) {
		dev_err(card->dev, "%s(), param error\n", __func__);
		return -EINVAL;
	}

	priv->dc_det = devm_kzalloc(card->dev, sizeof(struct dc_det_desc)
					* priv->dc_det_irq_num, GFP_KERNEL);
	if (!priv->dc_det) {
		dev_err(card->dev, "%s(), fail to allocate dc detection desc.\n",
			__func__);
		return -ENOMEM;
	}

	gpio_names_num = of_property_read_string_array(card->dev->of_node,
			"dc_gpio-names", &priv->gpio_names[0], DC_DET_IRQ_MAX);
	if (ret < 0) {
		dev_err(card->dev, "%s(), fail to get gpio-names\n", __func__);
		goto fail;
	}

	irq_names_num = of_property_read_string_array(card->dev->of_node,
			"dc_irq-names", &priv->irq_names[0], DC_DET_IRQ_MAX);
	if (ret < 0) {
		dev_err(card->dev, "%s(), fail to get irq-names\n", __func__);
		goto fail;
	}

	if ((priv->dc_det_irq_num != gpio_names_num)
			|| (priv->dc_det_irq_num != irq_names_num)
			|| (gpio_names_num != irq_names_num)) {
		dev_err(card->dev, "%s(), params error\n", __func__);
		goto fail;
	}

	for (index = 0; index < priv->dc_det_irq_num; index++) {
		priv->dc_det[index].gpio = of_get_named_gpio_flags(card->dev->of_node,
				priv->gpio_names[index], 0, NULL);
		if (!gpio_is_valid(priv->dc_det[index].gpio)) {
			dev_err(card->dev, "%s(), Unable to read %s\n",
				__func__, priv->gpio_names[index]);
			goto fail;
		}

		if (gpio_direction_input(priv->dc_det[index].gpio) < 0) {
			dev_err(card->dev, "%s(), fail to set gpio input r\n", __func__);
			goto fail;
		}

		priv->dc_det[index].irq_registerred = false;
		priv->dc_det[index].dc_result = false;
		priv->dc_det[index].dc_count = 0;

		// init dc work queue
		if (index == DC_DET_R) {
			INIT_DELAYED_WORK(&priv->dc_det[DC_DET_R].work, dc_det_r_work_func);
		} else if (index == DC_DET_L) {
			INIT_DELAYED_WORK(&priv->dc_det[DC_DET_L].work, dc_det_l_work_func);
		} else {
			dev_err(card->dev, "%s(), param error for init work queue\n", __func__);
			goto fail;
		}
	}

	ret = of_property_read_u32(card->dev->of_node,
			"dc_det_debounce_count_max", &priv->dc_det_debounce_count_max);
	if (ret < 0) {
		dev_err(card->dev, "%s(), failed to get dc_det_debounce_count_max\n", __func__);
		priv->dc_det_debounce_count_max = DC_DET_DEBOUNCE_COUNT_MAX;
	}

	ret = of_property_read_u32(card->dev->of_node,
			"dc_det_debounce_interval_time_ms", &priv->dc_det_debounce_interval_time_ms);
	if (ret < 0) {
		dev_err(card->dev, "%s(), failed to get dc_det_debounce_interval_time_ms\n", __func__);
		priv->dc_det_debounce_interval_time_ms = DC_DET_DEBOUNCE_INTERVAL_TIME_MS_DEFAULT;
	}

	ret = of_property_read_u32(card->dev->of_node,
			"dc_det_disable_pa_threshold", &priv->dc_det_disable_pa_threshold);
	if (ret < 0) {
		dev_err(card->dev, "%s(), failed to get dc_det_disable_pa_threshold\n", __func__);
		priv->dc_det_disable_pa_threshold = DC_DET_DISABLE_PA_THRESHOLD_DEFAULT;
	}

	priv->dc_det_vsim2= devm_regulator_get(card->dev, "dc_det_vsim2");
	if (IS_ERR(priv->dc_det_vsim2)) {
		dev_err(card->dev, "%s(), failed to get dc_det_vsim2\n", __func__);
		goto fail;
	}

	return 0;

fail:
	if (priv->dc_det) {
		devm_kfree(card->dev, priv->dc_det);
		priv->dc_det = NULL;
	}
	return -EINVAL;
}

static int dc_det_prepare(struct snd_soc_card *card)
{
	struct mt8169_raspite_priv *priv = snd_soc_card_get_drvdata(card);
	int ret = 0;

	priv->dc_detection_supported = of_property_read_bool(card->dev->of_node,
				"dc_detection_support");
	if (priv->dc_detection_supported == false) {
		dev_info(card->dev, "%s(), do not support dc detection\n", __func__);
		return -EPERM;
	}

	ret = of_property_read_u32(card->dev->of_node,
			"dc_det_irq_num", &priv->dc_det_irq_num);
	if (ret < 0) {
		dev_err(card->dev, "%s(), do not support dc detection\n", __func__);
		return -EINVAL;
	}

	if ((priv->dc_det_irq_num != DC_DET_IRQ_NORMAL)
			&& (priv->dc_det_irq_num != DC_DET_IRQ_MAX)) {
		dev_err(card->dev, "%s(), param error\n", __func__);
		return -EINVAL;
	}

	if (aud_pinctrl) {
		priv->dc_det_r_func = pinctrl_lookup_state(aud_pinctrl, "dc_det_r_func");
		if (IS_ERR(priv->dc_det_r_func)) {
			pr_err("%s: can't find dc_det_r_func\n", __func__);
		} else {
			pinctrl_select_state(aud_pinctrl, priv->dc_det_r_func);
		}

		if (priv->dc_det_irq_num == DC_DET_IRQ_MAX) {
			priv->dc_det_l_func = pinctrl_lookup_state(aud_pinctrl, "dc_det_l_func");
			if (IS_ERR(priv->dc_det_l_func)) {
				pr_err("%s: can't find dc_det_l_func\n", __func__);
			} else {
				pinctrl_select_state(aud_pinctrl, priv->dc_det_l_func);
			}
		} else {
			priv->dc_det_l_default = pinctrl_lookup_state(aud_pinctrl, "dc_det_l_default");
			if (IS_ERR(priv->dc_det_l_default)) {
				pr_err("%s: can't find dc_det_l_default\n", __func__);
			} else {
				pinctrl_select_state(aud_pinctrl, priv->dc_det_l_default);
			}
		}
	}

	return 0;
}

static int restore_default_status(struct snd_soc_card *card)
{
	struct mt8169_raspite_priv *priv = snd_soc_card_get_drvdata(card);

	// restore gpio96 & gpio118 to default
	if (aud_pinctrl) {
		priv->dc_det_r_default = pinctrl_lookup_state(aud_pinctrl, "dc_det_r_default");
		if (IS_ERR(priv->dc_det_r_default)) {
			pr_err("%s: can't find dc_det_r_default\n", __func__);
		} else {
			pinctrl_select_state(aud_pinctrl, priv->dc_det_r_default);
		}

		priv->dc_det_l_default = pinctrl_lookup_state(aud_pinctrl, "dc_det_l_default");
		if (IS_ERR(priv->dc_det_l_default)) {
			pr_err("%s: can't find dc_det_l_default\n", __func__);
		} else {
			pinctrl_select_state(aud_pinctrl, priv->dc_det_l_default);
		}
	}

	return 0;
}

static bool hw_board_support_dc_det(struct snd_soc_card *card)
{
	struct mt8169_raspite_priv *priv = snd_soc_card_get_drvdata(card);
	unsigned int board_id_0_val;
	unsigned int board_id_1_val;
	unsigned int board_id_2_val;
	unsigned int hw_board_id = 0;

	priv->board_id_0 = of_get_named_gpio_flags(card->dev->of_node , "board_id0", 0, NULL);
	priv->board_id_1 = of_get_named_gpio_flags(card->dev->of_node , "board_id1", 0, NULL);
	priv->board_id_2 = of_get_named_gpio_flags(card->dev->of_node , "board_id2", 0, NULL);
	if ((!gpio_is_valid(priv->board_id_0))
		|| (!gpio_is_valid(priv->board_id_1))
		|| (!gpio_is_valid(priv->board_id_2))) {
		dev_err(card->dev, "%s(), fail to get hw board id\n", __func__);
		return false;
	}

	board_id_0_val = gpio_get_value(priv->board_id_0);
	board_id_1_val = gpio_get_value(priv->board_id_1);
	board_id_2_val = gpio_get_value(priv->board_id_2);

	hw_board_id = (board_id_2_val<<2) | (board_id_1_val<<1) | (board_id_0_val);
	dev_info(card->dev, "%s(), hw_board_id=b%d%d%d\n",
		__func__, board_id_2_val, board_id_1_val, board_id_0_val);

	if (hw_board_id == DC_DET_SUPPORT_BOARD_ID)
		return true;
	else
		return false;
}

static int dc_detection_init(struct snd_soc_card *card)
{
	struct mt8169_raspite_priv *priv = snd_soc_card_get_drvdata(card);
	int ret = 0;
	int index = 0;

	ret = hw_board_support_dc_det(card);
	if (ret == false) {
		dev_info(card->dev, "%s(), hw does not support dc detection\n", __func__);
		restore_default_status(card);
		return -EPERM;
	}

	ret = dc_det_prepare(card);
	if (ret < 0) {
		dev_err(card->dev, "%s(), fail to prepare for dc detection\n", __func__);
		restore_default_status(card);
		return -EPERM;
	}

	ret = dc_det_parse_dc_det_data(card);
	if (ret) {
		dev_err(card->dev, "%s(), fail to get dc detection data\n", __func__);
		restore_default_status(card);
		return -EINVAL;
	}

	ret = regulator_enable(priv->dc_det_vsim2);
	if (ret) {
		dev_err(card->dev, "%s(), failed to enable vsim2 regulator\n", __func__);
		goto fail;
	}

	mutex_init(&priv->dc_det_res_lock);
	priv->dc_det_tocal_count = 0;
	priv->disable_pa = false;

	// register irq
	ret = dc_det_register_irq(card);
	if (ret) {
		dev_err(card->dev, "%s(), faile to register dc detection irq\n", __func__);
		goto fail;
	}

	// create sys file node
	ret = dc_det_create_attr(&mt8169_raspite_driver.driver);
	if (ret) {
		dev_err(card->dev, "%s(), faile to create sys file node\n", __func__);
		goto fail;
	}

	// enable irq
	for (index = 0; index < priv->dc_det_irq_num; index++)
		enable_irq(priv->dc_det[index].irq);

	dev_info(card->dev, "%s(), dc detection init successfully\n", __func__);
	return 0;

fail:
	if (priv->dc_det) {
		for (index = 0; index < priv->dc_det_irq_num; index++) {
			if (priv->dc_det[index].irq_registerred) {
				devm_free_irq(card->dev, priv->dc_det[index].irq, priv);
				priv->dc_det[index].irq_registerred = false;
			}
		}

		if (priv->dc_det_vsim2) {
			regulator_disable(priv->dc_det_vsim2);
			priv->dc_det_vsim2 = NULL;
		}

		devm_kfree(card->dev, priv->dc_det);
		priv->dc_det = NULL;
	}
	restore_default_status(card);
	dev_err(card->dev, "%s(), dc detection init failed\n", __func__);
	return ret;
}
#endif

static int mt8169_ras_dev_probe(struct platform_device *pdev)
{
	struct snd_soc_card *card = &mt8169_raspite_soc_card;
	struct device_node *platform_node, *codec_node, *dsp_node;
	struct snd_soc_dai_link *dai_link;
	struct mt8169_raspite_priv *priv;
	int ret, i;

	dev_info(&pdev->dev, "%s(), ++\n", __func__);

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

	dev_info(&pdev->dev, "%s(), update audio-codec dai\n", __func__);
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

	dev_info(&pdev->dev, "%s(), update platform dai\n", __func__);
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
	/* todo, headset accdect driver */
#if IS_ENABLED(CONFIG_SND_SOC_MT6357_ACCDET)
	mt8169_mt6357_headset_dev.dlc.of_node =
		of_parse_phandle(pdev->dev.of_node,
				"mediatek,headset-codec", 0);
	if (mt8169_mt6357_headset_dev.dlc.of_node) {
		card->aux_dev = &mt8169_mt6357_headset_dev;
		card->num_aux_devs = 1;
	} else
		dev_info(&pdev->dev,
			"Property 'mediatek,headset-codec' missing/invalid\n");
	mt6357_accdet_set_drvdata(card);
#elif IS_ENABLED(CONFIG_SND_SOC_MT6358_ACCDET)
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

	mt8169_ras_gpio_probe(card);

	mt8169_ras_parse_of(card, pdev->dev.of_node);
	/* init gpio */
	ret = mt8169_afe_gpio_init(&pdev->dev);
	if (ret)
		dev_info(&pdev->dev, "init gpio error\n");

	mutex_init(&priv->pa_power_lock);

	dev_info(&pdev->dev, "%s(), devm_snd_soc_register_card\n", __func__);
	ret = devm_snd_soc_register_card(&pdev->dev, card);
	if (ret) {
		dev_info(&pdev->dev, "%s snd_soc_register_card fail %d\n",
			__func__, ret);
	} else {
		dev_info(&pdev->dev, "%s(): snd_soc_register_card successfully %d\n",
			__func__, ret);
#if IS_ENABLED(CONFIG_SND_SOC_DC_DETECTION_SUPPORT)
		dev_info(&pdev->dev, "%s(): dc offset detection initialization\n", __func__);
		dc_detection_init(card);
#endif
	}

	return ret;
}

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id mt8169_raspite_dt_match[] = {
	{.compatible = "mediatek,mt8169-raspite-sound",},
	{}
};
#endif

static const struct dev_pm_ops mt8169_raspite_pm_ops = {
	.poweroff = snd_soc_poweroff,
	.restore = snd_soc_resume,
};

static struct platform_driver mt8169_raspite_driver = {
	.driver = {
		.name = "mt8169-ras",
#if IS_ENABLED(CONFIG_OF)
		.of_match_table = mt8169_raspite_dt_match,
#endif
		.pm = &mt8169_raspite_pm_ops,
	},
	.probe = mt8169_ras_dev_probe,
};

module_platform_driver(mt8169_raspite_driver);

/* Module information */
MODULE_DESCRIPTION("MT8169 raspite ALSA SoC machine driver");
MODULE_AUTHOR("Jiaxin Yu <jiaxin.yu@mediatek.com>");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("mt8169 raspite soc card");
