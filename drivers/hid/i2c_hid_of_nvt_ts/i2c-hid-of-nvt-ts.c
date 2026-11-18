/*
 * Copyright (C) 2022 Novatek, Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 */

#include <linux/module.h>
#include <linux/i2c.h>
#include <linux/interrupt.h>
#include <linux/input.h>
#include <linux/irq.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/pm.h>
#include <linux/device.h>
#include <linux/wait.h>
#include <linux/err.h>
#include <linux/string.h>
#include <linux/list.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/hid.h>
#include <linux/mutex.h>
#include <linux/miscdevice.h>
#include <linux/of.h>
#include <linux/types.h>
#include <linux/hidraw.h>
#include <linux/uaccess.h>

#include "i2c-hid.h"
#include "nvt_i2c_hid.h"

#define NVT_HID_I2C_DRIVER_VERSION "2.0.0"

#define NVT_TOUCH_WKG_REPORT_PERIOD 600

#define NVT_TOUCH_GESTURE_DISABLE 0
#define NVT_TOUCH_GESTURE_ENABLE 1

#define IRQ_DISABLE 0
#define IRQ_ENABLE 1

struct i2c_client *hid_i2c_client = NULL;
struct device *hid_i2c_dev;
uint32_t set_f_pkt[GET_SET_F_PKT_LENGTH];
uint32_t get_f_pkt[GET_SET_F_PKT_LENGTH];
uint8_t prefix_pkt[PREFIX_PKT_LENGTH] = { 0x00, 0x00, 0x3B, 0x00, 0x00 };
uint32_t wakeup_id;
atomic_t nvt_irq_stat = ATOMIC_INIT(IRQ_ENABLE);
static DEFINE_SPINLOCK(nvt_irq_spin);
bool gesture_switch_failed = false;
bool gesture_mode = false;
static unsigned long gesture_timer;
struct input_dev *wkg_input_dev;
struct mutex probe_lock;

#if PEN_TIMESTAMP_SUPPORT
static bool pen_timestamp_support = false;
static struct input_dev *pen_input = NULL;
static int32_t pen_scantime = 0;
static int32_t pen_prev_scantime = 0;
static long pen_delta_scantime = 0;
static unsigned long pen_jiffies = 0;
static int32_t pen_timestamp = 0;
#endif

static const struct i2c_hid_cmd hid_wkg_on_cmd = { I2C_HID_CMD(0x0E) };
static const struct i2c_hid_cmd hid_wkg_off_cmd = { I2C_HID_CMD(0x0F) };

#if IS_ENABLED(CONFIG_HID_TOUCH_METRICS)
touch_metrics_info_t touch_metrics_data;
#endif

#if CHECK_DATA_CHECKSUM
static int32_t nvt_ts_data_checksum(uint8_t *buf, uint8_t length)
{
	uint8_t checksum = 0;
	int32_t i = 0;

	for (i = 0; i < length - 1; i++)
		checksum += buf[i];
	checksum = (~checksum + 1);

	if (checksum != buf[length - 1]) {
		NVT_ERR("checksum not match. (buf[%d]=0x%02X, checksum=0x%02X)\n",
			length - 1, buf[length - 1], checksum);

		return -1;
	}

	return 0;
}
#endif

#if PEN_TIMESTAMP_SUPPORT
static bool nvt_pen_input_timestamp_set(void)
{
	struct i2c_hid *ihid = NULL;
	struct hid_device *hid = NULL;
	struct hid_report *report = NULL;
	int field_count = 0;
	ihid = i2c_get_clientdata(hid_i2c_client);
	hid = ihid->hid;

	list_for_each_entry(report, &hid->report_enum[HID_INPUT_REPORT].report_list, list) {
		for (field_count = 0; field_count < report->maxfield; field_count++) {
			if ((report->field[field_count]->application == HID_DG_PEN) &&
					report->field[field_count]->usage->hid == HID_DG_SCANTIME &&
					report->field[field_count]->flags == HID_MAIN_ITEM_VARIABLE) {
				NVT_LOG("Pen timestamp is set up, enable it\n");
				pen_timestamp_support = true;
				pen_input = report->field[field_count]->hidinput->input;
				input_set_capability(pen_input, EV_MSC, MSC_TIMESTAMP);
				break;
			}
		}
		if (pen_timestamp_support)
			break;
	}

	return pen_timestamp_support;
}
#endif

