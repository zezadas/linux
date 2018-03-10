/*
 * wm8994_samsung.c  --  WM8994 ALSA Soc Audio driver
 *
 * Copyright 2010 Wolfson Microelectronics PLC.
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 * Notes:
 *  The WM8994 is a multichannel codec with S/PDIF support, featuring six
 *  DAC channels and two ADC channels.
 *
 *  Currently only the primary audio interface is supported - S/PDIF and
 *  the secondary audio interfaces are not.
 */

#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/delay.h>
#include <linux/pm.h>
#include <linux/i2c.h>
#include <linux/slab.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/clk.h>
#include <sound/core.h>
#include <sound/pcm.h>
#include <sound/pcm_params.h>
#include <sound/soc.h>
#include <sound/soc-dapm.h>
#include <sound/tlv.h>
#include <sound/initval.h>
#include <asm/div64.h>
#include <linux/io.h>
#include <linux/gpio.h>

#include <linux/mfd/wm8994/core.h>
#include <linux/mfd/wm8994/registers.h>
#include <linux/mfd/wm8994/pdata.h>
#include <linux/mfd/wm8994/gpio.h>

#include "wm8994_samsung.h"

#ifdef CONFIG_SND_VOODOO
#include "wm8994_voodoo.h"
#endif

#define WM8994_VERSION "0.1"
#define SUBJECT "wm8994_samsung.c"

#if defined(CONFIG_VIDEO_TV20) && defined(CONFIG_SND_S5P_WM8994_MASTER)
#define HDMI_USE_AUDIO
#endif

/* extern const u16 wm8994_reg_defaults[WM8994_CACHE_SIZE]; */

/*
 *Definitions of clock related.
*/

static struct {
	int ratio;
	int clk_sys_rate;
} clk_sys_rates[] = {
	{ 64,   0 },
	{ 128,  1 },
	{ 192,  2 },
	{ 256,  3 },
	{ 384,  4 },
	{ 512,  5 },
	{ 768,  6 },
	{ 1024, 7 },
	{ 1408, 8 },
	{ 1536, 9 },
};

static struct {
	int rate;
	int sample_rate;
} sample_rates[] = {
	{ 8000,  0  },
	{ 11025, 1  },
	{ 12000, 2  },
	{ 16000, 3  },
	{ 22050, 4  },
	{ 24000, 5  },
	{ 32000, 6  },
	{ 44100, 7  },
	{ 48000, 8  },
	{ 88200, 9  },
	{ 96000, 10  },
};

static struct {
	int div;
	int bclk_div;
} bclk_divs[] = {
	{ 1,   0  },
	{ 2,   1  },
	{ 4,   2  },
	{ 6,   3  },
	{ 8,   4  },
	{ 12,  5  },
	{ 16,  6  },
	{ 24,  7  },
	{ 32,  8  },
	{ 48,  9  },
};

/*
 * Definitions of sound path
 */
select_route universal_wm8994_playback_paths[] = {
	wm8994_disable_path,			/* OFF */
	wm8994_set_playback_receiver,		/* RCV */
	wm8994_set_playback_speaker,		/* SPK */
	wm8994_set_playback_headset,		/* HP */
	wm8994_set_playback_headset,		/* HP_NO_MIC */
	wm8994_set_playback_bluetooth,		/* BT */
	wm8994_set_playback_speaker_headset,	/* SPK_HP */
	wm8994_set_playback_speaker_headset,	/* RING_SPK */
	wm8994_set_playback_speaker_headset,	/* RING_HP */
	wm8994_set_playback_speaker_headset,	/* RING_NO_MIC */
	wm8994_set_playback_speaker_headset,	/* RING_SPK_HP */
	wm8994_set_playback_extra_dock_speaker,	/* LINEOUT */
	wm8994_set_playback_speaker_lineout		/* SPK_LINEOUT */
};

select_route universal_wm8994_voicecall_paths[] = {
	wm8994_disable_path, wm8994_set_voicecall_receiver,
	wm8994_set_voicecall_speaker, wm8994_set_voicecall_headset,
	wm8994_set_voicecall_headphone, wm8994_set_voicecall_bluetooth
};

select_mic_route universal_wm8994_mic_paths[] = {
	wm8994_record_main_mic,
	wm8994_record_headset_mic,
	wm8994_record_bluetooth,
};

select_clock_control universal_clock_controls = wm8994_configure_clock;
int gain_code;
static struct workqueue_struct *wm8994_workq;

static int wm8994_ldo_control(struct wm8994_platform_data *pdata, int en)
{

	if (!pdata) {
		pr_err("failed to control wm8994 ldo\n");
		return -EINVAL;
	}

	gpio_set_value(pdata->ldo, en);

	if (en)
		msleep(10);
	else
		msleep(125);

	return 0;

}

/*
 * Functions related volume.
 */
static const DECLARE_TLV_DB_SCALE(dac_tlv, -12750, 50, 1);

static int wm899x_outpga_put_volsw_vu(struct snd_kcontrol *kcontrol,
				      struct snd_ctl_elem_value *ucontrol)
{
	int ret;
	u16 val;

	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_codec *codec = component->codec;
	struct soc_mixer_control *mc =
	    (struct soc_mixer_control *)kcontrol->private_value;
	int reg = mc->reg;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	DEBUG_LOG("");

	ret = snd_soc_put_volsw_2r(kcontrol, ucontrol);
	if (ret < 0)
		return ret;

	/* Volume changes in the headphone path mean we need to
	 * recallibrate DC servo */
	if (strcmp(kcontrol->id.name, "Playback Spkr Volume") == 0 ||
	    strcmp(kcontrol->id.name, "Playback Volume") == 0)
		memset(wm8994->dc_servo, 0, sizeof(wm8994->dc_servo));

	val = snd_soc_read(codec, reg);

	return snd_soc_write(codec, reg, val | 0x0100);
}

static int wm899x_inpga_put_volsw_vu(struct snd_kcontrol *kcontrol,
				     struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_codec *codec = component->codec;
	struct soc_mixer_control *mc =
	    (struct soc_mixer_control *)kcontrol->private_value;
	int reg = mc->reg;
	int ret;
	u16 val;

	ret = snd_soc_put_volsw(kcontrol, ucontrol);

	if (ret < 0)
		return ret;

	val = snd_soc_read(codec, reg);

	return snd_soc_write(codec, reg, val | 0x0100);

}

/*
 * Implementation of sound path
 */
#define MAX_PLAYBACK_PATHS 12
#define MAX_VOICECALL_PATH 5
static const char *playback_path[] = {
	"OFF", "RCV", "SPK", "HP", "HP_NO_MIC", "BT", "SPK_HP",
	"RING_SPK", "RING_HP", "RING_NO_MIC", "RING_SPK_HP",
	"LINEOUT", "SPK_LINEOUT"
};
static const char *voicecall_path[] = {
	"OFF", "RCV", "SPK", "HP", "HP_NO_MIC", "BT"
};
static const char *mic_path[] = {
	"Main Mic", "Hands Free Mic", "BT Sco Mic", "MIC OFF"
};
static const char *input_source_state[] = {
	"Default", "Voice Recognition", "Camcorder"
};
static const char *tuning_control[] = {"OFF", "ON"};

/* ON_OTHER is for 3rd party App. ie skype*/
static const char *voip_call_state[] = {"OFF", "ON", "ON_OTHER", "KT_ON"};

#ifdef WM8994_FACTORY_LOOPBACK
static const char *loopback_path[] = {"spk", "ear",  "ear_pmic", "off"};
#endif

static const char *analog_vol_control[] = {
	"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10",
	"11", "12", "13", "14", "15", "RESET"
};

static const char *sales_code[] = {"Default", "EUR", "NonEUR"};

#ifdef WM8994_MUTE_STATE
static const char *state_mute[] = {"OFF", "RX_MUTE", "TX_MUTE"};
#endif

#ifdef WM8994_DOCK_STATE
static const char * const state_dock[] = {"OFF", "ON"};
#endif

#ifdef WM8994_VOIP_BT_NREC
static const char *state_voip_bt_nrec[] = {"OFF", "ON"};
#endif

#ifdef WM8994_MUTE_STATE
static int wm8994_get_mute_state(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_codec *codec = component->codec;
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);


	int config_value = wm8994->mute_state;
	ucontrol->value.integer.value[0] = config_value;
	DEBUG_LOG("wm8994_get_mute_state = %d", config_value);
	return 0;
}
static int wm8994_set_mute_state(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_codec *codec = component->codec;
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);
	struct soc_enum *mc = (struct soc_enum *)kcontrol->private_value;
	int config_value = ucontrol->value.integer.value[0];
	wm8994->mute_state = config_value;

	if (strcmp(mc->texts[config_value], state_mute[config_value])) {
		DEBUG_LOG_ERR("Unknown mute %s", mc->texts[config_value]);
		return -ENODEV;
	}

	switch (config_value) {
	case MUTE_OFF:
		DEBUG_LOG("wm8994_set_mute_state MUTE_OFF");
		break;
	case RX_MUTE:
		DEBUG_LOG("wm8994_set_mute_state RX_MUTE");
		break;
	case TX_MUTE:
		break;
	default:
		return -EINVAL;
		break;
	}

	return 0;
}
#endif

