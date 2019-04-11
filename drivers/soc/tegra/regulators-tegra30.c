/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Voltage regulators coupling resolver for NVIDIA Tegra30
 *
 * Author: Dmitry Osipenko <digetx@gmail.com>
 */

#define pr_fmt(fmt)	"tegra voltage-coupler: " fmt

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/regulator/driver.h>
#include <linux/regulator/machine.h>

#include <soc/tegra/fuse.h>

struct tegra_regulators_coupler {
	struct regulators_coupler coupler;
	int core_min_uV;
};

static const char *cpu_names[] = {
	"vdd_cpu,vdd_sys",
	"+V1.0_VDD_CPU",
	"vdd_cpu",
};

static const char *core_names[] = {
	"tps62361-vout",
	"tps62362-vout",
	"vdd_core",
};

static inline struct tegra_regulators_coupler *
to_tegra_coupler(struct regulators_coupler *coupler)
{
	return container_of(coupler, struct tegra_regulators_coupler, coupler);
}

static int tegra30_core_limit(struct tegra_regulators_coupler *tegra,
			      struct regulator_dev *core_rdev)
{
	if (tegra->core_min_uV > 0)
		return tegra->core_min_uV;

	tegra->core_min_uV = regulator_get_voltage_rdev(core_rdev);
	if (tegra->core_min_uV > 0)
		pr_info("core minimum voltage limited to %duV\n",
			tegra->core_min_uV);

	return tegra->core_min_uV;
}

static int tegra30_core_cpu_limit(int cpu_uV)
{
	if (cpu_uV < 800000)
		return 950000;

	if (cpu_uV < 900000)
		return 1000000;

	if (cpu_uV < 1000000)
		return 1100000;

	if (cpu_uV < 1100000)
		return 1200000;

	if (cpu_uV < 1250000) {
		switch (tegra_sku_info.cpu_speedo_id) {
		case 0 ... 1:
		case 4:
		case 7:
		case 8:
			return 1200000;

		default:
			return 1300000;
		}
	}

	return -EINVAL;
}

static int tegra30_cpu_core_limit(int core_uV)
{
	if (core_uV < 950000)
		return 800000;

	if (core_uV < 1000000)
		return 900000;

	if (core_uV < 1100000)
		return 1000000;

	if (core_uV < 1200000)
		return 1100000;

	return INT_MAX;
}

static int tegra30_voltage_update(struct tegra_regulators_coupler *tegra,
				  struct regulator_dev *cpu_rdev,
				  struct regulator_dev *core_rdev)
{
	int cpu_min_uV, cpu_max_uV = cpu_rdev->constraints->max_uV;
	int core_min_uV, core_max_uV = core_rdev->constraints->max_uV;
	int core_min_limited_uV;
	int core_target_uV;
	int cpu_target_uV;
	int core_uV;
	int cpu_uV;
	int err;

	/*
	 * The CORE voltage scaling is currently not hooked up in drivers,
	 * hence we will limit the minimum CORE voltage to the initial value.
	 * This should be good enough for the time being.
	 */
	core_min_uV = tegra30_core_limit(tegra, core_rdev);
	if (core_min_uV < 0)
		return core_min_uV;

	err = regulator_check_consumers(core_rdev, &core_min_uV, &core_max_uV,
					PM_SUSPEND_ON);
	if (err)
		return err;

	cpu_min_uV = core_min_uV - 300000;

	err = regulator_check_consumers(cpu_rdev, &cpu_min_uV, &cpu_max_uV,
					PM_SUSPEND_ON);
	if (err)
		return err;

	cpu_uV = regulator_get_voltage_rdev(cpu_rdev);
	if (cpu_uV < 0)
		return cpu_uV;

	core_uV = regulator_get_voltage_rdev(core_rdev);
	if (core_uV < 0)
		return core_uV;

	while (cpu_uV != cpu_min_uV) {
		if (cpu_uV < cpu_min_uV) {
			cpu_target_uV = min(cpu_uV + 100000, cpu_min_uV);
			cpu_target_uV = min(tegra30_cpu_core_limit(core_uV),
					    cpu_target_uV);
		} else {
			cpu_target_uV = max(cpu_uV - 100000, cpu_min_uV);
			cpu_target_uV = max(core_uV - 300000, cpu_target_uV);
		}

		err = regulator_set_voltage_rdev(cpu_rdev,
						 cpu_target_uV,
						 cpu_max_uV,
						 PM_SUSPEND_ON);
		if (err)
			return err;

		cpu_uV = cpu_target_uV;

		core_min_limited_uV = tegra30_core_cpu_limit(cpu_uV);
		if (core_min_limited_uV < 0)
			return core_min_limited_uV;

		core_target_uV = max(core_min_limited_uV, core_min_uV);

		err = regulator_set_voltage_rdev(core_rdev,
						 core_target_uV,
						 core_max_uV,
						 PM_SUSPEND_ON);
		if (err)
			return err;

		core_uV = core_target_uV;
	}

	return 0;
}

static struct regulator_dev *lookup_rdev(struct regulator_dev *rdev,
					 const char **names,
					 unsigned int num_names)
{
	struct coupling_desc *c_desc = &rdev->coupling_desc;
	unsigned int i, k;

	for (i = 0; i < num_names; i++) {
		if (!strcmp(names[i], rdev_get_name(rdev)))
			return rdev;
	}

	for (k = 0; k < c_desc->n_coupled; k++) {
		rdev = c_desc->coupled_rdevs[k];

		for (i = 0; i < num_names; i++) {
			if (!strcmp(names[i], rdev_get_name(rdev)))
				return rdev;
		}
	}

	pr_err_once("%s: failed for %s\n", __func__, rdev_get_name(rdev));

	for (i = 0; i < num_names; i++)
		pr_err_once("%s: entry%u: %s\n", __func__, i, names[i]);

	return NULL;
}

static int tegra30_regulator_balance_voltage(struct regulators_coupler *coupler,
					     struct regulator_dev *rdev,
					     suspend_state_t state)
{
	struct tegra_regulators_coupler *tegra = to_tegra_coupler(coupler);
	struct regulator_dev *core_rdev;
	struct regulator_dev *cpu_rdev;

	core_rdev = lookup_rdev(rdev, core_names, ARRAY_SIZE(core_names));
	cpu_rdev  = lookup_rdev(rdev, cpu_names, ARRAY_SIZE(cpu_names));

	if (!core_rdev || !cpu_rdev || state != PM_SUSPEND_ON) {
		pr_err_once("regulators are not coupled properly\n");
		return -EINVAL;
	}

	return tegra30_voltage_update(tegra, cpu_rdev, core_rdev);
}

static struct tegra_regulators_coupler tegra30_coupler = {
	.coupler = {
		.balance_voltage = tegra30_regulator_balance_voltage,
	},
};

static int __init tegra_regulators_coupler_init(void)
{
	if (!of_machine_is_compatible("nvidia,tegra30"))
		return 0;

	return regulators_coupler_register(&tegra30_coupler.coupler);
}
arch_initcall(tegra_regulators_coupler_init);