static int nvt_irq_callback(struct hid_device *hid, int type, const u8 *data, u32 size)
{
	uint32_t pen_serial = PEN_INVALID_SERIAL;
	unsigned long jdelta = jiffies_to_usecs(jiffies - pen_jiffies);
#if IS_ENABLED(CONFIG_STYLUS_BATTERY_ALGO)
	static u32 stylus_frame_num = 0;
	stylus_packet_info_t stylus_packet_info = { 0 };
#endif

	if (!hid)
		return -ENODEV;

#if CHECK_DATA_CHECKSUM
	if (nvt_ts_data_checksum((uint8_t *)data, size))
		return -EINVAL;
#endif
#if PEN_TIMESTAMP_SUPPORT
	if (pen_timestamp_support && data[2] == PEN_ID) {
		pen_scantime = data[59] + (data[60] << 8);
		pen_delta_scantime = pen_scantime - pen_prev_scantime;
		pen_jiffies = jiffies;
		if (pen_delta_scantime < 0)
			pen_delta_scantime += PEN_MAX_DELTA;
		pen_delta_scantime *= 100;
		if (jdelta > MAX_TIMESTAMP_INTERVAL)
			pen_timestamp = 0;
		else
			pen_timestamp += pen_delta_scantime;
		pen_prev_scantime = pen_scantime;
		input_event(pen_input, EV_MSC, MSC_TIMESTAMP, pen_timestamp);
	}
#endif

	if (data[PEN_FLAG_INDEX] == PEN_ID) {
		memcpy(&pen_serial, data + PEN_SERIAL_INDEX, sizeof(pen_serial));
		if (pen_serial != PEN_INVALID_SERIAL) {
			ts->pen_touch_time = ktime_get_boottime();

			if (ts->pen_serial != pen_serial) {
				NVT_LOG(
					"stylus_power %d, stylus_vid 0x%x, old_stylus_id 0x%x, stylus_id 0x%x\n",
					data[PEN_BATTERY_STRENGTH],
					(data[PEN_VENDOR_USAGE_1_H] << 8) |
					data[PEN_VENDOR_USAGE_1_L],
					ts->pen_serial, pen_serial);
				ts->pen_serial = pen_serial;
#if IS_ENABLED(CONFIG_STYLUS_BATTERY_ALGO)
				stylus_frame_num = 0;
#endif
			}
			NVT_DEBUG_LOG(
				"stylus_battery_capacity %d, stylus_oem_vendor_id 0x%x, pen_serial 0x%x\n",
				data[PEN_BATTERY_STRENGTH],
				(data[PEN_VENDOR_USAGE_1_H] << 8) |
				data[PEN_VENDOR_USAGE_1_L],
				ts->pen_serial);

#if IS_ENABLED(CONFIG_STYLUS_BATTERY_ALGO)
			if ((++stylus_frame_num % PEN_FRAME_COUNT) == 0) {
				/* get stylus capacity, stylus fw version and oem_vendor_id */
				stylus_packet_info.pen_id = ts->pen_serial;
				stylus_packet_info.vendor_id =
					data[PEN_VENDOR_USAGE_1_H] << 8 |
					data[PEN_VENDOR_USAGE_1_L];
				stylus_packet_info.fw_version =
					data[NVT_HID_PEN_FW_VERSION_H] << 8 |
					data[NVT_HID_PEN_FW_VERSION_L];
				stylus_packet_info.capacity = data[PEN_BATTERY_STRENGTH];
				stylus_packet_info.time = ktime_to_ms(ts->pen_touch_time);
				stylus_info_packet(stylus_packet_info);
				stylus_info_process();
			}
#endif
		}
	}

	if (wakeup_id && gesture_mode && (wakeup_id == data[2])) {
		if (jiffies_to_msecs(jiffies - gesture_timer) >
		    NVT_TOUCH_WKG_REPORT_PERIOD) {
			pm_wakeup_event(&wkg_input_dev->dev, 5000);

			input_report_key(wkg_input_dev, KEY_POWER, 1);
			input_sync(wkg_input_dev);
			input_report_key(wkg_input_dev, KEY_POWER, 0);
			input_sync(wkg_input_dev);

			gesture_timer = jiffies;
			NVT_LOG("Wake up geature detected\n");
			return -EIO;
		}
	}
	return 0;
}