#ifdef WM8994_DOCK_STATE
static int wm8994_get_dock_state(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_codec *codec = component->codec;
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);

	int config_value = wm8994->dock_state;
	ucontrol->value.integer.value[0] = config_value;
	DEBUG_LOG("wm8994_get_dock_state = %d", config_value);
	return 0;
}
static int wm8994_set_dock_state(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_codec *codec = component->codec;
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);

	int val;
	int config_value = ucontrol->value.integer.value[0];
	wm8994->dock_state = config_value;
	if (wm8994->voip_call_active) {
		DEBUG_LOG("voip_call_active  duck_state =%d ",
			wm8994->dock_state);

		if (wm8994->dock_state) {
			val = snd_soc_read(codec,
				WM8994_LEFT_LINE_INPUT_1_2_VOLUME);
			val &= ~(WM8994_IN1L_VOL_MASK);
			val |= (WM8994_IN1L_VU | 0x8);
			snd_soc_write(codec, WM8994_LEFT_LINE_INPUT_1_2_VOLUME,
				val);

		} else {
			val = snd_soc_read(codec,
				WM8994_LEFT_LINE_INPUT_1_2_VOLUME);
			val &= ~(WM8994_IN1L_VOL_MASK);
			val |= (WM8994_IN1L_VU | 0xA);
			snd_soc_write(codec, WM8994_LEFT_LINE_INPUT_1_2_VOLUME,
				val);
		}
	}
	DEBUG_LOG("wm8994_set_duck_state (%d)", config_value);
	return 0;
}
#endif

#ifdef WM8994_VOIP_BT_NREC
static int wm8994_get_voip_bt_nrec_state(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_codec *codec = component->codec;
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);
	int config_value = wm8994->voip_bt_nrec_state;

	ucontrol->value.integer.value[0] = config_value;
	DEBUG_LOG("wm8994_get_voip_bt_nrec_state = %d", config_value);
	return 0;
}
static int wm8994_set_voip_bt_nrec_state(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_codec *codec = component->codec;
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);
	int config_value = ucontrol->value.integer.value[0];

	wm8994->voip_bt_nrec_state = config_value;

	DEBUG_LOG("%s() %d\n", __func__, wm8994->voip_bt_nrec_state);

	switch (config_value) {
	case VOIP_BT_NREC_OFF:
		DEBUG_LOG("%s() VOIP_BT_NREC_OFF\n", __func__);
		break;
	case VOIP_BT_NREC_ON:
		DEBUG_LOG("%s() VOIP_BT_NREC_ON\n", __func__);
		break;
	default:
		return -EINVAL;
		break;
	}

	return 0;
}
#endif

static int wm8994_get_voip_call(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);

	int config_value = wm8994->voip_call_active;
	ucontrol->value.integer.value[0] = config_value;
	DEBUG_LOG("wm8994_get_voip_call = %d", config_value);
	return 0;
}
static int wm8994_set_voip_call(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);

	int config_value = ucontrol->value.integer.value[0];
	wm8994->voip_call_active = config_value;
	DEBUG_LOG("wm8994_set_voip_call (%d)", config_value);
	return 0;
}

#ifdef WM8994_FACTORY_LOOPBACK
static int wm8994_get_loopback_path(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);


	int config_value = wm8994->loopback_path_control;
	ucontrol->value.integer.value[0] = config_value;
	DEBUG_LOG("wm8994_get_loopback_path = %d", config_value);
	return 0;
}

static int wm8994_set_loopback_path(struct snd_kcontrol *kcontrol,
				struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_codec *codec = component->codec;
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);

	struct soc_enum *mc = (struct soc_enum *)kcontrol->private_value;
	int val;
	int path_num = ucontrol->value.integer.value[0];
	wm8994->loopback_path_control = path_num;

	if (strcmp(mc->texts[path_num], loopback_path[path_num])) {
		DEBUG_LOG_ERR("Unknown path %s\n", mc->texts[path_num]);
		return -ENODEV;
	}

	switch (path_num) {
	case off:
		DEBUG_LOG("Switching off output path for loopback test!");
		wm8994->stream_state = PCM_STREAM_DEACTIVE;
		wm8994->codec_state = DEACTIVE;
		wm8994->pdata->set_mic_bias(false);
		wm8994->power_state = CODEC_OFF;
		wm8994->cur_path = OFF;
		wm8994->rec_path = MIC_OFF;
		wm8994->ringtone_active = RING_OFF;
		snd_soc_write(codec, WM8994_SOFTWARE_RESET, 0x0000);
		break;
	case spk:
		DEBUG_LOG("routing to %s\n", loopback_path[path_num]);
		break;
	case ear:
		DEBUG_LOG("routing to %s\n", loopback_path[path_num]);
		break;
	case ear_pmic:
		DEBUG_LOG("routing to %s\n", loopback_path[path_num]);
		break;
	default:
		return -EINVAL;
		break;
	}

	DEBUG_LOG("wm8994_set_loopback_path (%d)", path_num);
	return 0;
}
#endif

static int wm8994_get_codec_tuning(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);


	int config_value = wm8994->testmode_config_flag;
	ucontrol->value.integer.value[0] = config_value;
	DEBUG_LOG("wm8994_get_codec_tuning = %d", config_value);
	return 0;
}

static int wm8994_set_codec_tuning(struct snd_kcontrol *kcontrol,
				   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);

	int config_value = ucontrol->value.integer.value[0];
	wm8994->testmode_config_flag = config_value;
	if (config_value == 0)
		wm8994->codecgain_reserve = 0;
	DEBUG_LOG("wm8994_set_codec_tuning (%d)", config_value);
	return 0;
}

static int wm8994_get_mic_path(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = wm8994->rec_path;

	return 0;
}

static int wm8994_set_mic_path(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_codec *codec = component->codec;
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);

	DEBUG_LOG("");

	wm8994->codec_state |= CAPTURE_ACTIVE;

	switch (ucontrol->value.integer.value[0]) {
	case 0:
		wm8994->rec_path = MAIN;
#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
		wm8994->pdata->set_dap_connection(0);
#endif
		break;
	case 1:
		wm8994->rec_path = EAR;
#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
		wm8994->pdata->set_dap_connection(0);
#endif
		break;
	case 2:
		wm8994->rec_path = BT_REC;
#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
		wm8994->pdata->set_dap_connection(1);
#endif
		break;
	case 3:
		wm8994_disable_rec_path(codec);
		return 0;
	default:
		return -EINVAL;
	}

	wm8994->universal_mic_path[wm8994->rec_path] (codec);

	return 0;
}

static int wm8994_get_path(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = wm8994->cur_path;

	return 0;
}

static int wm8994_set_path(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_codec *codec = component->codec;
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);
	struct soc_enum *mc = (struct soc_enum *)kcontrol->private_value;
	int val;
	int path_num = ucontrol->value.integer.value[0];

	if (strcmp(mc->texts[path_num], playback_path[path_num])) {
		DEBUG_LOG_ERR("Unknown path %s\n", mc->texts[path_num]);
		return -ENODEV;
	}

	if (path_num > MAX_PLAYBACK_PATHS) {
		DEBUG_LOG_ERR("Unknown Path\n");
		return -ENODEV;
	}


	switch (path_num) {
	case OFF:
		DEBUG_LOG("Switching off output path");
		break;
	case RCV:
	case SPK:
	case HP:
	case HP_NO_MIC:
	case BT:
	case SPK_HP:
	case LINEOUT:
	case SPK_LINEOUT:
		DEBUG_LOG("routing to %s\n", mc->texts[path_num]);
		wm8994->ringtone_active = RING_OFF;
		break;
	case RING_SPK:
	case RING_HP:
	case RING_NO_MIC:
		DEBUG_LOG("routing to %s\n", mc->texts[path_num]);
		wm8994->ringtone_active = RING_ON;
		path_num -= 5;
		break;
	case RING_SPK_HP:
		DEBUG_LOG("routing to %s\n", mc->texts[path_num]);
		wm8994->ringtone_active = RING_ON;
		path_num -= 4;
		break;
	default:
		DEBUG_LOG_ERR("audio path[%d] does not exists!!\n", path_num);
		return -ENODEV;
		break;
	}

	wm8994->codec_state |= PLAYBACK_ACTIVE;

	if (wm8994->codec_state & CALL_ACTIVE) {
		wm8994->codec_state &= ~(CALL_ACTIVE);

		val = snd_soc_read(codec, WM8994_CLOCKING_1);
		val &= ~(WM8994_DSP_FS2CLK_ENA_MASK | WM8994_SYSCLK_SRC_MASK);
		snd_soc_write(codec, WM8994_CLOCKING_1, val);
	}

	if (path_num == RCV) {
		path_num = SPK;
		DEBUG_LOG("RCV play back path will be changed SPK!!!!!");
	}

#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
	if (path_num == BT)
		wm8994->pdata->set_dap_connection(1);
	else
		wm8994->pdata->set_dap_connection(0);
