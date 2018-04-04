/*
 * CMC623 drm_panel driver.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <linux/backlight.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/regulator/consumer.h>
#include <linux/slab.h>
#include <linux/cmc623.h>

#include <drm/drmP.h>
#include <drm/drm_crtc.h>
#include <drm/drm_panel.h>

#include <video/display_timing.h>
#include <video/of_display_timing.h>
#include <video/videomode.h>

struct cmc623_panel {
	struct drm_panel panel;
	struct device *dev;

	// struct backlight_device *backlight;
};

static inline struct cmc623_panel *to_panel_cmc623(struct drm_panel *panel)
{
	return container_of(panel, struct cmc623_panel, panel);
}

static int cmc623_disable(struct drm_panel *panel)
{
	struct cmc623_panel *cmc623 = to_panel_cmc623(panel);
	dev_dbg(cmc623->dev, "%s\n", __func__);

	return 0;
}

static int cmc623_unprepare(struct drm_panel *panel)
{
	struct cmc623_panel *cmc623 = to_panel_cmc623(panel);
	dev_dbg(cmc623->dev, "%s\n", __func__);

	cmc623_suspend();

	return 0;
}

static int cmc623_prepare(struct drm_panel *panel)
{
	struct cmc623_panel *cmc623 = to_panel_cmc623(panel);
	dev_dbg(cmc623->dev, "%s\n", __func__);

	msleep(1);
	cmc623_resume();

	return 0;
}

static int cmc623_enable(struct drm_panel *panel)
{
	struct cmc623_panel *cmc623 = to_panel_cmc623(panel);
	dev_dbg(cmc623->dev, "%s\n", __func__);

	return 0;
}

/* 
 *	For Magna D10E50T6332 TFT Display timing controller:
 *	Signal					Min. 			Typ.			Max
 *	Frame Frequency(TV)			815			-			850
 *	Vertical Active Display Term(TVD) 	- 			800			-
 *	One Line Scanning Time(TH)		1350			1408			1460
 *	Horizontal Active Display Term(THD) 	-			1280			-
 *
 *	Total clocks per line (TH) = hsw + hfp + columns (THD) + hbp
 *			     1408  = 48  + 16  + 1280          + 64
 *						  
 *	Total LCD lines (TV) 	  = vsw + vfp + rows (TVD)  + vbp
 *			816	  = 3   + 1   + 800	    + 12
 *					
 *	From this data,
 *	- single line takes (48 + 16 + 1280 + 64) clocks = 1408 clocks/line
 *	- full frame takes (3 + 1 + 800 + 12) lines = 816 lines/frame
 *	- full frame in clocks = 1408 * 816 = 1148928 clocks/frame
 *	- 20MHz, the LCD would refresh at 20M/1148928 = 17.4Hz
 *	- 70MHz, the LCD would refresh at 68.94M/1148928 = 60Hz
 */
static const struct drm_display_mode default_mode = {
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

static int cmc623_get_modes(struct drm_panel *panel)
{
	struct cmc623_panel *cmc623 = to_panel_cmc623(panel);
	struct drm_connector *connector = cmc623->panel.connector;
	struct drm_display_mode *mode;

	mode = drm_mode_duplicate(panel->drm, &default_mode);
	if (!mode) {
		DRM_ERROR("failed to add mode %ux%ux@%u\n",
			default_mode.hdisplay, default_mode.vdisplay,
			default_mode.vrefresh);
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

static const struct drm_panel_funcs cmc623_funcs = {
	.disable = cmc623_disable,
	.unprepare = cmc623_unprepare,
	.prepare = cmc623_prepare,
	.enable = cmc623_enable,
	.get_modes = cmc623_get_modes,
};

static int cmc623_probe(struct platform_device *pdev)
{
	struct cmc623_panel *cmc623;
	struct device_node *np;
	int ret;

	np = of_parse_phandle(pdev->dev.of_node, "image-convertor", 0);
	if (np) {
		struct i2c_client* client = of_find_i2c_device_by_node(np);
		of_node_put(np);

		if (!client)
			return -EPROBE_DEFER;

		put_device(&client->dev);
	}

	np = of_parse_phandle(pdev->dev.of_node, "backlight", 0);
	if (np) {
		struct platform_device* pdev = of_find_device_by_node(np);
		of_node_put(np);

		if (!pdev)
			return -EPROBE_DEFER;

		put_device(&pdev->dev);
	}

	pr_info("%s+\n", __func__);

	cmc623 = devm_kzalloc(&pdev->dev, sizeof(*cmc623), GFP_KERNEL);
	if (!cmc623)
		return -ENOMEM;

	cmc623->dev = &pdev->dev;

	/* Register the panel. */
	drm_panel_init(&cmc623->panel);
	cmc623->panel.dev = cmc623->dev;
	cmc623->panel.funcs = &cmc623_funcs;

	ret = drm_panel_add(&cmc623->panel);
	if (ret < 0)
		goto error;

	dev_set_drvdata(cmc623->dev, cmc623);

	pr_info("%s-\n", __func__);

	return 0;

error:
	// put_device(&lvds->backlight->dev);
	return ret;
}

static int cmc623_remove(struct platform_device *pdev)
{
	struct cmc623_panel *cmc623 = dev_get_drvdata(&pdev->dev);

	drm_panel_detach(&cmc623->panel);
	drm_panel_remove(&cmc623->panel);

	cmc623_disable(&cmc623->panel);

	return 0;
}

static const struct of_device_id cmc623_of_table[] = {
	{ .compatible = "samsung,cmc623-panel", },
	{ },
};

MODULE_DEVICE_TABLE(of, cmc623_of_table);

static struct platform_driver cmc623_driver = {
	.probe		= cmc623_probe,
	.remove		= cmc623_remove,
	.driver		= {
		.name	= "cmc623-panel",
		.of_match_table = cmc623_of_table,
	},
};

module_platform_driver(cmc623_driver);

MODULE_AUTHOR("ryang <decatf@gmail.com>");
MODULE_DESCRIPTION("CMC623 Panel Driver");
MODULE_LICENSE("GPL");
