
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/iopoll.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/platform_device.h>
#include <linux/pm_qos.h>
#include <linux/sort.h>
#include <linux/types.h>
#include <linux/cpufreq.h>

#include <asm/cacheflush.h>

#include <soc/tegra/fuse.h>

#include "mc.h"


#define TEGRA_EMC_BRIDGE_RATE_MIN	300000000

#define EMC_STATUS_UPDATE_TIMEOUT	100

enum {
	DLL_CHANGE_NONE = 0,
	DLL_CHANGE_ON,
	DLL_CHANGE_OFF,
};

#define EMC_INTSTATUS				0x0
#define EMC_REFRESH_OVERFLOW_INT			BIT(3)
#define EMC_INTSTATUS_CLKCHANGE_COMPLETE		BIT(4)

#define EMC_INTMASK				0x004

#define EMC_DBG					0x8
#define EMC_DBG_WRITE_MUX_ACTIVE		BIT(1)

#define EMC_CFG					0xc
#define EMC_CFG_PERIODIC_QRST			BIT(21)
#define EMC_CFG_DYN_SREF_ENABLE			BIT(28)
#define EMC_CFG_PWR_MASK			(0xF << 28)

#define EMC_REFCTRL				0x20
#define EMC_REFCTRL_DEV_SEL_SHIFT		0
#define EMC_REFCTRL_DEV_SEL_MASK		(0x3 << EMC_REFCTRL_DEV_SEL_SHIFT)
#define EMC_REFCTRL_ENABLE			BIT(31)
#define EMC_REFCTRL_ENABLE_ALL(num)		\
	((((num > 1) ? 0 : 2) << EMC_REFCTRL_DEV_SEL_SHIFT) \
	 | EMC_REFCTRL_ENABLE)
#define EMC_REFCTRL_DISABLE_ALL(num)		\
	(((num > 1) ? 0 : 2) << EMC_REFCTRL_DEV_SEL_SHIFT)

#define EMC_TIMING_CONTROL			0x28
#define EMC_RC					0x2c
#define EMC_RFC					0x30
#define EMC_RAS					0x34
#define EMC_RP					0x38
#define EMC_R2W					0x3c
#define EMC_W2R					0x40
#define EMC_R2P					0x44
#define EMC_W2P					0x48
#define EMC_RD_RCD				0x4c
#define EMC_WR_RCD				0x50
#define EMC_RRD					0x54
#define EMC_REXT				0x58
#define EMC_WDV					0x5c
#define EMC_QUSE				0x60
#define EMC_QRST				0x64
#define EMC_QSAFE				0x68
#define EMC_RDV					0x6c
#define EMC_REFRESH				0x70
#define EMC_BURST_REFRESH_NUM			0x74
#define EMC_PDEX2WR				0x78
#define EMC_PDEX2RD				0x7c
#define EMC_PCHG2PDEN				0x80
#define EMC_ACT2PDEN				0x84
#define EMC_AR2PDEN				0x88
#define EMC_RW2PDEN				0x8c
#define EMC_TXSR				0x90
#define EMC_TCKE				0x94
#define EMC_TFAW				0x98
#define EMC_TRPAB				0x9c
#define EMC_TCLKSTABLE				0xa0
#define EMC_TCLKSTOP				0xa4
#define EMC_TREFBW				0xa8
#define EMC_QUSE_EXTRA				0xac
#define EMC_ODT_WRITE				0xb0
#define EMC_ODT_READ				0xb4
#define EMC_WEXT				0xb8
#define EMC_CTT					0xbc

#define EMC_MRS_WAIT_CNT			0xc8
#define EMC_MRS_WAIT_CNT_SHORT_WAIT_SHIFT	0
#define EMC_MRS_WAIT_CNT_SHORT_WAIT_MASK	\
	(0x3FF << EMC_MRS_WAIT_CNT_SHORT_WAIT_SHIFT)
#define EMC_MRS_WAIT_CNT_LONG_WAIT_SHIFT	16
#define EMC_MRS_WAIT_CNT_LONG_WAIT_MASK		\
	(0x3FF << EMC_MRS_WAIT_CNT_LONG_WAIT_SHIFT)

#define EMC_MRS					0xcc
#define EMC_MODE_SET_DLL_RESET			BIT(8)
#define EMC_MODE_SET_LONG_CNT			BIT(26)
#define EMC_EMRS				0xd0
#define EMC_REF					0xd4
#define EMC_REF_FORCE_CMD			1

#define EMC_SELF_REF				0xe0
#define EMC_SELF_REF_CMD_ENABLED		BIT(0)
#define EMC_SELF_REF_DEV_SEL_SHIFT		30
#define EMC_SELF_REF_DEV_SEL_MASK		(0x3 << EMC_SELF_REF_DEV_SEL_SHIFT)

enum {
	DRAM_DEV_SEL_ALL = 0,
	DRAM_DEV_SEL_0	 = (2 << EMC_SELF_REF_DEV_SEL_SHIFT),
	DRAM_DEV_SEL_1   = (1 << EMC_SELF_REF_DEV_SEL_SHIFT),
};

#define DRAM_BROADCAST(num)			\
	(((num) > 1) ? DRAM_DEV_SEL_ALL : DRAM_DEV_SEL_0)

#define EMC_MRW					0xe8
#define EMC_MRR					0xec
#define EMC_MRR_MA_SHIFT			16
#define EMC_MRR_MA_MASK				(0xFF << EMC_MRR_MA_SHIFT)
#define EMC_MRR_DATA_MASK			((0x1 << EMC_MRR_MA_SHIFT) - 1)
#define LPDDR2_MR4_TEMP_SHIFT			0
#define LPDDR2_MR4_TEMP_MASK			(0x7 << LPDDR2_MR4_TEMP_SHIFT)

#define EMC_XM2DQSPADCTRL3			0xf8
#define EMC_XM2DQSPADCTRL3_VREF_ENABLE		BIT(5)
#define EMC_FBIO_SPARE				0x100

#define EMC_FBIO_CFG5				0x104
#define EMC_CFG5_TYPE_SHIFT			0x0
#define EMC_CFG5_TYPE_MASK			(0x3 << EMC_CFG5_TYPE_SHIFT)

enum {
	DRAM_TYPE_DDR3   = 0,
	DRAM_TYPE_LPDDR2 = 2,
};

#define EMC_CFG5_QUSE_MODE_SHIFT		13
#define EMC_CFG5_QUSE_MODE_MASK			(0x7 << EMC_CFG5_QUSE_MODE_SHIFT)

enum {
	EMC_CFG5_QUSE_MODE_NORMAL = 0,
	EMC_CFG5_QUSE_MODE_ALWAYS_ON,
	EMC_CFG5_QUSE_MODE_INTERNAL_LPBK,
	EMC_CFG5_QUSE_MODE_PULSE_INTERN,
	EMC_CFG5_QUSE_MODE_PULSE_EXTERN,
};

#define EMC_FBIO_CFG6				0x114
#define EMC_CFG_RSV				0x120
#define EMC_AUTO_CAL_CONFIG			0x2a4
#define EMC_AUTO_CAL_INTERVAL			0x2a8
#define EMC_AUTO_CAL_STATUS			0x2ac
#define EMC_AUTO_CAL_STATUS_ACTIVE		BIT(31)
#define EMC_STATUS				0x2b4
#define EMC_STATUS_TIMING_UPDATE_STALLED	BIT(23)
#define EMC_STATUS_MRR_DIVLD			BIT(20)

#define EMC_CFG_2				0x2b8
#define EMC_CFG_2_MODE_SHIFT			0
#define EMC_CFG_2_MODE_MASK			(0x7 << EMC_CFG_2_MODE_SHIFT)
#define EMC_CFG_2_SREF_MODE			0x1
#define EMC_CFG_2_PD_MODE			0x3

