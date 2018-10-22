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
#include <linux/clk.h>
#include <linux/clk-provider.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_gpio.h>
#include <linux/platform_device.h>

#include <video/display_timing.h>
#include <video/of_display_timing.h>
#include <video/videomode.h>

#include "cmc6230r.h"

#define CMC623_INITIAL_BRIGHTNESS		100
#define CMC623_MAX_BRIGHTNESS			255

#define CMC623_INITIAL_INTENSITY		800
#define CMC623_MAX_INTENSITY			1600

enum cmc623_gpios {
	IMA_N_RST = 0,
	IMA_PWREN,
	IMA_BYPASS,
	IMA_SLEEP,
	LVDS_N_SHDN,
	MLCD_ON,
	MLCD_ON1,
	BL_RESET,
	NUM_GPIOS,
};

struct cmc623_backlight {
	struct backlight_device *bd;
	struct backlight_properties last_prop;
	struct mutex pwm_mutex;

	int max_brightness;
	int mid_brightness;
	int low_brightness;
	int dim_brightness;

	int max_intensity;
	int mid_intensity;
	int low_intensity;
	int dim_intensity;
	int dark_intensity;
};

struct cmc623_data {
	struct i2c_client *client;
	struct gpio_desc *gpios[NUM_GPIOS];
	struct clk *clk_parent;
	struct clk *clk;

	/* model specific properties */
	struct mutex tuning_mutex;
	const struct cmc623_register_set *init_regs;
	int ninit_regs;
	const struct cmc623_register_set *tune_regs;
	int ntune_regs;
	void (*resume_gpios)(struct i2c_client *client);

	struct cmc623_backlight bl;

	bool suspended;
	bool initialized;
};


static int cmc623_brightness_to_intensity(struct i2c_client *client,
	int brightness);


static int cmc623_panel_type = CMC623_TYPE_LSI;

#ifdef MODULE
module_param(cmc623_panel_type, int, 0644);
#else
static int __init cmc623_arg(char *p)
{
	pr_info("%s: panel type=cmc623f\n", __func__);
	cmc623_panel_type = CMC623_TYPE_FUJITSU;
	return 0;
}
early_param("CMC623F", cmc623_arg);
#endif

static int cmc623_write_reg(struct i2c_client *client,
	unsigned char addr, unsigned long data)
{
	struct i2c_msg msg[1];
	unsigned char buf[3];
	int err;

	buf[0] = addr;
	buf[1] = (data >> 8) & 0xFF;
	buf[2] = data & 0xFF;
	msg->addr = client->addr;
	msg->flags = 0;
	msg->len = 3;
	msg->buf = buf;

	err = i2c_transfer(client->adapter, msg, 1);
	if (err < 0) {
		dev_err(&client->dev, "%s: i2c_transfer failed(%d) "
			"addr = %x, data = %lx\n", __func__, err, addr, data);
		return err;
	}

	return 0;
}

static int cmc623_write_regs(struct i2c_client *client,
	const struct cmc623_register_set *regs, int nregs)
{
	int i, err = 0;

	for (i = 0; i < nregs; i++) {
		err = cmc623_write_reg(client, regs[i].addr, regs[i].data);
		if (err)
			goto done;

		if (regs[i].addr == CMC623_REG_SWRESET &&
				regs[i].data == 0xffff)
			usleep_range(2000, 2100);
	}

done:
	return err;
}

static void cmc623_write_cabc_registers(struct i2c_client *client, int value)
{
	const unsigned char *p_plut;
	u16 min_duty;
	unsigned long reg;

	p_plut = cmc623_default_plut;
	min_duty = p_plut[7] * value / 100;

	if (min_duty < 4) {
		reg = 0xc000 | (max(1, (value*p_plut[3]/100)));
	} else {
		reg = (p_plut[0] * value / 100) << 8 | (p_plut[1] * value / 100);
		cmc623_write_reg(client, 0x76, reg);
		reg = (p_plut[2] * value / 100) << 8 | (p_plut[3] * value / 100);
		cmc623_write_reg(client, 0x77, reg);
		reg = (p_plut[4] * value / 100) << 8 | (p_plut[5] * value / 100);
		cmc623_write_reg(client, 0x78, reg);
		reg = (p_plut[6] * value / 100) << 8 | (p_plut[7] * value / 100);
		cmc623_write_reg(client, 0x79, reg);
		reg = (p_plut[8] * value / 100) << 8;
		cmc623_write_reg(client, 0x7a, reg);

		reg = 0x5000 | (value<<4);
	}

	cmc623_write_reg(client, 0xB4, reg);
}

