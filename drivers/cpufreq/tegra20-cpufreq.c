/*
 * Copyright (C) 2010 Google, Inc.
 *
 * Author:
 *	Colin Cross <ccross@google.com>
 *	Based on arch/arm/plat-omap/cpu-omap.c, (C) 2005 Nokia Corporation
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

#include <linux/clk.h>
#include <linux/cpu.h>
#include <linux/cpu_cooling.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm_opp.h>
#include <linux/regulator/consumer.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <soc/tegra/fuse.h>

#define PLLX_PREPARE		BIT(0)
#define PLLX_PREPARED		BIT(1)

struct tegra_cpufreq {
	struct device *dev;
	struct device *cpu_dev;
	struct regulator *reg_cpu;
	struct opp_table *opp_table;
	struct cpufreq_driver driver;
	struct thermal_cooling_device *cdev;
	struct cpufreq_frequency_table *freq_table;
	struct clk *cpu_clk;
	struct clk *pll_x_clk;
	struct clk *intermediate_clk;
	unsigned long intermediate_rate;
	unsigned int state;

	/* deferred voltage change */
	struct delayed_work work;
	struct dev_pm_opp_supply supply_cpu;
	unsigned long actual_cpu_uV;
};

/*
 * Voltage changing usually introduces quite huge latency which is not
 * taken into account by the governor, hence we will simply avoid the
 * frequent voltage changes by deferring the voltage dropping.
 */
static unsigned int voltage_drop_interval_ms = 1000;

static unsigned int tegra_get_intermediate(struct cpufreq_policy *policy,
					   unsigned int index)
{
	struct tegra_cpufreq *cpufreq = cpufreq_get_driver_data();
	struct clk *cpu_parent = clk_get_parent(cpufreq->cpu_clk);
	unsigned long new_rate = cpufreq->freq_table[index].frequency * 1000;
	int err;

	/*
	 * Make sure that intermediate clock rate stays consistent during
	 * transition by entering into critical section of the intermediate
	 * clock.
	 */
	err = clk_rate_exclusive_get(cpufreq->intermediate_clk);
	/* this shouldn't fail */
	WARN_ON_ONCE(err);

	/*
	 * When target rate is equal to intermediate rate, we don't need to
	 * switch to intermediate clock and so the intermediate routine isn't
	 * called. Also, we wouldn't be using PLLX anymore and must not
	 * take extra reference to it, as it can be disabled to save some
	 * power.
	 */
	cpufreq->intermediate_rate = clk_get_rate(cpufreq->intermediate_clk);

	if (new_rate == cpufreq->intermediate_rate)
		cpufreq->state &= ~PLLX_PREPARE;
	else
		cpufreq->state |= PLLX_PREPARE;

	/* don't switch to intermediate freq if we are already at it */
	if (clk_is_match(cpu_parent, cpufreq->intermediate_clk))
		return 0;

	return cpufreq->intermediate_rate / 1000;
}

static int tegra_target_intermediate(struct cpufreq_policy *policy,
				     unsigned int index)
{
	struct tegra_cpufreq *cpufreq = cpufreq_get_driver_data();
	unsigned int state = cpufreq->state;
	int err;

	/*
	 * Take an extra reference to the main PLLX so it doesn't turn off
	 * when we move the CPU clock to intermediate clock as enabling it
	 * again  while we switch to it from tegra_target() would take
	 * additional time.
	 */
	if ((state & (PLLX_PREPARED | PLLX_PREPARE)) == PLLX_PREPARE) {
		err = clk_prepare_enable(cpufreq->pll_x_clk);
		if (WARN_ON_ONCE(err))
			goto err_exclusive_put;

		cpufreq->state |= PLLX_PREPARED;
	}

	err = clk_set_parent(cpufreq->cpu_clk, cpufreq->intermediate_clk);
	if (WARN_ON_ONCE(err))
		goto err_exclusive_put;

	return 0;

err_exclusive_put:
	clk_rate_exclusive_put(cpufreq->intermediate_clk);

	if (cpufreq->state & PLLX_PREPARED) {
		clk_disable_unprepare(cpufreq->pll_x_clk);
		cpufreq->state &= ~PLLX_PREPARED;
	}

	return err;
}

