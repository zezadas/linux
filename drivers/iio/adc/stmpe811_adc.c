/*
 * STMicroelectronics STMPE ADC Driver
 *
 * ryang <decatf@gmail.com>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>

#include <linux/iio/iio.h>
#include <linux/iio/machine.h>
#include <linux/iio/driver.h>

#include <linux/mfd/stmpe.h>

#define STMPE_REG_ADC_CTRL1		0x20
#define STMPE_REG_ADC_CTRL2		0x21
#define STMPE_REG_TSC_CTRL		0x40

#define STMPE811_REG_ADC_INT_EN		0x0E
#define STMPE811_REG_ADC_INT_STA		0x0F
#define STMPE811_REG_ADC_CAPT		0x22
#define STMPE811_REG_ADC_DATA_CH0		0x30

#define OP_MOD_XYZ			0

#define SAMPLE_TIME(x)			((x & 0xf) << 4)
#define MOD_12B(x)			((x & 0x1) << 3)
#define REF_SEL(x)			((x & 0x1) << 1)
#define ADC_FREQ(x)			(x & 0x3)
#define OP_MODE(x)			((x & 0x7) << 1)

#define STMPE_ADC_NAME			"stmpe-adc"

struct stmpe_adc {
	struct stmpe *stmpe;
	struct device *dev;
	struct mutex adc_lock;

	u8 sample_time;
	u8 mod_12b;
	u8 ref_sel;
	u8 adc_freq;

	u16 chan_data[8];
	struct completion completion;
};

static int stmpe_adc_read_channel(struct stmpe_adc *adc, u8 channel, u32 *data)
{
	struct stmpe *stmpe = adc->stmpe;
	u8 chan_mask = (1 << channel);
	long timeout;
	int ret;

	if (!data || channel > 7)
		return -EINVAL;

	mutex_lock(&adc->adc_lock);

	reinit_completion(&adc->completion);

	ret = stmpe_reg_write(stmpe, STMPE811_REG_ADC_CAPT, chan_mask);
	if (ret < 0) {
		dev_err(stmpe->dev, "Could not start ADC data capture\n");
		goto done;
	}

	timeout = wait_for_completion_interruptible_timeout(&adc->completion,
		msecs_to_jiffies(1000));

	if (timeout == 0) {
		ret = -ETIMEDOUT;
		goto done;
	} else if (timeout < 0) {
		ret = timeout;
		goto done;
	}

	*data = adc->chan_data[channel];
	adc->chan_data[channel] = 0;
	ret = 0;

	dev_dbg(stmpe->dev, "data = 0x%X\n", *data);

done:
	mutex_unlock(&adc->adc_lock);
	return ret;
}

static irqreturn_t stmpe_adc_event_handler(int irq, void *data)
{
	struct stmpe_adc *adc = data;
	struct stmpe *stmpe = adc->stmpe;
	int status, clear;

	status = stmpe_reg_read(stmpe, STMPE811_REG_ADC_INT_STA);
	clear = status;

	while (status) {
		int channel = __ffs(status);
		u8 buf[2];
		u8 chan_size = sizeof(buf);
		u8 chan_reg = STMPE811_REG_ADC_DATA_CH0 + (channel * chan_size);

		stmpe_block_read(stmpe, chan_reg, chan_size, buf);

		adc->chan_data[channel] = (((u16)buf[0]<<8) | buf[1]);
		status &= ~(1 << channel);

		dev_dbg(stmpe->dev, "chan_data[%u] = 0x%X\n",
			channel, adc->chan_data[channel]);
	}

	stmpe_reg_write(stmpe, STMPE811_REG_ADC_INT_STA, clear);

	complete(&adc->completion);
	return IRQ_HANDLED;
}

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

static const struct iio_chan_spec stmpe811_adc_channels[] = {
	{
		.indexed = 1,
		.type = IIO_VOLTAGE,
		.channel = 0,
		.datasheet_name = "ADC_DATA_CH0",
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.indexed = 1,
		.type = IIO_VOLTAGE,
		.channel = 1,
		.datasheet_name = "ADC_DATA_CH1",
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.indexed = 1,
		.type = IIO_VOLTAGE,
		.channel = 2,
		.datasheet_name = "ADC_DATA_CH2",
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.indexed = 1,
		.type = IIO_VOLTAGE,
		.channel = 3,
		.datasheet_name = "ADC_DATA_CH3",
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.indexed = 1,
		.type = IIO_VOLTAGE,
		.channel = 4,
		.datasheet_name = "ADC_DATA_CH4",
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.indexed = 1,
		.type = IIO_VOLTAGE,
		.channel = 5,
		.datasheet_name = "ADC_DATA_CH5",
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.indexed = 1,
		.type = IIO_VOLTAGE,
		.channel = 6,
		.datasheet_name = "ADC_DATA_CH6",
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},
	{
		.indexed = 1,
		.type = IIO_VOLTAGE,
		.channel = 7,
		.datasheet_name = "ADC_DATA_CH7",
		.info_mask_separate = BIT(IIO_CHAN_INFO_RAW),
	},

};

static int stmpe_adc_read_raw(struct iio_dev *indio_dev,
			struct iio_chan_spec const *chan,
			int *val, int *val2, long mask)
{
	struct stmpe_adc *adc = iio_priv(indio_dev);
	u32 data;
	int ret;

	mutex_lock(&indio_dev->mlock);
	switch (mask) {
	case IIO_CHAN_INFO_RAW:
		ret = stmpe_adc_read_channel(adc, chan->channel, &data);
		if (ret)
			break;

		*val = data;
		ret = IIO_VAL_INT;
		break;
	default:
		ret = -EINVAL;
	}
	mutex_unlock(&indio_dev->mlock);

	return ret;
}

static const struct iio_info stmpe_adc_iio_info = {
	.read_raw = &stmpe_adc_read_raw,
};

/* for consumer drivers */
static struct iio_map stmpe811_adc_default_maps[] = {
	IIO_MAP("ADC_DATA_CH0", STMPE_ADC_NAME, "stmpe-adc-ch0"),
	IIO_MAP("ADC_DATA_CH1", STMPE_ADC_NAME, "stmpe-adc-ch1"),
	IIO_MAP("ADC_DATA_CH2", STMPE_ADC_NAME, "stmpe-adc-ch2"),
	IIO_MAP("ADC_DATA_CH3", STMPE_ADC_NAME, "stmpe-adc-ch3"),
	IIO_MAP("ADC_DATA_CH4", STMPE_ADC_NAME, "stmpe-adc-ch4"),
	IIO_MAP("ADC_DATA_CH5", STMPE_ADC_NAME, "stmpe-adc-ch5"),
	IIO_MAP("ADC_DATA_CH6", STMPE_ADC_NAME, "stmpe-adc-ch6"),
	IIO_MAP("ADC_DATA_CH7", STMPE_ADC_NAME, "stmpe-adc-ch7"),
	{},
};