#endif

	wm8994->cur_path = path_num;
	wm8994->universal_playback_path[wm8994->cur_path] (codec);

	/* if tuning flag is on then enter the test mode
	 * to skip overwrite volume value
	 */
	if (wm8994->testmode_config_flag && !wm8994->codecgain_reserve) {
		DEBUG_LOG("Enter Tuning mode");
		wm8994->codecgain_reserve = 1;
	}

	return 0;
}

static int wm8994_get_voice_path(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = wm8994->cur_path;

	return 0;
}

static int wm8994_set_voice_path(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_codec *codec = component->codec;
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);
	struct soc_enum *mc = (struct soc_enum *)kcontrol->private_value;

	int path_num = ucontrol->value.integer.value[0];

	if (strcmp(mc->texts[path_num], voicecall_path[path_num])) {
		DEBUG_LOG_ERR("Unknown path %s\n", mc->texts[path_num]);
		return -ENODEV;
	}

	switch (path_num) {
	case OFF:
		DEBUG_LOG("Switching off output path\n");
		break;
	case RCV:
	case SPK:
	case HP:
	case HP_NO_MIC:
#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
		DEBUG_LOG("routing voice path to %s\n", mc->texts[path_num]);
		wm8994->pdata->set_dap_connection(0);
		break;
#endif
	case BT:
		DEBUG_LOG("routing  voice path to %s\n", mc->texts[path_num]);
#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
		wm8994->pdata->set_dap_connection(1);
#endif
		break;
	default:
		DEBUG_LOG_ERR("path[%d] does not exists!\n", path_num);
		return -ENODEV;
		break;
	}

	if (wm8994->cur_path != path_num ||
			!(wm8994->codec_state & CALL_ACTIVE)) {
		wm8994->codec_state |= CALL_ACTIVE;
		wm8994->cur_path = path_num;
		wm8994->universal_voicecall_path[wm8994->cur_path] (codec);
	} else {
		int val;
		val = snd_soc_read(codec, WM8994_AIF1_DAC1_FILTERS_1);
		val &= ~(WM8994_AIF1DAC1_MUTE_MASK);
		val |= (WM8994_AIF1DAC1_UNMUTE);
		snd_soc_write(codec, WM8994_AIF1_DAC1_FILTERS_1, val);
	}

	return 0;
}

static int wm8994_get_input_source(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);

	DEBUG_LOG("input_source_state = [%d]", wm8994->input_source);

	return wm8994->input_source;
}

static int wm8994_set_input_source(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);

	int control_flag = ucontrol->value.integer.value[0];

	DEBUG_LOG("Changed input_source state [%d] => [%d]",
			wm8994->input_source, control_flag);

	wm8994->input_source = control_flag;

	return 0;
}

static int wm8994_get_headset_analog_vol(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	DEBUG_LOG("");
	return 0;
}

static int wm8994_set_headset_analog_vol(struct snd_kcontrol *kcontrol,
				 struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct snd_soc_codec *codec = component->codec;
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);

	struct soc_enum *mc = (struct soc_enum *)kcontrol->private_value;
	int control_flag = ucontrol->value.integer.value[0];
	u16 val;

#if defined(CONFIG_MACH_SAMSUNG_P4)
#if defined(CONFIG_TARGET_LOCALE_KOR)
	unsigned short analog_vol_table[] = {0x8, 0x8, 0xB, 0xF, 0x13, 0x16,
						0x19, 0x1C, 0x20, 0x23, 0x26,
						0x29, 0x2C, 0x2F, 0x32, 0x36};
#else
	/* Europe */
	unsigned short analog_vol_table_EUR[] = {0x17, 0x17, 0x17, 0x17, 0x17,
						0x17, 0x1A, 0x1D, 0x1F, 0x21,
						0x23, 0x25, 0x27, 0x29, 0x2C,
						0x2F};
	/* Non Europe */
	unsigned short analog_vol_table_USA[] = {0x8, 0x8, 0xB, 0xF, 0x13, 0x16,
						0x19, 0x1C, 0x20, 0x23, 0x26,
						0x29, 0x2C, 0x2F, 0x32, 0x36};

	unsigned short *analog_vol_table;

	if (wm8994->target_locale == LC_EUR)
		analog_vol_table = analog_vol_table_EUR;
	else if (wm8994->target_locale == LC_NONEUR)
		analog_vol_table = analog_vol_table_USA;
	else
		analog_vol_table = analog_vol_table_EUR;
#endif
#elif defined(CONFIG_MACH_SAMSUNG_P4WIFI)
#if defined(CONFIG_TARGET_LOCALE_KOR)
	unsigned short analog_vol_table[] = {0x8, 0x8, 0xB, 0xF, 0x13, 0x16,
						0x19, 0x1C, 0x20, 0x23, 0x26,
						0x29, 0x2C, 0x2F, 0x32, 0x36};
#else
	/* Europe */
	unsigned short analog_vol_table_EUR[] = {0x17, 0x17, 0x17, 0x17, 0x17,
						0x17, 0x1A, 0x1D, 0x1F, 0x21,
						0x23, 0x25, 0x27, 0x29, 0x2C,
						0x2F};
	/* Non Europe */
	unsigned short analog_vol_table_USA[] = {0x8, 0x8, 0xB, 0xF, 0x13, 0x16,
						0x19, 0x1C, 0x20, 0x23, 0x26,
						0x29, 0x2C, 0x2F, 0x32, 0x36};

	unsigned short *analog_vol_table;

	if (wm8994->target_locale == LC_EUR)
		analog_vol_table = analog_vol_table_EUR;
	else if (wm8994->target_locale == LC_NONEUR)
		analog_vol_table = analog_vol_table_USA;
	else
		analog_vol_table = analog_vol_table_USA;

#endif
#elif defined(CONFIG_MACH_SAMSUNG_P4LTE)
	unsigned short analog_vol_table[] = {0x8, 0x8, 0xB, 0xF, 0x13, 0x16,
						0x19, 0x1C, 0x20, 0x23, 0x26,
						0x29, 0x2C, 0x2F, 0x32, 0x36};
#elif defined(CONFIG_MACH_SAMSUNG_P5)
#if defined(CONFIG_TARGET_LOCALE_KOR)
	unsigned short analog_vol_table[] = {0x8, 0x8, 0xB, 0xF, 0x13, 0x16,
						0x19, 0x1C, 0x20, 0x23, 0x26,
						0x29, 0x2C, 0x2F, 0x32, 0x36};

	if (wm8994->voip_call_active)
		return;

#else
	/* Europe */
	unsigned short analog_vol_table_EUR[] = {0x17, 0x17, 0x17, 0x17, 0x17,
						0x17, 0x1A, 0x1D, 0x1F, 0x21,
						0x23, 0x25, 0x27, 0x29, 0x2C,
						0x30};
	/* Non Europe */
	unsigned short analog_vol_table_USA[] = {0x8, 0x8, 0xB, 0xF, 0x13,
						0x16, 0x19, 0x1C, 0x20, 0x23,
						0x26, 0x29, 0x2C, 0x2F, 0x32,
						0x36};

	unsigned short *analog_vol_table;

	if (wm8994->target_locale == LC_EUR)
		analog_vol_table = analog_vol_table_EUR;
	else if (wm8994->target_locale == LC_NONEUR)
		analog_vol_table = analog_vol_table_USA;
	else
		analog_vol_table = analog_vol_table_EUR;
#endif
#else
	unsigned short analog_vol_table[] = {0x17, 0x17, 0x17, 0x17, 0x17, 0x17,
						0x1A, 0x1D, 0x1F, 0x21, 0x23,
						0x25, 0x27, 0x29, 0x2C, 0x30};
#endif

	if (strcmp(mc->texts[control_flag], analog_vol_control[control_flag])) {
		DEBUG_LOG_ERR("Unknown analog gain %s\n",
			mc->texts[control_flag]);
		return -ENODEV;
	}

	if (!strcmp("RESET", analog_vol_control[control_flag])) {
		val = wm8994_get_codec_gain(PLAYBACK_MODE, PLAYBACK_HP,
						WM8994_LEFT_OUTPUT_VOLUME);
		if ((val &  WM8994_HPOUT1L_VOL_MASK) ==
			(snd_soc_read(codec, WM8994_LEFT_OUTPUT_VOLUME) &
			WM8994_HPOUT1L_VOL_MASK)) {
			return 0;
		} else if (wm8994->cur_path != HP && wm8994->cur_path !=
			HP_NO_MIC) {
			DEBUG_LOG("Not Analog Gain Reset, Not HP Path\n");
			return 0;
		} else {
			queue_delayed_work(wm8994_workq, &wm8994->delayed_work,
						msecs_to_jiffies(100));
			DEBUG_LOG("RESET analog gain = 0x%x\n", val);
			return 0;
		}
	} else {
		val = analog_vol_table[control_flag];

		DEBUG_LOG("volume [%d Step] = [0x%x]",
			control_flag, analog_vol_table[control_flag]);
	}


	if (delayed_work_pending(&wm8994->delayed_work)) {
		cancel_delayed_work_sync(&wm8994->delayed_work);
		DEBUG_LOG("canceled wm8994 delayed work queue!");
	}

	snd_soc_update_bits(codec, WM8994_LEFT_OUTPUT_VOLUME,
		WM8994_HPOUT1L_VOL_MASK | WM8994_HPOUT1L_ZC,
		WM8994_HPOUT1L_ZC | val);

	snd_soc_update_bits(codec, WM8994_RIGHT_OUTPUT_VOLUME,
		WM8994_HPOUT1R_VOL_MASK | WM8994_HPOUT1_VU |
		WM8994_HPOUT1R_ZC, WM8994_HPOUT1_VU |
		WM8994_HPOUT1R_ZC | val);

	return 0;
}