static int parse_data_from_dts(struct i2c_client *client)
{
	struct device *dev = &client->dev;
	int ret = 0;

	ret = of_property_read_u32(dev->of_node, "novatek,wakeup-id", &wakeup_id);
	if (!ret)
		NVT_LOG("wakeup gesture is supported, id = 0x%02X\n", wakeup_id);
	else
		NVT_LOG("wakeup gesture is not supported\n");

	ts->pen_support = of_property_read_bool(dev->of_node, "novatek,pen-support");
	NVT_LOG("novatek,pen-support=%d\n", ts->pen_support);

	return ret;
}


int nvt_hid_i2c_get_feature(struct i2c_client *client, uint8_t *hid_buf, uint32_t hid_len,
		uint8_t *buf_recv, uint16_t read_len)
{
	uint8_t *i2c_data, msg_num;
	uint32_t i2c_len = 0;
	int32_t ret = 0;
	struct i2c_msg msg[2] = { 0 };

	i2c_len = hid_len + 6;
	i2c_data = kzalloc(i2c_len, GFP_ATOMIC);
	if (!i2c_data) {
		ret = -ENOMEM;
		return ret;
	}

	mutex_lock(&ts->xbuf_lock);
	i2c_data[0] = (uint8_t)get_f_pkt[0];
	i2c_data[1] = (uint8_t)get_f_pkt[1];
	i2c_data[2] = (uint8_t)get_f_pkt[2];
	i2c_data[3] = (uint8_t)get_f_pkt[3];
	i2c_data[4] = (uint8_t)get_f_pkt[4];
	i2c_data[5] = (uint8_t)get_f_pkt[5];
	memcpy(i2c_data + 6, hid_buf, hid_len);

	msg[0].addr = client->addr;
	msg[0].flags = client->flags & I2C_M_TEN;
	msg[0].len = 6;
	msg[0].buf = i2c_data;
	msg[1].addr = client->addr;
	msg[1].flags = client->flags & I2C_M_TEN;
	msg[1].flags |= I2C_M_RD;
	msg[1].len = read_len;
	msg[1].buf = buf_recv;
	msg_num = 2;
	if (i2c_transfer(client->adapter, msg, msg_num) != msg_num) {
		if (!buf_recv)
			NVT_ERR("i2c read error, data = %*ph, buf_recv = NULL\n", i2c_len, i2c_data);
		else
			NVT_ERR("i2c read error, data = %*ph, buf_recv = %*ph\n",
				i2c_len, i2c_data, read_len, buf_recv);
		ret = -EIO;
	} else {
		ret = hid_len - 2;
	}
	mutex_unlock(&ts->xbuf_lock);
	kfree(i2c_data);

	return ret;
}

int nvt_hid_i2c_set_feature(struct i2c_client *client, uint8_t *hid_buf, uint32_t hid_len)
{
	uint8_t *i2c_data, msg_num;
	uint32_t i2c_len = 0;
	int32_t ret = 0;
	struct i2c_msg msg[1] = { 0 };

	i2c_len = hid_len + 8;
	i2c_data = kzalloc(i2c_len, GFP_ATOMIC);
	if (!i2c_data) {
		ret = -ENOMEM;
		return ret;
	}

	mutex_lock(&ts->xbuf_lock);
	i2c_data[0] = (uint8_t)set_f_pkt[0];
	i2c_data[1] = (uint8_t)set_f_pkt[1];
	i2c_data[2] = (uint8_t)set_f_pkt[2];
	i2c_data[3] = (uint8_t)set_f_pkt[3];
	i2c_data[4] = (uint8_t)set_f_pkt[4];
	i2c_data[5] = (uint8_t)set_f_pkt[5];
	i2c_data[6] = (hid_len + 2) & 0xFF;
	i2c_data[7] = ((hid_len + 2) & 0xFFFF) >> 16;
	memcpy(i2c_data + 8, hid_buf, hid_len);

	msg[0].addr = client->addr;
	msg[0].flags = client->flags & I2C_M_TEN;
	msg[0].len = i2c_len;
	msg[0].buf = i2c_data;
	msg_num = 1;
	if (i2c_transfer(client->adapter, msg, msg_num) != msg_num) {
		NVT_ERR("i2c write error, data = %*ph\n", i2c_len, i2c_data);
		ret = -EIO;
	} else {
		ret = hid_len;
	}
	mutex_unlock(&ts->xbuf_lock);
	kfree(i2c_data);

	return ret;
}