static int stmpe_adc_probe(struct platform_device *pdev)
{
	struct stmpe *stmpe = dev_get_drvdata(pdev->dev.parent);
	struct stmpe_adc *adc;
	struct iio_dev *indio_dev;
	int adc_irq;
	int ret;

	indio_dev = devm_iio_device_alloc(&pdev->dev, sizeof(*adc));
	if (!indio_dev) {
		dev_err(&pdev->dev, "failed to allocate iio device.\n");
		ret = -ENOMEM;
		goto err;
	}

	platform_set_drvdata(pdev, indio_dev);
	adc = iio_priv(indio_dev);
	adc->stmpe = stmpe;
	adc->dev = &pdev->dev;

	stmpe_adc_get_platform_info(pdev, adc);

	init_completion(&adc->completion);

	adc_irq = platform_get_irq(pdev, 0);
	if (adc_irq < 0) {
		dev_err(&pdev->dev,
			"device configured in no-irq mode: irqs are not available\n");
		ret = adc_irq;
		goto err;
	}

	ret = devm_request_threaded_irq(&pdev->dev, adc_irq,
					  NULL, stmpe_adc_event_handler,
					  IRQF_ONESHOT, STMPE_ADC_NAME, adc);
	if (ret) {
		dev_err(&pdev->dev, "Failed to request IRQ %d\n", adc_irq);
		goto err;
	}

	ret = stmpe_init_hw(adc);
	if (ret)
		goto err;

	ret = stmpe_reg_write(stmpe, STMPE811_REG_ADC_INT_EN, 0xFF);
	if (ret) {
		dev_err(&pdev->dev, "set interrupt ret (%d)\n", ret);
		goto err;
	}

	ret = stmpe_set_altfunc(stmpe, 0xFF, STMPE_BLOCK_ADC);
	if (ret) {
		dev_err(&pdev->dev, "set altfunc ret (%d)\n", ret);
		goto err;
	}

	mutex_init(&adc->adc_lock);

	/* register as an iio provider */
	indio_dev->dev.parent = &pdev->dev;
	indio_dev->name = pdev->name;
	indio_dev->channels = stmpe811_adc_channels;
	indio_dev->num_channels = ARRAY_SIZE(stmpe811_adc_channels);
	indio_dev->info = &stmpe_adc_iio_info;
	indio_dev->modes = INDIO_DIRECT_MODE;
	ret = iio_map_array_register(indio_dev, stmpe811_adc_default_maps);
	if (ret < 0) {
		dev_err(&pdev->dev, "unable to register iio mappings\n");
		goto err;
	}

	ret = devm_iio_device_register(&pdev->dev, indio_dev);
	if (ret < 0) {
		dev_err(&pdev->dev, "unable to register iio device\n");
		goto err_iio_register;
	}

	return 0;

err_iio_register:
	iio_map_array_unregister(indio_dev);
	mutex_destroy(&adc->adc_lock);
err:
	return ret;
}

static int stmpe_adc_remove(struct platform_device *pdev)
{
	struct iio_dev *indio_dev = platform_get_drvdata(pdev);
	struct stmpe_adc *adc = iio_priv(indio_dev);

	stmpe_disable(adc->stmpe, STMPE_BLOCK_ADC);
	iio_map_array_unregister(indio_dev);
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

static int __init stmpe_adc_driver_init(void)
{
	return platform_driver_register(&stmpe_adc_driver);
}
subsys_initcall(stmpe_adc_driver_init);

static void __exit stmpe_adc_driver_exit(void) \
{
	platform_driver_unregister(&stmpe_adc_driver);
}
module_exit(stmpe_adc_driver_exit);

static const struct of_device_id stmpe_adc_ids[] = {
	{ .compatible = "st,stmpe-adc", },
	{ },
};
MODULE_DEVICE_TABLE(of, stmpe_adc_ids);

MODULE_AUTHOR("ryang <decatf@gmail.com>");
MODULE_DESCRIPTION("STMPE ADC driver");
MODULE_LICENSE("GPL");
