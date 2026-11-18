/*
 * ILITEK Touch IC driver
 *
 * Copyright (C) 2022 ILI Technology Corporation.
 *
 * Author: Dicky Chiang <dicky_chiang@ilitek.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 */

#include <linux/hid.h>
#include <linux/input.h>
#include "ilitek_9882t.h"

struct i2c_client *ilits_i2c;
u16 ilits_hid_descriptor_addr;

int ili_i2c_hid_input_init(void)
{
	int ret = 0;
	struct input_dev *input = NULL;

	input = input_allocate_device();
	if (ERR_ALLOC_MEM(input)) {
		ILI_ERR("Failed to allocate touch input device\n");
		input_free_device(input);
		return -EINVAL;
	}

	ILI_INFO("Register touch to input subsystem\n");

	input->name = HID_TOUCH_INPUT_DEVICE;

	/* set the supported event type for input device */
	__set_bit(KEY_GESTURE_POWER, input->keybit);
	input->dev.parent = ilits->dev;
	input_set_capability(input, EV_KEY, KEY_GESTURE_POWER);

	ilits->input_hid = input;

	ret = input_register_device(input);
	if (ret < 0) {
		ILI_ERR("Failed to register touch input device\n");
		input_unregister_device(ilits->input_hid);
		input_free_device(ilits->input_hid);
		return -EINVAL;
	}

	ilits->input_dev_status = 0;
	return ret;
}

void ili_i2c_hid_input_remove(void)
{
	ILI_INFO("Unregister HID_TOUCH_INPUT_DEVICE from input subsystem\n");

	if (!ilits->input_dev_status) {
		input_unregister_device(ilits->input_hid);
		input_free_device(ilits->input_hid);
	}
}

static u8 check_raw_data(u8 *buf, u8 len)
{
	int i = 0;
	u8 check_sum = 0;

	for (i = 3; i < len; i++)
		check_sum += buf[i];
	check_sum = ((-check_sum) & 0xFF);

	return check_sum;
}

/*
 * raw_data still carries the 2 bytes length header of the i2c-hid input report.
 */
static int ili_i2c_hid_report(u8 *raw_data, int size,
			      struct hid_report *report)
{
	u8 check_sum = 0;
	u32 timestamp = 0;
	uint32_t pen_serial = INVALID_PEN_SERIAL;
	struct hid_field *field = report->field[0];
#if IS_ENABLED(CONFIG_STYLUS_BATTERY_ALGO)
	static u32 stylus_frame_num;
	stylus_packet_info_t stylus_packet_info = { 0 };
#endif

	check_sum = check_raw_data(raw_data, size - 1);
	if (raw_data[size - 1] != check_sum) {
		ILI_ERR("ILITEK check sum error (%d), packet checksum (%d) !!\n ",
			check_sum, raw_data[size - 1]);
		ili_hid_ic_cascade_get_pc_latch(DO_I2C_RECOVER);
		return -EINVAL;
	}

	if (raw_data[I2C_HID_RID_IDX] == I2C_HID_RID &&
	    raw_data[I2C_HID_GESTURE_LEN_IDX] == I2C_HID_GESTURE_LEN &&
	    raw_data[I2C_HID_PACKET_ID_IDX] == P5_X_GESTURE_PACKET_ID &&
	    raw_data[I2C_HID_GESTURE_TYPE_IDX] == GESTURE_DOUBLECLICK &&
	    atomic_read(&ilits->gesture)) {
		ILI_INFO("Gesture key event\n");
		input_report_key(ilits->input_hid, KEY_GESTURE_POWER, 1);
		input_sync(ilits->input_hid);
		input_report_key(ilits->input_hid, KEY_GESTURE_POWER, 0);
		input_sync(ilits->input_hid);
		ilits->gcoord->type = GESTURE_DOUBLECLICK;
		ilits->gcoord->clockwise = 1;
		ILI_INFO("Type = %d, clockwise = %d\n", ilits->gcoord->type, ilits->gcoord->clockwise);
	} else if (raw_data[I2C_HID_CHECK_MP_INT_IDX] == 0xAD) {
		ilits->hid_report_irq = true;
	} else if (raw_data[I2C_HID_RID_IDX] == I2C_HID_PEN_RID) {
		pen_serial = (raw_data[ILITEK_HID_PEN_SERIAL_4] << 24) |
			     (raw_data[ILITEK_HID_PEN_SERIAL_3] << 16) |
			     (raw_data[ILITEK_HID_PEN_SERIAL_2] << 8) |
			     raw_data[ILITEK_HID_PEN_SERIAL_1];
		if (pen_serial != INVALID_PEN_SERIAL) {
			ilits->pen_touch_time = ktime_get_boottime();
			if (ilits->pen_serial != pen_serial) {
				ILI_INFO(
					"stylus_power %d, stylus_vid 0x%x, old_stylus_id 0x%x, stylus_id 0x%x\n",
					raw_data[I2C_HID_STYLUS_BATTERY_STRENGTH],
					(raw_data[ILITEK_HID_PEN_VENDOR_USAGE_1_H]
					<< 8) | raw_data[ILITEK_HID_PEN_VENDOR_USAGE_1_L],
					ilits->pen_serial, pen_serial);
				ilits->pen_serial = pen_serial;
#if IS_ENABLED(CONFIG_STYLUS_BATTERY_ALGO)
				stylus_frame_num = 0;
#endif
			}

			ILI_DEBUG_INFO(
				"stylus_battery_capacity %d, stylus_oem_vendor_id 0x%x, pen_serial 0x%x\n",
				raw_data[I2C_HID_STYLUS_BATTERY_STRENGTH],
				(raw_data[ILITEK_HID_PEN_VENDOR_USAGE_1_H]
				<< 8) | raw_data[ILITEK_HID_PEN_VENDOR_USAGE_1_L],
				ilits->pen_serial);

#if IS_ENABLED(CONFIG_STYLUS_BATTERY_ALGO)
			if ((++stylus_frame_num % PEN_FRAME_COUNT) == 0) {
				/* get stylus capacity, stylus fw version and oem_vendor_id */
				stylus_packet_info.pen_id = ilits->pen_serial;
				stylus_packet_info.vendor_id =
					raw_data[ILITEK_HID_PEN_VENDOR_USAGE_1_H] << 8 |
					raw_data[ILITEK_HID_PEN_VENDOR_USAGE_1_L];
				stylus_packet_info.fw_version =
					raw_data[ILITEK_HID_PEN_FW_VERSION_H] << 8 |
					raw_data[ILITEK_HID_PEN_FW_VERSION_L];
				stylus_packet_info.capacity =
					raw_data[I2C_HID_STYLUS_BATTERY_STRENGTH];
				stylus_packet_info.time = ktime_to_ms(ilits->pen_touch_time);
				stylus_info_packet(stylus_packet_info);
				stylus_info_process();
			}
#endif
		}
		timestamp = (raw_data[I2C_HID_TIME_STAMP_H_IDX] << 8) |
			    raw_data[I2C_HID_TIME_STAMP_L_IDX];
		set_bit(MSC_TIMESTAMP, field->hidinput->input->mscbit);
		input_event(field->hidinput->input, EV_MSC, MSC_TIMESTAMP, timestamp);
	}

