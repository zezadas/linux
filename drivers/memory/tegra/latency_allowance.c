/*
 * arch/arm/mach-tegra/latency_allowance.c
 *
 * Copyright (C) 2011-2012, NVIDIA CORPORATION. All rights reserved.
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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm_qos.h>

#include "mc.h"

#define ENABLE_LA_DEBUG		1
#define la_debug(fmt, ...) \
	if (ENABLE_LA_DEBUG) { \
		printk(KERN_INFO pr_fmt(fmt), ##__VA_ARGS__); \
	}

/* Bug 995270 */
#define HACK_LA_FIFO 1

/* maximum valid value for latency allowance */
#define MC_LA_MAX_VALUE		255

#define RA(r) ((u32)(MC_LA_##r))

#define ID(id) \
	TEGRA_LA_##id

#define LA_INFO(f, e, a, m, s, id, ss) \
{f, e, RA(a), m, s, ID(id), __stringify(id), ss}

#define VALIDATE_ID(id) \
	do { \
		if (id >= TEGRA_LA_MAX_ID || id_to_index[id] == 0xFFFF) { \
			pr_err("%s: invalid Id=%d", __func__, id); \
			return -EINVAL; \
		} \
		BUG_ON(la_info_array[id_to_index[id]].id != id); \
	} while (0)

#define VALIDATE_BW(bw_in_mbps) \
	do { \
		if (bw_in_mbps >= 4096) \
			return -EINVAL; \
	} while (0)

#define VALIDATE_THRESHOLDS(tl, tm, th) \
	do { \
		if (tl > 100 || tm > 100 || th > 100) \
			return -EINVAL; \
	} while (0)


enum tegra_la_id {
	TEGRA_LA_AFIR = 0,			/* T30 specific */
	TEGRA_LA_AFIW,				/* T30 specific */
	TEGRA_LA_AVPC_ARM7R,
	TEGRA_LA_AVPC_ARM7W,
	TEGRA_LA_DISPLAY_0A,
	TEGRA_LA_DISPLAY_0B,
	TEGRA_LA_DISPLAY_0C,
	TEGRA_LA_DISPLAY_1B,			/* T30 specific */
	TEGRA_LA_DISPLAY_HC,
	TEGRA_LA_DISPLAY_0AB,
	TEGRA_LA_DISPLAY_0BB,
	TEGRA_LA_DISPLAY_0CB,
	TEGRA_LA_DISPLAY_1BB,			/* T30 specific */
	TEGRA_LA_DISPLAY_HCB,
	TEGRA_LA_EPPUP,
	TEGRA_LA_EPPU,
	TEGRA_LA_EPPV,
	TEGRA_LA_EPPY,
	TEGRA_LA_G2PR,
	TEGRA_LA_G2SR,
	TEGRA_LA_G2DR,
	TEGRA_LA_G2DW,
	TEGRA_LA_HOST1X_DMAR,
	TEGRA_LA_HOST1XR,
	TEGRA_LA_HOST1XW,
	TEGRA_LA_HDAR,
	TEGRA_LA_HDAW,
	TEGRA_LA_ISPW,
	TEGRA_LA_MPCORER,
	TEGRA_LA_MPCOREW,
	TEGRA_LA_MPCORE_LPR,
	TEGRA_LA_MPCORE_LPW,
	TEGRA_LA_MPE_UNIFBR,			/* T30 specific */
	TEGRA_LA_MPE_IPRED,			/* T30 specific */
	TEGRA_LA_MPE_AMEMRD,			/* T30 specific */
	TEGRA_LA_MPE_CSRD,			/* T30 specific */
	TEGRA_LA_MPE_UNIFBW,			/* T30 specific */
	TEGRA_LA_MPE_CSWR,			/* T30 specific */
	TEGRA_LA_FDCDRD,
	TEGRA_LA_IDXSRD,
	TEGRA_LA_TEXSRD,
	TEGRA_LA_FDCDWR,
	TEGRA_LA_FDCDRD2,
	TEGRA_LA_IDXSRD2,			/* T30 specific */
	TEGRA_LA_TEXSRD2,			/* T30 specific */
	TEGRA_LA_FDCDWR2,
	TEGRA_LA_PPCS_AHBDMAR,
	TEGRA_LA_PPCS_AHBSLVR,
	TEGRA_LA_PPCS_AHBDMAW,
	TEGRA_LA_PPCS_AHBSLVW,
	TEGRA_LA_PTCR,
	TEGRA_LA_SATAR,				/* T30 specific */
	TEGRA_LA_SATAW,				/* T30 specific */
	TEGRA_LA_VDE_BSEVR,
	TEGRA_LA_VDE_MBER,
	TEGRA_LA_VDE_MCER,
	TEGRA_LA_VDE_TPER,
	TEGRA_LA_VDE_BSEVW,
	TEGRA_LA_VDE_DBGW,
	TEGRA_LA_VDE_MBEW,
	TEGRA_LA_VDE_TPMW,
	TEGRA_LA_VI_RUV,			/* T30 specific */
	TEGRA_LA_VI_WSB,
	TEGRA_LA_VI_WU,
	TEGRA_LA_VI_WV,
	TEGRA_LA_VI_WY,