static void cmc623_pwm_cabc(struct i2c_client *client, int value)
{
	struct cmc623_data *data = i2c_get_clientdata(client);

	dev_dbg(&client->dev, "%s: intensity = %d\n", __func__, value);

	mutex_lock(&data->tuning_mutex);
	cmc623_write_reg(client, 0x00, 0x0000);
	cmc623_write_cabc_registers(client, value);
	cmc623_write_reg(client, 0x28, 0x0000);
	mutex_unlock(&data->tuning_mutex);
}

static void cmc623_pwm_nocabc(struct i2c_client *client,int value)
{
	struct cmc623_data *data = i2c_get_clientdata(client);

	mutex_lock(&data->tuning_mutex);
	cmc623_write_reg(client, 0x00, 0x0000);
	cmc623_write_reg(client, 0xB4, 0xc000 | (value));
	cmc623_write_reg(client, 0x28, 0x0000);
	mutex_unlock(&data->tuning_mutex);
}

static void cmc623_set_backlight(struct i2c_client *client, int intensity)
{
	int pwm;

	dev_dbg(&client->dev, "%s: intensity = %d\n", __func__, intensity);

	pwm = max(0, min(CMC623_MAX_INTENSITY, intensity));

	/* scale to a range of 1 - 100 */
	pwm = max(1, (pwm * 100) / CMC623_MAX_INTENSITY);

	cmc623_pwm_cabc(client, pwm);
}

static void cmc623_suspend(struct i2c_client *client)
{
	struct cmc623_data *data = i2c_get_clientdata(client);
	
	if (!data->initialized || data->suspended)
		return;

	dev_dbg(&client->dev, "%s\n", __func__);

	gpiod_set_value(data->gpios[BL_RESET], 0);
	msleep(100);

	gpiod_set_value(data->gpios[IMA_SLEEP], 0);
	gpiod_set_value(data->gpios[IMA_BYPASS], 0);

	usleep_range(1000, 2000);

	gpiod_set_value(data->gpios[IMA_PWREN] , 0);
	gpiod_set_value(data->gpios[LVDS_N_SHDN], 0);

	gpiod_set_value(data->gpios[MLCD_ON1], 0);
	usleep_range(1000, 2000);

	gpiod_set_value(data->gpios[MLCD_ON], 0);

	msleep(200);
	data->suspended = true;
}

static void cmc623_resume_gpios_fujitsu(struct i2c_client *client)
{
	struct cmc623_data *data = i2c_get_clientdata(client);

	usleep_range(1000, 2000);
	gpiod_set_value(data->gpios[IMA_BYPASS], 1);
	usleep_range(1000, 2000);

	gpiod_set_value(data->gpios[IMA_SLEEP], 1);
	usleep_range(5000, 6000);

	gpiod_set_value(data->gpios[IMA_PWREN] , 1);
	usleep_range(5000, 6000);
}

static void cmc623_resume_gpios_lsi(struct i2c_client *client)
{
	struct cmc623_data *data = i2c_get_clientdata(client);

	usleep_range(1000, 2000);
	gpiod_set_value(data->gpios[IMA_PWREN] , 1);
	usleep_range(1000, 2000);

	gpiod_set_value(data->gpios[IMA_BYPASS], 1);
	usleep_range(1000, 2000);

	gpiod_set_value(data->gpios[IMA_SLEEP], 1);
	usleep_range(1000, 2000);
}

static void cmc623_resume_gpios(struct i2c_client *client)
{
	struct cmc623_data *data = i2c_get_clientdata(client);

	gpiod_set_value(data->gpios[IMA_N_RST], 1);
	gpiod_set_value(data->gpios[IMA_PWREN] , 0);
	gpiod_set_value(data->gpios[IMA_BYPASS], 0);
	gpiod_set_value(data->gpios[IMA_SLEEP], 0);
	gpiod_set_value(data->gpios[LVDS_N_SHDN], 0);
	gpiod_set_value(data->gpios[MLCD_ON], 0);
	gpiod_set_value(data->gpios[MLCD_ON1], 0);
	gpiod_set_value(data->gpios[BL_RESET], 0);
	msleep(200);

	gpiod_set_value(data->gpios[MLCD_ON], 1);
	usleep_range(30, 100);

	gpiod_set_value(data->gpios[MLCD_ON1], 1);

	if (data->resume_gpios)
		data->resume_gpios(client);

	gpiod_set_value(data->gpios[IMA_N_RST], 0);
	usleep_range(5000, 6000);

	gpiod_set_value(data->gpios[IMA_N_RST], 1);
	usleep_range(5000, 6000);
}