static int wm8994_get_locale(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);


	int config_value = wm8994->target_locale;
	ucontrol->value.integer.value[0] = config_value;
	DEBUG_LOG("wm8994_get_locale = %s (%d)", sales_code[config_value],
		config_value);
	return 0;
}

static int wm8994_set_locale(struct snd_kcontrol *kcontrol,
			struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);


	int config_value = ucontrol->value.integer.value[0];
	wm8994->target_locale = config_value;
	DEBUG_LOG("wm8994_set_locale (%d) = %s", config_value,
		sales_code[config_value]);
	return 0;
}

#define  SOC_WM899X_OUTPGA_DOUBLE_R_TLV(xname, reg_left, reg_right,\
		xshift, xmax, xinvert, tlv_array) \
{	.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname),\
	.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |\
		 SNDRV_CTL_ELEM_ACCESS_READWRITE,\
	.tlv.p = (tlv_array), \
	.info = snd_soc_info_volsw, \
	.get = snd_soc_get_volsw_2r, .put = wm899x_outpga_put_volsw_vu, \
	.private_value = (unsigned long)&(struct soc_mixer_control) \
		{.reg = reg_left, .rreg = reg_right, .shift = xshift, \
		.max = xmax, .invert = xinvert} }

#define SOC_WM899X_OUTPGA_SINGLE_R_TLV(xname, reg, shift, max, invert,\
		tlv_array) {\
		.iface = SNDRV_CTL_ELEM_IFACE_MIXER, .name = (xname), \
		.access = SNDRV_CTL_ELEM_ACCESS_TLV_READ |\
				SNDRV_CTL_ELEM_ACCESS_READWRITE,\
		.tlv.p = (tlv_array), \
		.info = snd_soc_info_volsw, \
		.get = snd_soc_get_volsw, .put = wm899x_inpga_put_volsw_vu, \
		.private_value = SOC_SINGLE_VALUE(reg, shift, max, invert, 0) }

static const DECLARE_TLV_DB_SCALE(digital_tlv, -7162, 37, 1);
static const DECLARE_TLV_DB_LINEAR(digital_tlv_spkr, -5700, 600);
static const DECLARE_TLV_DB_LINEAR(digital_tlv_rcv, -5700, 600);
static const DECLARE_TLV_DB_LINEAR(digital_tlv_headphone, -5700, 600);
static const DECLARE_TLV_DB_LINEAR(digital_tlv_mic, -7162, 7162);

static const struct soc_enum path_control_enum[] = {
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(playback_path), playback_path),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(voicecall_path), voicecall_path),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(mic_path), mic_path),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(input_source_state), input_source_state),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(tuning_control), tuning_control),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(voip_call_state), voip_call_state),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(analog_vol_control), analog_vol_control),
#ifdef WM8994_FACTORY_LOOPBACK
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(loopback_path), loopback_path),
#endif
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(sales_code), sales_code),
#ifdef WM8994_MUTE_STATE
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(state_mute), state_mute),
#endif
#ifdef WM8994_DOCK_STATE
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(state_dock), state_dock),
#endif
#ifdef WM8994_VOIP_BT_NREC
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(state_voip_bt_nrec), state_voip_bt_nrec),
#endif
};

static const struct snd_kcontrol_new wm8994_snd_controls[] = {
	SOC_WM899X_OUTPGA_DOUBLE_R_TLV("Playback Volume",
				       WM8994_LEFT_OPGA_VOLUME,
				       WM8994_RIGHT_OPGA_VOLUME, 0, 0x3F, 0,
				       digital_tlv_rcv),
	SOC_WM899X_OUTPGA_DOUBLE_R_TLV("Playback Spkr Volume",
				       WM8994_SPEAKER_VOLUME_LEFT,
				       WM8994_SPEAKER_VOLUME_RIGHT, 1, 0x3F, 0,
				       digital_tlv_spkr),
	SOC_WM899X_OUTPGA_DOUBLE_R_TLV("Playback Headset Volume",
				       WM8994_LEFT_OUTPUT_VOLUME,
				       WM8994_RIGHT_OUTPUT_VOLUME, 1, 0x3F, 0,
				       digital_tlv_headphone),
	SOC_WM899X_OUTPGA_SINGLE_R_TLV("Capture Volume",
				       WM8994_AIF1_ADC1_LEFT_VOLUME,
				       0, 0xEF, 0, digital_tlv_mic),
	/* Path Control */
	SOC_ENUM_EXT("Playback Path", path_control_enum[0],
		     wm8994_get_path, wm8994_set_path),

	SOC_ENUM_EXT("Voice Call Path", path_control_enum[1],
		     wm8994_get_voice_path, wm8994_set_voice_path),

	SOC_ENUM_EXT("Capture MIC Path", path_control_enum[2],
		     wm8994_get_mic_path, wm8994_set_mic_path),

#if defined USE_INFINIEON_EC_FOR_VT
	SOC_ENUM_EXT("Clock Control", clock_control_enum[0],
		     s3c_pcmdev_get_clock, s3c_pcmdev_set_clock),
#endif
	SOC_ENUM_EXT("Input Source", path_control_enum[3],
		     wm8994_get_input_source, wm8994_set_input_source),

	SOC_ENUM_EXT("Codec Tuning", path_control_enum[4],
		wm8994_get_codec_tuning, wm8994_set_codec_tuning),

	SOC_ENUM_EXT("VoIP Call Active", path_control_enum[5],
		wm8994_get_voip_call, wm8994_set_voip_call),

	SOC_ENUM_EXT("Headset Volume Control", path_control_enum[6],
		wm8994_get_headset_analog_vol, wm8994_set_headset_analog_vol),
#ifdef WM8994_FACTORY_LOOPBACK
	SOC_ENUM_EXT("factory_test_loopback", path_control_enum[7],
		wm8994_get_loopback_path, wm8994_set_loopback_path),
#endif
	SOC_ENUM_EXT("Locale Code", path_control_enum[8],
		wm8994_get_locale, wm8994_set_locale),

#ifdef WM8994_MUTE_STATE
	SOC_ENUM_EXT("set_Codec_Mute", path_control_enum[9],
		     wm8994_get_mute_state, wm8994_set_mute_state),
#endif
#ifdef WM8994_DOCK_STATE
	SOC_ENUM_EXT("GTalk Dock", path_control_enum[10],
		     wm8994_get_dock_state, wm8994_set_dock_state),
#endif
#ifdef WM8994_VOIP_BT_NREC
	SOC_ENUM_EXT("set_Codec_NREC", path_control_enum[10],
		     wm8994_get_voip_bt_nrec_state,
		     wm8994_set_voip_bt_nrec_state),
#endif
};

/* Add non-DAPM controls */
static int wm8994_add_controls(struct snd_soc_codec *codec)
{
	return snd_soc_add_codec_controls(codec, wm8994_snd_controls,
				    ARRAY_SIZE(wm8994_snd_controls));
}