#define EMC_CFG_DIG_DLL				0x2bc
#define EMC_CFG_DIG_DLL_PERIOD			0x2c0
#define EMC_CTT_DURATION			0x2d8
#define EMC_CTT_TERM_CTRL			0x2dc
#define EMC_ZCAL_INTERVAL			0x2e0
#define EMC_ZCAL_WAIT_CNT			0x2e4

#define EMC_ZQ_CAL				0x2ec
#define EMC_ZQ_CAL_CMD				BIT(0)
#define EMC_ZQ_CAL_LONG				BIT(4)
#define EMC_ZQ_CAL_LONG_CMD_DEV0		\
	(DRAM_DEV_SEL_0 | EMC_ZQ_CAL_LONG | EMC_ZQ_CAL_CMD)
#define EMC_ZQ_CAL_LONG_CMD_DEV1		\
	(DRAM_DEV_SEL_1 | EMC_ZQ_CAL_LONG | EMC_ZQ_CAL_CMD)

#define EMC_XM2CMDPADCTRL			0x2f0
#define EMC_XM2DQSPADCTRL2			0x2fc
#define EMC_XM2DQSPADCTRL2_VREF_ENABLE		BIT(5)
#define EMC_XM2DQPADCTRL2			0x304
#define EMC_XM2CLKPADCTRL			0x308
#define EMC_XM2COMPPADCTRL			0x30c
#define EMC_XM2COMPPADCTRL_VREF_CAL_ENABLE	BIT(10)
#define EMC_XM2VTTGENPADCTRL			0x310
#define EMC_XM2VTTGENPADCTRL2			0x314
#define EMC_XM2QUSEPADCTRL			0x318
#define EMC_XM2QUSEPADCTRL_IVREF_ENABLE		BIT(4)
#define EMC_DLL_XFORM_DQS0			0x328
#define EMC_DLL_XFORM_DQS1			0x32c
#define EMC_DLL_XFORM_DQS2			0x330
#define EMC_DLL_XFORM_DQS3			0x334
#define EMC_DLL_XFORM_DQS4			0x338
#define EMC_DLL_XFORM_DQS5			0x33c
#define EMC_DLL_XFORM_DQS6			0x340
#define EMC_DLL_XFORM_DQS7			0x344
#define EMC_DLL_XFORM_QUSE0			0x348
#define EMC_DLL_XFORM_QUSE1			0x34c
#define EMC_DLL_XFORM_QUSE2			0x350
#define EMC_DLL_XFORM_QUSE3			0x354
#define EMC_DLL_XFORM_QUSE4			0x358
#define EMC_DLL_XFORM_QUSE5			0x35c
#define EMC_DLL_XFORM_QUSE6			0x360
#define EMC_DLL_XFORM_QUSE7			0x364
#define EMC_DLL_XFORM_DQ0			0x368
#define EMC_DLL_XFORM_DQ1			0x36c
#define EMC_DLL_XFORM_DQ2			0x370
#define EMC_DLL_XFORM_DQ3			0x374
#define EMC_DLI_TRIM_TXDQS0			0x3a8
#define EMC_DLI_TRIM_TXDQS1			0x3ac
#define EMC_DLI_TRIM_TXDQS2			0x3b0
#define EMC_DLI_TRIM_TXDQS3			0x3b4
#define EMC_DLI_TRIM_TXDQS4			0x3b8
#define EMC_DLI_TRIM_TXDQS5			0x3bc
#define EMC_DLI_TRIM_TXDQS6			0x3c0
#define EMC_DLI_TRIM_TXDQS7			0x3c4
#define EMC_STALL_BEFORE_CLKCHANGE		0x3c8
#define EMC_STALL_AFTER_CLKCHANGE		0x3cc
#define EMC_UNSTALL_RW_AFTER_CLKCHANGE		0x3d0
#define EMC_SEL_DPD_CTRL			0x3d8
#define EMC_SEL_DPD_CTRL_QUSE_DPD_ENABLE	BIT(9)
#define EMC_PRE_REFRESH_REQ_CNT			0x3dc
#define EMC_DYN_SELF_REF_CONTROL		0x3e0
#define EMC_TXSRDLL				0x3e4

#define EMC_RC					0x2c
#define EMC_RFC					0x30
#define EMC_RAS					0x34
#define EMC_RP					0x38
#define EMC_R2W					0x3c
#define EMC_W2R					0x40
#define EMC_R2P					0x44
#define EMC_W2P					0x48
#define EMC_RD_RCD				0x4c
#define EMC_WR_RCD				0x50
#define EMC_RRD					0x54
#define EMC_REXT				0x58
#define EMC_WDV					0x5c
#define EMC_QUSE				0x60
#define EMC_QRST				0x64
#define EMC_QSAFE				0x68
#define EMC_RDV					0x6c
#define EMC_REFRESH				0x70
#define EMC_BURST_REFRESH_NUM			0x74
#define EMC_PRE_REFRESH_REQ_CNT			0x3dc
#define EMC_PDEX2WR				0x78
#define EMC_PDEX2RD				0x7c
#define EMC_PCHG2PDEN				0x80
#define EMC_ACT2PDEN				0x84
#define EMC_AR2PDEN				0x88
#define EMC_RW2PDEN				0x8c
#define EMC_TXSR				0x90
#define EMC_TXSRDLL				0x3e4
#define EMC_TCKE				0x94
#define EMC_TFAW				0x98
#define EMC_TRPAB				0x9c
#define EMC_TCLKSTABLE				0xa0
#define EMC_TCLKSTOP				0xa4
#define EMC_TREFBW				0xa8
#define EMC_QUSE_EXTRA				0xac
#define EMC_FBIO_CFG6				0x114
#define EMC_ODT_WRITE				0xb0
#define EMC_ODT_READ				0xb4
#define EMC_FBIO_CFG5				0x104
#define EMC_CFG_DIG_DLL				0x2bc
#define EMC_CFG_DIG_DLL_PERIOD			0x2c0
#define EMC_DLL_XFORM_DQS0			0x328
#define EMC_DLL_XFORM_DQS1			0x32c
#define EMC_DLL_XFORM_DQS2			0x330
#define EMC_DLL_XFORM_DQS3			0x334
#define EMC_DLL_XFORM_DQS4			0x338
#define EMC_DLL_XFORM_DQS5			0x33c
#define EMC_DLL_XFORM_DQS6			0x340
#define EMC_DLL_XFORM_DQS7			0x344
#define EMC_DLL_XFORM_QUSE0			0x348
#define EMC_DLL_XFORM_QUSE1			0x34c
#define EMC_DLL_XFORM_QUSE2			0x350
#define EMC_DLL_XFORM_QUSE3			0x354
#define EMC_DLL_XFORM_QUSE4			0x358
#define EMC_DLL_XFORM_QUSE5			0x35c
#define EMC_DLL_XFORM_QUSE6			0x360
#define EMC_DLL_XFORM_QUSE7			0x364
#define EMC_DLI_TRIM_TXDQS0			0x3a8
#define EMC_DLI_TRIM_TXDQS1			0x3ac
#define EMC_DLI_TRIM_TXDQS2			0x3b0
#define EMC_DLI_TRIM_TXDQS3			0x3b4
#define EMC_DLI_TRIM_TXDQS4			0x3b8
#define EMC_DLI_TRIM_TXDQS5			0x3bc
#define EMC_DLI_TRIM_TXDQS6			0x3c0
#define EMC_DLI_TRIM_TXDQS7			0x3c4
#define EMC_DLL_XFORM_DQ0			0x368
#define EMC_DLL_XFORM_DQ1			0x36c
#define EMC_DLL_XFORM_DQ2			0x370
#define EMC_DLL_XFORM_DQ3			0x374
#define EMC_XM2CMDPADCTRL			0x2f0
#define EMC_XM2DQSPADCTRL2			0x2fc
#define EMC_XM2DQPADCTRL2			0x304
#define EMC_XM2CLKPADCTRL			0x308
#define EMC_XM2COMPPADCTRL			0x30c
#define EMC_XM2VTTGENPADCTRL			0x310
#define EMC_XM2VTTGENPADCTRL2			0x314
#define EMC_XM2QUSEPADCTRL			0x318
#define EMC_XM2DQSPADCTRL3			0xf8
#define EMC_CTT_TERM_CTRL			0x2dc
#define EMC_ZCAL_INTERVAL			0x2e0
#define EMC_ZCAL_WAIT_CNT			0x2e4
// #define EMC_MRS_WAIT_CNT			0xc8
#define EMC_AUTO_CAL_CONFIG			0x2a4
#define EMC_CTT					0xbc
#define EMC_CTT_DURATION			0x2d8
#define EMC_DYN_SELF_REF_CONTROL		0x3e0
#define EMC_FBIO_SPARE				0x100
#define EMC_CFG_RSV				0x120

