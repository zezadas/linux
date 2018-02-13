/*
 * driver/misc/sec_misc.c
 *
 * driver supporting miscellaneous functions for Samsung P3 device
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

#include <linux/module.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/vmalloc.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/firmware.h>
#include <linux/blkdev.h>
#include <linux/gpio.h>

#include "sec_misc.h"

#define GPIO_IFCONSENSE 118

int check_usb_status = CHARGER_BATTERY;

int check_jig_on(void)
{
	u32 value = gpio_get_value(GPIO_IFCONSENSE);
	return !value;
}

struct class *sec_class;
EXPORT_SYMBOL(sec_class);

static __init int sec_class_init(void)
{
	sec_class = class_create(THIS_MODULE, "sec");
	if (IS_ERR(sec_class)) {
		pr_err("%s: Failed to create sec class!\n", __func__);
		return -ENODEV;
	}

	pr_info("%s: Created sec class.\n", __func__);
	return 0;
}
core_initcall(sec_class_init);
