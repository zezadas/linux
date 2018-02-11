/*
 * Simple dynamic voltage scaling driver for NVIDIA Tegra 2 SoC's
 *
 * Author: Dmitry Osipenko <digetx@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

// #define DEBUG 1

#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/cpufreq.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/regulator/consumer.h>
#include <linux/clk-provider.h>
#include <linux/debugfs.h>

#include <soc/tegra/fuse.h>

#define MAX_DVFS_FREQS	24

#define _100uV			100000
#define _170uV			170000

/* RTC domain defines */
#define RTC_MAX_VDD		1300
#define RTC_MIN_VDD		950

/* CPU domain defines */
#define CPU_MAX_VDD    1375
static const int cpu_millivolts_process0[MAX_DVFS_FREQS] =
{
	750,   // 216 Mhz
	750,   // 312 Mhz
	800,   // 456 Mhz
	850,   // 608 Mhz
	900,   // 760 Mhz
	950,   // 816 Mhz
	1000,  // 912 Mhz
	1025,  // 1000 Mhz
	1225,  // 1200 Mhz
	1250,  // 1400 Mhz
	1300,  // 1500 Mhz
	1375,  // 1600 Mhz
	CPU_MAX_VDD,
};

static const int cpu_millivolts_process1[MAX_DVFS_FREQS] =
{
	750,   // 216 Mhz
	750,   // 312 Mhz
	800,   // 456 Mhz
	850,   // 608 Mhz
	875,   // 760 Mhz
	950,   // 816 Mhz
	950,   // 912 Mhz
	1000,  // 1000 Mhz
	1000,  // 1200 Mhz
	1200,  // 1400 Mhz
	1250,  // 1500 Mhz
	1325,  // 1600 Mhz
	CPU_MAX_VDD,
};


#define DVFS_CPU_CLIENT(_name, _speedo_id, _process_id, _voltages, _freqs...)			\
	{								\
		.clk = NULL,						\
		.clk_name = #_name,					\
		.speedo_id = _speedo_id,					\
		.process_id = _process_id,				\
		.freqs = { _freqs, -1 },				\
		.voltages_mv =  _voltages,				\
	}

/* core domain defines */
// #define CORE_MAX_VDD    1300
#define CORE_MAX_VDD (CPU_MAX_VDD+_100uV)
static const int core_millivolts[7] = {950, 1000, 1100, 1200, 1225, 1275, CORE_MAX_VDD,};


#define DVFS_CORE_CLIENT(_name, _process_id, _freqs...)			\
	{								\
		.clk = NULL,						\
		.clk_name = #_name,					\
		.speedo_id = -1,					\
		.process_id = _process_id,				\
		.freqs = { _freqs, -1 },				\
		.voltages_mv =  core_millivolts,				\
	}

#define DVFS(_name, _clients)				\
	static struct dvfs_domain dvfs_##_name = {			\
		.clients = _clients,			\
		.nclients = ARRAY_SIZE(_clients),			\
	}


static DEFINE_MUTEX(dvfs_lock);
static bool dvfs_enabled;
static struct device *dvfs_dev;
static struct regulator *cpu_reg, *core_reg, *rtc_reg;

struct dvfs_client {
	struct delayed_work	work;
	struct dvfs_domain	*dvfs;
	struct notifier_block	nb;
	struct list_head	node;
	struct mutex		deferred_work_lock;
	struct clk		*clk;
	const char		*clk_name;
	int		speedo_id;
	int		process_id;
	unsigned		index;
	int			freqs[MAX_DVFS_FREQS];
	const int		*voltages_mv;
};

struct dvfs_domain {
	struct list_head	active_clients;
	struct dvfs_client	*clients;
	int			nclients;
};

static struct dvfs_client dvfs_cpu_clients[] = {
	DVFS_CPU_CLIENT(cpu, 1, 0, cpu_millivolts_process0, 216, 312, 456, 608, 750, 816, 912, 1000, 1200, 1400, 1500, 1600),
	DVFS_CPU_CLIENT(cpu, 1, 1, cpu_millivolts_process1, 216, 312, 456, 608, 750, 816, 912, 1000, 1200, 1400, 1500, 1600),
};


static struct dvfs_client dvfs_core_clients[] = {
	// DVFS_CLIENT(disp1, 158000,	158000,	190000),
	// DVFS_CLIENT(disp2, 158000,	158000,	190000),