static int tegra_target(struct cpufreq_policy *policy, unsigned int index)
{
	struct tegra_cpufreq *cpufreq = cpufreq_get_driver_data();
	unsigned long new_rate = cpufreq->freq_table[index].frequency * 1000;
	unsigned int state = cpufreq->state;
	int ret;

	/*
	 * Drop refcount to PLLX only if we switched to intermediate clock
	 * earlier during transitioning to a target frequency and we are going
	 * to stay with the intermediate clock.
	 */
	if ((state & (PLLX_PREPARED | PLLX_PREPARE)) == PLLX_PREPARED) {
		clk_disable_unprepare(cpufreq->pll_x_clk);
		state &= ~PLLX_PREPARED;
	}

	/*
	 * Switch to new OPP, note that this will change PLLX rate and
	 * not the CCLK.
	 */
	ret = dev_pm_opp_set_rate(cpufreq->cpu_dev, new_rate);
	if (ret)
		goto exclusive_put;

	/*
	 * Target rate == intermediate rate leaves PLLX turned off, CPU is
	 * kept running off the intermediate clock. This should save us some
	 * power by keeping one more PLL disabled because the intermediate
	 * clock assumed to be always-on. In this case PLLX_PREPARE flag will
	 * be omitted.
	 */
	if (state & PLLX_PREPARE) {
		/*
		 * CCF doesn't return error if clock-enabling fails on
		 * re-parent, hence enable it now.
		 */
		ret = clk_prepare_enable(cpufreq->pll_x_clk);
		if (WARN_ON_ONCE(ret))
			goto exclusive_put;

		ret = clk_set_parent(cpufreq->cpu_clk, cpufreq->pll_x_clk);

		clk_disable_unprepare(cpufreq->pll_x_clk);
	}

	/*
	 * Drop refcount to PLLX only if we switched to intermediate clock
	 * earlier during transitioning to a target frequency.
	 */
	if (state & PLLX_PREPARED) {
		clk_disable_unprepare(cpufreq->pll_x_clk);
		state &= ~PLLX_PREPARED;
	}

exclusive_put:
	clk_rate_exclusive_put(cpufreq->intermediate_clk);

	cpufreq->state = state;

	return ret;
}

static int tegra_cpu_opp_set_cpu_voltage(struct tegra_cpufreq *cpufreq,
					 struct dev_pm_opp_supply *supply)
{
	int ret;

	ret = regulator_set_voltage_triplet(cpufreq->reg_cpu,
					    supply->u_volt_min,
					    supply->u_volt,
					    supply->u_volt_max);
	if (ret)
		dev_err_ratelimited(cpufreq->dev,
			"failed to set cpu voltage (%lu %lu %lu): %d\n",
			supply->u_volt_min, supply->u_volt,
			supply->u_volt_max, ret);

	return ret;
}

static void tegra_cpu_deferred_voltage_drop(struct work_struct *work)
{
	struct tegra_cpufreq *cpufreq = container_of(work, struct tegra_cpufreq,
						     work.work);
	int err;

	err = tegra_cpu_opp_set_cpu_voltage(cpufreq, &cpufreq->supply_cpu);
	if (err)
		return;

	cpufreq->actual_cpu_uV = cpufreq->supply_cpu.u_volt;
}

static int tegra_cpu_opp_raise_voltage(struct tegra_cpufreq *cpufreq,
				       struct dev_pm_opp_supply *supply_cpu)
{
	int err;

	err = tegra_cpu_opp_set_cpu_voltage(cpufreq, supply_cpu);
	if (err)
		return err;

	cpufreq->actual_cpu_uV = supply_cpu->u_volt;

	return 0;
}

static void tegra_cpu_opp_schedule_voltage_drop(
					struct tegra_cpufreq *cpufreq,
					struct dev_pm_opp_supply *supply_cpu)
{
	cpufreq->supply_cpu = *supply_cpu;

