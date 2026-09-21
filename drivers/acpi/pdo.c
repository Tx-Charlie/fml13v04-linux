// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  acpi_ac.c - ACPI AC Adapter Driver (Revision: 27)
 *
 *  Copyright (C) 2001, 2002 Andy Grover <andrew.grover@intel.com>
 *  Copyright (C) 2001, 2002 Paul Diefenbaugh <paul.s.diefenbaugh@intel.com>
 */

#define pr_fmt(fmt) "ACPI: PDO: " fmt

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/dmi.h>
#include <linux/delay.h>
#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/acpi.h>
#include <acpi/battery.h>

struct acpi_pdo {
	struct power_supply *pdo;
	struct power_supply_desc pdo_desc;
	struct acpi_device *device;
};

#define to_acpi_pdo(x) power_supply_get_drvdata(x)

/* AC PDO */
static int acpi_ac_get_current(struct acpi_pdo *pdo)
{
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_status status = AE_OK;
	union acpi_object *ret_value;
	union acpi_object *obj;
	int max_current;

	if (!pdo)
		return -EINVAL;

	status = acpi_evaluate_object(pdo->device->handle, "GPDO", NULL,
				      &buffer);
	if (ACPI_FAILURE(status)) {
		acpi_handle_info(pdo->device->handle,
				 "Error reading PDO data: %s\n",
				 acpi_format_exception(status));
		return -EINVAL;
	}

	ret_value = (union acpi_object *)buffer.pointer;
	if (ret_value->type != ACPI_TYPE_PACKAGE)
		max_current = -EINVAL;
	else {
		obj = &ret_value->package.elements[0];
		if (obj->type != ACPI_TYPE_INTEGER)
			max_current = -EINVAL;
		max_current = (int)obj->integer.value * 10 * 1000;
	}
	kfree(buffer.pointer);

	return max_current;
}

static int acpi_ac_get_voltage(struct acpi_pdo *pdo)
{
	struct acpi_buffer buffer = { ACPI_ALLOCATE_BUFFER, NULL };
	acpi_status status = AE_OK;
	union acpi_object *ret_value;
	union acpi_object *obj;
	int max_voltage;

	if (!pdo)
		return -EINVAL;

	status = acpi_evaluate_object(pdo->device->handle, "GPDO", NULL,
				      &buffer);
	if (ACPI_FAILURE(status)) {
		acpi_handle_info(pdo->device->handle,
				 "Error reading PDO data: %s\n",
				 acpi_format_exception(status));
		return -EINVAL;
	}

	ret_value = (union acpi_object *)buffer.pointer;
	if (ret_value->type != ACPI_TYPE_PACKAGE)
		max_voltage = -EINVAL;
	else {
		obj = &ret_value->package.elements[1];
		if (obj->type != ACPI_TYPE_INTEGER)
			max_voltage = -EINVAL;
		max_voltage = (int)obj->integer.value * 50 * 1000;
	}
	kfree(buffer.pointer);

	return max_voltage;
}

/* sysfs I/F */
static int get_pdo_property(struct power_supply *psy,
			    enum power_supply_property psp,
			    union power_supply_propval *val)
{
	struct acpi_pdo *pdo = to_acpi_pdo(psy);

	if (!pdo)
		return -ENODEV;

	switch (psp) {
	case POWER_SUPPLY_PROP_VOLTAGE_NOW:
		val->intval = acpi_ac_get_voltage(pdo);
		break;
	case POWER_SUPPLY_PROP_CURRENT_NOW:
		val->intval = acpi_ac_get_current(pdo);
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static enum power_supply_property pdo_props[] = {
	POWER_SUPPLY_PROP_VOLTAGE_NOW,
	POWER_SUPPLY_PROP_CURRENT_NOW,
};

static int acpi_pdo_probe(struct platform_device *pdev)
{
	struct acpi_device *device = ACPI_COMPANION(&pdev->dev);
	struct power_supply_config psy_cfg = {};
	struct acpi_pdo *apdo;
	int result = 0;

	if (!device)
		return -ENODEV;

	apdo = devm_kzalloc(&pdev->dev, sizeof(*apdo), GFP_KERNEL);
	if (!apdo)
		return -ENOMEM;

	apdo->device = device;
	platform_set_drvdata(pdev, apdo);

	psy_cfg.drv_data = apdo;

	apdo->pdo_desc.name = acpi_device_bid(device);
	apdo->pdo_desc.type = POWER_SUPPLY_TYPE_MAINS;
	apdo->pdo_desc.properties = pdo_props;
	apdo->pdo_desc.num_properties = ARRAY_SIZE(pdo_props);
	apdo->pdo_desc.get_property = get_pdo_property;
	apdo->pdo = power_supply_register(&pdev->dev, &apdo->pdo_desc,
					  &psy_cfg);
	if (IS_ERR(apdo->pdo)) {
		result = PTR_ERR(apdo->pdo);
		return result;
	}

	pr_info("%s [%s] \n", acpi_dev_name(device),
		acpi_device_bid(device));

	return 0;

}

static void acpi_pdo_remove(struct platform_device *pdev)
{
	struct acpi_pdo *apdo = platform_get_drvdata(pdev);

	if (!apdo)
		return;

	power_supply_unregister(apdo->pdo);
}

static const struct acpi_device_id pdo_device_ids[] = {
	{"CIXHA032", 0},
	{"", 0},
};
MODULE_DEVICE_TABLE(acpi, pdo_device_ids);

static struct platform_driver acpi_pdo_driver = {
	.probe = acpi_pdo_probe,
	.remove = acpi_pdo_remove,
	.driver = {
		.name = "pdo",
		.acpi_match_table = ACPI_PTR(pdo_device_ids),
	},
};
module_platform_driver(acpi_pdo_driver);