	DVFS_CORE_CLIENT(host1x, -1, 133000,	166000),
	DVFS_CORE_CLIENT(epp, -1, 133000,	171000,	247000,	300000),
	DVFS_CORE_CLIENT(2d, -1, 133000,	171000,	247000,	300000),
	DVFS_CORE_CLIENT(hdmi, -1, 0,	0,	0,	148500),
	DVFS_CORE_CLIENT(emc, -1, 57000,  333000, 380000, 666000),
	DVFS_CORE_CLIENT(3d, -1, 218500, 256500, 323000, 380000, 400000),
	DVFS_CORE_CLIENT(mpe, 2, 190000,	237500,	300000),
	DVFS_CORE_CLIENT(vi, -1, 85000,	100000,	150000),
	DVFS_CORE_CLIENT(csi, -1, 0,	0,	0,	0, 72000),
	DVFS_CORE_CLIENT(sclk, 2, 152000,	180500,	229500,	260000, 285000,	300000),
	DVFS_CORE_CLIENT(vde, 2, 152000,	209000,	285000,	300000),
	DVFS_CORE_CLIENT(mipi, -1, 40000,	40000,	40000,	40000, 60000),
	DVFS_CORE_CLIENT(usbd, -1, 400000,	400000,	400000,	480000),
	DVFS_CORE_CLIENT(usb2, -1, 0,	0,	480000),
	DVFS_CORE_CLIENT(usb3, -1, 400000,	400000,	400000,	480000),
};

DVFS(cpu, dvfs_cpu_clients);
DVFS(core, dvfs_core_clients);

struct dentry *debugfs_file = NULL;

static void dvfs_update_cpu_voltage(int new_uV)
{
	int ret;

	dev_dbg(dvfs_dev, "setting CPU voltage %duV -> %duV\n",
		regulator_get_voltage(cpu_reg), new_uV);

	ret = regulator_set_voltage(cpu_reg, new_uV, CPU_MAX_VDD * 1000);
	if (ret)
		dev_err(dvfs_dev, "failed to update CPU voltage\n");
}

static void dvfs_update_core_voltage(int new_uV)
{
	int ret;

	dev_dbg(dvfs_dev, "setting core voltage %duV -> %duV\n",
		regulator_get_voltage(core_reg), new_uV);

	ret = regulator_set_voltage(core_reg, new_uV, CORE_MAX_VDD * 1000);
	if (ret)
		dev_err(dvfs_dev, "failed to update core voltage\n");
}

static void dvfs_update_rtc_voltage(int new_uV)
{
	int ret;

	dev_dbg(dvfs_dev, "setting RTC voltage %duV -> %duV\n",
		regulator_get_voltage(rtc_reg), new_uV);

	ret = regulator_set_voltage(rtc_reg, new_uV, RTC_MAX_VDD * 1000);
	if (ret)
		dev_err(dvfs_dev, "failed to update RTC voltage\n");
}

#define UPDATE_STEP	150000
static void update_core_vdd(int new_core_vdd)
{
	int current_rtc_vdd = regulator_get_voltage(rtc_reg);
	int current_core_vdd = regulator_get_voltage(core_reg);
	int core_rtc_delta = new_core_vdd - current_rtc_vdd;

	dev_dbg(dvfs_dev, "current_core_vdd=%duV, current_rtc_vdd=%duV, core_rtc_delta=%duV\n",
		current_core_vdd, current_rtc_vdd, core_rtc_delta);

	if (abs(core_rtc_delta) > _170uV) {
		int update_step, steps_nb;

		if (current_rtc_vdd != current_core_vdd)
			dvfs_update_rtc_voltage(current_core_vdd);

		update_step = (core_rtc_delta > 0) ? UPDATE_STEP : -UPDATE_STEP;

		steps_nb = abs(new_core_vdd - current_core_vdd) / UPDATE_STEP;

		dev_dbg(dvfs_dev, "steps_nb = %d\n", steps_nb);

		while (steps_nb--) {
			current_core_vdd += update_step;
			dvfs_update_rtc_voltage(current_core_vdd);
			dvfs_update_core_voltage(current_core_vdd);
		};
	}

	if (current_core_vdd != new_core_vdd)
		dvfs_update_core_voltage(new_core_vdd);
}