	TEGRA_LA_MAX_ID
};

struct la_client_info {
	unsigned int fifo_size_in_atoms;
	unsigned int expiration_in_ns;	/* worst case expiration value */
	unsigned long reg_addr;
	unsigned long mask;
	unsigned long shift;
	enum tegra_la_id id;
	char *name;
	bool scaling_supported;
};

#define MC_LA_AFI_0		0x2e0
#define MC_LA_AVPC_ARM7_0	0x2e4
#define MC_LA_DC_0		0x2e8
#define MC_LA_DC_1		0x2ec
#define MC_LA_DC_2		0x2f0
#define MC_LA_DCB_0		0x2f4
#define MC_LA_DCB_1		0x2f8
#define MC_LA_DCB_2		0x2fc
#define MC_LA_EPP_0		0x300
#define MC_LA_EPP_1		0x304
#define MC_LA_G2_0		0x308
#define MC_LA_G2_1		0x30c
#define MC_LA_HC_0		0x310
#define MC_LA_HC_1		0x314
#define MC_LA_HDA_0		0x318
#define MC_LA_ISP_0		0x31C
#define MC_LA_MPCORE_0		0x320
#define MC_LA_MPCORELP_0	0x324
#define MC_LA_MPE_0		0x328
#define MC_LA_MPE_1		0x32c
#define MC_LA_MPE_2		0x330
#define MC_LA_NV_0		0x334
#define MC_LA_NV_1		0x338
#define MC_LA_NV2_0		0x33c
#define MC_LA_NV2_1		0x340
#define MC_LA_PPCS_0		0x344
#define MC_LA_PPCS_1		0x348
#define MC_LA_PTC_0		0x34c
#define MC_LA_SATA_0		0x350
#define MC_LA_VDE_0		0x354
#define MC_LA_VDE_1		0x358
#define MC_LA_VDE_2		0x35c
#define MC_LA_VDE_3		0x360
#define MC_LA_VI_0		0x364
#define MC_LA_VI_1		0x368
#define MC_LA_VI_2		0x36c

/*
 * The rule for getting the fifo_size_in_atoms is:
 * 1.If REORDER_DEPTH exists, use it(default is overridden).
 * 2.Else if (write_client) use RFIFO_DEPTH.
 * 3.Else (read client) use RDFIFO_DEPTH.
 * Multiply the value by 2 for wide clients.
 * A client is wide, if CMW is larger than MW.
 * Refer to project.h file.
 */