	schedule_delayed_work(&cpufreq->work,
			      msecs_to_jiffies(voltage_drop_interval_ms));
}

static int tegra_cpu_set_opp(struct dev_pm_set_opp_data *data)
{
	struct tegra_cpufreq *cpufreq = cpufreq_get_driver_data();
	struct dev_pm_opp_supply *supply_cpu;
	int err;

	cancel_delayed_work_sync(&cpufreq->work);

	supply_cpu = &data->new_opp.supplies[0];
	cpufreq->reg_cpu = data->regulators[0];

	/* Scaling up? Scale voltage before frequency */
	if (data->old_opp.rate < data->new_opp.rate) {
		if (cpufreq->actual_cpu_uV < supply_cpu->u_volt) {
			err = tegra_cpu_opp_raise_voltage(cpufreq, supply_cpu);
			if (err)
				return err;
		} else {
			tegra_cpu_opp_schedule_voltage_drop(cpufreq,
							    supply_cpu);
		}
	}

	err = clk_set_rate(data->clk, data->new_opp.rate);
	if (err) {
		dev_err(cpufreq->dev, "failed to change pllx clock rate: %d\n",
			err);
		return err;
	}

	if (data->old_opp.rate > data->new_opp.rate)
		tegra_cpu_opp_schedule_voltage_drop(cpufreq, supply_cpu);

	return 0;
}

static int tegra_cpu_setup_opp(struct tegra_cpufreq *cpufreq)
{
	const char * const regulators[] = { "cpu" };
	struct device *dev = cpufreq->cpu_dev;
	struct opp_table *opp_table;
	u32 versions[2];
	int err;

	if (of_machine_is_compatible("nvidia,tegra20")) {
		versions[0] = BIT(tegra_sku_info.cpu_process_id);
		versions[1] = BIT(tegra_sku_info.soc_speedo_id);
	} else {
		versions[0] = BIT(tegra_sku_info.cpu_process_id);
		versions[1] = BIT(tegra_sku_info.cpu_speedo_id);
	}

	cpufreq->opp_table = dev_pm_opp_set_supported_hw(dev, versions, 2);
	if (IS_ERR(cpufreq->opp_table)) {
		err = PTR_ERR(cpufreq->opp_table);
		dev_err(cpufreq->dev,
			"failed to setup opp supported hw: %d\n", err);
		return err;
	}

	opp_table = dev_pm_opp_set_regulators(dev, regulators, 1, true);
	if (IS_ERR(opp_table)) {
		err = PTR_ERR(opp_table);
		dev_err(dev, "failed to setup opp regulators: %d\n", err);
		goto err_put_supported_hw;
	}

	opp_table = dev_pm_opp_set_clkname(dev, "pll_x");
	if (IS_ERR(opp_table)) {
		err = PTR_ERR(opp_table);
		dev_err(cpufreq->dev, "failed to change opp clk: %d\n", err);
		goto err_put_regulators;
	}

	opp_table = dev_pm_opp_register_set_opp_helper(dev, tegra_cpu_set_opp);
	if (IS_ERR(opp_table)) {
		err = PTR_ERR(opp_table);
		dev_err(cpufreq->dev, "failed to set opp helper: %d\n", err);
		goto err_put_regulators;
	}

	err = dev_pm_opp_of_cpumask_add_table(cpu_possible_mask);
	if (err) {
		dev_err(cpufreq->dev, "failed to add opp table: %d\n", err);
		goto err_unregister_opp_helper;
	}

	err = dev_pm_opp_init_cpufreq_table(dev, &cpufreq->freq_table);
	if (err) {
		dev_err(cpufreq->dev,
			"failed to initialize opp table: %d\n", err);
		goto err_remove_table;
	}

	return 0;

err_remove_table:
	dev_pm_opp_of_cpumask_remove_table(cpu_possible_mask);

err_unregister_opp_helper:
	dev_pm_opp_unregister_set_opp_helper(cpufreq->opp_table);

err_put_regulators:
	dev_pm_opp_put_regulators(cpufreq->opp_table);

err_put_supported_hw:
	dev_pm_opp_put_supported_hw(cpufreq->opp_table);

	return err;
}