/*
static const struct snd_soc_dapm_widget wm8994_dapm_widgets[] = {
};

static const struct snd_soc_dapm_route audio_map[] = {
};
static int wm8994_add_widgets(struct snd_soc_codec *codec)
{
	snd_soc_dapm_new_controls(&codec->dapm, wm8994_dapm_widgets,
			ARRAY_SIZE(wm8994_dapm_widgets));

	snd_soc_dapm_add_routes(&codec->dapm, audio_map, ARRAY_SIZE(audio_map));

	snd_soc_dapm_new_widgets(&codec->dapm);
	return 0;
}
*/
static int configure_clock(struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	unsigned int reg;

	reg = snd_soc_read(codec, WM8994_AIF1_CLOCKING_1);
	if (wm8994->codec_state != DEACTIVE) {
		if (reg != 0) {
			DEBUG_LOG("Codec clock is already actvied");
			return 0;
		} else
			pr_err("%s:wm8994->codec_state is wrong\n", __func__);

	}

	reg &= ~WM8994_AIF1CLK_ENA;
	reg &= ~WM8994_AIF1CLK_SRC_MASK;
	snd_soc_write(codec, WM8994_AIF1_CLOCKING_1, reg);

	switch (wm8994->sysclk_source) {
	case WM8994_SYSCLK_MCLK:
		DEBUG_LOG("Using %dHz MCLK", wm8994->mclk_rate);

		reg = snd_soc_read(codec, WM8994_AIF1_CLOCKING_1);
		reg &= ~WM8994_AIF1CLK_ENA;
		snd_soc_write(codec, WM8994_AIF1_CLOCKING_1, reg);

		reg = snd_soc_read(codec, WM8994_AIF1_CLOCKING_1);
		reg &= 0x07;

		if (wm8994->mclk_rate > 13500000) {
			reg |= WM8994_AIF1CLK_DIV;
			wm8994->sysclk_rate = wm8994->mclk_rate / 2;
		} else {
			reg &= ~WM8994_AIF1CLK_DIV;
			wm8994->sysclk_rate = wm8994->mclk_rate;
		}
		reg |= WM8994_AIF1CLK_ENA;
		snd_soc_write(codec, WM8994_AIF1_CLOCKING_1, reg);

		/* Enable clocks to the Audio core and sysclk of wm8994 */
		reg = snd_soc_read(codec, WM8994_CLOCKING_1);
		reg &= ~(WM8994_SYSCLK_SRC_MASK | WM8994_DSP_FSINTCLK_ENA_MASK
				| WM8994_DSP_FS1CLK_ENA_MASK);
		reg |= (WM8994_DSP_FS1CLK_ENA | WM8994_DSP_FSINTCLK_ENA);
		snd_soc_write(codec, WM8994_CLOCKING_1, reg);
		break;

	case WM8994_SYSCLK_FLL:
		switch (wm8994->fs) {
		case 8000:
			snd_soc_write(codec, WM8994_FLL1_CONTROL_2, 0x2F00);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_3, 0x3126);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_4, 0x0100);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		case 11025:
			snd_soc_write(codec, WM8994_FLL1_CONTROL_2, 0x1F00);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_3, 0x86C2);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_4, 0x00e0);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		case 12000:
			snd_soc_write(codec, WM8994_FLL1_CONTROL_2, 0x1F00);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_3, 0x3126);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_4, 0x0100);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		case 16000:
			snd_soc_write(codec, WM8994_FLL1_CONTROL_2, 0x1900);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_3, 0xE23E);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_4, 0x0100);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		case 22050:
			snd_soc_write(codec, WM8994_FLL1_CONTROL_2, 0x0F00);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_3, 0x86C2);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_4, 0x00E0);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		case 24000:
			snd_soc_write(codec, WM8994_FLL1_CONTROL_2, 0x0F00);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_3, 0x3126);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_4, 0x0100);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		case 32000:
			snd_soc_write(codec, WM8994_FLL1_CONTROL_2, 0x0C00);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_3, 0xE23E);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_4, 0x0100);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		case 44100:
			snd_soc_write(codec, WM8994_FLL1_CONTROL_2, 0x0700);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_3, 0x86C2);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_4, 0x00E0);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		case 48000:
			snd_soc_write(codec, WM8994_FLL1_CONTROL_2, 0x0700);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_3, 0x3126);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_5, 0x0C88);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_4, 0x0100);
			snd_soc_write(codec, WM8994_FLL1_CONTROL_1,
				WM8994_FLL1_FRACN_ENA | WM8994_FLL1_ENA);
			break;

		default:
			DEBUG_LOG_ERR("Unsupported Frequency\n");
			break;
		}

		reg = snd_soc_read(codec, WM8994_AIF1_CLOCKING_1);
		reg |= WM8994_AIF1CLK_ENA;
		reg |= WM8994_AIF1CLK_SRC_FLL1;
		snd_soc_write(codec, WM8994_AIF1_CLOCKING_1, reg);

		/* Enable clocks to the Audio core and sysclk of wm8994*/
		reg = snd_soc_read(codec, WM8994_CLOCKING_1);
		reg &= ~(WM8994_SYSCLK_SRC_MASK | WM8994_DSP_FSINTCLK_ENA_MASK |
				WM8994_DSP_FS1CLK_ENA_MASK);
		reg |= (WM8994_DSP_FS1CLK_ENA | WM8994_DSP_FSINTCLK_ENA);
		snd_soc_write(codec, WM8994_CLOCKING_1, reg);
		break;

	default:
		dev_err(codec->dev, "System clock not configured\n");
		return -EINVAL;
	}

	DEBUG_LOG("CLK_SYS is %dHz\n", wm8994->sysclk_rate);

	return 0;
}

static int wm8994_set_bias_level(struct snd_soc_codec *codec,
				 enum snd_soc_bias_level level)
{
	struct snd_soc_dapm_context *dapm = snd_soc_codec_get_dapm(codec);
	DEBUG_LOG("");

	switch (level) {
	case SND_SOC_BIAS_ON:
	case SND_SOC_BIAS_PREPARE:
		/* VMID=2*40k */
		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_1,
				    WM8994_VMID_SEL_MASK, 0x2);
		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_2,
				    WM8994_TSHUT_ENA, WM8994_TSHUT_ENA);
		break;

	case SND_SOC_BIAS_STANDBY:
		if (dapm->bias_level == SND_SOC_BIAS_OFF) {
			/* Bring up VMID with fast soft start */
			snd_soc_update_bits(codec, WM8994_ANTIPOP_2,
					    WM8994_STARTUP_BIAS_ENA |
					    WM8994_VMID_BUF_ENA |
					    WM8994_VMID_RAMP_MASK |
					    WM8994_BIAS_SRC,
					    WM8994_STARTUP_BIAS_ENA |
					    WM8994_VMID_BUF_ENA |
					    WM8994_VMID_RAMP_MASK |
					    WM8994_BIAS_SRC);
			/* VMID=2*40k */
			snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_1,
					    WM8994_VMID_SEL_MASK |
					    WM8994_BIAS_ENA,
					    WM8994_BIAS_ENA | 0x2);

			/* Switch to normal bias */
			snd_soc_update_bits(codec, WM8994_ANTIPOP_2,
					    WM8994_BIAS_SRC |
					    WM8994_STARTUP_BIAS_ENA, 0);
		}

		/* VMID=2*240k */
		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_1,
				    WM8994_VMID_SEL_MASK, 0x4);

		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_2,
				    WM8994_TSHUT_ENA, 0);
		break;

	case SND_SOC_BIAS_OFF:
		snd_soc_update_bits(codec, WM8994_ANTIPOP_1,
				    WM8994_LINEOUT_VMID_BUF_ENA, 0);

		snd_soc_update_bits(codec, WM8994_POWER_MANAGEMENT_1,
				    WM8994_VMID_SEL_MASK | WM8994_BIAS_ENA, 0);
		break;
	}

	dapm->bias_level = level;

	return 0;
}

static int wm8994_set_sysclk(struct snd_soc_dai *codec_dai,
			     int clk_id, unsigned int freq, int dir)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	DEBUG_LOG("clk_id =%d ", clk_id);

	switch (clk_id) {
	case WM8994_SYSCLK_MCLK:
		wm8994->mclk_rate = freq;
		wm8994->sysclk_source = clk_id;
		break;
	case WM8994_SYSCLK_FLL:
		wm8994->sysclk_rate = freq;
		wm8994->sysclk_source = clk_id;
		break;

	default:
		pr_err("%s: clk_id is invalid\n", __func__);
		return -EINVAL;
	}

	return 0;
}

static int wm8994_set_dai_fmt(struct snd_soc_dai *dai, unsigned int fmt)
{
	struct snd_soc_codec *codec = dai->codec;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	unsigned int aif1 = snd_soc_read(codec, WM8994_AIF1_CONTROL_1);
	unsigned int aif2 = snd_soc_read(codec, WM8994_AIF1_MASTER_SLAVE);
	DEBUG_LOG("");

	aif1 &= ~(WM8994_AIF1_LRCLK_INV | WM8994_AIF1_BCLK_INV |
			WM8994_AIF1_WL_MASK | WM8994_AIF1_FMT_MASK);

	aif2 &= ~(WM8994_AIF1_LRCLK_FRC_MASK |
			WM8994_AIF1_CLK_FRC | WM8994_AIF1_MSTR);

	switch (fmt & SND_SOC_DAIFMT_MASTER_MASK) {
	case SND_SOC_DAIFMT_CBS_CFS:
		wm8994->master = 0;
		break;
	case SND_SOC_DAIFMT_CBS_CFM:
		aif2 |= (WM8994_AIF1_MSTR | WM8994_AIF1_LRCLK_FRC);
		wm8994->master = 1;
		break;
	case SND_SOC_DAIFMT_CBM_CFS:
		aif2 |= (WM8994_AIF1_MSTR | WM8994_AIF1_CLK_FRC);
		wm8994->master = 1;
		break;
	case SND_SOC_DAIFMT_CBM_CFM:
		aif2 |= (WM8994_AIF1_MSTR | WM8994_AIF1_CLK_FRC |
				WM8994_AIF1_LRCLK_FRC);
		wm8994->master = 1;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_DSP_B:
		aif1 |= WM8994_AIF1_LRCLK_INV;
	case SND_SOC_DAIFMT_DSP_A:
		aif1 |= 0x18;
		break;
	case SND_SOC_DAIFMT_I2S:
		aif1 |= 0x10;
		break;
	case SND_SOC_DAIFMT_RIGHT_J:
		break;
	case SND_SOC_DAIFMT_LEFT_J:
		aif1 |= 0x8;
		break;
	default:
		return -EINVAL;
	}

	switch (fmt & SND_SOC_DAIFMT_FORMAT_MASK) {
	case SND_SOC_DAIFMT_DSP_A:
	case SND_SOC_DAIFMT_DSP_B:
		switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
		case SND_SOC_DAIFMT_NB_NF:
			break;
		case SND_SOC_DAIFMT_IB_NF:
			aif1 |= WM8994_AIF1_BCLK_INV;
			break;
		default:
			return -EINVAL;
		}
		break;

	case SND_SOC_DAIFMT_I2S:
	case SND_SOC_DAIFMT_RIGHT_J:
	case SND_SOC_DAIFMT_LEFT_J:
		switch (fmt & SND_SOC_DAIFMT_INV_MASK) {
		case SND_SOC_DAIFMT_NB_NF:
			break;
		case SND_SOC_DAIFMT_IB_IF:
			aif1 |= WM8994_AIF1_BCLK_INV | WM8994_AIF1_LRCLK_INV;
			break;
		case SND_SOC_DAIFMT_IB_NF:
			aif1 |= WM8994_AIF1_BCLK_INV;
			break;
		case SND_SOC_DAIFMT_NB_IF:
			aif1 |= WM8994_AIF1_LRCLK_INV;
			break;
		default:
			return -EINVAL;
		}
		break;
	default:
		return -EINVAL;
	}

	aif1 |= 0x4000;
	snd_soc_write(codec, WM8994_AIF1_CONTROL_1, aif1);
	snd_soc_write(codec, WM8994_AIF1_MASTER_SLAVE, aif2);
	snd_soc_write(codec, WM8994_AIF1_CONTROL_2, 0x4000);

	return 0;
}

