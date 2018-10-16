/*
 * Copyright (c) 2012-2015, NVIDIA Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef HOST1X_DEV_H
#define HOST1X_DEV_H

#include <linux/clk.h>
#include <linux/debugfs.h>
#include <linux/device.h>
#include <linux/host1x.h>
#include <linux/io.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/reset.h>

struct host1x_sid_entry {
	unsigned int base;
	unsigned int offset;
	unsigned int limit;
};

struct host1x_info {
	unsigned int nb_channels; /* host1x: number of channels supported */
	unsigned int nb_pts; /* host1x: number of syncpoints supported */
	unsigned int nb_bases; /* host1x: number of syncpoint bases supported */
	unsigned int nb_mlocks; /* host1x: number of mlocks supported */
	int (*init)(struct host1x *host1x); /* initialize per SoC ops */
	unsigned int sync_offset; /* offset of syncpoint registers */
	u64 dma_mask; /* mask of addressable memory */
	bool has_hypervisor; /* has hypervisor registers */
	unsigned int num_sid_entries;
	const struct host1x_sid_entry *sid_table;
};

struct host1x {
	const struct host1x_info *info;

	void __iomem *regs;
	void __iomem *hv_regs; /* hypervisor region */
	struct device *dev;
	struct clk *clk;
	struct reset_control *rst;

	struct dentry *debugfs;
	struct mutex devices_lock;
	struct list_head devices;
	struct list_head list;
};

extern struct platform_driver tegra_mipi_driver;

#endif