struct la_client_info la_info_array[] = {
	LA_INFO(32,	150,	AFI_0,	0xFF, 0,		AFIR,		false),
	LA_INFO(32,	150,	AFI_0,	0xFF, 16,	AFIW,		false),
	LA_INFO(2,	150,	AVPC_ARM7_0, 0xFF, 0,	AVPC_ARM7R,	false),
	LA_INFO(2,	150,	AVPC_ARM7_0, 0xFF, 16,	AVPC_ARM7W,	false),
	LA_INFO(128,	1050,	DC_0,	0xFF, 0,		DISPLAY_0A,	true),
	LA_INFO(64,	1050,	DC_0,	0xFF, 16,	DISPLAY_0B,	true),
	LA_INFO(128,	1050,	DC_1,	0xFF, 0,		DISPLAY_0C,	true),
	LA_INFO(64,	1050,	DC_1,	0xFF, 16,	DISPLAY_1B,	true),
	LA_INFO(2,	1050,	DC_2,	0xFF, 0,		DISPLAY_HC,	false),
	LA_INFO(128,	1050,	DCB_0,	0xFF, 0,		DISPLAY_0AB,	true),
	LA_INFO(64,	1050,	DCB_0,	0xFF, 16,	DISPLAY_0BB,	true),
	LA_INFO(128,	1050,	DCB_1,	0xFF, 0,		DISPLAY_0CB,	true),
	LA_INFO(64,	1050,	DCB_1,	0xFF, 16,	DISPLAY_1BB,	true),
	LA_INFO(2,	1050,	DCB_2,	0xFF, 0,		DISPLAY_HCB,	false),
	LA_INFO(8,	150,	EPP_0,	0xFF, 0,		EPPUP,		false),
	LA_INFO(64,	150,	EPP_0,	0xFF, 16,	EPPU,		false),
	LA_INFO(64,	150,	EPP_1,	0xFF, 0,		EPPV,		false),
	LA_INFO(64,	150,	EPP_1,	0xFF, 16,	EPPY,		false),
	LA_INFO(64,	150,	G2_0,	0xFF, 0,		G2PR,		false),
	LA_INFO(64,	150,	G2_0,	0xFF, 16,	G2SR,		false),
	LA_INFO(48,	150,	G2_1,	0xFF, 0,		G2DR,		false),
	LA_INFO(128,	150,	G2_1,	0xFF, 16,	G2DW,		false),
	LA_INFO(16,	150,	HC_0,	0xFF, 0,		HOST1X_DMAR,	false),
	LA_INFO(8,	150,	HC_0,	0xFF, 16,	HOST1XR,	false),
	LA_INFO(32,	150,	HC_1,	0xFF, 0,		HOST1XW,	false),
	LA_INFO(16,	150,	HDA_0,	0xFF, 0,		HDAR,		false),
	LA_INFO(16,	150,	HDA_0,	0xFF, 16,	HDAW,		false),
	LA_INFO(64,	150,	ISP_0,	0xFF, 0,		ISPW,		false),
	LA_INFO(14,	150,	MPCORE_0, 0xFF, 0,	MPCORER,	false),
	LA_INFO(24,	150,	MPCORE_0, 0xFF, 16,	MPCOREW,	false),
	LA_INFO(14,	150,	MPCORELP_0, 0xFF, 0,	MPCORE_LPR,	false),
	LA_INFO(24,	150,	MPCORELP_0, 0xFF, 16,	MPCORE_LPW,	false),
	LA_INFO(8,	150,	MPE_0,	0xFF, 0,		MPE_UNIFBR,	false),
	LA_INFO(2,	150,	MPE_0,	0xFF, 16,	MPE_IPRED,	false),
	LA_INFO(64,	150,	MPE_1,	0xFF, 0,		MPE_AMEMRD,	false),
	LA_INFO(8,	150,	MPE_1,	0xFF, 16,	MPE_CSRD,	false),
	LA_INFO(8,	150,	MPE_2,	0xFF, 0,		MPE_UNIFBW,	false),
	LA_INFO(8,	150,	MPE_2,	0xFF, 16,	MPE_CSWR,	false),
	LA_INFO(96,	150,	NV_0,	0xFF, 0,		FDCDRD,		false),
	LA_INFO(64,	150,	NV_0,	0xFF, 16,	IDXSRD,		false),
	LA_INFO(64,	150,	NV_1,	0xFF, 0,		TEXSRD,		false),
	LA_INFO(96,	150,	NV_1,	0xFF, 16,	FDCDWR,		false),
	LA_INFO(96,	150,	NV2_0,	0xFF, 0,		FDCDRD2,	false),
	LA_INFO(64,	150,	NV2_0,	0xFF, 16,	IDXSRD2,	false),
	LA_INFO(64,	150,	NV2_1,	0xFF, 0,		TEXSRD2,	false),
	LA_INFO(96,	150,	NV2_1,	0xFF, 16,	FDCDWR2,	false),
	LA_INFO(2,	150,	PPCS_0,	0xFF, 0,		PPCS_AHBDMAR,	false),
	LA_INFO(8,	150,	PPCS_0,	0xFF, 16,	PPCS_AHBSLVR,	false),
	LA_INFO(2,	150,	PPCS_1,	0xFF, 0,		PPCS_AHBDMAW,	false),
	LA_INFO(4,	150,	PPCS_1,	0xFF, 16,	PPCS_AHBSLVW,	false),
	LA_INFO(2,	150,	PTC_0,	0xFF, 0,		PTCR,		false),
	LA_INFO(32,	150,	SATA_0,	0xFF, 0,		SATAR,		false),
	LA_INFO(32,	150,	SATA_0,	0xFF, 16,	SATAW,		false),
	LA_INFO(8,	150,	VDE_0,	0xFF, 0,		VDE_BSEVR,	false),
	LA_INFO(4,	150,	VDE_0,	0xFF, 16,	VDE_MBER,	false),
	LA_INFO(16,	150,	VDE_1,	0xFF, 0,		VDE_MCER,	false),
	LA_INFO(16,	150,	VDE_1,	0xFF, 16,	VDE_TPER,	false),
	LA_INFO(4,	150,	VDE_2,	0xFF, 0,		VDE_BSEVW,	false),
	LA_INFO(16,	150,	VDE_2,	0xFF, 16,	VDE_DBGW,	false),
	LA_INFO(2,	150,	VDE_3,	0xFF, 0,		VDE_MBEW,	false),
	LA_INFO(16,	150,	VDE_3,	0xFF, 16,	VDE_TPMW,	false),
	LA_INFO(8,	1050,	VI_0,	0xFF, 0,		VI_RUV,		false),
	LA_INFO(64,	1050,	VI_0,	0xFF, 16,	VI_WSB,		true),
	LA_INFO(64,	1050,	VI_1,	0xFF, 0,		VI_WU,		true),
	LA_INFO(64,	1050,	VI_1,	0xFF, 16,	VI_WV,		true),
	LA_INFO(64,	1050,	VI_2,	0xFF, 0,		VI_WY,		true),

