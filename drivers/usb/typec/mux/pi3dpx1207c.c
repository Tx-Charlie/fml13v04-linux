// SPDX-License-Identifier: GPL-2.0+
/*
 * Pericom PI3DPX1207C Type-C cross switch / mux driver
 *
 * Copyright (c) 2023 Dillon Min <dillon.min@nio.com>
 *
 * based on:
 * Pericom PI3USB30532 Type-C cross switch / mux driver
 * Copyright (c) 2017-2018 Hans de Goede <hdegoede@redhat.com>
 */

#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/usb/typec_dp.h>
#include <linux/usb/typec_mux.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/delay.h>

#include <linux/gpio.h>
#ifdef CONFIG_OF
#include <linux/of_gpio.h>
#endif

#define PI3DPX1207C_CONF			0x03

#define PI3DPX1207C_CONF_OPEN			0x02
#define PI3DPX1207C_CONF_SWAP			0x10
#define PI3DPX1207C_CONF_4LANE_DP		0x22
#define PI3DPX1207C_CONF_USB3			0x42
#define PI3DPX1207C_CONF_USB3_AND_2LANE_DP	0x62
#define PI3DPX1207C_CONF_4LANE_DP_FLIP_AUX_NOT_FLIP  0x22
#define PI3DPX1207C_CONF_4LANE_DP_FLIP_AUX_FLIP  0x32
#define PI3DPX1207C_CONF_USB3_FLIP  0x52
#define PI3DPX1207C_CONF_USB3_AND_2LANE_DP_FILP  0x72
#define PI3DPX1207C_CONF_USB3_AND_2LANE_DP_FILP_AUX_NOT_FILP  0x82

struct pi3dpx1207c {
	struct i2c_client *client;
	struct mutex lock; /* protects the cached conf register */
	struct typec_switch_dev *sw;
	struct typec_mux_dev *mux;
	u8 need_aux_flip;
	enum typec_orientation orientation;
	u8 conf;
	unsigned int en_pin;
	u8 force_hpd;
};

static int pi3dpx1207c_set_conf(struct pi3dpx1207c *pi, u8 new_conf)
{
	int ret = 0;
	u8 write_conf[5] = {0};

	dev_err(&pi->client->dev, "%s: %d -> %x\n", __func__, __LINE__, new_conf);

	if (pi->conf == new_conf)
		return 0;


	write_conf[0] = 0x13;
	write_conf[1] = 0x11;
	write_conf[2] = 0x20;
	write_conf[3] = new_conf;

	if(pi->force_hpd) {
		write_conf[4] = 0x06;
	} else {
		write_conf[4] = 0;
	}

	ret = i2c_master_send(pi->client, write_conf, 5);

	if (ret < 0) {
		dev_err(&pi->client->dev, "Error writing conf: %d\n", ret);
		return ret;
	}

	pi->conf = new_conf;
	return 0;
}

static int pi3dpx1207c_sw_set(struct typec_switch_dev *sw,
			      enum typec_orientation orientation)
{
	struct pi3dpx1207c *pi = typec_switch_get_drvdata(sw);
	u8 new_conf;
	int ret;

	mutex_lock(&pi->lock);
	new_conf = pi->conf;

	switch (orientation) {
	case TYPEC_ORIENTATION_NONE:
		new_conf = PI3DPX1207C_CONF_OPEN;
		break;
	case TYPEC_ORIENTATION_NORMAL:
		new_conf &= ~PI3DPX1207C_CONF_SWAP;
		break;
	case TYPEC_ORIENTATION_REVERSE:
		new_conf |= PI3DPX1207C_CONF_SWAP;
		break;
	}

	pi->orientation = orientation;
	ret = pi3dpx1207c_set_conf(pi, new_conf);
	mutex_unlock(&pi->lock);

	return ret;
}

