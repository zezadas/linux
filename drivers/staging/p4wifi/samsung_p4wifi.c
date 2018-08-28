/*
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/of_platform.h>
#include <linux/of_address.h>
#include <linux/gpio.h>
#include <linux/gpio/machine.h>

#include <asm/setup.h>
#include <asm/system_info.h>

/******************************************************************************
 * bootloader cmdline args
 *****************************************************************************/
/*
 * Set androidboot.mode=charger based on the
 * Samsung p4 charging_mode parameter.
 */
static int __init charging_mode_arg(char *p)
{
	unsigned long charging_mode;

	if (!of_machine_is_compatible("samsung,p4wifi"))
		return 0;

	if (kstrtoul(p, 16, &charging_mode))
		return 1;

	if (charging_mode)
		strlcat(boot_command_line, " androidboot.mode=charger",
			COMMAND_LINE_SIZE);

	return 0;
}
early_param("charging_mode", charging_mode_arg);

/******************************************************************************
 * System revision
 *****************************************************************************/

#define GPIO_HW_REV0		9	/* TEGRA_GPIO_PB1 */
#define GPIO_HW_REV1		87	/* TEGRA_GPIO_PK7 */
#define GPIO_HW_REV2		164	/* TEGRA_GPIO_PU4 */
#define GPIO_HW_REV3		48	/* TEGRA_GPIO_PG0 */
#define GPIO_HW_REV4		49	/* TEGRA_GPIO_PG1 */

struct board_revision {
	unsigned int value;
	unsigned int gpio_value;
	char string[20];
};

/* We'll enumerate board revision from 10
 * to avoid a confliction with revision numbers of P3
*/
static struct __init board_revision p4_board_rev[] = {
	{10, 0x16, "Rev00"},
	{11, 0x01, "Rev01"},
	{12, 0x02, "Rev02" },
	{13, 0x03, "Rev03" },
	{14, 0x04, "Rev04" },
};

static int __init p4wifi_init_hwrev(void)
{
	unsigned int value, rev_no, i;
	struct board_revision *board_rev;

	if (!of_machine_is_compatible("samsung,p4wifi"))
		return 0;

	board_rev = p4_board_rev;
	rev_no = ARRAY_SIZE(p4_board_rev);

	value = gpio_get_value(GPIO_HW_REV0) |
		(gpio_get_value(GPIO_HW_REV1)<<1) |
		(gpio_get_value(GPIO_HW_REV2)<<2) |
		(gpio_get_value(GPIO_HW_REV3)<<3) |
		(gpio_get_value(GPIO_HW_REV4)<<4);

	for (i = 0; i < rev_no; i++) {
		if (board_rev[i].gpio_value == value)
			break;
	}

	if (i == rev_no) {
		pr_warn("%s: Valid revision NOT found!\n", __func__);
		pr_warn("%s: Latest one will be assigned!\n", __func__);
	}

	system_rev = board_rev[i].value;;
	pr_info("%s: system_rev = %d (gpio value = 0x%02x)\n", __func__,
		system_rev, value);

	return 0;
}
subsys_initcall(p4wifi_init_hwrev);