static int dvfs_get_cpu_voltage(void)
{
	struct dvfs_client *client;
	int dvfs_cpu_voltage = 0;
	int soc_speedo_id = tegra_sku_info.soc_speedo_id;
	int cpu_process_id = tegra_sku_info.cpu_process_id;

	client = list_first_entry_or_null(&dvfs_cpu.active_clients,
		struct dvfs_client, node);

	// Should only ever be one active client for the single CPU
	BUG_ON(!client || (client->speedo_id != soc_speedo_id &&
		client->process_id != cpu_process_id));

	dvfs_cpu_voltage = max(dvfs_cpu_voltage,
			client->voltages_mv[client->index] * 1000);

	return dvfs_cpu_voltage;
}

static int dvfs_get_core_voltage(void)
{
	struct dvfs_client *client;

	int dvfs_core_voltage = 0;

	list_for_each_entry(client, &dvfs_core.active_clients, node) {
		dev_dbg(dvfs_dev, "active core client %s index = %d freq = %dhz voltage = %dmV\n",
			client->clk_name, client->index, client->freqs[client->index],
			client->voltages_mv[client->index]);
		dvfs_core_voltage = max(dvfs_core_voltage,
			client->voltages_mv[client->index] * 1000);
	}

	return dvfs_core_voltage;
}

/*
 * Rules:
 * 	1) VDD_CORE – VDD_CPU >/= 100mV
 * 	2) VDD_CORE must stay within 170mV of VDD_RTC when VDD_CORE is powered
 */
static void update_voltages(int new_cpu_vdd, int new_core_vdd)
{
	int current_cpu_vdd;

	if (!dvfs_enabled)
		return;

	current_cpu_vdd = regulator_get_voltage(cpu_reg);

	/*
	 * Re-calculate VDD's according to the rules
	 */
	if (new_core_vdd - new_cpu_vdd < _100uV) {
		dev_dbg(dvfs_dev, "new_core_vdd(%d) - new_cpu_vdd(%d) < _100uV",
			new_core_vdd, new_cpu_vdd);
		new_core_vdd = new_cpu_vdd + _100uV;
	}

	/*
	 * What goes update first?
	 */
	if (new_core_vdd - current_cpu_vdd < _100uV) {
		if (current_cpu_vdd != new_cpu_vdd)
			dvfs_update_cpu_voltage(new_cpu_vdd);
		update_core_vdd(new_core_vdd);
	} else {
		update_core_vdd(new_core_vdd);
		if (current_cpu_vdd != new_cpu_vdd)
			dvfs_update_cpu_voltage(new_cpu_vdd);
	}
}

/*
 * Get the lowest suitable voltage index
 */
static void update_freq_index(struct dvfs_client *c, unsigned long freq)
{
	// for (c->index = 0; c->freqs[c->index + 1] >= 0; c->index++)
	// 	if (c->freqs[c->index] * 1000 >= freq)
	// 		break;


	int i = 0;
	while (c->freqs[i + 1] >= 0 && freq > c->freqs[i] * 1000) {
		dev_dbg(dvfs_dev, "%s freq=%lu table_freq=%d i=%d\n",
			c->clk_name, freq, c->freqs[i] * 1000, i);
		i++;
	}

	c->index = i;
}

static int dvfs_cpu_change_notify(struct notifier_block *nb,
				  unsigned long flags, void *data)
{
	struct dvfs_client *client = container_of(nb, struct dvfs_client, nb);
	struct cpufreq_freqs *freqs = data;

	/*
	 * Ignore duplicated notify
	 */
	if (freqs->cpu > 0)
		return NOTIFY_OK;

	switch (flags) {
	case CPUFREQ_PRECHANGE:
		/*
		 * Perform update before going from low to high
		 */
		if (freqs->new > freqs->old) {
			dev_dbg(dvfs_dev, "%s PRE rate change %d -> %d\n",
				client->clk_name, freqs->old, freqs->new);

			mutex_lock(&dvfs_lock);

			update_freq_index(client, freqs->new);
			update_voltages(dvfs_get_cpu_voltage(),
					dvfs_get_core_voltage());

			mutex_unlock(&dvfs_lock);
		}
		break;

	case CPUFREQ_POSTCHANGE:
		/*
		 * Perform update after going from high to low
		 */
		if (freqs->new < freqs->old) {
			dev_dbg(dvfs_dev, "%s POST rate change %d -> %d\n",
				client->clk_name, freqs->old, freqs->new);

			mutex_lock(&dvfs_lock);

			update_freq_index(client, freqs->new);
			update_voltages(dvfs_get_cpu_voltage(),
					dvfs_get_core_voltage());

			mutex_unlock(&dvfs_lock);
		}
	}

	return NOTIFY_OK;
}

