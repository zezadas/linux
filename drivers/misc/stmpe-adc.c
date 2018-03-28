/*
 * STMicroelectronics STMPE811 ADC Driver
 *
 * (C) 2018 ryang <decatf@gmail.com>
 * All rights reserved.
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/i2c.h>
#include <linux/workqueue.h>

#include <linux/mfd/stmpe.h>
#include <linux/stmpe-adc.h>

#include <asm/delay.h>

#define STMPE_ADC_READ_RETRIES 10

#define STMPE811_REG_SYS_CTRL2		0x04
#define STMPE811_REG_GPIO_AF		0x17
#define STMPE811_REG_INT_EN		0x0A

/* Register layouts and functionalities are identical on all stmpexxx variants
 * with touchscreen controller
 */
#define STMPE_REG_ADC_CTRL1		0x20
#define STMPE_REG_ADC_CTRL2		0x21
#define STMPE811_ADC_CAPT		0x22
#define STMPE811_ADC_DATA_CH0	0x30
#define STMPE_REG_TSC_CTRL		0x40

#define OP_MOD_XYZ			0

#define STMPE_TSC_CTRL_TSC_EN		(1<<0)

#define SAMPLE_TIME(x)			((x & 0xf) << 4)
#define MOD_12B(x)			((x & 0x1) << 3)
#define REF_SEL(x)			((x & 0x1) << 1)
#define ADC_FREQ(x)			(x & 0x3)
#define OP_MODE(x)			((x & 0x7) << 1)

#define STMPE_ADC_NAME			"stmpe-adc"

static struct stmpe_adc {
	struct stmpe *stmpe;
	struct device *dev;
	struct mutex adc_lock;
	u8 sample_time;
	u8 mod_12b;
	u8 ref_sel;
	u8 adc_freq;
	bool initialized;
} stmpe_adc;