static int wm8994_hw_params(struct snd_pcm_substream *substream,
			    struct snd_pcm_hw_params *params,
			    struct snd_soc_dai *dai)
{
	struct snd_soc_codec *codec = dai->codec;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	int ret, i, best, best_val, cur_val;
	unsigned int clocking1, clocking3, aif1, aif4, aif5;
	DEBUG_LOG("");

	clocking1 = snd_soc_read(codec, WM8994_AIF1_BCLK);
	clocking1 &= ~WM8994_AIF1_BCLK_DIV_MASK;

	clocking3 = snd_soc_read(codec, WM8994_AIF1_RATE);
	clocking3 &= ~(WM8994_AIF1_SR_MASK | WM8994_AIF1CLK_RATE_MASK);

	aif1 = snd_soc_read(codec, WM8994_AIF1_CONTROL_1);
	aif1 &= ~WM8994_AIF1_WL_MASK;
	aif4 = snd_soc_read(codec, WM8994_AIF1ADC_LRCLK);
	aif4 &= ~WM8994_AIF1ADC_LRCLK_DIR;
	aif5 = snd_soc_read(codec, WM8994_AIF1DAC_LRCLK);
	aif5 &= ~WM8994_AIF1DAC_LRCLK_DIR_MASK;

	wm8994->fs = params_rate(params);
	wm8994->bclk = 2 * wm8994->fs;

	switch (params_format(params)) {
	case SNDRV_PCM_FORMAT_S16_LE:
		wm8994->bclk *= 16;
		break;

	case SNDRV_PCM_FORMAT_S20_3LE:
		wm8994->bclk *= 20;
		aif1 |= (0x01 << WM8994_AIF1_WL_SHIFT);
		break;

	case SNDRV_PCM_FORMAT_S24_LE:
		wm8994->bclk *= 24;
		aif1 |= (0x10 << WM8994_AIF1_WL_SHIFT);
		break;

	case SNDRV_PCM_FORMAT_S32_LE:
		wm8994->bclk *= 32;
		aif1 |= (0x11 << WM8994_AIF1_WL_SHIFT);
		break;

	default:
		return -EINVAL;
	}

	ret = configure_clock(codec);
	if (ret != 0)
		return ret;

	dev_dbg(codec->dev, "Target BCLK is %dHz\n", wm8994->bclk);

	/* Select nearest CLK_SYS_RATE */
	if (wm8994->fs == 8000)
		best = 3;
	else {
		best = 0;
		best_val = abs((wm8994->sysclk_rate / clk_sys_rates[0].ratio)
				- wm8994->fs);

		for (i = 1; i < ARRAY_SIZE(clk_sys_rates); i++) {
			cur_val = abs((wm8994->sysclk_rate /
					clk_sys_rates[i].ratio)	- wm8994->fs);

			if (cur_val < best_val) {
				best = i;
				best_val = cur_val;
			}
		}
		dev_dbg(codec->dev, "Selected CLK_SYS_RATIO of %d\n",
				clk_sys_rates[best].ratio);
	}

	clocking3 |= (clk_sys_rates[best].clk_sys_rate
			<< WM8994_AIF1CLK_RATE_SHIFT);

	/* Sampling rate */
	best = 0;
	best_val = abs(wm8994->fs - sample_rates[0].rate);
	for (i = 1; i < ARRAY_SIZE(sample_rates); i++) {
		cur_val = abs(wm8994->fs - sample_rates[i].rate);
		if (cur_val < best_val) {
			best = i;
			best_val = cur_val;
		}
	}
	dev_dbg(codec->dev, "Selected SAMPLE_RATE of %dHz\n",
			sample_rates[best].rate);

	clocking3 |= (sample_rates[best].sample_rate << WM8994_AIF1_SR_SHIFT);

	/* BCLK_DIV */
	best = 0;
	best_val = INT_MAX;
	for (i = 0; i < ARRAY_SIZE(bclk_divs); i++) {
		cur_val = ((wm8994->sysclk_rate) / bclk_divs[i].div)
				  - wm8994->bclk;
		if (cur_val < 0)
			break;
		if (cur_val < best_val) {
			best = i;
			best_val = cur_val;
		}
	}
	wm8994->bclk = (wm8994->sysclk_rate) / bclk_divs[best].div;

	dev_dbg(codec->dev, "Selected BCLK_DIV of %d for %dHz BCLK\n",
			bclk_divs[best].div, wm8994->bclk);

	clocking1 |= bclk_divs[best].bclk_div << WM8994_AIF1_BCLK_DIV_SHIFT;

	/* LRCLK is a simple fraction of BCLK */
	dev_dbg(codec->dev, "LRCLK_RATE is %d\n", wm8994->bclk / wm8994->fs);

	aif4 |= wm8994->bclk / wm8994->fs;
	aif5 |= wm8994->bclk / wm8994->fs;

#ifdef HDMI_USE_AUDIO
	/* set bclk to 32fs for 44.1kHz 16 bit playback.*/
	if (wm8994->fs == 44100)
		snd_soc_write(codec, WM8994_AIF1_BCLK, 0x70);
#endif

	snd_soc_write(codec, WM8994_AIF1_RATE, clocking3);
	snd_soc_write(codec, WM8994_AIF1_CONTROL_1, aif1);

	return 0;
}

static int wm8994_digital_mute(struct snd_soc_dai *codec_dai, int mute)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	int mute_reg;
	int reg;

	switch (codec_dai->id) {
	case 1:
		mute_reg = WM8994_AIF1_DAC1_FILTERS_1;
		break;
	case 2:
		mute_reg = WM8994_AIF2_DAC_FILTERS_1;
		break;
	default:
		return -EINVAL;
	}

	if (mute)
		reg = WM8994_AIF1DAC1_MUTE;
	else
		reg = 0;

	snd_soc_update_bits(codec, mute_reg, WM8994_AIF1DAC1_MUTE, reg);

	return 0;
}

static int wm8994_startup(struct snd_pcm_substream *substream,
			  struct snd_soc_dai *codec_dai)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK)
		wm8994->stream_state |=  PCM_STREAM_PLAYBACK;
	else
		wm8994->stream_state |= PCM_STREAM_CAPTURE;


	if (wm8994->power_state == CODEC_OFF) {
		wm8994->power_state = CODEC_ON;
		DEBUG_LOG("Turn on codec!! Power state =[%d]",
				wm8994->power_state);

		/* For initialize codec */
		snd_soc_write(codec, WM8994_POWER_MANAGEMENT_1,
				0x3 << WM8994_VMID_SEL_SHIFT | WM8994_BIAS_ENA);
		msleep(50);
		snd_soc_write(codec, WM8994_POWER_MANAGEMENT_1,
				WM8994_VMID_SEL_NORMAL | WM8994_BIAS_ENA);
		snd_soc_write(codec, WM8994_OVERSAMPLING, 0x0000);
	} else
		DEBUG_LOG("Already turned on codec!!");

	return 0;
}

void wm8994_shutdown(struct snd_pcm_substream *substream,
			    struct snd_soc_dai *codec_dai)
{
	struct snd_soc_codec *codec = codec_dai->codec;
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	int val;

	DEBUG_LOG("Stream_state = [0x%X],  Codec State = [0x%X]",
			wm8994->stream_state, wm8994->codec_state);

	if (wm8994->testmode_config_flag) {
		DEBUG_LOG_ERR("Testmode is activated!! Don't shutdown!!");
		return;
	}

	/* check and sync the capture flag */
	if (substream->stream == SNDRV_PCM_STREAM_CAPTURE) {
		wm8994->stream_state &=  ~(PCM_STREAM_CAPTURE);
		wm8994->codec_state &= ~(CAPTURE_ACTIVE);

		/* disable only rec path when other scenario is active */
		if (wm8994->codec_state)
			wm8994_disable_rec_path(codec);
	}

