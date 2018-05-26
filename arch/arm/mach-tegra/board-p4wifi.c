/*
 * arch/arm/mach-tegra/board-p4wifi.c
 *
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

#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/gpio.h>
#include <linux/clk.h>
#include <linux/err.h>
#include <linux/skbuff.h>
#include <linux/delay.h>
#include <linux/uaccess.h>
#include <linux/syscalls.h>
#include <linux/syscore_ops.h>
#include <linux/reboot.h>
#include <linux/platform_device.h>
#include <linux/highmem.h>
#include <linux/memblock.h>
#include <linux/regulator/consumer.h>
#include <linux/of_platform.h>
#include <linux/mmc/host.h>
#include <linux/file.h>
#include <linux/input.h>
#include <linux/gpio/machine.h>
#include <linux/of_address.h>
#include <linux/version.h>

#include <asm/setup.h>
#include <asm/system_info.h>

#include "iomap.h"
#include "pm.h"

int cmc623_current_type = 0;
EXPORT_SYMBOL(cmc623_current_type);

#ifdef CONFIG_SAMSUNG_LPM_MODE
int charging_mode_from_boot;

/* Get charging_mode status from kernel CMDLINE parameter. */
module_param_cb(charging_mode,  &param_ops_int,
		&charging_mode_from_boot, 0644);
MODULE_PARM_DESC(charging_mode_from_boot, "Charging mode parameter value.");
#endif

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

	if (kstrtoul(p, 16, &charging_mode))
		return 1;

	if (charging_mode)
		strlcat(boot_command_line, " androidboot.mode=charger", COMMAND_LINE_SIZE);

	return 0;
}
early_param("charging_mode", charging_mode_arg);

static int __init cmc623_arg(char *p)
{
	pr_info("%s: panel type=cmc623f\n", __func__);
	cmc623_current_type = 1;
	return 0;
}
early_param("CMC623F", cmc623_arg);

/******************************************************************************
* Memory reserve
*****************************************************************************/

/*
 * Due to conflicting restrictions on the placement of the framebuffer,
 * the bootloader is likely to leave the framebuffer pointed at a location
 * in memory that is outside the grhost aperture.  This function will move
 * the framebuffer contents from a physical address that is anywher (lowmem,
 * highmem, or outside the memory map) to a physical address that is outside
 * the memory map.
 */
void tegra_move_framebuffer(unsigned long to, unsigned long from,
	unsigned long size)
{
	struct page *page;
	void __iomem *to_io;
	void *from_virt;
	unsigned long i;
	unsigned char r, g, b;
	unsigned char b0, b1;

	BUG_ON(PAGE_ALIGN((unsigned long)to) != (unsigned long)to);
	BUG_ON(PAGE_ALIGN(from) != from);
	BUG_ON(PAGE_ALIGN(size) != size);

	to_io = ioremap(to, size*2);
	if (!to_io) {
		pr_err("%s: Failed to map target framebuffer\n", __func__);
		return;
	}

	if (pfn_valid(page_to_pfn(phys_to_page(from)))) {
		for (i = 0 ; i < size; i += PAGE_SIZE) {
			int j;
			page = phys_to_page(from + i);
			from_virt = kmap(page);

			for (j = 0; j < PAGE_SIZE; j += 2) {
				b0 = *((unsigned char *)(from_virt + j));
				b1 = *((unsigned char *)(from_virt + j+1));

				r = b0 & 0x1f;
				g = ((b1 & 0x07) << 3) | ((b0 & 0xe0) >> 5);
				b = (b1 & 0xf8) >> 3;

				*((unsigned char *)(to_io + (i + j)*2)) = r * 8;/*Red*/
				*((unsigned char *)(to_io + (i + j)*2 + 1)) = g * 4;/*Green*/
				*((unsigned char *)(to_io + (i + j)*2 + 2)) = b * 8;/*Blue*/
				*((unsigned char *)(to_io + (i + j)*2 + 3)) = 0x00;/*Alpha*/
			}
			kunmap(page);
		}
	} else {
		void __iomem *from_io = ioremap(from, size);
		if (!from_io) {
			pr_err("%s: Failed to map source framebuffer\n",
				__func__);
			goto out;
		}

		for (i = 0; i < size; i += 4)
			writel(readl(from_io + i), to_io + i);

		iounmap(from_io);
	}
out:
	iounmap(to_io);
}