/*
 * Perform deferred disable in order to avoid slowdown
 * caused by fast clk enable/disable
 */
static void deferred_disable(struct work_struct *work)
{
	struct dvfs_client *client =
			container_of(work, struct dvfs_client, work.work);

	dev_dbg(dvfs_dev, "deferred %s POST disable change\n",
		client->clk_name);

	mutex_lock(&client->deferred_work_lock);
	mutex_lock(&dvfs_lock);

	list_del_init(&client->node);
	update_voltages(dvfs_get_cpu_voltage(), dvfs_get_core_voltage());

	mutex_unlock(&dvfs_lock);
	mutex_unlock(&client->deferred_work_lock);
}

static int dvfs_core_change_notify(struct notifier_block *nb,
				   unsigned long flags, void *data)
{
	struct dvfs_client *client = container_of(nb, struct dvfs_client, nb);
	struct clk_notifier_data *cnd = data;

	switch (flags) {
	case PRE_RATE_CHANGE:
		/*
		 * Perform update before going from low to high
		 */
		if (cnd->new_rate > cnd->old_rate) {
			dev_dbg(dvfs_dev, "%s PRE rate change %lu -> %lu\n",
				client->clk_name, cnd->old_rate, cnd->new_rate);

			mutex_lock(&dvfs_lock);

			update_freq_index(client, cnd->new_rate);
			update_voltages(dvfs_get_cpu_voltage(),
					dvfs_get_core_voltage());

			mutex_unlock(&dvfs_lock);
		}
		break;

	case POST_RATE_CHANGE:
		/*
		 * Perform update after going from high to low
		 */
		if (cnd->new_rate < cnd->old_rate) {
			dev_dbg(dvfs_dev, "%s POST rate change %lu -> %lu\n",
				client->clk_name, cnd->old_rate, cnd->new_rate);

			mutex_lock(&dvfs_lock);

			update_freq_index(client, cnd->new_rate);
			update_voltages(dvfs_get_cpu_voltage(),
					dvfs_get_core_voltage());

			mutex_unlock(&dvfs_lock);
		}
		break;

	case PRE_ENABLE_CHANGE:
		dev_dbg(dvfs_dev, "%s PRE enable change\n", client->clk_name);

		if (__clk_get_enable_count(client->clk))
			break;

		mutex_lock(&client->deferred_work_lock);

		if (list_empty(&client->node)) {
			mutex_lock(&dvfs_lock);

			list_add(&client->node, &client->dvfs->active_clients);
			update_voltages(dvfs_get_cpu_voltage(),
					dvfs_get_core_voltage());

			mutex_unlock(&dvfs_lock);
		} else {
			dev_dbg(dvfs_dev, "canceled delayed %s POST disable\n",
				client->clk_name);

			cancel_delayed_work(&client->work);
		}

		mutex_unlock(&client->deferred_work_lock);
		break;

	case POST_DISABLE_CHANGE:
		dev_dbg(dvfs_dev, "%s POST disable change\n", client->clk_name);

		if (__clk_get_enable_count(client->clk))
			break;

		mutex_lock(&client->deferred_work_lock);

		if (dvfs_enabled) {
			dev_dbg(dvfs_dev, "scheduled %s POST disable\n",
				client->clk_name);

			schedule_delayed_work(&client->work, HZ / 2);
		} else {
			mutex_lock(&dvfs_lock);

			list_del_init(&client->node);

			mutex_unlock(&dvfs_lock);
		}

		mutex_unlock(&client->deferred_work_lock);
		break;

	case ABORT_RATE_CHANGE:
		dev_warn(dvfs_dev, "FIX ME! %s\n", client->clk_name);
	}

	return NOTIFY_OK;
}