static void tegra_cpu_release_opp(struct tegra_cpufreq *cpufreq)
{
	dev_pm_opp_free_cpufreq_table(cpufreq->cpu_dev, &cpufreq->freq_table);
	dev_pm_opp_of_cpumask_remove_table(cpu_possible_mask);
	dev_pm_opp_unregister_set_opp_helper(cpufreq->opp_table);
	dev_pm_opp_put_regulators(cpufreq->opp_table);
	dev_pm_opp_put_supported_hw(cpufreq->opp_table);
}

static int tegra_cpu_init_clocks(struct tegra_cpufreq *cpufreq)
{
	unsigned long intermediate_rate;
	int ret;

	ret = clk_rate_exclusive_get(cpufreq->intermediate_clk);
	if (ret) {
		dev_err(cpufreq->dev,
			"failed to make intermediate clock exclusive: %d\n",
			ret);
		goto err_exclusive_put_intermediate;
	}

	ret = clk_set_parent(cpufreq->cpu_clk, cpufreq->intermediate_clk);
	if (ret) {
		dev_err(cpufreq->dev,
			"failed to switch cpu to intermediate clock: %d\n",
			ret);
		goto err_exclusive_put_intermediate;
	}

	intermediate_rate = clk_get_rate(cpufreq->intermediate_clk);

	/*
	 * The CCLK has its own clock divider, that divider isn't getting
	 * disabled on clock reparent. Hence set CCLK parent to intermediate
	 * clock in order to disable the divider if it happens to be enabled,
	 * otherwise clk_set_rate() has no effect.
	 */
	ret = clk_set_rate(cpufreq->cpu_clk, intermediate_rate);
	if (ret) {
		dev_err(cpufreq->dev,
			"failed to change cpu clock rate: %d\n", ret);
		goto err_exclusive_put_intermediate;
	}

err_exclusive_put_intermediate:
	clk_rate_exclusive_put(cpufreq->intermediate_clk);

	return ret;
}

static int tegra_cpu_get_clocks(struct tegra_cpufreq *cpufreq)
{
	struct device_node *np = cpufreq->cpu_dev->of_node;
	int err;

	cpufreq->cpu_clk = devm_get_clk_from_child(cpufreq->dev, np, NULL);
	if (IS_ERR(cpufreq->cpu_clk)) {
		err = PTR_ERR(cpufreq->cpu_clk);
		dev_err(cpufreq->dev, "failed to get cpu clock: %d\n", err);
		dev_err(cpufreq->dev, "please update your device tree\n");
		return err;
	}

	cpufreq->pll_x_clk = devm_clk_get(cpufreq->dev, "pll_x");
	if (IS_ERR(cpufreq->pll_x_clk)) {
		err = PTR_ERR(cpufreq->pll_x_clk);
		dev_err(cpufreq->dev, "failed to get pllx clock: %d\n", err);
		return err;
	}

	cpufreq->intermediate_clk = devm_clk_get(cpufreq->dev, "pll_p");
	if (IS_ERR(cpufreq->intermediate_clk)) {
		err = PTR_ERR(cpufreq->intermediate_clk);
		dev_err(cpufreq->dev, "failed to get intermediate clock: %d\n",
			err);
		return err;
	}

	return 0;
}

static int tegra_get_cpu_resources(struct tegra_cpufreq *cpufreq)
{
	int err;

	err = tegra_cpu_get_clocks(cpufreq);
	if (err)
		return err;

	err = tegra_cpu_init_clocks(cpufreq);
	if (err)
		return err;

	return 0;
}