void __init tegra_release_bootloader_fb(unsigned long tegra_bootloader_fb_start,
	unsigned long tegra_bootloader_fb_size)
{
	/* Since bootloader fb is reserved in common.c, it is freed here. */
	if (tegra_bootloader_fb_size)
		if (memblock_free(tegra_bootloader_fb_start,
						tegra_bootloader_fb_size))
			pr_err("Failed to free bootloader fb.\n");
}

void __init p4_release_bootloader_fb(void)
{
	struct device_node *bootloader_fb_node;
	struct device_node *fb_node;
	u32 bootloader_fb_start, bootloader_fb_size;
	u32 fb_start, fb_size;

	bootloader_fb_node = of_find_node_by_name(NULL, "bootloader_framebuffer");
	fb_node = of_find_node_by_name(NULL, "framebuffer0");

	if (bootloader_fb_node == NULL || fb_node == NULL) {
		pr_err("Could not find framebuffer device nodes\n");
		return;
	}

	of_property_read_u32_index(fb_node, "reg", 0, &fb_start);
	of_property_read_u32_index(fb_node, "reg", 1, &fb_size);

	of_property_read_u32_index(bootloader_fb_node, "reg", 0, &bootloader_fb_start);
	of_property_read_u32_index(bootloader_fb_node, "reg", 1, &bootloader_fb_size);

	pr_info("tegra_fb_start: 0x%X\n", fb_start);
	pr_info("tegra_bootloader_fb_start: 0x%X\n", fb_size);
	pr_info("tegra_fb_size: 0x%X\n", bootloader_fb_start);
	pr_info("tegra_bootloader_fb_size: 0x%X\n", bootloader_fb_size);

	tegra_move_framebuffer(fb_start, bootloader_fb_start,
		min(fb_size, bootloader_fb_size));
	tegra_release_bootloader_fb(bootloader_fb_start, bootloader_fb_size);
}

static struct resource ram_console_resources[] = {
	{
		.flags = IORESOURCE_MEM,
	},
};

static struct platform_device ram_console_device = {
	.name 		= "ram_console",
	.id 		= -1,
	.num_resources	= ARRAY_SIZE(ram_console_resources),
	.resource	= ram_console_resources,
};

void __init tegra_ram_console_debug_init_mem(unsigned long start, unsigned long size)
{
	struct resource *res;

	res = platform_get_resource(&ram_console_device, IORESOURCE_MEM, 0);
	if (!res)
		goto fail;
	res->start = start;
	res->end = res->start + size - 1;

	return;

fail:
	ram_console_device.resource = NULL;
	ram_console_device.num_resources = 0;
	pr_err("Failed to reserve memory block for ram console\n");
}

void __init tegra_ram_console_debug_init(void)
{
	int err;

	err = platform_device_register(&ram_console_device);
	if (err) {
		pr_err("%s: ram console registration failed (%d)!\n", __func__, err);
	}
}

/******************************************************************************
* Reboot
*****************************************************************************/

#define GPIO_TA_nCONNECTED 178

/*
 * These defines must be kept in sync with the bootloader.
 */
#define REBOOT_MODE_NONE                0
#define REBOOT_MODE_DOWNLOAD            1
#define REBOOT_MODE_NORMAL              2
#define REBOOT_MODE_UPDATE              3
#define REBOOT_MODE_RECOVERY            4
#define REBOOT_MODE_FOTA                5
#define REBOOT_MODE_FASTBOOT            7
#define REBOOT_MODE_DOWNLOAD_FAILED     8
#define REBOOT_MODE_DOWNLOAD_SUCCESS    9


#if (LINUX_VERSION_CODE >= KERNEL_VERSION(4, 15, 0))
#define MISC_DEVICE "/dev/mmcblk1p6"
#else
#define MISC_DEVICE "/dev/mmcblk0p6"
#endif


struct bootloader_message {
	char command[32];
	char status[32];
};