static long device_ioctl(struct file *filep, uint32_t cmd, unsigned long arg)
{
	uint8_t *hid_buf = NULL;
	uint8_t *buf_recv = NULL;
	uint16_t hid_len = 0;
	uint16_t read_len = 0;
	int32_t ret = 0;
	void __user *user_arg = (void __user *)arg;

#if DRIVER_ESD_PROTECT
	if (ts->nvt_esd_wq)
		cancel_delayed_work(&ts->nvt_esd_work);
#endif

	mutex_lock(&ts->lock);
	hid_len = _IOC_SIZE(cmd) + 2;
	if (hid_len > HID_MAX_BUFFER_SIZE) {
		NVT_ERR("Invalid hid length\n");
		ret = -EINVAL;
		goto end;
	}
	hid_buf = kzalloc(hid_len, GFP_ATOMIC);
	if (!hid_buf) {
		ret = -ENOMEM;
		goto end;
	}

	hid_buf = memdup_user(user_arg, hid_len);


	if (_IOC_NR(cmd) == _IOC_NR(HIDIOCGFEATURE(0))) {
		read_len = hid_buf[1] + (hid_buf[2] << 8);
		if (read_len > HID_MAX_BUFFER_SIZE) {
			NVT_ERR("Invalid read length : %d, max buffer length is %d.\n",
				read_len, HID_MAX_BUFFER_SIZE);
			ret = -EINVAL;
			goto end;
		}
		buf_recv = kzalloc(read_len, GFP_ATOMIC);
		if (!buf_recv) {
			ret = -ENOMEM;
			goto end;
		}

		ret = nvt_hid_i2c_get_feature(hid_i2c_client, hid_buf, hid_len, buf_recv, read_len);
		if (ret > 0) {
			memcpy(hid_buf, buf_recv + 2, read_len - 2); // drop len
			ret = copy_to_user(user_arg, hid_buf, hid_len);
		}
	} else if (_IOC_NR(cmd) == _IOC_NR(HIDIOCSFEATURE(0))) {
		ret = nvt_hid_i2c_set_feature(hid_i2c_client, hid_buf, hid_len - 2);
	} else {
		NVT_ERR("Command %x is not HIDGET/SETFEATURE\n", cmd);
		ret = -EINVAL;
	}
end:
	mutex_unlock(&ts->lock);
#if DRIVER_ESD_PROTECT
	if (ts->nvt_esd_wq) {
		queue_delayed_work(ts->nvt_esd_wq, &ts->nvt_esd_work,
				msecs_to_jiffies(NVT_TOUCH_ESD_CHECK_PERIOD));
	}
#endif
	if (buf_recv != NULL) {
		kfree(buf_recv);
		buf_recv = NULL;
	}
	if (hid_buf != NULL) {
		kfree(hid_buf);
		hid_buf = NULL;
	}


	return ret;
}


void set_prefix_packet(void)
{
	get_f_pkt[0] = prefix_pkt[0];
	get_f_pkt[1] = prefix_pkt[1];
	get_f_pkt[2] = prefix_pkt[2];
	get_f_pkt[3] = GET_REPORT_CMD;
	get_f_pkt[4] = prefix_pkt[3];
	get_f_pkt[5] = prefix_pkt[4];
	set_f_pkt[0] = prefix_pkt[0];
	set_f_pkt[1] = prefix_pkt[1];
	set_f_pkt[2] = prefix_pkt[2];
	set_f_pkt[3] = SET_REPORT_CMD;
	set_f_pkt[4] = prefix_pkt[3];
	set_f_pkt[5] = prefix_pkt[4];

}


static ssize_t read_packet(struct file *file, char __user *buffer, size_t count, loff_t *ppos)
{
	char str[PAGE_SIZE];
	uint8_t str_len;
	ssize_t ret = 0;

	str_len = snprintf(str, PAGE_SIZE, "%c%c%c%c%c", prefix_pkt[0], prefix_pkt[1], prefix_pkt[2], prefix_pkt[3], prefix_pkt[4]);
	if (copy_to_user(buffer, str, str_len))
		ret = -EFAULT;

	return ret;
}