	/* end of list. */
	LA_INFO(0,	0,	AFI_0,	0, 0,		MAX_ID,		false)
};


#if HACK_LA_FIFO
/* divisor and multiplier for DC LA workaround */
static int la_dc_fifo_div = 4;
static int la_dc_fifo_mult = 1;

module_param(la_dc_fifo_div, int, 0644);
module_param(la_dc_fifo_mult, int, 0644);
#endif

static DEFINE_SPINLOCK(safety_lock);
static unsigned short id_to_index[ID(MAX_ID) + 1];


/* Sets latency allowance based on clients memory bandwitdh requirement.
 * Bandwidth passed is in mega bytes per second.
 */
int tegra_set_latency_allowance(struct tegra_mc *mc, enum tegra_la_id id,
				unsigned int bandwidth_in_mbps)
{
	int ideal_la;
	int la_to_set;
	unsigned long reg_read;
	unsigned long reg_write;
	unsigned int fifo_size_in_atoms;
	struct la_client_info *ci;
	int idx = id_to_index[id];
	int bytes_per_atom = mc->soc->atom_size;
	int ns_per_tick = mc->tick;

	VALIDATE_ID(id);
	VALIDATE_BW(bandwidth_in_mbps);

	ci = &la_info_array[idx];
	fifo_size_in_atoms = ci->fifo_size_in_atoms;

#if HACK_LA_FIFO
	/* pretend that our FIFO is only as deep as the lowest fullness
	 * we expect to see */
	if (id >= ID(DISPLAY_0A) && id <= ID(DISPLAY_HCB)) {
		la_debug("%s: scaling client by %d/%d\n", __func__,
			la_dc_fifo_mult, la_dc_fifo_div);

		fifo_size_in_atoms = (fifo_size_in_atoms * la_dc_fifo_mult) /
			la_dc_fifo_div;
	}
#endif

	if (bandwidth_in_mbps == 0) {
		la_to_set = MC_LA_MAX_VALUE;
	} else {
		ideal_la = (fifo_size_in_atoms * bytes_per_atom * 1000) /
			   (bandwidth_in_mbps * ns_per_tick);
		la_to_set = ideal_la - (ci->expiration_in_ns / ns_per_tick) - 1;
	}

	la_debug("%s:id=%d,idx=%d, bw=%dmbps, la_to_set=%d",
		__func__, id, idx, bandwidth_in_mbps, la_to_set);
	la_to_set = max(0, la_to_set);
	la_to_set = min(la_to_set, MC_LA_MAX_VALUE);

	spin_lock(&safety_lock);
	reg_read = readl(mc->regs + ci->reg_addr);
	reg_write = reg_read & ~(ci->mask << ci->shift);
	reg_write |= (la_to_set & ci->mask) << ci->shift;
	writel(reg_write, mc->regs + ci->reg_addr);
	spin_unlock(&safety_lock);

	la_debug("%s:reg_addr=0x%x, read=0x%x, write=0x%x",
		__func__, (u32)ci->reg_addr, (u32)reg_read, (u32)reg_write);

	return 0;
}