/* mc registers */
#define MC_EMEM_ADR_CFG				0x54

#define MC_EMEM_ARB_OUTSTANDING_REQ		0x94
#define MC_EMEM_ARB_OUTSTANDING_REQ_MAX_SHIFT	0
#define MC_EMEM_ARB_OUTSTANDING_REQ_MAX_MASK	\
	(0x1FF << MC_EMEM_ARB_OUTSTANDING_REQ_MAX_SHIFT)
#define MC_EMEM_ARB_OUTSTANDING_REQ_HOLDOFF_OVERRIDE	BIT(30)
#define MC_EMEM_ARB_OUTSTANDING_REQ_LIMIT_ENABLE	BIT(31)

#define MC_EMEM_ARB_OVERRIDE			0xe8
#define MC_EMEM_ARB_OVERRIDE_EACK_MASK		(0x3 << 0)

#define MC_TIMING_CONTROL			0xfc


#define BURST_REG_LIST \
	DEFINE_REG(EMC_RC),			\
	DEFINE_REG(EMC_RFC),			\
	DEFINE_REG(EMC_RAS),			\
	DEFINE_REG(EMC_RP),			\
	DEFINE_REG(EMC_R2W),			\
	DEFINE_REG(EMC_W2R),			\
	DEFINE_REG(EMC_R2P),			\
	DEFINE_REG(EMC_W2P),			\
	DEFINE_REG(EMC_RD_RCD),			\
	DEFINE_REG(EMC_WR_RCD),			\
	DEFINE_REG(EMC_RRD),			\
	DEFINE_REG(EMC_REXT),			\
	DEFINE_REG(EMC_WEXT),			\
	DEFINE_REG(EMC_WDV),			\
	DEFINE_REG(EMC_QUSE),			\
	DEFINE_REG(EMC_QRST),			\
	DEFINE_REG(EMC_QSAFE),			\
	DEFINE_REG(EMC_RDV),			\
	DEFINE_REG(EMC_REFRESH),		\
	DEFINE_REG(EMC_BURST_REFRESH_NUM),	\
	DEFINE_REG(EMC_PRE_REFRESH_REQ_CNT),	\
	DEFINE_REG(EMC_PDEX2WR),		\
	DEFINE_REG(EMC_PDEX2RD),		\
	DEFINE_REG(EMC_PCHG2PDEN),		\
	DEFINE_REG(EMC_ACT2PDEN),		\
	DEFINE_REG(EMC_AR2PDEN),		\
	DEFINE_REG(EMC_RW2PDEN),		\
	DEFINE_REG(EMC_TXSR),			\
	DEFINE_REG(EMC_TXSRDLL),		\
	DEFINE_REG(EMC_TCKE),			\
	DEFINE_REG(EMC_TFAW),			\
	DEFINE_REG(EMC_TRPAB),			\
	DEFINE_REG(EMC_TCLKSTABLE),		\
	DEFINE_REG(EMC_TCLKSTOP),		\
	DEFINE_REG(EMC_TREFBW),			\
	DEFINE_REG(EMC_QUSE_EXTRA),		\
	DEFINE_REG(EMC_FBIO_CFG6),		\
	DEFINE_REG(EMC_ODT_WRITE),		\
	DEFINE_REG(EMC_ODT_READ),		\
	DEFINE_REG(EMC_FBIO_CFG5),		\
	DEFINE_REG(EMC_CFG_DIG_DLL),		\
	DEFINE_REG(EMC_CFG_DIG_DLL_PERIOD),	\
	DEFINE_REG(EMC_DLL_XFORM_DQS0),		\
	DEFINE_REG(EMC_DLL_XFORM_DQS1),		\
	DEFINE_REG(EMC_DLL_XFORM_DQS2),		\
	DEFINE_REG(EMC_DLL_XFORM_DQS3),		\
	DEFINE_REG(EMC_DLL_XFORM_DQS4),		\
	DEFINE_REG(EMC_DLL_XFORM_DQS5),		\
	DEFINE_REG(EMC_DLL_XFORM_DQS6),		\
	DEFINE_REG(EMC_DLL_XFORM_DQS7),		\
	DEFINE_REG(EMC_DLL_XFORM_QUSE0),	\
	DEFINE_REG(EMC_DLL_XFORM_QUSE1),	\
	DEFINE_REG(EMC_DLL_XFORM_QUSE2),	\
	DEFINE_REG(EMC_DLL_XFORM_QUSE3),	\
	DEFINE_REG(EMC_DLL_XFORM_QUSE4),	\
	DEFINE_REG(EMC_DLL_XFORM_QUSE5),	\
	DEFINE_REG(EMC_DLL_XFORM_QUSE6),	\
	DEFINE_REG(EMC_DLL_XFORM_QUSE7),	\
	DEFINE_REG(EMC_DLI_TRIM_TXDQS0),	\
	DEFINE_REG(EMC_DLI_TRIM_TXDQS1),	\
	DEFINE_REG(EMC_DLI_TRIM_TXDQS2),	\
	DEFINE_REG(EMC_DLI_TRIM_TXDQS3),	\
	DEFINE_REG(EMC_DLI_TRIM_TXDQS4),	\
	DEFINE_REG(EMC_DLI_TRIM_TXDQS5),	\
	DEFINE_REG(EMC_DLI_TRIM_TXDQS6),	\
	DEFINE_REG(EMC_DLI_TRIM_TXDQS7),	\
	DEFINE_REG(EMC_DLL_XFORM_DQ0),		\
	DEFINE_REG(EMC_DLL_XFORM_DQ1),		\
	DEFINE_REG(EMC_DLL_XFORM_DQ2),		\
	DEFINE_REG(EMC_DLL_XFORM_DQ3),		\
	DEFINE_REG(EMC_XM2CMDPADCTRL),		\
	DEFINE_REG(EMC_XM2DQSPADCTRL2),		\
	DEFINE_REG(EMC_XM2DQPADCTRL2),		\
	DEFINE_REG(EMC_XM2CLKPADCTRL),		\
	DEFINE_REG(EMC_XM2COMPPADCTRL),		\
	DEFINE_REG(EMC_XM2VTTGENPADCTRL),	\
	DEFINE_REG(EMC_XM2VTTGENPADCTRL2),	\
	DEFINE_REG(EMC_XM2QUSEPADCTRL),		\
	DEFINE_REG(EMC_XM2DQSPADCTRL3),		\
	DEFINE_REG(EMC_CTT_TERM_CTRL),		\
	DEFINE_REG(EMC_ZCAL_INTERVAL),		\
	DEFINE_REG(EMC_ZCAL_WAIT_CNT),		\
	DEFINE_REG(EMC_MRS_WAIT_CNT),		\
	DEFINE_REG(EMC_AUTO_CAL_CONFIG),	\
	DEFINE_REG(EMC_CTT),			\
	DEFINE_REG(EMC_CTT_DURATION),		\
	DEFINE_REG(EMC_DYN_SELF_REF_CONTROL),	\
	DEFINE_REG(EMC_FBIO_SPARE),		\
	DEFINE_REG(EMC_CFG_RSV)