	/* check and sync the playback flag */
	if (substream->stream == SNDRV_PCM_STREAM_PLAYBACK) {
		wm8994->codec_state &= ~(PLAYBACK_ACTIVE);
		wm8994->stream_state &= ~(PCM_STREAM_PLAYBACK);

		/* mute only AIF1DAC1 when call, radio or recording is active */
		if (wm8994->codec_state) {
			val = snd_soc_read(codec, WM8994_AIF1_DAC1_FILTERS_1);
			val |= (WM8994_AIF1DAC1_MUTE);
			snd_soc_write(codec, WM8994_AIF1_DAC1_FILTERS_1, val);
		}
	}

	/* codec off */
	if ((wm8994->codec_state == DEACTIVE) &&
			(wm8994->stream_state == PCM_STREAM_DEACTIVE)) {
		DEBUG_LOG("Turn off Codec!!");
		wm8994->pdata->set_mic_bias(false);
		wm8994->power_state = CODEC_OFF;
		wm8994->cur_path = OFF;
		wm8994->rec_path = MIC_OFF;
		wm8994->ringtone_active = RING_OFF;
		snd_soc_write(codec, WM8994_SOFTWARE_RESET, 0x0000);
		/*
		*Due to reducing sleep currunt
		*CS/ADDR pull control register should be changed to pull up
		*/
#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
		val = snd_soc_read(codec, WM8994_PULL_CONTROL_2);
		val &= ~(WM8994_DMICDAT1_PD);
		snd_soc_write(codec, WM8994_PULL_CONTROL_2, val);
#endif

		return;
	}

	/* codec is alive */
	DEBUG_LOG("Preserve codec state = [0x%X], Stream State = [0x%X]",
			wm8994->codec_state, wm8994->stream_state);

}

#define WM8994_RATES SNDRV_PCM_RATE_44100
#define WM8994_FORMATS (SNDRV_PCM_FMTBIT_S16_LE | SNDRV_PCM_FMTBIT_S20_3LE |\
			SNDRV_PCM_FMTBIT_S24_LE | SNDRV_PCM_FMTBIT_S32_LE)
static struct snd_soc_dai_ops wm8994_ops = {
	.startup = wm8994_startup,
	.shutdown = wm8994_shutdown,
	.set_sysclk = wm8994_set_sysclk,
	.set_fmt = wm8994_set_dai_fmt,
	.hw_params = wm8994_hw_params,
	.digital_mute = NULL,
};

struct snd_soc_dai_driver wm8994_dai[] = {
	{
		.name = "wm8994-aif1",
		.playback = {
			.stream_name = "Playback",
			.channels_min = 1,
			.channels_max = 6,
			.rates = WM8994_RATES,
			.formats = WM8994_FORMATS,
		},
		.capture = {
			.stream_name = "Capture",
			.channels_min = 1,
			.channels_max = 2,
			.rates = WM8994_RATES,
			.formats = WM8994_FORMATS,
		},
		.ops = &wm8994_ops,
	},
};

static int __init gain_code_setup(char *str)
{

	gain_code = 0;

	if (!strcmp(str, "")) {
		pr_info("gain_code field is empty. use default value\n");
		return 0;
	}

	if (!strcmp(str, "1"))
		gain_code = 1;

	return 0;
}
__setup("gain_code=", gain_code_setup);

int gain_code_check(void)
{
	return gain_code;
}

#if 0
int Is_call_active()
{
	int ret = 0;
	struct snd_soc_codec *codec = wm8994_codec;
	struct wm8994_priv *wm8994 = codec->drvdata;

	if (wm8994->codec_state & CALL_ACTIVE)
		ret = 1;
	else
		ret = 0;
	DEBUG_LOG("Is_call_active = %d", ret);
	return ret;
}
EXPORT_SYMBOL(Is_call_active);
#endif

// static void *wm8994_pdata;
/*
 * initialise the WM8994 driver
 * register the mixer and dsp interfaces with the kernel
 */
static int wm8994_init(struct snd_soc_codec *codec,
		       struct wm8994_platform_data *pdata)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);
	int ret = 0;
	u16 val = 0;
	DEBUG_LOG("");

	wm8994->universal_playback_path = universal_wm8994_playback_paths;
	wm8994->universal_voicecall_path = universal_wm8994_voicecall_paths;
	wm8994->universal_mic_path = universal_wm8994_mic_paths;
	wm8994->universal_clock_control = universal_clock_controls;
	wm8994->stream_state = PCM_STREAM_DEACTIVE;
	wm8994->cur_path = OFF;
	wm8994->rec_path = MIC_OFF;
	wm8994->power_state = CODEC_OFF;
	wm8994->input_source = DEFAULT;
	wm8994->ringtone_active = RING_OFF;
	wm8994->pdata = pdata;

	wm8994->gain_code = gain_code_check();
	wm8994->testmode_config_flag = 0;
	wm8994->codecgain_reserve = 0;
	wm8994->voip_call_active = VOIP_OFF;
#ifdef WM8994_FACTORY_LOOPBACK
	wm8994->loopback_path_control = off;
#endif
#ifdef WM8994_MUTE_STATE
	wm8994->mute_state = MUTE_OFF;
#endif
#ifdef WM8994_VOIP_BT_NREC
	wm8994->voip_bt_nrec_state = VOIP_BT_NREC_OFF;
#endif

	wm8994->target_locale = LC_DEFAULT;

	INIT_DELAYED_WORK(&wm8994->delayed_work, wm8994_reset_analog_vol_work);
	wm8994_workq = create_workqueue("wm8994");
	if (wm8994_workq == NULL) {
		DEBUG_LOG_ERR("Fail to create workqueue\n");
		return -ENOMEM;
	}

	wm8994->hw_version = snd_soc_read(codec, WM8994_CHIP_REVISION);

	/*
	*Due to reducing sleep currunt
	*CS/ADDR pull control register should be changed to pull up
	*/
#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
	val = snd_soc_read(codec, WM8994_PULL_CONTROL_2);
	val &= ~(WM8994_DMICDAT1_PD);
	snd_soc_write(codec, WM8994_PULL_CONTROL_2, val);
#endif
	wm8994_add_controls(codec);
	// wm8994_add_widgets(codec);

	// wm8994_pdata = pdata;

	return ret;
}

/* If the i2c layer weren't so broken, we could pass this kind of data
   around */

#include "../../../arch/arm/mach-tegra/iomap.h"
#include "tegra_das.h"
// TEGRA_GPIO_PX5
#define GPIO_CODEC_LDO_EN	189
// TEGRA_GPIO_PX6
#define GPIO_MICBIAS_EN		190
#define TEGRA_APB_MISC_BASE		0x70000000

static void wm8994_set_mic_bias(bool on)
{
	pr_info("%s(on=%s)\n", __func__, on?"true":"false");
	gpio_set_value(GPIO_MICBIAS_EN, on);
}

static void *das_base = IO_ADDRESS(TEGRA_APB_MISC_BASE);

static inline unsigned long das_readl(unsigned long offset)
{
        return readl(das_base + offset);
}

static inline void das_writel(unsigned long value, unsigned long offset)
{
        writel(value, das_base + offset);
}