static ssize_t write_packet(struct file *file, const char __user *buffer, size_t count, loff_t *ppos)
{
	uint8_t *buf = NULL;
	ssize_t ret = 0;

	if (count != PREFIX_PKT_LENGTH) {
		NVT_ERR("size %d != %d\n", (uint32_t)count, PREFIX_PKT_LENGTH);
		ret = -EINVAL;
		goto end;
	}

	buf = memdup_user(buffer, count);
	if (IS_ERR(buf))
		return PTR_ERR(buf);

	memcpy(prefix_pkt, buf, count);
	NVT_LOG("prefix packet = %02X %02X %02X %02X %02X\n", prefix_pkt[0], prefix_pkt[1], prefix_pkt[2], prefix_pkt[3], prefix_pkt[4]);

	set_prefix_packet();
end:
	return ret;
}

const static struct file_operations fops = {
	.owner = THIS_MODULE,
	.read = read_packet,
	.write = write_packet,
	.unlocked_ioctl = device_ioctl,
};

static struct miscdevice nvt_ts_hidraw_flash_misc = {
	.name = "nvt_ts_hidraw_flash",
	.fops = &fops,
};
static ssize_t gesture_wakeup_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int32_t ret = 0;

	ret = snprintf(buf, PAGE_SIZE, "%d\n", gesture_mode);
	return ret;
}

static ssize_t gesture_wakeup_store(struct device *dev, struct device_attribute *attr, const char *buf, size_t count)
{
	uint8_t mode = 0;
	int wake_status = 0;

	if (kstrtou8(buf, 10, &mode) || mode > 1)
		return -EINVAL;

	switch (mode) {
	case NVT_TOUCH_GESTURE_DISABLE:
		NVT_LOG("Disable gesture Mode\n");
		if (i2c_hid_command(hid_i2c_client, &hid_wkg_off_cmd, NULL, 0)) {
			NVT_ERR("failed to disable WKG Mode.\n");
		} else {
			gesture_mode = false;
			wake_status = disable_irq_wake(hid_i2c_client->irq);
			if (wake_status)
				NVT_ERR("Failed to disable irq wake\n");
			panel_gesture_mode_set_by_nt36523n(false);
		}
		break;
	case NVT_TOUCH_GESTURE_ENABLE:
		NVT_LOG("Enable gesture Mode\n");
		if (i2c_hid_command(hid_i2c_client, &hid_wkg_on_cmd, NULL, 0)) {
			NVT_ERR("failed to enable WKG Mode.\n");
			gesture_switch_failed = true;
		} else {
			gesture_mode = true;
			wake_status = enable_irq_wake(hid_i2c_client->irq);
			if (wake_status)
				NVT_ERR("Failed to enable irq wake\n");
			panel_gesture_mode_set_by_nt36523n(true);
		}
		break;
	default:
		NVT_ERR("Invalid Input.\n");
		break;
	}

	return count;
}

static ssize_t tp_pen_serial_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	int32_t len = 0;
	int32_t pen_disconnect_diff = 0;
	ktime_t pen_touch_disconnect = ktime_get_boottime();

	pen_disconnect_diff = ktime_to_ms(pen_touch_disconnect) -
			ktime_to_ms(ts->pen_touch_time);
	if (pen_disconnect_diff  > PEN_DISCONNECT_MAX_TIME)
		ts->pen_serial = PEN_INVALID_SERIAL;

	len = snprintf(buf, PAGE_SIZE, "pen_serial = %08X\n", ts->pen_serial);
	if (len < 0) {
		NVT_ERR("snprintf copy string failed. (%d)\n", len);
		return -EINVAL;
	}

	return len;
}

static ssize_t nvt_touch_ready_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%d\n", ts->touch_ready);
}

static DEVICE_ATTR_RW(gesture_wakeup);
static DEVICE_ATTR_RO(tp_pen_serial);
static DEVICE_ATTR_RO(nvt_touch_ready);
static struct attribute *nvt_api_attrs[] = {
	&dev_attr_gesture_wakeup.attr,
	&dev_attr_tp_pen_serial.attr,
	&dev_attr_nvt_touch_ready.attr,
	NULL
};
static const struct attribute_group nvt_api_attribute_group = {
	.attrs = nvt_api_attrs,
};