static int tegra_mc_qos_request_notify(struct notifier_block *nb,
					unsigned long memory_bandwidth,
					void *data)
{
	struct tegra_mc *mc = container_of(nb, struct tegra_mc, qos_nb);
	int err = 0;

	memory_bandwidth /= 1024;
	tegra_set_latency_allowance(mc, TEGRA_LA_DISPLAY_0A, memory_bandwidth);
	tegra_set_latency_allowance(mc, TEGRA_LA_DISPLAY_0B, memory_bandwidth);
	tegra_set_latency_allowance(mc, TEGRA_LA_DISPLAY_0C, memory_bandwidth);
	tegra_set_latency_allowance(mc, TEGRA_LA_DISPLAY_1B, memory_bandwidth);

	return notifier_from_errno(err);
}

int tegra_latency_allowance_init(struct tegra_mc *mc)
{
	unsigned int i;
	int err;

	if (!of_machine_is_compatible("nvidia,tegra30"))
		return 0;

	memset(&id_to_index[0], 0xFF, sizeof(id_to_index));

	for (i = 0; i < ARRAY_SIZE(la_info_array); i++)
		id_to_index[la_info_array[i].id] = i;

	mc->qos_nb.notifier_call = tegra_mc_qos_request_notify;
	err = pm_qos_add_notifier(PM_QOS_MEMORY_BANDWIDTH, &mc->qos_nb);
	if (err) {
		dev_err(mc->dev, "failed to register QoS notifier: %d\n",
			err);
		return err;
	}

	return 0;
}