static void cmc623_resume(struct i2c_client *client)
{
	struct cmc623_data *data = i2c_get_clientdata(client);
	struct backlight_device *bd = data->bl.bd;
	int intensity;

	if (!data->initialized || !data->suspended)
		return;

	dev_dbg(&client->dev, "%s\n", __func__);

	intensity = cmc623_brightness_to_intensity(client, bd->props.brightness);

	cmc623_resume_gpios(client);

	mutex_lock(&data->tuning_mutex);
	cmc623_write_regs(client, data->init_regs, data->ninit_regs);
	cmc623_write_regs(client, data->tune_regs, data->ntune_regs);
	mutex_unlock(&data->tuning_mutex);

	cmc623_set_backlight(client, intensity);

	gpiod_set_value(data->gpios[LVDS_N_SHDN], 1);
	gpiod_set_value(data->gpios[BL_RESET], 1);

	msleep(10);
	
	data->suspended = false;
}

static void cmc623_shutdown(struct i2c_client *client)
{
	struct cmc623_data *data = i2c_get_clientdata(client);

	if (!data->initialized)
		return;

	gpiod_set_value(data->gpios[BL_RESET], 0);
	msleep(200);

	gpiod_set_value(data->gpios[IMA_SLEEP], 0);
	gpiod_set_value(data->gpios[IMA_BYPASS], 0);
	msleep(1);

	gpiod_set_value(data->gpios[IMA_PWREN] , 0);
	gpiod_set_value(data->gpios[LVDS_N_SHDN], 0);
	gpiod_set_value(data->gpios[MLCD_ON1], 0);
	msleep(1);

	gpiod_set_value(data->gpios[MLCD_ON], 0);
	msleep(400);
}

static int cmc623_brightness_to_intensity(struct i2c_client *client,
	int brightness)
{
	struct cmc623_data *data = i2c_get_clientdata(client);
	int intensity;
	int max_brightness = data->bl.max_brightness;
	int mid_brightness = data->bl.mid_brightness;
	int low_brightness = data->bl.low_brightness;
	int dim_brightness = data->bl.dim_brightness;

	int max_intensity = data->bl.max_intensity;
	int mid_intensity = data->bl.mid_intensity;
	int low_intensity = data->bl.low_intensity;
	int dim_intensity = data->bl.dim_intensity;
	int dark_intensity = data->bl.dark_intensity;

	if (brightness >= mid_brightness)
		intensity =  mid_intensity +
			((brightness - mid_brightness) * (max_intensity - mid_intensity) /
				(max_brightness - mid_brightness));
	else if (brightness >= low_brightness)
		intensity = low_intensity +
			((brightness - low_brightness) * (mid_intensity - low_intensity) /
				(mid_brightness - low_brightness));
	else if (brightness >= dim_brightness)
		intensity = dim_intensity +
			((brightness - dim_brightness) * (low_intensity - dim_intensity) /
				(low_brightness - dim_brightness));
	else if (brightness > 0)
		intensity = dark_intensity;
	else
		intensity = brightness;

	return intensity;
}

static int cmc623_update_status(struct backlight_device *bd)
{
	struct cmc623_data *data = dev_get_drvdata(&bd->dev);
	struct i2c_client *client = data->client;
	int brightness = bd->props.brightness;
	int intensity;

	dev_dbg(&bd->dev, "%s: brightness = %d\n", __func__, brightness);

	if (!data->initialized)
		return -EBUSY;

	if (bd->props.state & BL_CORE_FBBLANK) {
		cmc623_suspend(client);
	} else {
		if (data->bl.last_prop.state & BL_CORE_FBBLANK) {
			cmc623_resume(client);
		}

		mutex_lock(&data->bl.pwm_mutex);
		intensity = cmc623_brightness_to_intensity(client, brightness);
		cmc623_set_backlight(client, intensity);
		mutex_unlock(&data->bl.pwm_mutex);
	}

	data->bl.last_prop = bd->props;

	return 0;
}

static int cmc623_get_brightness(struct backlight_device *bd)
{
	struct cmc623_data *data = dev_get_drvdata(&bd->dev);
	struct i2c_client *client = data->client;
	return cmc623_brightness_to_intensity(client, bd->props.brightness);
}

static struct backlight_ops cmc623_backlight_ops = {
	.get_brightness = cmc623_get_brightness,
	.update_status  = cmc623_update_status,
};

