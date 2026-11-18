/*
 *
 * FocalTech TouchScreen driver.
 *
 * Copyright (c) 2012-2022, FocalTech Systems, Ltd., all rights reserved.
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
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/i2c.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>

#include "focal_hid_core.h"

struct i2c_hid *fts_ihid;

int fts_hid_of_core_probe(struct i2c_client *client, struct i2chid_ops *ops,
			  u16 hid_descriptor_address)
{
	int ret = 0;

	FTS_INFO("i2c_hid_of_probe....\n");
	if (!client || !ops) {
		FTS_ERROR("client or ops is NULL\n");
		return -ENODEV;
	}

	ops->irq_callback = fts_irq_callback;
	ops->report_desc_override = fts_report_fixup;

	ret = i2c_hid_core_probe(client, ops, hid_descriptor_address);
	if (ret < 0) {
		FTS_ERROR("i2c_hid_core_probe fail\n");
		return ret;
	}

	fts_ihid = i2c_get_clientdata(client);
	ret = fts_hid_probe_entry(fts_ihid->hid);

	return ret;
}
EXPORT_SYMBOL_GPL(fts_hid_of_core_probe);

int fts_ts_remove(struct i2c_client *client)
{
	if (!client) {
		FTS_ERROR("client is NULL\n");
		return -ENODEV;
	}

	fts_ts_remove_entry();

	return i2c_hid_core_remove(client);
}
EXPORT_SYMBOL_GPL(fts_ts_remove);

MODULE_DESCRIPTION("HID over I2C OF driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
