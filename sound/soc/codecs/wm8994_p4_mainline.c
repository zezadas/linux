
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

enum audio_path wm8994_path = OFF;
enum mic_path wm8994_mic_path = MIC_OFF;


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


/*
 * Definitions of sound path
 */
#if 1
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
#endif

select_mic_route universal_wm8994_mic_paths[] = {
	wm8994_record_main_mic,
	wm8994_record_headset_mic,
	wm8994_record_bluetooth,
};

static int wm8994_get_mic_path(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	// struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	// struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);
	// ucontrol->value.integer.value[0] = wm8994->rec_path;

	ucontrol->value.integer.value[0] = wm8994_mic_path;

	return 0;
}

static int wm8994_set_mic_path(struct snd_kcontrol *kcontrol,
			       struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	// struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);

	DEBUG_LOG("");
#if 0
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
#endif

	switch (ucontrol->value.integer.value[0]) {
	case 0:
		tegra_set_dap_connection(0);
		wm8994_set_mic_bias(true);
		wm8994_record_main_mic(component);
		wm8994_mic_path = MAIN;
		break;
	case 1:
		tegra_set_dap_connection(0);
		wm8994_set_mic_bias(true);
		wm8994_record_headset_mic(component);
		wm8994_mic_path = EAR;
		break;
	case 2:
		tegra_set_dap_connection(1);
		wm8994_set_mic_bias(true);
		wm8994_record_bluetooth(component);
		wm8994_mic_path = BT_REC;
		break;
	case 3:
		wm8994_set_mic_bias(false);
		wm8994_disable_rec_path(component);
		wm8994_mic_path = MIC_OFF;
		break;
	}

	return 0;
}

#if 1
static int wm8994_get_path(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	// struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	// struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);

	ucontrol->value.integer.value[0] = wm8994_path;

	return 0;
}

static int wm8994_set_path(struct snd_kcontrol *kcontrol,
			   struct snd_ctl_elem_value *ucontrol)
{
	struct snd_soc_component *component = snd_kcontrol_chip(kcontrol);
	// struct snd_soc_codec *codec = component->codec;
	// struct wm8994_priv *wm8994 = snd_soc_component_get_drvdata(component);
	// struct soc_enum *mc = (struct soc_enum *)kcontrol->private_value;
	int val;
	int path_num = ucontrol->value.integer.value[0];

#if 0
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
#endif
#ifdef CONFIG_MACH_SAMSUNG_VARIATION_TEGRA
	if (path_num == BT)
		// wm8994->pdata->set_dap_connection(1);
		tegra_set_dap_connection(1);
	else
		// wm8994->pdata->set_dap_connection(0);
		tegra_set_dap_connection(0);
#endif

	// wm8994->cur_path = path_num;
	// wm8994->universal_playback_path[wm8994->cur_path] (codec);
	wm8994_path = path_num;
	universal_wm8994_playback_paths[path_num](component);

#if 0
	/* if tuning flag is on then enter the test mode
	 * to skip overwrite volume value
	 */
	if (wm8994->testmode_config_flag && !wm8994->codecgain_reserve) {
		DEBUG_LOG("Enter Tuning mode");
		wm8994->codecgain_reserve = 1;
	}
#endif
	return 0;
}
#endif

static const char *playback_path[] = {
	"OFF", "RCV", "SPK", "HP", "HP_NO_MIC", "BT", "SPK_HP",
	"RING_SPK", "RING_HP", "RING_NO_MIC", "RING_SPK_HP",
	"LINEOUT", "SPK_LINEOUT"
};

static const char *mic_path[] = {
	"Main Mic", "Hands Free Mic", "BT Sco Mic", "MIC OFF"
};
static const struct soc_enum path_control_enum[] = {
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(playback_path), playback_path),
	SOC_ENUM_SINGLE_EXT(ARRAY_SIZE(mic_path), mic_path),
};

struct snd_kcontrol_new wm8994_p4_snd_controls[] = {
	// SOC_WM899X_OUTPGA_DOUBLE_R_TLV("Playback Volume",
	// 			       WM8994_LEFT_OPGA_VOLUME,
	// 			       WM8994_RIGHT_OPGA_VOLUME, 0, 0x3F, 0,
	// 			       digital_tlv_rcv),
	// SOC_WM899X_OUTPGA_DOUBLE_R_TLV("Playback Spkr Volume",
	// 			       WM8994_SPEAKER_VOLUME_LEFT,
	// 			       WM8994_SPEAKER_VOLUME_RIGHT, 1, 0x3F, 0,
	// 			       digital_tlv_spkr),
	// SOC_WM899X_OUTPGA_DOUBLE_R_TLV("Playback Headset Volume",
	// 			       WM8994_LEFT_OUTPUT_VOLUME,
	// 			       WM8994_RIGHT_OUTPUT_VOLUME, 1, 0x3F, 0,
	// 			       digital_tlv_headphone),
	// SOC_WM899X_OUTPGA_SINGLE_R_TLV("Capture Volume",
	// 			       WM8994_AIF1_ADC1_LEFT_VOLUME,
	// 			       0, 0xEF, 0, digital_tlv_mic),
	/* Path Control */
	SOC_ENUM_EXT("Playback Path",
		path_control_enum[0],
		wm8994_get_path, wm8994_set_path),

	// SOC_ENUM_EXT("Voice Call Path", path_control_enum[1],
	// 	     wm8994_get_voice_path, wm8994_set_voice_path),

	SOC_ENUM_EXT("Capture MIC Path",
		path_control_enum[1],
		wm8994_get_mic_path, wm8994_set_mic_path),

// #if defined USE_INFINIEON_EC_FOR_VT
// 	SOC_ENUM_EXT("Clock Control", clock_control_enum[0],
// 		     s3c_pcmdev_get_clock, s3c_pcmdev_set_clock),
// #endif
// 	SOC_ENUM_EXT("Input Source", path_control_enum[3],
// 		     wm8994_get_input_source, wm8994_set_input_source),

// 	SOC_ENUM_EXT("Codec Tuning", path_control_enum[4],
// 		wm8994_get_codec_tuning, wm8994_set_codec_tuning),

// 	SOC_ENUM_EXT("VoIP Call Active", path_control_enum[5],
// 		wm8994_get_voip_call, wm8994_set_voip_call),

// 	SOC_ENUM_EXT("Headset Volume Control", path_control_enum[6],
// 		wm8994_get_headset_analog_vol, wm8994_set_headset_analog_vol),
// #ifdef WM8994_FACTORY_LOOPBACK
// 	SOC_ENUM_EXT("factory_test_loopback", path_control_enum[7],
// 		wm8994_get_loopback_path, wm8994_set_loopback_path),
// #endif
// 	SOC_ENUM_EXT("Locale Code", path_control_enum[8],
// 		wm8994_get_locale, wm8994_set_locale),

// #ifdef WM8994_MUTE_STATE
// 	SOC_ENUM_EXT("set_Codec_Mute", path_control_enum[9],
// 		     wm8994_get_mute_state, wm8994_set_mute_state),
// #endif
// #ifdef WM8994_DOCK_STATE
// 	SOC_ENUM_EXT("GTalk Dock", path_control_enum[10],
// 		     wm8994_get_dock_state, wm8994_set_dock_state),
// #endif
// #ifdef WM8994_VOIP_BT_NREC
// 	SOC_ENUM_EXT("set_Codec_NREC", path_control_enum[10],
// 		     wm8994_get_voip_bt_nrec_state,
// 		     wm8994_set_voip_bt_nrec_state),
// #endif
};
EXPORT_SYMBOL(wm8994_p4_snd_controls);