static int write_bootloader_message(char *cmd, int mode)
{
	struct file *filp;
	mm_segment_t oldfs;
	int ret = 0;
	loff_t pos = 2048L;  /* bootloader message offset in MISC.*/

	struct bootloader_message  bootmsg;

	memset(&bootmsg, 0, sizeof(struct bootloader_message));

	if (mode == REBOOT_MODE_RECOVERY) {
		strcpy(bootmsg.command, "boot-recovery");
#ifdef CONFIG_KERNEL_DEBUG_SEC
		reboot_mode = REBOOT_MODE_RECOVERY;
		kernel_sec_set_debug_level(KERNEL_SEC_DEBUG_LEVEL_LOW);
		kernel_sec_clear_upload_magic_number();
#endif
	} else if (mode == REBOOT_MODE_FASTBOOT)
		strcpy(bootmsg.command, "boot-fastboot");
	else if (mode == REBOOT_MODE_NORMAL)
		strcpy(bootmsg.command, "boot-reboot");
	else if (mode == REBOOT_MODE_FOTA)
		strcpy(bootmsg.command, "boot-fota");
	else if (mode == REBOOT_MODE_NONE)
		strcpy(bootmsg.command, "boot-normal");
	else
		strcpy(bootmsg.command, cmd);

	bootmsg.status[0] = (char) mode;


	filp = filp_open(MISC_DEVICE, O_WRONLY | O_LARGEFILE, 0);

	if (IS_ERR(filp)) {
		pr_info("failed to open MISC : '%s'.\n", MISC_DEVICE);
		return 0;
	}

	oldfs = get_fs();
	set_fs(get_ds());

	ret = vfs_write(filp, (const char *)&bootmsg,
			sizeof(struct bootloader_message), &pos);

	set_fs(oldfs);

	vfs_fsync(filp, 0);
	filp_close(filp, NULL);

	if (ret < 0)
		pr_info("failed to write on MISC\n");
	else
		pr_info("command : %s written on MISC\n", bootmsg.command);

	return ret;
}

/* Boot Mode Physical Addresses and Magic Token */
#define BOOT_MODE_P_ADDR	(0x20000000 - 0x0C)
#define BOOT_MAGIC_P_ADDR	(0x20000000 - 0x10)
#define BOOT_MAGIC_TOKEN	0x626F6F74

static void write_bootloader_mode(char boot_mode)
{
	void __iomem *to_io;
#if 0
	to_io = ioremap(BOOT_MODE_P_ADDR, 4);
	writel((unsigned long)boot_mode, to_io);
	iounmap(to_io);
#endif
	/* Write a magic value to a 2nd memory location to distinguish between a
	 * cold boot and a reboot.
	 */
	to_io = ioremap(BOOT_MAGIC_P_ADDR, 4);
	writel(BOOT_MAGIC_TOKEN, to_io);
	iounmap(to_io);
}

static int p3_notifier_call(struct notifier_block *notifier,
				unsigned long event, void *cmd)
{
	int mode;
	u32 value;
	value = gpio_get_value(GPIO_TA_nCONNECTED);

	if (event == SYS_RESTART) {
		mode = REBOOT_MODE_NORMAL;
		if (cmd) {
			if (!strcmp((char *)cmd, "recovery"))
				mode = REBOOT_MODE_RECOVERY;
			else if (!strcmp((char *)cmd, "bootloader"))
				mode = REBOOT_MODE_FASTBOOT;
			else if (!strcmp((char *)cmd, "fota"))
				mode = REBOOT_MODE_FOTA;
			else if (!strcmp((char *)cmd, "download"))
				mode = REBOOT_MODE_DOWNLOAD;
		}
	} else if (event == SYS_POWER_OFF
#ifdef CONFIG_SAMSUNG_LPM_MODE
		&& (charging_mode_from_boot == true)
#endif
		&& !value)
		mode = REBOOT_MODE_NORMAL;
	else
		mode = REBOOT_MODE_NONE;

	pr_info("%s, Reboot Mode : %d\n", __func__, mode);

	write_bootloader_mode(mode);

	write_bootloader_message(cmd, mode);

	return NOTIFY_DONE;
}

void write_bootloader_recovery(void)
{
	write_bootloader_message("recovery", REBOOT_MODE_RECOVERY);
}
EXPORT_SYMBOL(write_bootloader_recovery);

static struct notifier_block p3_reboot_notifier = {
	.notifier_call = p3_notifier_call,
	.priority = INT_MAX,
};

/******************************************************************************
* System revision
*****************************************************************************/