static int
pi3dpx1207c_mux_set(struct typec_mux_dev *mux, struct typec_mux_state *state)
{
	struct pi3dpx1207c *pi = typec_mux_get_drvdata(mux);
	u8 new_conf;
	int ret;

	mutex_lock(&pi->lock);
	new_conf = pi->conf;
	switch (state->mode) {
	case TYPEC_STATE_SAFE:
		new_conf = (new_conf & PI3DPX1207C_CONF_SWAP) |
			   PI3DPX1207C_CONF_OPEN;
		break;
	case TYPEC_STATE_USB:
		new_conf = (new_conf & PI3DPX1207C_CONF_SWAP) |
			   PI3DPX1207C_CONF_USB3;
		break;
	case TYPEC_DP_STATE_C:
	case TYPEC_DP_STATE_E:
		new_conf = (new_conf & PI3DPX1207C_CONF_SWAP) |
			   PI3DPX1207C_CONF_4LANE_DP;
		break;
	case TYPEC_DP_STATE_D:
		new_conf = (new_conf & PI3DPX1207C_CONF_SWAP) |
			   PI3DPX1207C_CONF_USB3_AND_2LANE_DP;
		break;
	default:
		break;
	}

	if (!pi->need_aux_flip) {
		if (pi->orientation == TYPEC_ORIENTATION_REVERSE) {
			switch (state->mode) {
			case TYPEC_DP_STATE_C:
			case TYPEC_DP_STATE_E:
				new_conf = PI3DPX1207C_CONF_4LANE_DP_FLIP_AUX_NOT_FLIP;
				break;
			case TYPEC_DP_STATE_D:
				new_conf = PI3DPX1207C_CONF_USB3_AND_2LANE_DP_FILP_AUX_NOT_FILP;
				break;
			default:
				break;
			}
		}
	}
	ret = pi3dpx1207c_set_conf(pi, new_conf);
	mutex_unlock(&pi->lock);

	return ret;
}

static ssize_t get_conf(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct pi3dpx1207c *pi = (struct pi3dpx1207c *)dev_get_drvdata(dev);
	u8 read_conf[17] = {0};
	int ret;

	ret = i2c_smbus_read_i2c_block_data(pi->client, PI3DPX1207C_CONF, 17, read_conf);
	if (ret < 0) {
		dev_err(dev, "Error reading config register %d\n", ret);
		return ret;
	} else {
		dev_err(&pi->client->dev, "read_conf = %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x %x", read_conf[0], read_conf[1], read_conf[2], read_conf[3],
		read_conf[4], read_conf[5], read_conf[6], read_conf[7],
		read_conf[8], read_conf[9], read_conf[10], read_conf[11],
		read_conf[12], read_conf[13], read_conf[14], read_conf[15],read_conf[16]);
	}

	return sprintf(buf, "%x\n", pi->conf);
}

static ssize_t set_conf(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	struct pi3dpx1207c *pi = (struct pi3dpx1207c *)dev_get_drvdata(dev);
	u8 value = PI3DPX1207C_CONF_OPEN;
	int ret;

	ret = kstrtou8(buf, 16, &value);

	if (ret)
		return ret;

	mutex_lock(&pi->lock);
	ret = pi3dpx1207c_set_conf(pi, value);

	if (ret) {
		dev_err(&pi->client->dev, "Error writing conf: %d\n", ret);
		mutex_unlock(&pi->lock);
		return ret;
	}

	mutex_unlock(&pi->lock);

	return count;
}

static DEVICE_ATTR(conf, S_IWUSR | S_IRUGO, get_conf, set_conf);

static struct attribute *pi3dpx1207_attrs[] = {
	&dev_attr_conf.attr,
	NULL,
};

static const struct attribute_group pi3dpx1207_group = {
	.attrs = pi3dpx1207_attrs,
};

