/*
 * HID over I2C Open Firmware Subclass
 *
 * Copyright (c) 2012 Benjamin Tissoires <benjamin.tissoires@gmail.com>
 * Copyright (c) 2012 Ecole Nationale de l'Aviation Civile, France
 * Copyright (c) 2012 Red Hat, Inc
 *
 * This code was forked out of the core code, which was partly based on
 * "USB HID support for Linux":
 *
 *  Copyright (c) 1999 Andreas Gal
 *  Copyright (c) 2000-2005 Vojtech Pavlik <vojtech@suse.cz>
 *  Copyright (c) 2005 Michael Haboustak <mike-@cinci.rr.com> for Concept2, Inc
 *  Copyright (c) 2007-2008 Oliver Neukum
 *  Copyright (c) 2006-2010 Jiri Kosina
 *
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file COPYING in the main directory of this archive for
 * more details.
 */

#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/pm.h>
#include <linux/regulator/consumer.h>

#include "i2c-hid.h"

#define TP_VENDOR_FOCAL		0
#define TP_VENDOR_ILITEK	1
#define TP_VENDOR_NOVATEK	2

#define LCM_ID_FT8205_STARRY	0x05
#define LCM_ID_ILI9882T_TG	0x06
#define LCM_ID_FT8205_KD	0x09
#define LCM_ID_ILI9882T_KD	0x0A
#define LCM_ID_NT36523N_KD	0x0C
#define LCM_ID_ILI9882T_STARRY	0x0E
#define LCM_ID_INVALID		0xFF

extern unsigned int idme_get_lcmid_info(void);

extern int fts_hid_of_core_probe(struct i2c_client *client,
				 struct i2chid_ops *ops,
				 u16 hid_descriptor_address);
extern int fts_ts_remove(struct i2c_client *client);
extern int ili_hid_of_core_probe(struct i2c_client *client,
				 struct i2chid_ops *ops,
				 u16 hid_descriptor_address);
extern int ilitek_plat_remove(struct i2c_client *client);
extern int nvt_hid_of_core_probe(struct i2c_client *client,
				 struct i2chid_ops *ops,
				 u16 hid_descriptor_address);
extern int nvt_i2c_hid_core_remove(struct i2c_client *client);

static int vendor_id = LCM_ID_INVALID;
int (*tp_probe)(struct i2c_client *client, struct i2chid_ops *ops,
		u16 hid_descriptor_address);
int (*tp_remove)(struct i2c_client *client);

static int get_tp_vendor_id(struct device *dev)
{
	unsigned int lcm_id = idme_get_lcmid_info();

	switch (lcm_id) {
	case LCM_ID_FT8205_STARRY:
	case LCM_ID_FT8205_KD:
		vendor_id = TP_VENDOR_FOCAL;
		break;
	case LCM_ID_ILI9882T_TG:
	case LCM_ID_ILI9882T_KD:
	case LCM_ID_ILI9882T_STARRY:
		vendor_id = TP_VENDOR_ILITEK;
		break;
	case LCM_ID_NT36523N_KD:
		vendor_id = TP_VENDOR_NOVATEK;
		break;
	default:
		if (lcm_id == LCM_ID_INVALID)
			dev_err(dev, "get lcm_id failed.\n");
		dev_err(dev, "%s, vendor id:[%d] not support.\n", __func__,
			lcm_id);
		return -EINVAL;
	}

	dev_dbg(dev, "%s, vendor id = 0x%x\n", __func__, lcm_id);

	return 0;
}

static int i2c_hid_of_power_up(struct i2chid_ops *ops)
{
	struct i2c_hid_of *ihid_of = container_of(ops, struct i2c_hid_of, ops);
	struct device *dev = &ihid_of->client->dev;
	int ret;

	ret = regulator_bulk_enable(ARRAY_SIZE(ihid_of->supplies),
				    ihid_of->supplies);
	if (ret) {
		dev_warn(dev, "Failed to enable supplies: %d\n", ret);
		return ret;
	}

	if (ihid_of->post_power_delay_ms)
		msleep(ihid_of->post_power_delay_ms);

	return 0;
}

static void i2c_hid_of_power_down(struct i2chid_ops *ops)
{
	struct i2c_hid_of *ihid_of = container_of(ops, struct i2c_hid_of, ops);

	regulator_bulk_disable(ARRAY_SIZE(ihid_of->supplies),
			       ihid_of->supplies);
}