static int nvt_gesture_dev_and_node_create(void)
{
	int ret = 0;
	struct kobject *touchscreen_link = NULL;
	struct i2c_client *client = NULL;

	if (wakeup_id) {
		if (!wkg_input_dev) {
			wkg_input_dev = input_allocate_device();
			if (!wkg_input_dev) {
				NVT_ERR("can't add wkg input device");
				ret = -ENODEV;
				return ret;
			}
			wkg_input_dev->name = "nvt-ts-wkg";
			wkg_input_dev->phys = "input/ts";
			wkg_input_dev->id.bustype = BUS_I2C;
			ret = input_register_device(wkg_input_dev);
			if (ret) {
				NVT_ERR("register input device (%s) failed. ret=%d\n", wkg_input_dev->name, ret);
				return ret;
			}
			device_init_wakeup(&wkg_input_dev->dev, 1);
			input_set_capability(wkg_input_dev, EV_KEY, KEY_POWER);

			client = ts->client;

			touchscreen_link = kobject_create_and_add("touchscreen", NULL);
			if (touchscreen_link != NULL) {
				ret = sysfs_create_link(touchscreen_link,
						&client->dev.kobj, "link_input_dev");

				if (ret == -EEXIST) {
					NVT_ERR("sysfs link %s is already created.\n",
						"link_input_dev");
				} else if (ret == 0) {
					ret = sysfs_create_group(&client->dev.kobj,
							&nvt_api_attribute_group);
					if (ret) {
						NVT_ERR("Failed to create wake up gesture sysfs\n");
					} else {
						NVT_LOG(
							"create gesture_wakeup under /sys/touchscreen/%s Succeeded!\n",
							"link_input_dev");
					}
				} else {
					NVT_ERR("sysfs create link %s failed.\n", "link_input_dev");
				}
			}
		} else {
			NVT_LOG("wkg device is already created\n");
		}
	}

	return 0;
}