static int tegra_cpu_init(struct cpufreq_policy *policy)
{
	struct tegra_cpufreq *cpufreq = cpufreq_get_driver_data();
	struct device *cpu = cpufreq->cpu_dev;
	int err;

	err = tegra_cpu_setup_opp(cpufreq);
	if (err)
		return err;

	err = cpufreq_generic_init(policy, cpufreq->freq_table,
				   dev_pm_opp_get_max_clock_latency(cpu));
	if (err)
		goto err_release_opp;

	policy->clk = cpufreq->cpu_clk;
	policy->suspend_freq = dev_pm_opp_get_suspend_opp_freq(cpu) / 1000;

	return 0;

err_release_opp:
	tegra_cpu_release_opp(cpufreq);

	return err;
}

static int tegra_cpu_exit(struct cpufreq_policy *policy)
{
	struct tegra_cpufreq *cpufreq = cpufreq_get_driver_data();

	flush_delayed_work(&cpufreq->work);
	cpufreq_cooling_unregister(cpufreq->cdev);
	tegra_cpu_release_opp(cpufreq);

	return 0;
}

static void tegra_cpu_ready(struct cpufreq_policy *policy)
{
	struct tegra_cpufreq *cpufreq = cpufreq_get_driver_data();

	cpufreq->cdev = of_cpufreq_cooling_register(policy);
}

static int tegra_cpu_suspend(struct cpufreq_policy *policy)
{
	struct tegra_cpufreq *cpufreq = cpufreq_get_driver_data();
	int err;

	err = cpufreq_generic_suspend(policy);
	if (err)
		return err;

	flush_delayed_work(&cpufreq->work);

	return 0;
}

static int tegra_cpufreq_probe(struct platform_device *pdev)
{
	struct tegra_cpufreq *cpufreq;
	int err;

	cpufreq = devm_kzalloc(&pdev->dev, sizeof(*cpufreq), GFP_KERNEL);
	if (!cpufreq)
		return -ENOMEM;

	cpufreq->dev = &pdev->dev;
	cpufreq->cpu_dev = get_cpu_device(0);
	cpufreq->driver.get = cpufreq_generic_get;
	cpufreq->driver.attr = cpufreq_generic_attr;
	cpufreq->driver.init = tegra_cpu_init;
	cpufreq->driver.exit = tegra_cpu_exit;
	cpufreq->driver.ready = tegra_cpu_ready;
	cpufreq->driver.flags = CPUFREQ_NEED_INITIAL_FREQ_CHECK;
	cpufreq->driver.verify = cpufreq_generic_frequency_table_verify;
	cpufreq->driver.suspend = tegra_cpu_suspend;
	cpufreq->driver.driver_data = cpufreq;
	cpufreq->driver.target_index = tegra_target;
	cpufreq->driver.get_intermediate = tegra_get_intermediate;
	cpufreq->driver.target_intermediate = tegra_target_intermediate;
	snprintf(cpufreq->driver.name, CPUFREQ_NAME_LEN, "tegra");
	INIT_DELAYED_WORK(&cpufreq->work, tegra_cpu_deferred_voltage_drop);

	err = tegra_get_cpu_resources(cpufreq);
	if (err)
		return err;

	err = cpufreq_register_driver(&cpufreq->driver);
	if (err)
		return err;

	platform_set_drvdata(pdev, cpufreq);

	return 0;
}

static int tegra_cpufreq_remove(struct platform_device *pdev)
{
	struct tegra_cpufreq *cpufreq = platform_get_drvdata(pdev);

	cpufreq_unregister_driver(&cpufreq->driver);

	return 0;
}

static struct platform_driver tegra_cpufreq_driver = {
	.probe		= tegra_cpufreq_probe,
	.remove		= tegra_cpufreq_remove,
	.driver		= {
		.name	= "tegra20-cpufreq",
	},
};
module_platform_driver(tegra_cpufreq_driver);
module_param(voltage_drop_interval_ms, uint, 0644);

MODULE_ALIAS("platform:tegra20-cpufreq");
MODULE_AUTHOR("Colin Cross <ccross@android.com>");
MODULE_AUTHOR("Dmitry Osipenko <digetx@gmail.com>");
MODULE_DESCRIPTION("NVIDIA Tegra20 cpufreq driver");
MODULE_LICENSE("GPL");