struct cmc623_gpio_init {
	enum cmc623_gpios id;
	const char *name;
	enum gpiod_flags flags;
} static const cmc623_gpio_init_table[] = {
	{ .id = IMA_N_RST, .name = "ima-n-rst", .flags = GPIOD_OUT_HIGH },
	{ .id = IMA_PWREN, .name = "ima-pwren", .flags = GPIOD_OUT_HIGH },
	{ .id = IMA_BYPASS, .name = "ima-bypass", .flags = GPIOD_OUT_HIGH },
	{ .id = IMA_SLEEP, .name = "ima-sleep", .flags = GPIOD_OUT_HIGH },
	{ .id = LVDS_N_SHDN, .name = "lvds-n-shdn", .flags = GPIOD_OUT_HIGH },
	{ .id = MLCD_ON, .name = "mlcd-on", .flags = GPIOD_OUT_HIGH },
	{ .id = MLCD_ON1, .name = "mlcd-on1", .flags = GPIOD_OUT_HIGH },
	{ .id = BL_RESET, .name = "bl-reset", .flags = GPIOD_OUT_HIGH },
};

static int cmc623_init_gpios(struct i2c_client *client,
	struct cmc623_data *data)
{
	struct gpio_desc *desc;
	int i, err = 0;

	for (i = 0; i < ARRAY_SIZE(cmc623_gpio_init_table); i++) {
		const struct cmc623_gpio_init *item = &cmc623_gpio_init_table[i];

		desc = devm_gpiod_get(&client->dev, item->name, item->flags);
		if (IS_ERR(desc)) {
			err = PTR_ERR(desc);
			dev_err(&client->dev, "could not get %s gpio (%d)\n",
				item->name, err);
			return err;
		}

		data->gpios[item->id] = desc;
	}

	return err;
}

static void cmc623_parse_dt(struct i2c_client *client,
	struct cmc623_data *data)
{
	struct device_node *np = client->dev.of_node;

	if (!of_property_read_u32(np, "max-brightness", &data->bl.max_brightness))
		data->bl.max_brightness = 255;
	if (!of_property_read_u32(np, "mid-brightness", &data->bl.mid_brightness))
		data->bl.mid_brightness = 150;
	if (!of_property_read_u32(np, "low-brightness", &data->bl.low_brightness))
		data->bl.low_brightness = 50;
	if (!of_property_read_u32(np, "dim-brightness", &data->bl.dim_brightness))
		data->bl.dim_brightness = 15;

	if (!of_property_read_u32(np, "max-intensity", &data->bl.max_intensity))
		data->bl.max_intensity = 1600;
	if (!of_property_read_u32(np, "mid-intensity", &data->bl.mid_intensity))
		data->bl.mid_intensity = 800;
	if (!of_property_read_u32(np, "low-intensity", &data->bl.low_intensity))
		data->bl.low_intensity = 100;
	if (!of_property_read_u32(np, "dim-intensity", &data->bl.dim_intensity))
		data->bl.dim_intensity = 50;
	if (!of_property_read_u32(np, "dark-intensity", &data->bl.dark_intensity))
		data->bl.dark_intensity = 0;
}

#define CHECK_ROUNDING(rate) \
	do { \
		unsigned long res = clk_round_rate(data->clk_parent, rate); \
		dev_dbg(&client->dev, "%s: rate = %d, round rate = %ld, check = %d\n", __func__, \
			rate, res, res <= rate); \
	} while(false)

static int cmc623_initialize_clks(struct i2c_client *client,
	struct cmc623_data *data, unsigned long rate)
{
	int err = 0;

	if (!data->clk)
		return err;

	dev_dbg(&client->dev, "%s\n", __func__);

	if (data->clk_parent) {
		err = clk_set_parent(data->clk, data->clk_parent);
		if (err) {
			dev_err(&client->dev, "%s: Failed to set parent %s of %s\n",
				__func__, __clk_get_name(data->clk_parent),
				__clk_get_name(data->clk));
			return err;
		}
	}

	err = clk_set_rate(data->clk_parent, rate);
	if (err) {
		dev_err(&client->dev, "Could not set parent clock\n");
		return err;
	}

	dev_dbg(&client->dev, "%s: parent rate = %lu\n", __func__,
		clk_get_rate(data->clk_parent));

	return err;
}

