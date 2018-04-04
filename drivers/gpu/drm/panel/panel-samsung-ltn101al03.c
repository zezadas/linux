/*
 * cmc623.c
 *
 * driver supporting CMC623 ImageConverter functions for Samsung P3 device
 *
 * COPYRIGHT(C) Samsung Electronics Co., Ltd. 2006-2010 All Right Reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

#define DEBUG 1

#include <linux/backlight.h>
#include <linux/device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_panel.h>

#include <video/display_timing.h>
#include <video/of_display_timing.h>
#include <video/videomode.h>

struct ltn101al03_data {
	const struct drm_display_mode *mode;

	struct drm_panel panel;
	struct backlight_device *backlight;
};

// static int cmc623_panel_type = CMC623_TYPE_LSI;

// #ifdef MODULE
// module_param(cmc623_panel_type, int, 0644);
// #else
// static int __init cmc623_arg(char *p)
// {
// 	pr_info("%s: panel type=cmc623f\n", __func__);
// 	cmc623_panel_type = CMC623_TYPE_FUJITSU;
// 	return 0;
// }
// early_param("CMC623F", cmc623_arg);
// #endif


static inline struct ltn101al03_data *panel_to_ltn101al03(struct drm_panel *panel)
{
	return container_of(panel, struct ltn101al03_data, panel);
}

static int ltn101al03_unprepare(struct drm_panel *panel)
{
	struct ltn101al03_data *data = panel_to_ltn101al03(panel);
	dev_dbg(panel->dev, "%s\n", __func__);
	backlight_disable(data->backlight);
	return 0;
}

static int ltn101al03_prepare(struct drm_panel *panel)
{
	struct ltn101al03_data *data = panel_to_ltn101al03(panel);
	dev_dbg(panel->dev, "%s\n", __func__);
	backlight_enable(data->backlight);

	dump_stack();

	return 0;
}

static const struct drm_display_mode ltn101al03_mode = {
	.clock = 68750,
	.hdisplay = 1280,
	.hsync_start = 1280 + 16,
	.hsync_end = 1280 + 16 + 48,
	.htotal = 1280 + 16 + 48 +64,
	.vdisplay = 800,
	.vsync_start = 800 + 2,
	.vsync_end = 800 + 2 + 3,
	.vtotal = 800 + 2 + 3 + 11,
	.vrefresh = 60,
};

static const struct drm_display_mode ltn101al03f_mode = {
	.clock = 76000,
	.hdisplay = 1280,
	.hsync_start = 1280 + 16,
	.hsync_end = 1280 + 16 + 48,
	.htotal = 1280 + 16 + 48 +64,
	.vdisplay = 800,
	.vsync_start = 800 + 2,
	.vsync_end = 800 + 2 + 3,
	.vtotal = 800 + 2 + 3 + 11,
	.vrefresh = 60,
};

static int ltn101al03_get_modes(struct drm_panel *panel)
{
	struct ltn101al03_data *data = panel_to_ltn101al03(panel);
	struct drm_connector *connector = data->panel.connector;
	const struct drm_display_mode *panel_mode = data->mode;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(panel->drm, panel_mode);
	if (!mode) {
		DRM_ERROR("failed to add mode %ux%ux@%u\n",
			panel_mode->hdisplay, panel_mode->vdisplay,
			panel_mode->vrefresh);
		return -ENOMEM;
	}

	drm_mode_set_name(mode);

	mode->type |= DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = 217;
	connector->display_info.height_mm = 135;
	connector->display_info.bpc = 8;

	return 1;
}

static const struct drm_panel_funcs ltn101al03_panel_funcs = {
	.unprepare = ltn101al03_unprepare,
	.prepare = ltn101al03_prepare,
	.get_modes = ltn101al03_get_modes,
};

static int ltn101al03_probe(struct platform_device *pdev)
{
	struct ltn101al03_data *data;
	struct device_node *np;
	int err = 0;

	dev_info(&pdev->dev, "%s\n", __func__);
	data = devm_kzalloc(&pdev->dev, sizeof(struct ltn101al03_data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	// if (cmc623_panel_type == CMC623_TYPE_FUJITSU) {
	// 	data->mode = &cmc623f_mode;
	// } else if (cmc623_panel_type == CMC623_TYPE_LSI) {
	// 	data->mode = &cmc623_mode;
	// } else {
	// 	rate = 0;
	// 	WARN(1, "Unknown panel type.");
	// }

	data->mode = &ltn101al03_mode;

	np = of_parse_phandle(pdev->dev.of_node, "backlight", 0);
	if (np) {
		data->backlight = of_find_backlight_by_node(np);
		of_node_put(np);

		if (!data->backlight)
			return -EPROBE_DEFER;
	}

	/* Register the panel */
	drm_panel_init(&data->panel);
	data->panel.dev = &pdev->dev;
	data->panel.funcs = &ltn101al03_panel_funcs;

	err = drm_panel_add(&data->panel);
	if (err < 0)
		return err;

	return 0;
}

static int ltn101al03_remove(struct platform_device *pdev)
{
	struct ltn101al03_data *data = dev_get_drvdata(&pdev->dev);

	dev_dbg(&pdev->dev, "%s\n", __func__);

	drm_panel_detach(&data->panel);
	drm_panel_remove(&data->panel);

	dev_dbg(&pdev->dev, "%s: done.\n", __func__);

	return 0;
}

static const struct platform_device_id ltn101al03_id[] = {
	{ "ltn101al03", 0 },
	{ },
};
MODULE_DEVICE_TABLE(platform, ltn101al03_id);

static const struct of_device_id ltn101al03_of_table[] = {
	{ .compatible = "samsung,ltn101al03", },
	{  },
};
MODULE_DEVICE_TABLE(of, ltn101al03_of_table);

static struct platform_driver ltn101al03_driver = {
	.probe		= ltn101al03_probe,
	.remove		= ltn101al03_remove,
	.driver		= {
		.name	= "samsung-ltn101al03",
		.of_match_table = ltn101al03_of_table,
	},
	.id_table	= ltn101al03_id,
};

module_platform_driver(ltn101al03_driver);

MODULE_AUTHOR("Samsung");
MODULE_DESCRIPTION("Samsung ltn101al03 panel");
MODULE_LICENSE("GPL");