static void dvfs_init(struct dvfs_domain *dvfs,
	int soc_speedo_id, int process_id)
{
	struct dvfs_client *client;
	unsigned long rate;
	int ret;
	int i;

	INIT_LIST_HEAD(&dvfs->active_clients);

	for (i = 0; i < dvfs->nclients; i++) {
		client = &dvfs->clients[i];

		if (client->speedo_id != -1 &&
				client->speedo_id != soc_speedo_id)
			continue;

		if (client->process_id != -1 &&
				client->process_id != process_id)
			continue;

		client->clk = devm_clk_get(dvfs_dev, client->clk_name);
		if (IS_ERR(client->clk)) {
			dev_err(dvfs_dev, "Can't get %s clk\n",
				client->clk_name);

			client->clk = NULL;
			continue;
		}

		client->dvfs = dvfs;
		mutex_init(&client->deferred_work_lock);

		mutex_lock(&client->deferred_work_lock);

		if (dvfs == &dvfs_core) {
			client->nb.notifier_call = dvfs_core_change_notify;
			ret = clk_notifier_register(client->clk, &client->nb);
		} else {
			client->nb.notifier_call = dvfs_cpu_change_notify;
			ret = cpufreq_register_notifier(&client->nb,
						CPUFREQ_TRANSITION_NOTIFIER);
		}

		if (ret) {
			dev_err(dvfs_dev, "Can't register %s clk notifier\n",
				client->clk_name);

			devm_clk_put(dvfs_dev, client->clk);
			client->clk = NULL;
			mutex_unlock(&client->deferred_work_lock);
			continue;
		}

		INIT_DELAYED_WORK(&client->work, deferred_disable);
		INIT_LIST_HEAD(&client->node);

		rate = clk_get_rate(client->clk);

		/*
		 * cpu freqs are defined in MHz while rate is in Hz,
		 * update_freq_index() will compare cpu freqs in Khz.
		 * so convert rate to KHz
		 */
		if (dvfs == &dvfs_cpu)
			rate /= 1000;

		update_freq_index(client, rate);

		if (__clk_get_enable_count(client->clk)) {
			list_add(&client->node, &dvfs->active_clients);

			dev_dbg(dvfs_dev, "%s rate = %luHz index = %d\n",
				client->clk_name, rate, client->index);
		}

		mutex_unlock(&client->deferred_work_lock);
	}
}

static void dvfs_release(struct dvfs_domain *dvfs)
{
	struct dvfs_client *client;
	int i;

	for (i = 0; i < dvfs->nclients; i++) {
		client = &dvfs->clients[i];

		if (client->clk) {
			if (dvfs == &dvfs_core)
				clk_notifier_unregister(client->clk,
							&client->nb);
			else
				cpufreq_unregister_notifier(&client->nb,
						CPUFREQ_TRANSITION_NOTIFIER);

			mutex_destroy(&client->deferred_work_lock);
		}
	}
}

static void dvfs_start_locked(void)
{
	dvfs_enabled = true;
	update_voltages(dvfs_get_cpu_voltage(), dvfs_get_core_voltage());
}

static void dvfs_stop_locked(void)
{
	update_voltages(dvfs_get_cpu_voltage(), dvfs_get_core_voltage());
	dvfs_enabled = false;
}

extern int tegradc_probed;

static int attr_registers_show(struct seq_file *s, void *data)
{
	struct dvfs_client *client;

	seq_printf(s, "CPU voltage = %dmV\n", regulator_get_voltage(cpu_reg)/1000);
	seq_printf(s, "RTC voltage = %dmV\n", regulator_get_voltage(rtc_reg)/1000);
	seq_printf(s, "Core voltage = %dmV\n", regulator_get_voltage(core_reg)/1000);

	seq_printf(s, "Active core clients:\n");
	list_for_each_entry(client, &dvfs_core.active_clients, node) {
		seq_printf(s, "\t%s index = %d freq = %dMhz voltage = %dmV\n",
			client->clk_name, client->index, client->freqs[client->index]/1000,
			client->voltages_mv[client->index]);
	}

	return 0;
}

static int dvfs_open(struct inode *inode, struct file *file)
{
    return single_open(file, attr_registers_show, inode->i_private);
}

static const struct file_operations dvfs_fops = {
    .owner          = THIS_MODULE,
    .open           = dvfs_open,
    .read           = seq_read,
    .llseek         = seq_lseek,
    .release        = single_release,
};