#define DEFINE_REG(reg)	reg##_INDEX
enum {
	BURST_REG_LIST
};
#undef DEFINE_REG

#define DEFINE_REG(reg)	reg
static const u32 emc_burst_registers[] = {
	BURST_REG_LIST
};
#undef DEFINE_REG

struct emc_timing {
	u8 rev;
	unsigned long rate;

	u32 emc_burst_data[ARRAY_SIZE(emc_burst_registers)];

	u32 emc_zcal_cnt_long;
	u32 emc_acal_interval;
	u32 emc_periodic_qrst;
	u32 emc_mode_reset;
	u32 emc_mode_1;
	u32 emc_mode_2;
	u32 emc_dsr;
	int emc_min_mv;
};

struct tegra_emc {
	struct device *dev;
	struct completion clk_handshake_complete;
	struct notifier_block clk_nb;
	struct notifier_block cclk_g_nb;
	struct clk *clk;
	struct tegra_mc *mc;
	void __iomem *regs;


	struct emc_timing last_timing;
	struct emc_timing *timings;
	unsigned int num_timings;

	u32 dram_type;
	u32 dram_dev_num;
	u32 emc_cfg_saved;
	bool cfg_power_restore;
};

static irqreturn_t tegra_emc_isr(int irq, void *data)
{
	struct tegra_emc *emc = data;
	u32 intmask = EMC_REFRESH_OVERFLOW_INT |
		      EMC_INTSTATUS_CLKCHANGE_COMPLETE;
	u32 status;

	status = readl_relaxed(emc->regs + EMC_INTSTATUS) & intmask;
	if (!status)
		return IRQ_NONE;

	/* notify about EMC-CAR handshake completion */
	if (status & EMC_INTSTATUS_CLKCHANGE_COMPLETE)
		complete(&emc->clk_handshake_complete);

	/* notify about HW problem */
	if (status & EMC_REFRESH_OVERFLOW_INT)
		dev_err_ratelimited(emc->dev,
				    "refresh request overflow timeout\n");

	/* clear interrupts */
	writel_relaxed(status, emc->regs + EMC_INTSTATUS);

	return IRQ_HANDLED;
}

static int wait_for_update(struct tegra_emc *emc, u32 status_reg, u32 bit_mask,
			   bool updated_state)
{
	int i;
	for (i = 0; i < EMC_STATUS_UPDATE_TIMEOUT; i++) {
		if (!!(readl(emc->regs + status_reg) & bit_mask) ==
			    updated_state)
			return 0;
		udelay(1);
	}
	return -ETIMEDOUT;
}

static inline void emc_timing_update(struct tegra_emc *emc)
{
	int err;

	writel(0x1, emc->regs + EMC_TIMING_CONTROL);
	err = wait_for_update(emc, EMC_STATUS,
			      EMC_STATUS_TIMING_UPDATE_STALLED, false);
	if (err) {
		dev_err(emc->dev, "%s: timing update error: %d", __func__, err);
		BUG();
	}
}

static inline void set_dram_mode(struct tegra_emc *emc,
				 const struct emc_timing *next_timing,
				 const struct emc_timing *last_timing,
				 int dll_change)
{
	if (emc->dram_type == DRAM_TYPE_DDR3) {
		/* first mode_1, then mode_2, then mode_reset*/
		if (next_timing->emc_mode_1 != last_timing->emc_mode_1)
			writel(next_timing->emc_mode_1, emc->regs + EMC_EMRS);
		if (next_timing->emc_mode_2 != last_timing->emc_mode_2)
			writel(next_timing->emc_mode_2, emc->regs + EMC_EMRS);

		if ((next_timing->emc_mode_reset !=
		     last_timing->emc_mode_reset) ||
		    (dll_change == DLL_CHANGE_ON))
		{
			u32 reg = next_timing->emc_mode_reset &
				(~EMC_MODE_SET_DLL_RESET);
			if (dll_change == DLL_CHANGE_ON) {
				reg |= EMC_MODE_SET_DLL_RESET;
				reg |= EMC_MODE_SET_LONG_CNT;
			}
			writel(reg, emc->regs + EMC_MRS);
		}
	} else {
		/* first mode_2, then mode_1; mode_reset is not applicable */
		if (next_timing->emc_mode_2 != last_timing->emc_mode_2)
			writel(next_timing->emc_mode_2, emc->regs + EMC_MRW);
		if (next_timing->emc_mode_1 != last_timing->emc_mode_1)
			writel(next_timing->emc_mode_1, emc->regs + EMC_MRW);
	}
}

static inline void periodic_qrst_enable(struct tegra_emc *emc, u32 emc_cfg_reg,
					u32 emc_dbg_reg)
{
	/* enable write mux => enable periodic QRST => restore mux */
	writel(emc_dbg_reg | EMC_DBG_WRITE_MUX_ACTIVE, emc->regs + EMC_DBG);
	writel(emc_cfg_reg | EMC_CFG_PERIODIC_QRST, emc->regs + EMC_CFG);
	writel(emc_dbg_reg, emc->regs + EMC_DBG);
}

static inline bool need_qrst(struct tegra_emc *emc,
			     const struct emc_timing *next_timing,
			     const struct emc_timing *last_timing,
			     u32 emc_dpd_reg)
{
	u32 last_mode = (last_timing->emc_burst_data[EMC_FBIO_CFG5_INDEX] &
		EMC_CFG5_QUSE_MODE_MASK) >> EMC_CFG5_QUSE_MODE_SHIFT;
	u32 next_mode = (next_timing->emc_burst_data[EMC_FBIO_CFG5_INDEX] &
		EMC_CFG5_QUSE_MODE_MASK) >> EMC_CFG5_QUSE_MODE_SHIFT;

	/* QUSE DPD is disabled */
	bool ret = !(emc_dpd_reg & EMC_SEL_DPD_CTRL_QUSE_DPD_ENABLE) &&

	/* QUSE uses external mode before or after clock change */
		(((last_mode != EMC_CFG5_QUSE_MODE_PULSE_INTERN) &&
		  (last_mode != EMC_CFG5_QUSE_MODE_INTERNAL_LPBK)) ||
		 ((next_mode != EMC_CFG5_QUSE_MODE_PULSE_INTERN) &&
		  (next_mode != EMC_CFG5_QUSE_MODE_INTERNAL_LPBK)))  &&

	/* QUSE pad switches from schmitt to vref mode */
		(((last_timing->emc_burst_data[EMC_XM2QUSEPADCTRL_INDEX] &
		   EMC_XM2QUSEPADCTRL_IVREF_ENABLE) == 0) &&
		 ((next_timing->emc_burst_data[EMC_XM2QUSEPADCTRL_INDEX] &
		   EMC_XM2QUSEPADCTRL_IVREF_ENABLE) != 0));

	return ret;
}

static inline void overwrite_mrs_wait_cnt(struct tegra_emc *emc,
	const struct emc_timing *next_timing,
	bool zcal_long)
{
	u32 reg;
	u32 cnt = 512;

	/* For ddr3 when DLL is re-started: overwrite EMC DFS table settings
	   for MRS_WAIT_LONG with maximum of MRS_WAIT_SHORT settings and
	   expected operation length. Reduce the latter by the overlapping
	   zq-calibration, if any */
	if (zcal_long)
		cnt -= emc->dram_dev_num * 256;

	reg = (next_timing->emc_burst_data[EMC_MRS_WAIT_CNT_INDEX] &
		EMC_MRS_WAIT_CNT_SHORT_WAIT_MASK) >>
		EMC_MRS_WAIT_CNT_SHORT_WAIT_SHIFT;
	if (cnt < reg)
		cnt = reg;

	reg = (next_timing->emc_burst_data[EMC_MRS_WAIT_CNT_INDEX] &
		(~EMC_MRS_WAIT_CNT_LONG_WAIT_MASK));
	reg |= (cnt << EMC_MRS_WAIT_CNT_LONG_WAIT_SHIFT) &
		EMC_MRS_WAIT_CNT_LONG_WAIT_MASK;

	writel(reg, emc->regs + EMC_MRS_WAIT_CNT);
}