static int pi3dpx1207c_probe(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	struct typec_switch_desc sw_desc = { };
	struct typec_mux_desc mux_desc = { };
	struct pi3dpx1207c *pi;
	int ret;
	u8 default_conf = 0;
	u8 read_conf[4] = {0};
	
	pi = devm_kzalloc(dev, sizeof(*pi), GFP_KERNEL);
	if (!pi)
		return -ENOMEM;

	pi->client = client;
	mutex_init(&pi->lock);

	pi->need_aux_flip = 1; //default conf
#if 0
	pi->en_pin = of_get_named_gpio_flags(dev->of_node, "en-gpio", 0, &flags);

	if (!gpio_is_valid(pi->en_pin)) {
		dev_err(dev, "unable to get redriver gpio\n");
	} else {

		ret = devm_gpio_request_one(dev, pi->en_pin, GPIOF_OUT_INIT_HIGH, "en-gpio");
		if (ret < 0) {
			dev_err(dev, "en-gpio request fail\n");
		}
		gpio_set_value(pi->en_pin, 1);
		mdelay(15);
	}

	if (device_property_present(dev, "force_hpd")) {
		pi->force_hpd = 1;
		dev_info(dev, "force hpd\n");
	} else {
		pi->force_hpd = 0;
	}
#endif
	if (!fwnode_property_read_u8(dev->fwnode, "default_conf", &default_conf)) {
		dev_info(dev, "init default conf %02x, %02x\n", default_conf, pi->conf);
		pi3dpx1207c_set_conf(pi, default_conf);
	}

	if (!fwnode_property_read_u8(dev->fwnode, "need_aux_flip", &pi->need_aux_flip)) {
		dev_info(dev, "need flip %02x \n", pi->need_aux_flip);
	}

	ret = sysfs_create_group(&client->dev.kobj, &pi3dpx1207_group);

	if (ret) {
		dev_err(dev, "sysfs_create_group ret = %d\n", ret);
	}

	ret = i2c_smbus_read_i2c_block_data(client, PI3DPX1207C_CONF, 4, read_conf);
	if (ret < 0) {
		dev_err(dev, "Error reading config register %d\n", ret);
		return ret;
	} else {
		printk("read_conf = %hhu %hhu %hhu %hhu", read_conf[0], read_conf[1], read_conf[2], read_conf[3]);
	}

	pi->conf = read_conf[3];

	sw_desc.drvdata = pi;
	sw_desc.fwnode = dev->fwnode;
	sw_desc.set = pi3dpx1207c_sw_set;

	pi->sw = typec_switch_register(dev, &sw_desc);
	if (IS_ERR(pi->sw)) {
		dev_err(dev, "Error registering typec switch: %ld\n",
			PTR_ERR(pi->sw));
		return PTR_ERR(pi->sw);
	}

	mux_desc.drvdata = pi;
	mux_desc.fwnode = dev->fwnode;
	mux_desc.set = pi3dpx1207c_mux_set;

	pi->mux = typec_mux_register(dev, &mux_desc);
	if (IS_ERR(pi->mux)) {
		typec_switch_unregister(pi->sw);
		dev_err(dev, "Error registering typec mux: %ld\n",
			PTR_ERR(pi->mux));
		return PTR_ERR(pi->mux);
	}

	i2c_set_clientdata(client, pi);
	 dev_err(dev,"%s  success\n",__func__);
	return 0;
}

static void pi3dpx1207c_remove(struct i2c_client *client)
{
	struct pi3dpx1207c *pi = i2c_get_clientdata(client);

	typec_mux_unregister(pi->mux);
	typec_switch_unregister(pi->sw);
}

static const struct i2c_device_id pi3dpx1207c_table[] = {
	{ "pi3dpx1207c" },
	{ }
};
MODULE_DEVICE_TABLE(i2c, pi3dpx1207c_table);

#if IS_ENABLED(CONFIG_OF)
static const struct of_device_id pi3dpx1207c_dt_ids[] = {
    { .compatible = "pericom,pi3dpx1207c",  .data = NULL },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, pi3dpx1207c_dt_ids);
#endif

static const struct acpi_device_id pi3dpx1207c_acpi_ids[] = {
	{"CIXH200E", 0},
	{ }
};
MODULE_DEVICE_TABLE(acpi, pi3dpx1207c_acpi_ids);

static struct i2c_driver pi3dpx1207c_driver = {
	.driver = {
		.name = "pi3dpx1207c",
#if IS_ENABLED(CONFIG_OF)
		.of_match_table = pi3dpx1207c_dt_ids,
#endif
		.acpi_match_table = ACPI_PTR(pi3dpx1207c_acpi_ids),
	},
	.probe	= pi3dpx1207c_probe,
	.remove		= pi3dpx1207c_remove,
	.id_table	= pi3dpx1207c_table,
};

module_i2c_driver(pi3dpx1207c_driver);

MODULE_AUTHOR("Dillon Min <dillon.min@nio.com>");
MODULE_DESCRIPTION("Pericom PI3DPX1207C Type-C redriver");
MODULE_LICENSE("GPL");