static int tegra_dvfs_probe(struct platform_device *pdev)
{
	int soc_speedo_id = tegra_sku_info.soc_speedo_id;
	int cpu_process_id = tegra_sku_info.cpu_process_id;
	int core_process_id = tegra_sku_info.soc_process_id;

	if (!tegradc_probed) {
		pr_info("%s: defer probe\n", __func__);
		return -EPROBE_DEFER;
	}

	pr_info("%s: soc_speedo_id=%d cpu_process_id=%d core_process_id=%d\n",
		__func__, soc_speedo_id, cpu_process_id, core_process_id);

	dvfs_dev = &pdev->dev;

	cpu_reg = devm_regulator_get(dvfs_dev, "cpu");
	if (IS_ERR(cpu_reg)) {
		dev_err(dvfs_dev, "Can't get CPU regulator\n");
		return -EPROBE_DEFER;
	}

	core_reg = devm_regulator_get(dvfs_dev, "core");
	if (IS_ERR(core_reg)) {
		dev_err(dvfs_dev, "Can't get core regulator\n");
		return -EPROBE_DEFER;
	}

	rtc_reg = devm_regulator_get(dvfs_dev, "rtc");
	if (IS_ERR(rtc_reg)) {
		dev_err(dvfs_dev, "Can't get RTC regulator\n");
		return -EPROBE_DEFER;
	}

	mutex_lock(&dvfs_lock);

	dvfs_init(&dvfs_cpu, soc_speedo_id, cpu_process_id);
	dvfs_init(&dvfs_core, soc_speedo_id, core_process_id);

	dvfs_start_locked();

	mutex_unlock(&dvfs_lock);

	debugfs_file = debugfs_create_file("tegra_dvfs", S_IRUGO, NULL, NULL,
		&dvfs_fops);
	if (!debugfs_file) {
		dev_err(dvfs_dev, "failed to create debugfs file.\n");
		debugfs_file = NULL;
	}

	dev_dbg(dvfs_dev, "registered\n");

	return 0;
}

static void dvfs_lock_all_core_clients(void)
{
	struct dvfs_client *client;
	int i;

	for (i = 0; i < dvfs_core.nclients; i++) {
		client = &dvfs_core.clients[i];

		if (client->clk)
			mutex_lock(&client->deferred_work_lock);
	}
}

static void dvfs_unlock_all_core_clients(void)
{
	struct dvfs_client *client;
	int i;

	for (i = 0; i < dvfs_core.nclients; i++) {
		client = &dvfs_core.clients[i];

		if (client->clk)
			mutex_unlock(&client->deferred_work_lock);
	}
}

static int dvfs_suspend(struct device *dev)
{
	struct dvfs_client *client, *tmp;

	dev_dbg(dvfs_dev, "suspending...\n");

	dvfs_lock_all_core_clients();
	mutex_lock(&dvfs_lock);

	list_for_each_entry_safe(client, tmp, &dvfs_core.active_clients, node) {
		if (work_busy(&client->work.work)) {
			cancel_delayed_work(&client->work);
			list_del_init(&client->node);
		}
	}

	dvfs_stop_locked();

	mutex_unlock(&dvfs_lock);
	dvfs_unlock_all_core_clients();

	return 0;
}

static int dvfs_resume(struct device *dev)
{
	dev_dbg(dvfs_dev, "resuming...\n");

	dvfs_lock_all_core_clients();
	mutex_lock(&dvfs_lock);

	dvfs_start_locked();

	mutex_unlock(&dvfs_lock);
	dvfs_unlock_all_core_clients();

	return 0;
}

static int tegra_dvfs_remove(struct platform_device *pdev)
{
	dvfs_suspend(dvfs_dev);

	dvfs_release(&dvfs_cpu);
	dvfs_release(&dvfs_core);

	if (debugfs_file) {
		debugfs_remove(debugfs_file);
		debugfs_file = NULL;
	}

	return 0;
}

static void tegra_dvfs_shutdown(struct device *dev)
{
	dvfs_suspend(dvfs_dev);
}

static SIMPLE_DEV_PM_OPS(tegra_dvfs_pm_ops, dvfs_suspend, dvfs_resume);

static struct of_device_id tegra_dvfs_of_match[] = {
	{ .compatible = "tegra20-dvfs", },
	{ },
};
MODULE_DEVICE_TABLE(of, tegra_dvfs_of_match);

static struct platform_driver tegra_dvfs_driver = {
	.probe		= tegra_dvfs_probe,
	.remove		= tegra_dvfs_remove,
	.driver		= {
		.name		= "tegra-dvfs",
		.owner		= THIS_MODULE,
		.of_match_table = tegra_dvfs_of_match,
		.pm		= &tegra_dvfs_pm_ops,
		.shutdown	= tegra_dvfs_shutdown,
	},
};

module_platform_driver(tegra_dvfs_driver);

MODULE_AUTHOR("Dmitry Osipenko <digetx@gmail.com>");
MODULE_DESCRIPTION("tegra dvfs driver");
MODULE_LICENSE("GPL");