	return 0;
}

int ili_i2c_hid_report_handle(struct hid_device *hid, int type,
			      const u8 *data, u32 size)
{
	struct hid_report_enum *report_enum;
	struct hid_report *report;
	int ret = 0;

	if (!hid || !hid->driver)
		return -ENODEV;

	if (down_trylock(&hid->driver_input_lock))
		return -EBUSY;

	if (!size) {
		ILI_ERR("empty report\n");
		ili_hid_ic_cascade_get_pc_latch(DO_I2C_RECOVER);
		ret = -EINVAL;
		goto unlock;
	}

	report_enum = hid->report_enum + type;
	report = hid_get_report(report_enum, data + 2);
	if (!report) {
		ret = -EINVAL;
		goto unlock;
	}

	ret = ili_i2c_hid_report((u8 *)data, size, report);
	if (ret < 0)
		ILI_ERR("ilitek report error.\n");

unlock:
	up(&hid->driver_input_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(ili_i2c_hid_report_handle);

int ili_hid_of_core_probe(struct i2c_client *client, struct i2chid_ops *ops,
			  u16 hid_descriptor_address)
{
	int ret = 0;

	ILI_INFO("++++ ili_i2c_hid_of_probe ++++\n");

	if (!client || !ops) {
		ILI_ERR("client or ihid_of is NULL\n");
		return -ENODEV;
	}

	ilits_i2c = client;
	ilits_hid_descriptor_addr = hid_descriptor_address;

	ops->irq_callback = ili_i2c_hid_report_handle;

	ret = ilitek_hid_plat_dev_init(client);
	if (ret < 0) {
		ILI_ERR("Hid i2c plat probe error, remove registered resources\n");
		goto err_plat_probe;
	}

	ili_hid_pre_ping();

	ILI_INFO("Probe Ilitek HID\n");
	ret = i2c_hid_core_probe(ilits_i2c, ops, ilits_hid_descriptor_addr);
	if (ret < 0) {
		ILI_ERR("Hid probe error, remove registered resources\n");
		goto err_hid_probe;
	}

	ret = ili_i2c_hid_input_init();
	if (ret < 0) {
		ILI_ERR("Input_dev init fail, remove registered resources\n");
		goto err_input_init;
	}

	ret = ilitek_notifier_register();
	if (ret < 0) {
		ILI_ERR("Input_dev init fail, remove registered resources\n");
		goto err_notifier_register;
	}

	return ret;

err_notifier_register:
	ilitek_notifier_unregister();
	ili_i2c_hid_input_remove();
err_input_init:
	i2c_hid_core_remove(ilits_i2c);
err_hid_probe:
	ili_hid_dev_remove(ENABLE);
	ilitek_i2c_remove(ilits->i2c);
err_plat_probe:
	ilits_i2c = NULL;
	ops->irq_callback = NULL;
	return ret;
}
EXPORT_SYMBOL_GPL(ili_hid_of_core_probe);

MODULE_DESCRIPTION("HID over I2C OF driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