static inline int get_dll_change(struct tegra_emc *emc,
				 const struct emc_timing *next_timing,
				 const struct emc_timing *last_timing)
{
	bool next_dll_enabled = !(next_timing->emc_mode_1 & 0x1);
	bool last_dll_enabled = !(last_timing->emc_mode_1 & 0x1);

	if (next_dll_enabled == last_dll_enabled)
		return DLL_CHANGE_NONE;
	else if (next_dll_enabled)
		return DLL_CHANGE_ON;
	else
		return DLL_CHANGE_OFF;
}

static inline void auto_cal_disable(struct tegra_emc *emc)
{
	int err;

	writel(0, emc->regs + EMC_AUTO_CAL_INTERVAL);
	err = wait_for_update(emc, EMC_AUTO_CAL_STATUS,
			      EMC_AUTO_CAL_STATUS_ACTIVE, false);
	if (err) {
		dev_err(emc->dev, "%s: disable auto-cal error: %d", __func__,
			err);
		BUG();
	}
}

static inline bool dqs_preset(struct tegra_emc *emc,
			      const struct emc_timing *next_timing,
			      const struct emc_timing *last_timing)
{
	bool ret = false;

#define DQS_SET(reg, bit)						      \
	do {								      \
		if ((next_timing->emc_burst_data[EMC_##reg##_INDEX] &	      \
		     EMC_##reg##_##bit##_ENABLE) &&			      \
		    (!(last_timing->emc_burst_data[EMC_##reg##_INDEX] &	      \
		       EMC_##reg##_##bit##_ENABLE)))   {		      \
			writel(last_timing->emc_burst_data[EMC_##reg##_INDEX] \
				   | EMC_##reg##_##bit##_ENABLE,	      \
				   emc->regs + EMC_##reg);		      \
			ret = true;					      \
		}							      \
	} while (0)

	DQS_SET(XM2DQSPADCTRL2, VREF);
	DQS_SET(XM2DQSPADCTRL3, VREF);
	DQS_SET(XM2QUSEPADCTRL, IVREF);

	return ret;
}

static inline void disable_early_ack(struct tegra_emc *emc, u32 mc_override)
{
	static u32 override_val;

	override_val = mc_override & (~MC_EMEM_ARB_OVERRIDE_EACK_MASK);
	mc_writel(emc->mc, override_val, MC_EMEM_ARB_OVERRIDE);
	__cpuc_flush_dcache_area(&override_val, sizeof(override_val));
	outer_clean_range(__pa(&override_val), __pa(&override_val + 1));
	override_val |= mc_override & MC_EMEM_ARB_OVERRIDE_EACK_MASK;
}


static inline void set_mc_arbiter_limits(struct tegra_emc *emc)
{
	u32 reg = mc_readl(emc->mc, MC_EMEM_ARB_OUTSTANDING_REQ);
	u32 max_val = 0x50 << EMC_MRS_WAIT_CNT_SHORT_WAIT_SHIFT;

	if (!(reg & MC_EMEM_ARB_OUTSTANDING_REQ_HOLDOFF_OVERRIDE) ||
	    ((reg & MC_EMEM_ARB_OUTSTANDING_REQ_MAX_MASK) > max_val)) {
		reg = MC_EMEM_ARB_OUTSTANDING_REQ_LIMIT_ENABLE |
			MC_EMEM_ARB_OUTSTANDING_REQ_HOLDOFF_OVERRIDE | max_val;
		mc_writel(emc->mc, reg, MC_EMEM_ARB_OUTSTANDING_REQ);
		mc_writel(emc->mc, 0x1, MC_TIMING_CONTROL);
	}
}

static struct emc_timing *tegra_emc_find_timing(struct tegra_emc *emc,
						unsigned long rate)
{
	struct emc_timing *timing = NULL;
	unsigned int i;

	for (i = 0; i < emc->num_timings; i++) {
		if (emc->timings[i].rate == rate) {
			timing = &emc->timings[i];
			break;
		}
	}

	if (!timing) {
		dev_err(emc->dev, "no timing for rate %lu\n", rate);
		return NULL;
	}

	return timing;
}

/*
 * After deep sleep EMC power features are not restored.
 * Do it at run-time after the 1st clock change.
 */
static inline void emc_cfg_power_restore(struct tegra_emc *emc,
					 struct emc_timing *next_timing)
{
	u32 reg = readl(emc->regs + EMC_CFG);
	u32 pwr_mask = EMC_CFG_PWR_MASK;

	if (next_timing->rev >= 0x32)
		pwr_mask &= ~EMC_CFG_DYN_SREF_ENABLE;

	if ((reg ^ emc->emc_cfg_saved) & pwr_mask) {
		reg = (reg & (~pwr_mask)) | (emc->emc_cfg_saved & pwr_mask);
		writel(reg, emc->regs + EMC_CFG);
		emc_timing_update(emc);
	}
}

int emc_prepare_timing_change(struct tegra_emc *emc,
			      unsigned long rate)
{
	struct emc_timing *next_timing = tegra_emc_find_timing(emc, rate);
	struct emc_timing *last_timing = &emc->last_timing;
	// u32 clk_setting

	int i, dll_change, pre_wait;
	bool dyn_sref_enabled, vref_cal_toggle, qrst_used, zcal_long;
	u32 mc_override, emc_cfg_reg, emc_dbg_reg;

	if (!next_timing) {
		dev_err(emc->dev, "%s: next_timing not found\n", __func__);
		return -EINVAL;
	}

	if (!last_timing) {
		dev_err(emc->dev, "%s: last_timing not found\n", __func__);
		return -EINVAL;
	}

	mc_override = mc_readl(emc->mc, MC_EMEM_ARB_OVERRIDE);
	emc_cfg_reg = readl(emc->regs + EMC_CFG);
	emc_dbg_reg = readl(emc->regs + EMC_DBG);

	emc->emc_cfg_saved = emc_cfg_reg;

	dyn_sref_enabled = emc_cfg_reg & EMC_CFG_DYN_SREF_ENABLE;
	dll_change = get_dll_change(emc, next_timing, last_timing);
	zcal_long = (next_timing->emc_burst_data[EMC_ZCAL_INTERVAL_INDEX] != 0) &&
		(last_timing->emc_burst_data[EMC_ZCAL_INTERVAL_INDEX] == 0);

	/* 1. clear clkchange_complete interrupts */
	// writel(EMC_INTSTATUS_CLKCHANGE_COMPLETE, emc->regs + EMC_INTSTATUS);
	// writel(EMC_INTSTATUS_CLKCHANGE_COMPLETE, emc->regs + EMC_INTMASK);

	/* 2. disable dynamic self-refresh and preset dqs vref, then wait for
	   possible self-refresh entry/exit and/or dqs vref settled - waiting
	   before the clock change decreases worst case change stall time */
	if (dyn_sref_enabled) {
		emc_cfg_reg &= ~EMC_CFG_DYN_SREF_ENABLE;
		writel(emc_cfg_reg, emc->regs + EMC_CFG);
		pre_wait = 5;		/* 5us+ for self-refresh entry/exit */
	}

	/* 2.25 update MC arbiter settings */
	set_mc_arbiter_limits(emc);
	if (mc_override & MC_EMEM_ARB_OVERRIDE_EACK_MASK)
		disable_early_ack(emc, mc_override);

	/* 2.5 check dq/dqs vref delay */
	if (dqs_preset(emc, next_timing, last_timing)) {
		if (pre_wait < 3)
			pre_wait = 3;	/* 3us+ for dqs vref settled */
	}
	if (pre_wait) {
		emc_timing_update(emc);
		udelay(pre_wait);
	}

	/* 3. disable auto-cal if vref mode is switching */
	vref_cal_toggle = (next_timing->emc_acal_interval != 0) &&
		((next_timing->emc_burst_data[EMC_XM2COMPPADCTRL_INDEX] ^
		  last_timing->emc_burst_data[EMC_XM2COMPPADCTRL_INDEX]) &
		 EMC_XM2COMPPADCTRL_VREF_CAL_ENABLE);
	if (vref_cal_toggle)
		auto_cal_disable(emc);


	/* 4. program burst shadow registers */
	for (i = 0; i < ARRAY_SIZE(emc_burst_registers); i++) {
		if (i == EMC_XM2CLKPADCTRL_INDEX)
			continue;
		writel_relaxed(next_timing->emc_burst_data[i],
			       emc->regs + emc_burst_registers[i]);
	}

	tegra_mc_write_emem_configuration(emc->mc, rate);

	// TODO
	// if ((dram_type == DRAM_TYPE_LPDDR2) &&
	//     (dram_over_temp_state != DRAM_OVER_TEMP_NONE))
	// 	set_over_temp_timing(next_timing, dram_over_temp_state);

	wmb();
	barrier();

	/* On ddr3 when DLL is re-started predict MRS long wait count and
	   overwrite DFS table setting */
	if ((emc->dram_type == DRAM_TYPE_DDR3) && (dll_change == DLL_CHANGE_ON))
		overwrite_mrs_wait_cnt(emc, next_timing, zcal_long);

	/* the last read below makes sure prev writes are completed */
	qrst_used = need_qrst(emc, next_timing, last_timing,
			      readl(emc->regs + EMC_SEL_DPD_CTRL));

	/* 5. flow control marker 1 (no EMC read access after this) */
	writel(1, emc->regs + EMC_STALL_BEFORE_CLKCHANGE);

	/* 6. enable periodic QRST */
	if (qrst_used)
		periodic_qrst_enable(emc, emc_cfg_reg, emc_dbg_reg);

	/* 6.1 disable auto-refresh to save time after clock change */
	writel(EMC_REFCTRL_DISABLE_ALL(emc->dram_dev_num),
	       emc->regs + EMC_REFCTRL);

	/* 7. turn Off dll and enter self-refresh on DDR3 */
	if (emc->dram_type == DRAM_TYPE_DDR3) {
		if (dll_change == DLL_CHANGE_OFF)
			writel(next_timing->emc_mode_1, emc->regs + EMC_EMRS);
		writel(DRAM_BROADCAST(emc->dram_dev_num) |
			   EMC_SELF_REF_CMD_ENABLED, emc->regs + EMC_SELF_REF);
	}

	/* 8. flow control marker 2 */
	writel(1, emc->regs + EMC_STALL_AFTER_CLKCHANGE);

	/* 8.1 enable write mux, update unshadowed pad control */
	writel(emc_dbg_reg | EMC_DBG_WRITE_MUX_ACTIVE, emc->regs + EMC_DBG);
	writel(next_timing->emc_burst_data[EMC_XM2CLKPADCTRL_INDEX],
		   emc->regs +EMC_XM2CLKPADCTRL);

	/* 9. restore periodic QRST, and disable write mux */
	if ((qrst_used) || (next_timing->emc_periodic_qrst !=
			    last_timing->emc_periodic_qrst)) {
		emc_cfg_reg = next_timing->emc_periodic_qrst ?
			emc_cfg_reg | EMC_CFG_PERIODIC_QRST :
			emc_cfg_reg & (~EMC_CFG_PERIODIC_QRST);
		writel(emc_cfg_reg, emc->regs + EMC_CFG);
	}
	writel(emc_dbg_reg, emc->regs + EMC_DBG);

	/* 10. exit self-refresh on DDR3 */
	if (emc->dram_type == DRAM_TYPE_DDR3)
		writel(DRAM_BROADCAST(emc->dram_dev_num),
		       emc->regs + EMC_SELF_REF);

	/* 11. set dram mode registers */
	set_dram_mode(emc, next_timing, last_timing, dll_change);

	/* 12. issue zcal command if turning zcal On */
	if (zcal_long) {
		writel(EMC_ZQ_CAL_LONG_CMD_DEV0, emc->regs + EMC_ZQ_CAL);
		if (emc->dram_dev_num > 1)
			writel(EMC_ZQ_CAL_LONG_CMD_DEV1,
			       emc->regs + EMC_ZQ_CAL);
	}

	/* 13. flow control marker 3 */
	writel(1, emc->regs + EMC_UNSTALL_RW_AFTER_CLKCHANGE);

	/* complete prev writes */
	mc_readl(emc->mc, MC_EMEM_ADR_CFG);

	reinit_completion(&emc->clk_handshake_complete);

	return 0;
}

int emc_complete_timing_change(struct tegra_emc *emc, unsigned long rate,
			       bool flush)
{
	struct emc_timing *next_timing = tegra_emc_find_timing(emc, rate);
	struct emc_timing *last_timing = &emc->last_timing;
	bool dyn_sref_enabled, vref_cal_toggle, zcal_long;
	u32 mc_override;
	int err;

	/* complete prev writes */
	mc_override = mc_readl(emc->mc, MC_EMEM_ARB_OVERRIDE);

	if (flush) {
		readl(emc->regs);
		err = wait_for_update(emc, EMC_INTSTATUS,
				      EMC_INTSTATUS_CLKCHANGE_COMPLETE, true);
		if (err) {
			dev_err(emc->dev, "%s: clock change completion error: %d",
				__func__, err);
			BUG();
		}
	} else {
		struct completion *x = &emc->clk_handshake_complete;
		unsigned long timeout;

		timeout = usecs_to_jiffies(EMC_STATUS_UPDATE_TIMEOUT);
		timeout = wait_for_completion_timeout(x, timeout);
		if (timeout == 0) {
			dev_err(emc->dev, "EMC-CAR handshake failed\n");
			return -EIO;
		} else if (timeout < 0) {
			dev_err(emc->dev, "failed to wait for EMC-CAR handshake: %ld\n",
				timeout);
			return timeout;
		}
	}

	vref_cal_toggle = (next_timing->emc_acal_interval != 0) &&
		((next_timing->emc_burst_data[EMC_XM2COMPPADCTRL_INDEX] ^
		  last_timing->emc_burst_data[EMC_XM2COMPPADCTRL_INDEX]) &
		 EMC_XM2COMPPADCTRL_VREF_CAL_ENABLE);
	zcal_long = (next_timing->emc_burst_data[EMC_ZCAL_INTERVAL_INDEX] != 0) &&
		(last_timing->emc_burst_data[EMC_ZCAL_INTERVAL_INDEX] == 0);

	dyn_sref_enabled = emc->emc_cfg_saved & EMC_CFG_DYN_SREF_ENABLE;


	/* 14.1 re-enable auto-refresh */
	writel(EMC_REFCTRL_ENABLE_ALL(emc->dram_dev_num),
	       emc->regs + EMC_REFCTRL);

	/* 15. restore auto-cal */
	if (vref_cal_toggle)
		writel(next_timing->emc_acal_interval,
		       emc->regs + EMC_AUTO_CAL_INTERVAL);

	/* 16. restore dynamic self-refresh */
	if (next_timing->rev >= 0x32)
		dyn_sref_enabled = next_timing->emc_dsr;
	if (dyn_sref_enabled) {
		u32 emc_cfg_reg = readl(emc->regs + EMC_CFG);
		emc_cfg_reg |= EMC_CFG_DYN_SREF_ENABLE;
		writel(emc_cfg_reg, emc->regs + EMC_CFG);
	}

	/* 17. set zcal wait count */
	if (zcal_long)
		writel(next_timing->emc_zcal_cnt_long,
		       emc->regs + EMC_ZCAL_WAIT_CNT);

	/* 18. update restored timing */
	udelay(2);
	emc_timing_update(emc);

	/* 18.a restore early ACK */
	mc_writel(emc->mc, mc_override, MC_EMEM_ARB_OVERRIDE);

	if (emc->cfg_power_restore) {
		emc_cfg_power_restore(emc, next_timing);
		emc->cfg_power_restore = false;
	}

	emc->last_timing = *next_timing;

	return 0;
}

static int tegra_emc_clk_change_notify(struct notifier_block *nb,
				       unsigned long msg, void *data)
{
	struct tegra_emc *emc = container_of(nb, struct tegra_emc, clk_nb);
	struct clk_notifier_data *cnd = data;
	int err;

	switch (msg) {
	case PRE_RATE_CHANGE:
		err = emc_prepare_timing_change(emc, cnd->new_rate);
		break;

	case ABORT_RATE_CHANGE:
		err = emc_prepare_timing_change(emc, cnd->old_rate);
		if (err)
			break;

		err = emc_complete_timing_change(emc, cnd->old_rate, true);
		break;

	case POST_RATE_CHANGE:
		err = emc_complete_timing_change(emc, cnd->new_rate, false);
		break;

	default:
		return NOTIFY_DONE;
	}

	return notifier_from_errno(err);
}

static unsigned long tegra_emc_to_cpu_ratio(struct tegra_emc *emc,
					    unsigned long cpu_rate)
{
	static unsigned long emc_max_rate;

	emc_max_rate = clk_round_rate(emc->clk, ULONG_MAX);

	/* Vote on memory bus frequency based on cpu frequency */
	if (cpu_rate >= 925000)
		return emc_max_rate;	/* cpu >= 925 MHz, emc max */
	else if (cpu_rate >= 450000)
		return emc_max_rate/2;	/* cpu >= 450 MHz, emc max/2 */
	else if (cpu_rate >= 250000)
		return 100000000;	/* cpu >= 250 MHz, emc 100 MHz */
	else
		return 0;		/* emc min */
}

static int tegra_cclk_g_change_notify(struct notifier_block *nb,
				      unsigned long msg, void *data)
{
	struct tegra_emc *emc = container_of(nb, struct tegra_emc, cclk_g_nb);
	struct cpufreq_freqs *freq = data;
	unsigned long emc_rate;
	int err = 0;

	emc_rate = tegra_emc_to_cpu_ratio(emc, freq->new);

	switch (msg) {
	case CPUFREQ_PRECHANGE:
		if (freq->old < freq->new) {
			err = clk_set_rate(emc->clk, emc_rate);
		}
		break;

	case CPUFREQ_POSTCHANGE:
		if (freq->old  > freq->new) {
			err = clk_set_rate(emc->clk, emc_rate);
		}
		break;

	default:
		return NOTIFY_DONE;
	}

	return notifier_from_errno(err);
}

static void emc_read_current_timing(struct tegra_emc *emc,
				    struct emc_timing *timing)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(emc_burst_registers); ++i) {
		if (!emc_burst_registers[i])
			continue;
		timing->emc_burst_data[i] =
			readl(emc->regs + emc_burst_registers[i]);
	}

	timing->emc_zcal_cnt_long = 0;
	timing->emc_acal_interval = 0;
	timing->emc_periodic_qrst = 0;
	timing->emc_mode_reset = 0;
	timing->emc_mode_1 = 0;
	timing->emc_mode_2 = 0;
	timing->emc_dsr = 0;
	// timing->emc_min_mv = 0;
}

void tegra_emc_dram_type_init(/*struct clk *c*/ struct tegra_emc *emc)
{
	// emc = c;

	if (!emc->mc) {
		dev_err(emc->dev, "Can't init dram type. mc not available.\n");
		return;
	}

	emc->dram_type = (readl(emc->regs + EMC_FBIO_CFG5) &
		     EMC_CFG5_TYPE_MASK) >> EMC_CFG5_TYPE_SHIFT;

	// TODO: How to implement this?
	// if (dram_type == DRAM_TYPE_DDR3)
	// 	emc->min_rate = EMC_MIN_RATE_DDR3;

	/* 2 dev max */
	emc->dram_dev_num = (mc_readl(emc->mc, MC_EMEM_ADR_CFG) & 0x1) + 1;
	emc->emc_cfg_saved = readl(emc->regs + EMC_CFG);

	dev_info(emc->dev, "dram_type = 0x%X\n", emc->dram_type);
	dev_info(emc->dev, "dram_dev_num = 0x%X\n", emc->dram_dev_num);
	dev_info(emc->dev, "emc_cfg_saved = 0x%X\n", emc->emc_cfg_saved);
}

static int load_one_timing_from_dt(struct tegra_emc *emc,
				   struct emc_timing *timing,
				   struct device_node *node)
{
	u32 value;
	int err;

	err = of_property_read_u32(node, "clock-frequency", &value);
	if (err) {
		dev_err(emc->dev, "timing %pOFn: failed to read rate: %d\n",
			node, err);
		return err;
	}

	timing->rate = value;

	err = of_property_read_u32(node, "nvidia,emc-rev", &value);
	if (err) {
		dev_err(emc->dev, "timing %pOFn: failed to emc revision: %d\n",
			node, err);
		return err;
	}

	timing->rev = value;

	err = of_property_read_u32_array(node, "nvidia,emc-configuration",
					 timing->emc_burst_data,
					 ARRAY_SIZE(timing->emc_burst_data));
	if (err) {
		dev_err(emc->dev,
			"timing %pOFn: failed to read emc burst data: %d\n",
			node, err);
		return err;
	}

#define EMC_READ_PROP(prop, dtprop) { \
	err = of_property_read_u32(node, dtprop, &timing->prop); \
	if (err) { \
		dev_err(emc->dev, "timing %pOFn: failed to read " #prop ": %d\n", \
			node, err); \
		return err; \
	} \
}

	EMC_READ_PROP(emc_zcal_cnt_long, "nvidia,emc-zcal-cnt-long")
	EMC_READ_PROP(emc_acal_interval, "nvidia,emc-auto-cal-interval")
	EMC_READ_PROP(emc_periodic_qrst, "nvidia,emc-periodic-qrst")
	EMC_READ_PROP(emc_mode_reset, "nvidia,emc-mode-reset")
	EMC_READ_PROP(emc_mode_1, "nvidia,emc-mode-1")
	EMC_READ_PROP(emc_mode_2, "nvidia,emc-mode-2")
	EMC_READ_PROP(emc_dsr, "nvidia,emc-dsr")
	/* EMC_READ_PROP(emc_min_mv, "nvidia,emc-min-mv") */

#undef EMC_READ_PROP

	return 0;
}

static int cmp_timings(const void *_a, const void *_b)
{
	const struct emc_timing *a = _a;
	const struct emc_timing *b = _b;

	if (a->rate < b->rate)
		return -1;

	if (a->rate > b->rate)
		return 1;

	return 0;
}

static int tegra_emc_load_timings_from_dt(struct tegra_emc *emc,
					  struct device_node *node)
{
	struct device_node *child;
	struct emc_timing *timing;
	int child_count;
	int err;

	child_count = of_get_child_count(node);
	if (!child_count) {
		dev_err(emc->dev, "no memory timings in DT node: %pOF\n", node);
		return -EINVAL;
	}

	emc->timings = devm_kcalloc(emc->dev, child_count, sizeof(*timing),
				    GFP_KERNEL);
	if (!emc->timings)
		return -ENOMEM;

	emc->num_timings = child_count;
	timing = emc->timings;

	for_each_child_of_node(node, child) {
		err = load_one_timing_from_dt(emc, timing++, child);
		if (err) {
			of_node_put(child);
			return err;
		}
	}

	sort(emc->timings, emc->num_timings, sizeof(*timing), cmp_timings,
	     NULL);

	return 0;
}
static struct device_node *
tegra_emc_find_node_by_ram_code(struct device_node *node, u32 ram_code)
{
	struct device_node *np;
	int err;

	for_each_child_of_node(node, np) {
		u32 value;

		err = of_property_read_u32(np, "nvidia,ram-code", &value);
		if (err || (value != ram_code))
			continue;

		return np;
	}

	return NULL;
}

static int init_emc_min_rate(struct tegra_emc *emc)
{
	struct clk *emc_clk;
	unsigned long rate;
	int err;

	if (emc->dram_type == DRAM_TYPE_LPDDR2) {
		return 0;
	}

	emc_clk = of_clk_get_by_name(emc->dev->of_node, "emc");

	rate = clk_round_rate(emc_clk, TEGRA_EMC_BRIDGE_RATE_MIN);

	// TODO: Check the voltage
	// mv = tegra_dvfs_predict_millivolts(emc, rate);
	// if (IS_ERR_VALUE(mv) || (mv > TEGRA_EMC_BRIDGE_MVOLTS_MIN))
	// 	return false;

	err = clk_set_min_rate(emc_clk, rate);
	if (err) {
		dev_err(emc->dev, "could not set emc clock min rate.\n");
		return err;
	}

	dev_info(emc->dev, "emc min rate = %lu\n", rate);

	return 0;
}


static void emc_setup_hw(struct tegra_emc *emc)
{
	u32 reg;
	u32 intmask = EMC_REFRESH_OVERFLOW_INT |
		      EMC_INTSTATUS_CLKCHANGE_COMPLETE;

	/* configure clock change mode according to dram type */
	reg = readl(emc->regs + EMC_CFG_2) & (~EMC_CFG_2_MODE_MASK);
	reg |= ((emc->dram_type == DRAM_TYPE_LPDDR2) ? EMC_CFG_2_PD_MODE :
		EMC_CFG_2_SREF_MODE) << EMC_CFG_2_MODE_SHIFT;
	writel(reg, emc->regs + EMC_CFG_2);

	/* initialize interrupt */
	writel_relaxed(intmask, emc->regs + EMC_INTMASK);
	writel_relaxed(intmask, emc->regs + EMC_INTSTATUS);
}

static int emc_init(struct tegra_emc *emc, unsigned long rate)
{
	int err;

	err = clk_set_rate(emc->clk, rate);
	if (err) {
		dev_err(emc->dev,
			"failed to change emc rate: %d\n", err);
		return err;
	}

	return 0;
}

static int tegra_emc_probe(struct platform_device *pdev)
{
	struct platform_device *mc;
	struct device_node *np;
	struct tegra_emc *emc;
	struct resource *res;
	u32 ram_code;
	int irq, err;

	dev_info(&pdev->dev, "%s\n", __func__);

	/* driver has nothing to do in a case of memory timing absence */
	if (of_get_child_count(pdev->dev.of_node) == 0) {
		dev_info(&pdev->dev,
			"EMC device tree node doesn't have memory timings\n");
		return 0;
	}

	irq = platform_get_irq(pdev, 0);
	if (irq < 0) {
		dev_err(&pdev->dev, "interrupt not specified\n");
		dev_err(&pdev->dev, "please update your device tree\n");
		return irq;
	}

	emc = devm_kzalloc(&pdev->dev, sizeof(*emc), GFP_KERNEL);
	if (!emc)
		return -ENOMEM;

	init_completion(&emc->clk_handshake_complete);
	emc->clk_nb.notifier_call = tegra_emc_clk_change_notify;
	emc->cclk_g_nb.notifier_call = tegra_cclk_g_change_notify;
	emc->dev = &pdev->dev;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	emc->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(emc->regs))
		return PTR_ERR(emc->regs);

	err = devm_request_irq(&pdev->dev, irq, tegra_emc_isr, 0,
			       dev_name(&pdev->dev), emc);
	if (err) {
		dev_err(&pdev->dev, "failed to request IRQ#%u: %d\n", irq, err);
		return err;
	}

	emc->clk = devm_clk_get(&pdev->dev, "emc");
	if (IS_ERR(emc->clk)) {
		err = PTR_ERR(emc->clk);
		dev_err(&pdev->dev, "failed to get emc clock: %d\n", err);
		return err;
	}

	err = clk_notifier_register(emc->clk, &emc->clk_nb);
	if (err) {
		dev_err(&pdev->dev, "failed to register clk notifier: %d\n",
			err);
		return err;
	}

	err = cpufreq_register_notifier(&emc->cclk_g_nb,
					CPUFREQ_TRANSITION_NOTIFIER);
	if (err) {
		dev_err(&pdev->dev, "failed to register cpufreq notifier: %d\n",
			err);
		return err;
	}

	np = of_parse_phandle(pdev->dev.of_node, "nvidia,memory-controller", 0);
	if (!np) {
		dev_err(&pdev->dev, "could not get memory controller\n");
		err = -ENOENT;
		goto unreg_clk_notifier;
	}

	mc = of_find_device_by_node(np);
	of_node_put(np);
	if (!mc) {
		err = -ENOENT;
		goto unreg_clk_notifier;
	}

	emc->mc = platform_get_drvdata(mc);
	if (!emc->mc) {
		err = -EPROBE_DEFER;
		goto unreg_clk_notifier;
	}

	tegra_emc_dram_type_init(emc);
	if (emc->dram_type != DRAM_TYPE_DDR3) {
		dev_err(&pdev->dev, "Unsupported dram type.\n");
		err = -EINVAL;
		goto unreg_clk_notifier;
	}

	ram_code = tegra_read_ram_code();
	dev_info(&pdev->dev, "ram_code = 0x%X\n", ram_code);

	np = tegra_emc_find_node_by_ram_code(pdev->dev.of_node, ram_code);
	if (!np) {
		err = -EINVAL;
		goto unreg_clk_notifier;
	}

	err = tegra_emc_load_timings_from_dt(emc, np);
	of_node_put(np);
	if (err) {
		goto unreg_clk_notifier;
	}

	if (emc->num_timings == 0) {
		dev_err(&pdev->dev,
			"no memory timings for RAM code %u registered\n",
			ram_code);
		err = -ENOENT;
		goto unreg_clk_notifier;
	}

	// TODO: Match EMC source/divider settings with table entries

	// TODO: adjust_emc_dvfs_table (???)

	// TODO: implement is_emc_bridge
	err = init_emc_min_rate(emc);
	if (err) {
		goto unreg_clk_notifier;
	}

	emc_read_current_timing(emc, &emc->last_timing);

	platform_set_drvdata(pdev, emc);

	emc_setup_hw(emc);

	/* set DRAM clock rate to maximum */
	err = emc_init(emc, emc->timings[emc->num_timings - 1].rate);
	if (err) {
		dev_err(&pdev->dev, "failed to initialize EMC clock rate: %d\n",
			err);
		goto unreg_clk_notifier;
	}

	dev_info(&pdev->dev, "%s: done.\n", __func__);

	return 0;

unreg_clk_notifier:
	clk_notifier_unregister(emc->clk, &emc->clk_nb);

	return err;
}

static int __maybe_unused tegra_emc_resume(struct device *dev)
{
	struct tegra_emc *emc = dev_get_drvdata(dev);

	emc->cfg_power_restore = true;

	return 0;
}
static SIMPLE_DEV_PM_OPS(tegra_emc_pm_ops, NULL, tegra_emc_resume);

static const struct of_device_id tegra_emc_of_match[] = {
	{ .compatible = "nvidia,tegra30-emc", },
	{},
};

static struct platform_driver tegra_emc_driver = {
	.probe = tegra_emc_probe,
	.driver = {
		.name = "tegra30-emc",
		.of_match_table = tegra_emc_of_match,
		.pm	= &tegra_emc_pm_ops,
		.suppress_bind_attrs = true,
	},
};

static int __init tegra_emc_init(void)
{
	return platform_driver_register(&tegra_emc_driver);
}
subsys_initcall(tegra_emc_init);