static int nvt_i2c_hid_driver_load(struct i2chid_ops *ops, u16 hid_descriptor_address)
{
	int ret = 0;
	struct device_node *np = NULL;
#if IS_ENABLED(CONFIG_HID_TOUCH_METRICS)
	touch_metrics_data.fw_version = DEFAULT_FW_VERSION;
	touch_metrics_data.bootup_status = BOOT_UP_INIT_STATUS;
	touch_metrics_data.fw_upgrade_result = FW_UPGRADE_STATE_INIT;
#endif

	mutex_lock(&probe_lock);
	hid_i2c_dev = &hid_i2c_client->dev;
	np = hid_i2c_client->dev.of_node;

	NVT_LOG("%s : NVT_HID_I2C_DRIVER_VERSION = %s\n", __func__,	NVT_HID_I2C_DRIVER_VERSION);

	if (!ts) {
		ts = kzalloc(sizeof(struct nvt_ts_data), GFP_KERNEL);
		if (ts == NULL) {
			NVT_ERR("failed to allocated memory for nvt ts data\n");
			ret = -ENOSPC;
			goto err_free_ts;
		}
	}
	ts->touch_ready = 0;
	ts->fw_flag = 0xFF;
	ts->pen_serial = PEN_INVALID_SERIAL;

	if (!ts->xbuf) {
		ts->xbuf = kzalloc(NVT_XBUF_LEN, GFP_KERNEL);
		if (ts->xbuf == NULL) {
			NVT_ERR("kzalloc for xbuf failed!\n");
			ret = -ENOSPC;
			goto err_free_ts;
		}
	}

	if (!nvt_ts_hidraw_flash_misc.this_device) {
		ret = misc_register(&nvt_ts_hidraw_flash_misc);
		if (ret < 0) {
			NVT_ERR("Register /dev/%s failed\n", nvt_ts_hidraw_flash_misc.name);
			ret = -ENODEV;
			goto err_free_ts;
		}
		NVT_LOG("create /dev/%s\n", nvt_ts_hidraw_flash_misc.name);
	} else {
		NVT_LOG("/dev/%s is already created\n", nvt_ts_hidraw_flash_misc.name);
	}

	ops->irq_callback = nvt_irq_callback;

	ret = parse_data_from_dts(hid_i2c_client);
	if (ret < 0)
		goto err_free_ts;

	nt36523n_share_tp_resetpin_control(1);
	NVT_LOG("display share tp resetpin control init\n");

	mutex_init(&ts->lock);
	mutex_init(&ts->xbuf_lock);
	ts->client = hid_i2c_client;

	ret = nvt_ts_check_chip_ver_trim_and_idle();
	if (ret) {
		NVT_ERR("chip is not identified\n");
		goto err_free_ts;
	}

	ret = nvt_get_flash_info();
	if (ret) {
		NVT_ERR("failed to get flash info\n");
		goto err_free_ts;
	}

	nvt_get_fw_cus_settings();

	nvt_get_fw_info();

	/* Firmware upgrade before hid probe */
	ret = nvt_fwupg_init(ts);
	if (ret) {
		NVT_ERR("init fw upgrade fail");
		goto err_nvt_fwupg_init_failed;
	}

	nvt_bootloader_reset();

	ret = i2c_hid_core_probe(hid_i2c_client, ops, hid_descriptor_address);
	if (ret) {
		NVT_ERR("i2c_hid_core_probe failed\n");
		goto err_nvt_fwupg_init_failed;
	}

	if (!nvt_pen_input_timestamp_set())
		NVT_LOG("Pen timestamp is not enabled");

	ret = nvt_gesture_dev_and_node_create();
	if (ret) {
		NVT_ERR("nvt add  input device failed. ret=%d\n", ret);
		goto err_i2c_hid_core_probe_failed;
	}

	ret = nvt_extra_proc_init();
	if (ret) {
		NVT_ERR("nvt extra proc init failed. ret=%d\n", ret);
		goto err_extra_proc_init_failed;
	}

	ret = nvt_mp_proc_init();
	if (ret) {
		NVT_ERR("nvt mp proc init failed. ret=%d\n", ret);
		goto err_mp_proc_init_failed;
	}

#if IS_ENABLED(CONFIG_DRM_MEDIATEK)
	ts->disp_notifier.notifier_call = nvt_disp_notifier_callback;
	ret = mtk_disp_notifier_register("NVT Touch", &ts->disp_notifier);
	if (ret)
		NVT_ERR("Failed to register disp notifier client:%d", ret);
#endif
	ts->touch_is_awake = true;

#if DRIVER_ESD_PROTECT
	ret = nvt_esd_protect_init(ts);
	if (ret) {
		NVT_ERR("init fw esd protect fail");
		goto err_nvt_esd_protect_init_failed;
	}
#endif
	mutex_unlock(&probe_lock);

	NVT_LOG("end of probe\n");

	return 0;

#if DRIVER_ESD_PROTECT
err_nvt_esd_protect_init_failed:
	nvt_mp_proc_deinit();
#endif
err_mp_proc_init_failed:
	nvt_extra_proc_deinit();
err_extra_proc_init_failed:
	input_free_device(wkg_input_dev);
	wkg_input_dev = NULL;
err_i2c_hid_core_probe_failed:
	i2c_hid_core_remove(hid_i2c_client);
	hid_i2c_client = NULL;
err_nvt_fwupg_init_failed:
	nvt_fwupg_deinit(ts);

err_free_ts:
	nvt_ts_free();
	mutex_unlock(&probe_lock);
	return ret;
}

int nvt_hid_of_core_probe(struct i2c_client *client, struct i2chid_ops *ops,
		u16 hid_descriptor_address)
{
	int32_t ret = 0;

	if (!client || !ops) {
		NVT_ERR("client or ops is NULL\n");
		return -ENODEV;
	}

	mutex_init(&probe_lock);
	hid_i2c_client = client;

	ret = nvt_i2c_hid_driver_load(ops, hid_descriptor_address);
	if (ret) {
		NVT_ERR("nt36523n driver load failed\n");
		return ret;
	}

	return 0;
}

int nvt_i2c_hid_core_remove(struct i2c_client *client)
{
	struct i2c_hid *ihid = i2c_get_clientdata(client);
	mutex_lock(&probe_lock);
	NVT_LOG("%s\n", __func__);
	ts->touch_ready = 0;
#if DRIVER_ESD_PROTECT
	nvt_esd_protect_deinit(ts);
#endif
	nvt_fwupg_deinit(ts);
	nvt_extra_proc_deinit();
	nvt_mp_proc_deinit();
	if (ihid->hid)
		i2c_hid_core_remove(client);

	nvt_ts_free();

	mutex_unlock(&probe_lock);

	return 0;
}