static int cmc623_i2c_probe(struct i2c_client *client,
			const struct i2c_device_id *id)
{
	struct cmc623_data *data;
	struct backlight_properties props;
	struct backlight_device *bd;
	unsigned long rate;
	int err = 0;

	dev_info(&client->dev, "%s\n", __func__);
	data = devm_kzalloc(&client->dev, sizeof(struct cmc623_data), GFP_KERNEL);
	if (!data) {
		err = -ENOMEM;
		goto error;
	}

	err = cmc623_init_gpios(client, data);
	if (err)
		goto error;

	cmc623_parse_dt(client, data);


	data->clk = devm_clk_get(&client->dev, NULL);
	if (IS_ERR(data->clk)) {
		dev_err(&client->dev, "failed to get clock\n");
		err = PTR_ERR(data->clk);
		goto error;
	}

	data->clk_parent = devm_clk_get(&client->dev, "parent");
	if (IS_ERR(data->clk_parent)) {
		dev_err(&client->dev, "failed to get parent clock\n");
		err = PTR_ERR(data->clk_parent);
		goto error;
	}


	if (cmc623_panel_type == CMC623_TYPE_FUJITSU) {
		data->resume_gpios = cmc623_resume_gpios_fujitsu;
		data->init_regs = cmc623f_init;
		data->ninit_regs = ARRAY_SIZE(cmc623f_init);
		// rate = data->mode->clock * 1000;
		rate = 76000 * 1000;
	} else if (cmc623_panel_type == CMC623_TYPE_LSI) {
		data->resume_gpios = cmc623_resume_gpios_lsi;
		data->init_regs = cmc623_init;
		data->ninit_regs = ARRAY_SIZE(cmc623_init);
		// rate = data->mode->clock * 1000;
		rate = 68750 * 1000;
	} else {
		rate = 0;
		WARN(1, "Unknown panel type.");
	}

	data->tune_regs = standard_ui_cabcon;
	data->ntune_regs = ARRAY_SIZE(standard_ui_cabcon);
	data->client = client;
	data->suspended = false;
	mutex_init(&data->tuning_mutex);
	i2c_set_clientdata(client, data);


	/* Register the backlight */
	mutex_init(&data->bl.pwm_mutex);
	props.type = BACKLIGHT_RAW;

	bd = backlight_device_register("pwm-backlight",
		&client->dev, data, &cmc623_backlight_ops, &props);

	if (IS_ERR(bd)) {
		err = PTR_ERR(bd);
		goto error_backlight;
	}

	bd->props.max_brightness = CMC623_MAX_BRIGHTNESS;
	bd->props.brightness = CMC623_INITIAL_BRIGHTNESS;
	data->bl.last_prop.state &= ~BL_CORE_FBBLANK;
	data->bl.bd = bd;

	data->initialized = true;

	/*
	 * The display cannot handle clock rate change while the panel
	 * is on. The bootloader will bring up the panel. Then during kernel
	 * init the clock rates can/will change resulting in mangled display.
	 * Re-initialize the panel and clock rate to ensure stable display.
	 */
	if (rate) {
		cmc623_suspend(client);
		cmc623_initialize_clks(client, data, rate);
		cmc623_resume(client);
	}

	return 0;

error_backlight:
	mutex_destroy(&data->bl.pwm_mutex);
	mutex_destroy(&data->tuning_mutex);
error:
	dev_err(&client->dev, "probe error(%d).\n", err);
	return err;
}

static int cmc623_i2c_remove(struct i2c_client *client)
{
	struct cmc623_data *data = i2c_get_clientdata(client);
	struct backlight_device *bd = data->bl.bd;

	dev_dbg(&client->dev, "%s\n", __func__);

	bd->props.brightness = 0;
	bd->props.power = 0;
	cmc623_update_status(bd);
	mutex_destroy(&data->bl.pwm_mutex);
	backlight_device_unregister(bd);

	mutex_destroy(&data->tuning_mutex);
	i2c_set_clientdata(client, NULL);

	dev_dbg(&client->dev, "%s: done.\n", __func__);

	return 0;
}

static const struct i2c_device_id cmc623_id[] = {
	{ "cmc6230r", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, cmc623_id);

static const struct of_device_id cmc623_dt_match[] = {
	{ .compatible = "samsung,cmc6230r" },
	{ },
};
MODULE_DEVICE_TABLE(of, cmc623_dt_match);

struct i2c_driver cmc623_i2c_driver = {
	.driver	= {
		.name	= "cmc6230r",
		.of_match_table = cmc623_dt_match,
		.owner = THIS_MODULE,
	},
	.probe		= cmc623_i2c_probe,
	.remove		= cmc623_i2c_remove,
	.id_table	= cmc623_id,
	.shutdown = cmc623_shutdown,
};
module_i2c_driver(cmc623_i2c_driver);

MODULE_AUTHOR("Robert Yang <decatf@gmail.com>");
MODULE_DESCRIPTION("cmc6230r LCD driver");
MODULE_LICENSE("GPL");