static int i2c_hid_of_probe(struct i2c_client *client,
			    const struct i2c_device_id *dev_id)
{
	struct device *dev = &client->dev;
	struct i2c_hid_of *ihid_of;
	u16 hid_descriptor_address;
	int ret;
	u32 val;

	dev_warn(&client->dev, "%s called for i2c 0x%02x\n", __func__,
		 client->addr);

	/* Make sure there is something at this address */
	ret = i2c_smbus_read_byte(client);
	if (ret < 0) {
		dev_err(&client->dev, "nothing at this address 0x%x: %d\n",
			client->addr, ret);
		return -ENODEV;
	}

	ihid_of = devm_kzalloc(&client->dev, sizeof(*ihid_of), GFP_KERNEL);
	if (!ihid_of)
		return -ENOMEM;

	ihid_of->ops.power_up = i2c_hid_of_power_up;
	ihid_of->ops.power_down = i2c_hid_of_power_down;

	ret = of_property_read_u32(dev->of_node, "hid-descr-addr", &val);
	if (ret) {
		dev_err(&client->dev, "HID register address not provided\n");
		return -ENODEV;
	}
	if (val >> 16) {
		dev_err(&client->dev, "Bad HID register address: 0x%08x\n",
			val);
		return -EINVAL;
	}
	hid_descriptor_address = val;

	if (!device_property_read_u32(&client->dev, "post-power-on-delay-ms",
				      &val))
		ihid_of->post_power_delay_ms = val;

	ihid_of->supplies[0].supply = "vdd";
	ihid_of->supplies[1].supply = "vddl";
	ret = devm_regulator_bulk_get(&client->dev,
				      ARRAY_SIZE(ihid_of->supplies),
				      ihid_of->supplies);
	if (ret)
		return ret;

	ret = get_tp_vendor_id(&client->dev);
	if (ret < 0) {
		dev_err(&client->dev, "vendor_id:[%d]not support: %d\n",
			vendor_id, ret);
		return -EINVAL;
	}

	switch (vendor_id) {
	case TP_VENDOR_FOCAL:
		tp_probe = fts_hid_of_core_probe;
		break;
	case TP_VENDOR_ILITEK:
		tp_probe = ili_hid_of_core_probe;
		break;
	case TP_VENDOR_NOVATEK:
		tp_probe = nvt_hid_of_core_probe;
		break;
	default:
		tp_probe = i2c_hid_core_probe;
		break;
	}

	ret = tp_probe(client, &ihid_of->ops, hid_descriptor_address);
	if (ret < 0) {
		dev_err(&client->dev, "vendor_id:[%d] init failed: %d\n",
			vendor_id, ret);
		return -ENODEV;
	}

	return ret;
}

static int i2c_hid_of_remove(struct i2c_client *client)
{
	int ret;

	switch (vendor_id) {
	case TP_VENDOR_FOCAL:
		tp_remove = fts_ts_remove;
		break;
	case TP_VENDOR_ILITEK:
		tp_remove = ilitek_plat_remove;
		break;
	case TP_VENDOR_NOVATEK:
		tp_remove = nvt_i2c_hid_core_remove;
		break;
	default:
		tp_remove = i2c_hid_core_remove;
		break;
	}

	ret = tp_remove(client);
	if (ret < 0) {
		dev_err(&client->dev, "vendor_id:[%d] tp_remove failed: %d\n",
			vendor_id, ret);
		return -ENODEV;
	}

	return ret;
}

static const struct of_device_id i2c_hid_of_match[] = {
	{ .compatible = "hid-over-i2c" },
	{},
};
MODULE_DEVICE_TABLE(of, i2c_hid_of_match);

static const struct i2c_device_id i2c_hid_of_id_table[] = {
	{ "hid", 0 },
	{ "hid-over-i2c", 0 },
	{ "focal_hid-over-i2c", 0 },
	{ "ilitek_hid-over-i2c", 0 },
	{ "i2c-hid-of-nvt-ts", 0 },
	{ },
};
MODULE_DEVICE_TABLE(i2c, i2c_hid_of_id_table);

static struct i2c_driver i2c_hid_of_driver = {
	.driver = {
		.name	= "i2c_hid_of",
#if defined(CONFIG_PM_SLEEP) && !defined(CONFIG_I2C_HID_NO_USE_PM)
		.pm	= &i2c_hid_core_pm,
#endif
		.probe_type = PROBE_PREFER_ASYNCHRONOUS,
		.of_match_table = of_match_ptr(i2c_hid_of_match),
	},

	.probe		= i2c_hid_of_probe,
	.remove		= i2c_hid_of_remove,
	.shutdown	= i2c_hid_core_shutdown,
	.id_table	= i2c_hid_of_id_table,
};

module_i2c_driver(i2c_hid_of_driver);

MODULE_DESCRIPTION("HID over I2C OF driver");
MODULE_AUTHOR("Benjamin Tissoires <benjamin.tissoires@gmail.com>");
MODULE_LICENSE("GPL");