int stmpe_adc_get_data(u8 channel, u16 *data)
{
	struct stmpe_adc *adc = &stmpe_adc;
	struct stmpe *stmpe = adc->stmpe;
	u8 buf[2];
	u8 chan_size = sizeof(buf);
	u8 chan_mask = (1 << channel);
	u8 chan_addr = STMPE811_ADC_DATA_CH0 + (channel * chan_size);
	int retries = 0;
	int ret;

	if (!data)
		return -EINVAL;

	if (channel > 7)
		return -EINVAL;

	if (!adc->initialized)
		return -ENODEV;

	mutex_lock(&adc->adc_lock);

	ret = stmpe_reg_write(stmpe, STMPE811_ADC_CAPT, (1 << channel));
	if (ret < 0) {
		dev_err(stmpe->dev, "STMPE811_ADC_CAPT reg write error. (%d)\n", ret);
		goto done;
	}

	do {
		if (retries++ > STMPE_ADC_READ_RETRIES) {
			dev_err(stmpe->dev, "ADC data not ready after %d retries\n", retries);
			ret = -EIO;
			goto done;
		}

		// ADC conversion time is based on clock frequency.
		// Use longest conversion time.
		udelay(60);
		ret = stmpe_block_read(stmpe, STMPE811_ADC_CAPT, (u8)1, buf);
		if (ret < 0) {
			dev_err(stmpe->dev, "STMPE811_ADC_CAPT block read error. (%d)\n", ret);
			goto done;
		}
	} while ((buf[0] & chan_mask) == 0);

	dev_dbg(stmpe->dev,"%s: channel = %u, chan_addr = 0x%x\n",
		__func__, channel, chan_addr);

	ret = stmpe_block_read(stmpe, chan_addr, chan_size, buf);
	if (ret < 0) {
		dev_err(stmpe->dev, "STMPE811_ADC_CAPT block read error. (%d)\n", ret);
		goto done;
	}

	*data = ((buf[0]<<8) | buf[1]) & 0x0FFF;

done:
	mutex_unlock(&adc->adc_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(stmpe_adc_get_data);

static int stmpe_init_hw(struct stmpe_adc *adc)
{
	int ret;
	u8 adc_ctrl1, adc_ctrl1_mask;
	struct stmpe *stmpe = adc->stmpe;
	struct device *dev = adc->dev;

	ret = stmpe_enable(stmpe, STMPE_BLOCK_ADC);
	if (ret) {
		dev_err(dev, "Could not enable clock for ADC\n");
		return ret;
	}

	adc_ctrl1 = SAMPLE_TIME(adc->sample_time) | MOD_12B(adc->mod_12b) |
		REF_SEL(adc->ref_sel);
	adc_ctrl1_mask = SAMPLE_TIME(0xff) | MOD_12B(0xff) | REF_SEL(0xff);

	ret = stmpe_set_bits(stmpe, STMPE_REG_ADC_CTRL1,
			adc_ctrl1_mask, adc_ctrl1);
	if (ret) {
		dev_err(dev, "Could not setup ADC\n");
		return ret;
	}

	ret = stmpe_set_bits(stmpe, STMPE_REG_ADC_CTRL2,
			ADC_FREQ(0xff), ADC_FREQ(adc->adc_freq));
	if (ret) {
		dev_err(dev, "Could not setup ADC\n");
		return ret;
	}

	ret = stmpe_set_bits(stmpe, STMPE_REG_TSC_CTRL,
			OP_MODE(0xff), OP_MODE(OP_MOD_XYZ));
	if (ret) {
		dev_err(dev, "Could not set mode\n");
		return ret;
	}

	return 0;
}

static void stmpe_adc_get_platform_info(struct platform_device *pdev,
					struct stmpe_adc *adc)
{
	struct device_node *np = pdev->dev.of_node;
	u32 val;

	if (np) {
		if (!of_property_read_u32(np, "st,sample-time", &val))
			adc->sample_time = val;
		if (!of_property_read_u32(np, "st,mod-12b", &val))
			adc->mod_12b = val;
		if (!of_property_read_u32(np, "st,ref-sel", &val))
			adc->ref_sel = val;
		if (!of_property_read_u32(np, "st,adc-freq", &val))
			adc->adc_freq = val;
	}
}

static int stmpe_adc_probe(struct platform_device *pdev)
{
	struct stmpe *stmpe = dev_get_drvdata(pdev->dev.parent);
	struct stmpe_adc *adc = &stmpe_adc;
	int error;

	platform_set_drvdata(pdev, adc);
	adc->stmpe = stmpe;
	adc->dev = &pdev->dev;

	stmpe_adc_get_platform_info(pdev, adc);

	error = stmpe_init_hw(adc);
	if (error)
		return error;

	error = stmpe_reg_write(stmpe, STMPE811_REG_INT_EN, 0x00);
	if (error) {
		dev_err(&pdev->dev, "set interrupt error (%d)\n", error);
		return error;
	}

	error = stmpe_set_altfunc(stmpe, 0xFF, STMPE_BLOCK_TOUCHSCREEN);
	if (error) {
		dev_err(&pdev->dev, "set altfunc error (%d)\n", error);
		return error;
	}

	/*
	 * set_altfunc will enable the GPIO block.
	 * Disable it since it is not used.
	 */
	error = stmpe_disable(stmpe, STMPE_BLOCK_GPIO);
	if (error) {
		dev_err(&pdev->dev, "error disabling gpio block (%d)\n", error);
		return error;
	}

	mutex_init(&adc->adc_lock);
	adc->initialized = true;

	dev_info(&pdev->dev, "STMPE811_REG_SYS_CTRL2 = 0x%x\n",
		stmpe_reg_read(stmpe, STMPE811_REG_SYS_CTRL2));
	dev_info(&pdev->dev, "STMPE_REG_ADC_CTRL1 = 0x%x\n",
		stmpe_reg_read(stmpe, STMPE_REG_ADC_CTRL1));
	dev_info(&pdev->dev, "STMPE_REG_ADC_CTRL2 = 0x%x\n",
		stmpe_reg_read(stmpe, STMPE_REG_ADC_CTRL2));
	dev_info(&pdev->dev, "STMPE_REG_TSC_CTRL = 0x%x\n",
		stmpe_reg_read(stmpe, STMPE_REG_TSC_CTRL));
	dev_info(&pdev->dev, "STMPE811_REG_INT_EN = 0x%x\n",
		stmpe_reg_read(stmpe, STMPE811_REG_INT_EN));
	dev_info(&pdev->dev, "STMPE811_REG_GPIO_AF = 0x%x\n",
		stmpe_reg_read(stmpe, STMPE811_REG_GPIO_AF));

	return 0;
}

static int stmpe_adc_remove(struct platform_device *pdev)
{
	struct stmpe_adc *adc = platform_get_drvdata(pdev);

	stmpe_disable(adc->stmpe, STMPE_BLOCK_ADC);
	mutex_destroy(&adc->adc_lock);

	return 0;
}

static struct platform_driver stmpe_adc_driver = {
	.driver = {
		.name = STMPE_ADC_NAME,
	},
	.probe = stmpe_adc_probe,
	.remove = stmpe_adc_remove,
};
module_platform_driver(stmpe_adc_driver);

static const struct of_device_id stmpe_adc_ids[] = {
	{ .compatible = "st,stmpe-adc", },
	{ },
};
MODULE_DEVICE_TABLE(of, stmpe_adc_ids);

MODULE_AUTHOR("ryang <decatf@gmail.com>");
MODULE_DESCRIPTION("STMPEXXX adc driver");
MODULE_LICENSE("GPL");