#define GPIO_HW_REV0		9  /* TEGRA_GPIO_PB1 */
#define GPIO_HW_REV1		87 /* TEGRA_GPIO_PK7 */
#define GPIO_HW_REV2		50 /* TEGRA_GPIO_PG2 */
#define GPIO_HW_REV3		48 /* TEGRA_GPIO_PG0 */
#define GPIO_HW_REV4		49 /* TEGRA_GPIO_PG1 */

struct board_revision {
	unsigned int value;
	unsigned int gpio_value;
	char string[20];
};

/* We'll enumerate board revision from 10
 * to avoid a confliction with revision numbers of P3
*/
struct board_revision p4_board_rev[] = {
	{10, 0x16, "Rev00"},
	{11, 0x01, "Rev01"},
	{12, 0x02, "Rev02" },
	{13, 0x03, "Rev03" },
	{14, 0x04, "Rev04" },
};

static void p4_check_hwrev(void)
{
	unsigned int value, rev_no, i;
	struct board_revision *board_rev;

	board_rev = p4_board_rev;
	rev_no = ARRAY_SIZE(p4_board_rev);

	gpio_request(GPIO_HW_REV0, "GPIO_HW_REV0");
	gpio_request(GPIO_HW_REV1, "GPIO_HW_REV1");
	gpio_request(GPIO_HW_REV2, "GPIO_HW_REV2");
	gpio_request(GPIO_HW_REV3, "GPIO_HW_REV3");
	gpio_request(GPIO_HW_REV4, "GPIO_HW_REV4");

	gpio_direction_input(GPIO_HW_REV0);
	gpio_direction_input(GPIO_HW_REV1);
	gpio_direction_input(GPIO_HW_REV2);
	gpio_direction_input(GPIO_HW_REV3);
	gpio_direction_input(GPIO_HW_REV4);

	value = gpio_get_value(GPIO_HW_REV0) |
			(gpio_get_value(GPIO_HW_REV1)<<1) |
			(gpio_get_value(GPIO_HW_REV2)<<2) |
			(gpio_get_value(GPIO_HW_REV3)<<3) |
			(gpio_get_value(GPIO_HW_REV4)<<4);

	gpio_free(GPIO_HW_REV0);
	gpio_free(GPIO_HW_REV1);
	gpio_free(GPIO_HW_REV2);
	gpio_free(GPIO_HW_REV3);
	gpio_free(GPIO_HW_REV4);

	for (i = 0; i < rev_no; i++) {
		if (board_rev[i].gpio_value == value)
			break;
	}

	system_rev = (i == rev_no) ? board_rev[rev_no-1].value : board_rev[i].value;

	if (i == rev_no)
		pr_warn("%s: Valid revision NOT found! Latest one will be assigned!\n", __func__);

	pr_info("%s: system_rev = %d (gpio value = 0x%02x)\n", __func__, system_rev, value);
}

#include <soc/tegra/pmc.h>


#define GPIO_ACCESSORY_EN	70 // TEGRA_GPIO_PI6
#define GPIO_V_ACCESSORY_5V	25  // TEGRA_GPIO_PD1
#define GPIO_OTG_EN		143 // TEGRA_GPIO_PR7
#define GPIO_CP_ON		115 //TEGRA_GPIO_PO3
#define GPIO_CP_RST		185 // TEGRA_GPIO_PX1

void __init p4wifi_machine_init(void)
{
	pr_info("%s()\n", __func__);
	tegra_ram_console_debug_init_mem(0x2E600000, 0x00100000);
	tegra_ram_console_debug_init();


	p4_release_bootloader_fb();

	p4_check_hwrev();

	register_reboot_notifier(&p3_reboot_notifier);

	tegra_powergate_power_off(TEGRA_POWERGATE_PCIE);

	gpio_direction_output(GPIO_ACCESSORY_EN, 0);
	gpio_direction_input(GPIO_V_ACCESSORY_5V);
	pr_info("%s: accessory gpio %s\n", __func__,
		gpio_get_value(GPIO_V_ACCESSORY_5V)==0 ? "disabled" : "enabled");

	gpio_direction_output(GPIO_OTG_EN, 0);

	gpio_set_value(GPIO_CP_ON, 0);
	gpio_set_value(GPIO_CP_RST, 0);
}