static void tegra_set_dap_connection(bool on)
{
	int reg_val;

	pr_info("Board P4 : %s : %d\n", __func__, on);
	if (on) {
		/* DAP1 */
		reg_val = das_readl(APB_MISC_DAS_DAP_CTRL_SEL_0);

		reg_val &= ~(DAP_MS_SEL_DEFAULT_MASK << DAP_MS_SEL_SHIFT);
		reg_val |= (1 << DAP_MS_SEL_SHIFT);

		reg_val &= ~(DAP_CTRL_SEL_DEFAULT_MASK << DAP_CTRL_SEL_SHIFT);
		reg_val |= (DAP_CTRL_SEL_DAC1 << DAP_CTRL_SEL_SHIFT);

		das_writel(reg_val, APB_MISC_DAS_DAP_CTRL_SEL_0);

		/* DAP2 */
		reg_val = das_readl(APB_MISC_DAS_DAP_CTRL_SEL_1);

		reg_val &= ~(DAP_MS_SEL_DEFAULT_MASK << DAP_MS_SEL_SHIFT);
		reg_val |= (1 << DAP_MS_SEL_SHIFT);

		reg_val &= ~(DAP_CTRL_SEL_DEFAULT_MASK << DAP_CTRL_SEL_SHIFT);
		reg_val |= (DAP_CTRL_SEL_DAP4 << DAP_CTRL_SEL_SHIFT);

		das_writel(reg_val, APB_MISC_DAS_DAP_CTRL_SEL_1);

		/* DAP3 */
		reg_val = das_readl(APB_MISC_DAS_DAP_CTRL_SEL_2);

		reg_val &= ~(DAP_MS_SEL_DEFAULT_MASK << DAP_MS_SEL_SHIFT);
		reg_val |= (1 << DAP_MS_SEL_SHIFT);

		reg_val &= ~(DAP_CTRL_SEL_DEFAULT_MASK << DAP_CTRL_SEL_SHIFT);
		reg_val |= (DAP_CTRL_SEL_DAP4 << DAP_CTRL_SEL_SHIFT);

		das_writel(reg_val, APB_MISC_DAS_DAP_CTRL_SEL_2);

		/* DAP4 */
		reg_val = das_readl(APB_MISC_DAS_DAP_CTRL_SEL_3);

		reg_val &= ~(DAP_MS_SEL_DEFAULT_MASK << DAP_MS_SEL_SHIFT);
		reg_val |= (0 << DAP_MS_SEL_SHIFT);

		reg_val &= ~(DAP_CTRL_SEL_DEFAULT_MASK << DAP_CTRL_SEL_SHIFT);
		reg_val |= (DAP_CTRL_SEL_DAP2 << DAP_CTRL_SEL_SHIFT);

		das_writel(reg_val, APB_MISC_DAS_DAP_CTRL_SEL_3);

		/* DAC1 */
		reg_val = das_readl(APB_MISC_DAS_DAC_INPUT_DATA_CLK_SEL_0);

		reg_val &= ~(DAC_SDATA2_SEL_DEFAULT_MASK
			<< DAC_SDATA2_SEL_SHIFT);
		reg_val |= ((DAP_CTRL_SEL_DAP1 - DAP_CTRL_SEL_DAP1)
			<< DAC_SDATA2_SEL_SHIFT);

		reg_val &= ~(DAC_SDATA1_SEL_DEFAULT_MASK
			<< DAC_SDATA1_SEL_SHIFT);
		reg_val |= ((DAP_CTRL_SEL_DAP1 - DAP_CTRL_SEL_DAP1)
			<< DAC_SDATA1_SEL_SHIFT);

		reg_val &= ~(DAC_CLK_SEL_DEFAULT_MASK << DAC_CLK_SEL_SHIFT);
		reg_val |= (DAP_CTRL_SEL_DAP1 << DAC_CLK_SEL_SHIFT);

		das_writel(reg_val, APB_MISC_DAS_DAC_INPUT_DATA_CLK_SEL_0);
	} else {
		das_writel(DAP_CTRL_SEL_DAP3, APB_MISC_DAS_DAP_CTRL_SEL_1);
		das_writel((DAP_MS_SEL_MASTER | DAP_CTRL_SEL_DAP2),
			APB_MISC_DAS_DAP_CTRL_SEL_2);
	}
}

static struct wm8994_platform_data wm8994_pdata = {
	.ldo = GPIO_CODEC_LDO_EN,
	.set_mic_bias = wm8994_set_mic_bias,
	.set_dap_connection = tegra_set_dap_connection,
};

static int wm8994_codec_probe(struct snd_soc_codec *codec)
{
	int ret = -ENODEV;
	struct wm8994_platform_data *pdata;

	DEBUG_LOG("");

	pdata = &wm8994_pdata;

	if (!pdata) {
		dev_err(codec->dev, "failed to initialize WM8994. No Pdata.\n");
		goto err_bad_pdata;
	}

	if (!pdata->set_mic_bias) {
		dev_err(codec->dev, "bad pdata WM8994\n");
		goto err_bad_pdata;
	}

	codec->hw_write = (hw_write_t) i2c_master_send;
	codec->control_data = to_i2c_client(codec->dev);

	ret = wm8994_init(codec, pdata);
	if (ret) {
		dev_err(codec->dev, "failed to initialize WM8994\n");
		goto err_init;
	}

#ifdef CONFIG_SND_VOODOO
	voodoo_hook_wm8994_pcm_probe(codec);
#endif

	return ret;

err_init:
err_ldo:
err_bad_pdata:
	return ret;
}

static int  wm8994_codec_remove(struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994_priv = snd_soc_codec_get_drvdata(codec);

	destroy_workqueue(wm8994_workq);

	return 0;
}

#ifdef CONFIG_PM
static int wm8994_codec_suspend(struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	DEBUG_LOG("Codec State = [0x%X], Stream State = [0x%X]",
			wm8994->codec_state, wm8994->stream_state);

	if (wm8994->codec_state == DEACTIVE &&
		wm8994->stream_state == PCM_STREAM_DEACTIVE) {
		wm8994->power_state = CODEC_OFF;
		snd_soc_write(codec, WM8994_SOFTWARE_RESET, 0x0000);
		wm8994_ldo_control(wm8994->pdata, 0);
	}

	return 0;
}

static int wm8994_codec_resume(struct snd_soc_codec *codec)
{
	struct wm8994_priv *wm8994 = snd_soc_codec_get_drvdata(codec);

	DEBUG_LOG("%s..", __func__);
	DEBUG_LOG_ERR("------WM8994 Revision = [%d]-------",
		      wm8994->hw_version);

	if (wm8994->power_state == CODEC_OFF) {
		/* Turn on sequence by recommend Wolfson.*/
		wm8994_ldo_control(wm8994->pdata, 1);
	}
	return 0;
}
#endif

static struct regmap *wm8994_get_regmap(struct device *dev)
{
	struct wm8994 *control = dev_get_drvdata(dev->parent);

	return control->regmap;
}

static struct snd_soc_codec_driver soc_codec_dev_wm8994 = {
	.probe =	wm8994_codec_probe,
	.remove =	wm8994_codec_remove,
	.suspend =	wm8994_codec_suspend,
	.resume =	wm8994_codec_resume,
	.get_regmap =   wm8994_get_regmap,
	.set_bias_level = wm8994_set_bias_level,
};

//////////////////////////////////////////////////////////////////////////////////////////
// platform driver
//////////////////////////////////////////////////////////////////////////////////////////

static int wm8994_probe(struct platform_device *pdev)
{
	struct wm8994_priv *wm8994;

	wm8994 = devm_kzalloc(&pdev->dev, sizeof(struct wm8994_priv),
			      GFP_KERNEL);
	if (wm8994 == NULL)
		return -ENOMEM;
	platform_set_drvdata(pdev, wm8994);

	// mutex_init(&wm8994->fw_lock);

	// wm8994->wm8994 = dev_get_drvdata(pdev->dev.parent);

	pm_runtime_enable(&pdev->dev);
	pm_runtime_idle(&pdev->dev);

	return snd_soc_register_codec(&pdev->dev, &soc_codec_dev_wm8994,
			wm8994_dai, ARRAY_SIZE(wm8994_dai));
}

static int wm8994_remove(struct platform_device *pdev)
{
	snd_soc_unregister_codec(&pdev->dev);
	pm_runtime_disable(&pdev->dev);

	return 0;
}

static int wm8994_suspend(struct device *dev)
{
	struct wm8994_priv *wm8994 = dev_get_drvdata(dev);

	/* Drop down to power saving mode when system is suspended */
	// if (wm8994->jackdet && !wm8994->active_refcount)
	// 	regmap_update_bits(wm8994->wm8994->regmap, WM8994_ANTIPOP_2,
	// 			   WM1811_JACKDET_MODE_MASK,
	// 			   wm8994->jackdet_mode);

	return 0;
}

static int wm8994_resume(struct device *dev)
{
	struct wm8994_priv *wm8994 = dev_get_drvdata(dev);

	// if (wm8994->jackdet && wm8994->jackdet_mode)
	// 	regmap_update_bits(wm8994->wm8994->regmap, WM8994_ANTIPOP_2,
	// 			   WM1811_JACKDET_MODE_MASK,
	// 			   WM1811_JACKDET_MODE_AUDIO);

	return 0;
}

static const struct dev_pm_ops wm8994_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(wm8994_suspend, wm8994_resume)
};

static struct platform_driver wm8994_codec_driver = {
	.driver = {
		.name = "wm8994-codec",
		.pm = &wm8994_pm_ops,
	},
	.probe = wm8994_probe,
	.remove = wm8994_remove,
};

module_platform_driver(wm8994_codec_driver);

//////////////////////////////////////////////////////////////////////////////////////////

void wm8994_reset_analog_vol_work(struct work_struct *work)
{
	struct wm8994_priv *wm8994 =
		container_of(work, struct wm8994_priv, delayed_work.work);
	struct snd_soc_codec *codec = wm8994->codec;
	u16 val;

	val = wm8994_get_codec_gain(PLAYBACK_MODE, PLAYBACK_HP,
					WM8994_LEFT_OUTPUT_VOLUME);

	snd_soc_update_bits(codec, WM8994_LEFT_OUTPUT_VOLUME,
			WM8994_HPOUT1L_VOL_MASK, WM8994_HPOUT1L_ZC |
			val);

	snd_soc_update_bits(codec, WM8994_RIGHT_OUTPUT_VOLUME,
			WM8994_HPOUT1R_VOL_MASK, WM8994_HPOUT1_VU |
			WM8994_HPOUT1R_ZC |
			val);

	DEBUG_LOG("");
	DEBUG_LOG("RESET analog gain = 0x%x\n", val);
}

MODULE_DESCRIPTION("ASoC WM8994 driver");
MODULE_AUTHOR("Shaju Abraham shaju.abraham@samsung.com");
MODULE_LICENSE("GPL");