void nvt_hid_irq_enable(void)
{
	unsigned long flag = 0;

	spin_lock_irqsave(&nvt_irq_spin, flag);

	if (atomic_read(&nvt_irq_stat) == IRQ_ENABLE) {
		goto out;
	} else {
		enable_irq(ts->client->irq);
		atomic_set(&nvt_irq_stat, IRQ_ENABLE);
	}

out:
	spin_unlock_irqrestore(&nvt_irq_spin, flag);
}

void nvt_hid_irq_disable(void)
{
	unsigned long flag = 0;

	spin_lock_irqsave(&nvt_irq_spin, flag);

	if (atomic_read(&nvt_irq_stat) == IRQ_DISABLE) {
		goto out;
	} else {
		disable_irq_nosync(ts->client->irq);
		atomic_set(&nvt_irq_stat, IRQ_DISABLE);
	}

out:
	spin_unlock_irqrestore(&nvt_irq_spin, flag);
}

static int32_t nvt_ts_resume(struct device *dev)
{
	if (ts->touch_is_awake) {
		NVT_LOG("Touch is already resume\n");
		return 0;
	}

#if DRIVER_ESD_PROTECT
	if (ts->nvt_esd_wq) {
		queue_delayed_work(ts->nvt_esd_wq, &ts->nvt_esd_work,
				msecs_to_jiffies(NVT_TOUCH_ESD_CHECK_PERIOD));
	}
#endif

	nvt_hid_irq_enable();

	ts->touch_is_awake = true;

	NVT_LOG("nvt ts resume.\n");
	return 0;
}

static int32_t nvt_ts_suspend(struct device *dev)
{
	int wake_status = 0;

	if (!ts->touch_is_awake) {
		NVT_LOG("Touch is already suspend\n");
		return 0;
	}

#if DRIVER_ESD_PROTECT
	if (ts->nvt_esd_wq)
		cancel_delayed_work_sync(&ts->nvt_esd_work);
#endif

	if (gesture_switch_failed) {
		NVT_LOG("Enable gesture Mode\n");
		if (i2c_hid_command(hid_i2c_client, &hid_wkg_on_cmd, NULL, 0)) {
			NVT_ERR("failed to enable WKG Mode.\n");
		} else {
			gesture_mode = true;
			wake_status = enable_irq_wake(hid_i2c_client->irq);
			if (wake_status)
				NVT_ERR("Failed to enable irq wake\n");
			panel_gesture_mode_set_by_nt36523n(true);
			gesture_switch_failed = false;
		}
	}

	if (!gesture_mode)
		nvt_hid_irq_disable();
	ts->touch_is_awake = false;

	NVT_LOG("nvt ts suspend.\n");
	return 0;
}

void nvt_ts_free(void)
{
	if (ts->xbuf) {
		kfree(ts->xbuf);
		ts->xbuf = NULL;
	}
	if (ts) {
		kfree(ts);
		ts = NULL;
	}
}

#if IS_ENABLED(CONFIG_DRM_MEDIATEK)
int nvt_disp_notifier_callback(struct notifier_block *nb,
		unsigned long value, void *v)
{
	int *data = NULL;
	struct nvt_ts_data *ts = container_of(nb, struct nvt_ts_data, disp_notifier);

	if (ts && v) {
		data = (int *)v;
		if (value == MTK_DISP_EARLY_EVENT_BLANK) {
			if (*data == MTK_DISP_BLANK_POWERDOWN)
				nvt_ts_suspend(&ts->client->dev);
		} else if (value == MTK_DISP_EVENT_BLANK) {
			if (*data == MTK_DISP_BLANK_UNBLANK)
				nvt_ts_resume(&ts->client->dev);
		} else {
			NVT_LOG("value = %lu, not care\n", value);
		}
	} else {
		NVT_ERR("NT36523N touch IC can not suspend or resume");
		return -EINVAL;
	}

	return 0;
}
#endif

EXPORT_SYMBOL_GPL(nvt_hid_of_core_probe);
EXPORT_SYMBOL_GPL(nvt_i2c_hid_core_remove);

MODULE_DESCRIPTION("HID over I2C OF driver");
MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(VFS_internal_I_am_really_a_filesystem_and_am_NOT_a_driver);
