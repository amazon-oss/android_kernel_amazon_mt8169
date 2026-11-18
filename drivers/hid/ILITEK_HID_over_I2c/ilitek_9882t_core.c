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

#include "ilitek_9882t.h"

#define DTS_INT_GPIO "touch,irq-gpio"
#define DTS_RESET_GPIO "touch,reset-gpio"
#define DTS_OF_NAME "tchip,ilitek"

void ili_hid_tp_reset(void)
{
	ILI_INFO("edge delay = %d\n", ilits->rst_edge_delay);

	/* Need accurate power sequence, do not change it to msleep */
	gpio_direction_output(ilits->tp_rst, 1);
	mdelay(1);
	gpio_set_value(ilits->tp_rst, 0);
	mdelay(5);
	gpio_set_value(ilits->tp_rst, 1);
	mdelay(ilits->rst_edge_delay);
}
static int ilitek_plat_gpio_register(void)
{
	int ret = 0;

	return ret;
}

void ili_hid_irq_disable(void)
{
	unsigned long flag;
	struct i2c_client *client = ilits->i2c;

	spin_lock_irqsave(&ilits->irq_spin, flag);

	if (atomic_read(&ilits->irq_stat) == DISABLE)
		goto out;

	if (!client->irq) {
		ILI_ERR("gpio_to_irq (%d) is incorrect\n", client->irq);
		goto out;
	}

	if (atomic_read(&ilits->irq_stat)) {
		disable_irq_nosync(client->irq);
		atomic_set(&ilits->irq_stat, DISABLE);
	}
	ILI_DBG("Disable irq success\n");

out:
	spin_unlock_irqrestore(&ilits->irq_spin, flag);
}

void ili_hid_irq_enable(void)
{
	unsigned long flag;
	struct i2c_client *client = ilits->i2c;

	spin_lock_irqsave(&ilits->irq_spin, flag);

	if (atomic_read(&ilits->irq_stat) == ENABLE)
		goto out;

	if (!client->irq) {
		ILI_ERR("gpio_to_irq (%d) is incorrect\n", client->irq);
		goto out;
	}

	if (!(atomic_read(&ilits->irq_stat))) {
		enable_irq(client->irq);
		atomic_set(&ilits->irq_stat, ENABLE);
	}
	ILI_DBG("Enable irq success\n");

out:
	spin_unlock_irqrestore(&ilits->irq_spin, flag);
}

static irqreturn_t ilitek_plat_isr_top_half(int irq, void *dev_id)
{
	if (irq != ilits->irq_num) {
		ILI_ERR("Incorrect irq number (%d)\n", irq);
		return IRQ_NONE;
	}

	if (atomic_read(&ilits->cmd_int_check) == ENABLE) {
		atomic_set(&ilits->cmd_int_check, DISABLE);
		ILI_DBG("CMD INT detected, ignore\n");
		wake_up(&(ilits->inq));
		return IRQ_HANDLED;
	}

	if (ilits->prox_near) {
		ILI_INFO("Proximity event, ignore interrupt!\n");
		return IRQ_HANDLED;
	}

	ILI_DBG("report: %d, rst: %d, fw: %d, switch: %d, mp: %d, sleep: %d, esd: %d, igr:%d\n",
		ilits->report, atomic_read(&ilits->tp_reset),
		atomic_read(&ilits->fw_stat), atomic_read(&ilits->tp_sw_mode),
		atomic_read(&ilits->mp_stat), atomic_read(&ilits->tp_sleep),
		atomic_read(&ilits->esd_stat),
		atomic_read(&ilits->ignore_report));

	if (!ilits->report || atomic_read(&ilits->tp_reset) ||
	    atomic_read(&ilits->ignore_report) ||
	    atomic_read(&ilits->fw_stat) || atomic_read(&ilits->tp_sw_mode) ||
	    atomic_read(&ilits->mp_stat) || atomic_read(&ilits->tp_sleep) ||
	    atomic_read(&ilits->esd_stat)) {
		ILI_DBG("ignore interrupt !\n");
		return IRQ_HANDLED;
	}

	return IRQ_WAKE_THREAD;
}

static irqreturn_t ilitek_plat_isr_bottom_half(int irq, void *dev_id)
{
	if (mutex_is_locked(&ilits->touch_mutex)) {
		ILI_DBG("touch is locked, ignore\n");
		return IRQ_HANDLED;
	}
	mutex_lock(&ilits->touch_mutex);
	mutex_unlock(&ilits->touch_mutex);
	return IRQ_HANDLED;
}

void ili_hid_irq_unregister(void)
{
	devm_free_irq(ilits->dev, ilits->irq_num, NULL);
}

int ili_hid_irq_register(int type)
{
	int ret = 0;
	static bool get_irq_pin;

	atomic_set(&ilits->irq_stat, DISABLE);

	if (get_irq_pin == false) {
		ilits->irq_num = gpio_to_irq(ilits->tp_int);
		get_irq_pin = true;
	}

	ILI_INFO("ilits->irq_num = %d\n", ilits->irq_num);

	ret = devm_request_threaded_irq(ilits->dev, ilits->irq_num,
					ilitek_plat_isr_top_half,
					ilitek_plat_isr_bottom_half,
					type | IRQF_ONESHOT, "ilitek", NULL);

	if (type == IRQF_TRIGGER_FALLING)
		ILI_INFO("IRQ TYPE = IRQF_TRIGGER_FALLING\n");
	if (type == IRQF_TRIGGER_RISING)
		ILI_INFO("IRQ TYPE = IRQF_TRIGGER_RISING\n");

	if (ret != 0)
		ILI_ERR("Failed to register irq handler, irq = %d, ret = %d\n",
			ilits->irq_num, ret);

	atomic_set(&ilits->irq_stat, ENABLE);

	return ret;
}
#if 1
#if SPRD_SYSFS_SUSPEND_RESUME
static ssize_t ts_suspend_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", ilits->tp_suspend ? "true" : "false");
}

static ssize_t ts_suspend_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	if ((buf[0] == '1') && !ilits->tp_suspend)
		ili_hid_sleep_handler(TP_DEEP_SLEEP);
	else if ((buf[0] == '0') && ilits->tp_suspend)
		ili_hid_sleep_handler(TP_RESUME);

	return count;
}
static DEVICE_ATTR_RW(ts_suspend);

static struct attribute *ilitek_dev_suspend_atts[] = {
	&dev_attr_ts_suspend.attr, NULL
};

static const struct attribute_group ilitek_dev_suspend_atts_group = {
	.attrs = ilitek_dev_suspend_atts,
};

static const struct attribute_group *ilitek_dev_attr_groups[] = {
	&ilitek_dev_suspend_atts_group, NULL
};

int ili_sysfs_add_device(struct device *dev)
{
	int ret = 0, i;

	for (i = 0; ilitek_dev_attr_groups[i]; i++) {
		ret = sysfs_create_group(&dev->kobj, ilitek_dev_attr_groups[i]);
		if (ret) {
			while (--i >= 0) {
				sysfs_remove_group(&dev->kobj,
						   ilitek_dev_attr_groups[i]);
			}
			break;
		}
	}

	return ret;
}

int ili_sysfs_remove_device(struct device *dev)
{
	int i;

	sysfs_remove_link(NULL, "touchscreen");
	for (i = 0; ilitek_dev_attr_groups[i]; i++) {
		sysfs_remove_group(&dev->kobj, ilitek_dev_attr_groups[i]);
	}

	return 0;
}
#endif // 1
#if IS_ENABLED(CONFIG_DRM_MEDIATEK)
static int ilitek9882t_ts_disp_notifier_callback(struct notifier_block *self,
						 unsigned long event,
						 void *value)
{
	struct ilitek_ts_hid_data *ilits =
		container_of(self, struct ilitek_ts_hid_data, disp_notifier);
	int *data = (int *)value;

	if (ilits && value) {
		if (event == MTK_DISP_EVENT_BLANK) {
			if (*data == MTK_DISP_BLANK_UNBLANK) {
				ILI_INFO("ili_ts_resume IN");
				if (ili_hid_sleep_handler(TP_RESUME) < 0) {
					ILI_ERR("TP resume failed\n");
					return -1;
				}
			}
		} else if (event == MTK_DISP_EARLY_EVENT_BLANK) {
			if (*data == MTK_DISP_BLANK_POWERDOWN) {
				ILI_INFO("ili_ts_suspend IN");
				if (ili_hid_sleep_handler(TP_SUSPEND) < 0) {
					ILI_ERR("TP suspend failed\n");
					return -1;
				}
			}
		} else {
			ILI_ERR("Invalid notification callback event");
			return -1;
		}
	} else {
		ILI_ERR("ilitek9882t touch IC can not suspend or resume");
		return -1;
	}
	return 0;
}
#else
static int ilitek_plat_notifier_fb(struct notifier_block *self,
				   unsigned long event, void *data)
{
	int *blank;
	struct fb_event *evdata = data;

	ILI_INFO("Notifier's event = %ld\n", event);

	/*
	 *	FB_EVENT_BLANK(0x09): A hardware display blank change occurred.
	 *	FB_EARLY_EVENT_BLANK(0x10): A hardware display blank early change occurred.
	 */
	if (evdata && evdata->data) {
		blank = evdata->data;
		switch (*blank) {
#ifdef CONFIG_DRM_MSM
		case MSM_DRM_BLANK_POWERDOWN:
#else
		case FB_BLANK_POWERDOWN:
#endif
#if CONFIG_PLAT_SPRD
		case DRM_MODE_DPMS_OFF:
#endif /* CONFIG_PLAT_SPRD */
			if (TP_SUSPEND_PRIO) {
#ifdef CONFIG_DRM_MSM
				if (event != MSM_DRM_EARLY_EVENT_BLANK)
#else
				if (event != FB_EARLY_EVENT_BLANK)
#endif
					return NOTIFY_DONE;
			} else {
#ifdef CONFIG_DRM_MSM
				if (event != MSM_DRM_EVENT_BLANK)
#else
				if (event != FB_EVENT_BLANK)
#endif
					return NOTIFY_DONE;
			}
			if (ili_hid_sleep_handler(TP_DEEP_SLEEP) < 0)
				ILI_ERR("TP suspend failed\n");
			break;
#ifdef CONFIG_DRM_MSM
		case MSM_DRM_BLANK_UNBLANK:
		case MSM_DRM_BLANK_NORMAL:
#else
		case FB_BLANK_UNBLANK:
		case FB_BLANK_NORMAL:
#endif

#if CONFIG_PLAT_SPRD
		case DRM_MODE_DPMS_ON:
#endif /* CONFIG_PLAT_SPRD */

#ifdef CONFIG_DRM_MSM
			if (event == MSM_DRM_EVENT_BLANK)
#else
			if (event == FB_EVENT_BLANK)
#endif
			{
				if (ili_hid_sleep_handler(TP_RESUME) < 0)
					ILI_ERR("TP resume failed\n");
			}
			break;
		default:
			ILI_ERR("Unknown event, blank = %d\n", *blank);
			break;
		}
	}
	return NOTIFY_OK;
}
#endif /*#ifdef __DRM_PANEL_H__*/

#endif

int ilitek_notifier_register(void)
{
	int ret = 0;
	ILI_INFO("%s\n", __func__);

#if IS_ENABLED(CONFIG_DRM_MEDIATEK)
	ilits->disp_notifier.notifier_call =
		ilitek9882t_ts_disp_notifier_callback;
	ret = mtk_disp_notifier_register("ilitek ic Touch",
					 &ilits->disp_notifier);
	if (ret) {
		ILI_ERR("Failed to register disp notifier client:%d", ret);
		return ret;
	}
	ilits->disp_notifier_status = 0;
	ILI_INFO("TP_notifier register on disp_notifier chain\n");
#endif
	return ret;
}

void ilitek_notifier_unregister(void)
{
	ILI_INFO("Unregister TP_notifier on disp_notifier chain\n");
#if IS_ENABLED(CONFIG_DRM_MEDIATEK)
	if (!ilits->disp_notifier_status)
		mtk_disp_notifier_unregister(&ilits->disp_notifier);
	ilits->disp_notifier.notifier_call = NULL;
#endif
}

int ilitek_hid_plat_probe(void)
{
	ILI_INFO("platform probe\n");
	if (ilitek_plat_gpio_register() < 0)
		ILI_ERR("Register gpio failed\n");

	//ili_hid_irq_register(ilits->irq_tirgger_type);
	atomic_set(&ilits->irq_stat, ENABLE);

	if (ili_hid_tddi_init() < 0) {
		ILI_ERR("ILITEK Driver probe failed\n");
		//ili_hid_irq_unregister();
		ili_hid_dev_remove(DISABLE);
		return -ENODEV;
	}

	ilits->pm_suspend = false;
	init_completion(&ilits->pm_completion);

	ILI_INFO("ILITEK Driver loaded successfully!#\n");
	return 0;
}

static int ilitek_tp_pm_suspend(struct device *dev)
{
	ILI_INFO("CALL BACK TP PM SUSPEND");
	ilits->pm_suspend = true;
#if KERNEL_VERSION(3, 12, 0) >= LINUX_VERSION_CODE
	ilits->pm_completion.done = 0;
#else
	reinit_completion(&ilits->pm_completion);
#endif
	return 0;
}

static int ilitek_tp_pm_resume(struct device *dev)
{
	ILI_INFO("CALL BACK TP PM RESUME");
	ilits->pm_suspend = false;
	complete(&ilits->pm_completion);
	return 0;
}

int ilitek_plat_remove(struct i2c_client *client)
{
	ILI_INFO("remove plat dev\n");
	sysfs_remove_link(NULL, "touchscreen");
	ili_hid_dev_remove(ENABLE);
	return i2c_hid_core_remove(client);
}
EXPORT_SYMBOL_GPL(ilitek_plat_remove);

static const struct dev_pm_ops tp_pm_ops = {
	.suspend = ilitek_tp_pm_suspend,
	.resume = ilitek_tp_pm_resume,
};

static const struct of_device_id tp_match_table[] = {
	{ .compatible = DTS_OF_NAME },
	{},
};

#ifdef ROI
struct ts_device_ops ilitek_ops = {
	.chip_roi_rawdata = ili_knuckle_roi_rawdata,
	.chip_roi_switch = ili_knuckle_roi_switch,
};
#endif

struct ilitek_hwif_info hwif = {
	.bus_type = TDDI_INTERFACE,
	.plat_type = TP_PLAT_QCOM,
	.owner = THIS_MODULE,
	.name = TDDI_DEV_ID,
	.of_match_table = of_match_ptr(tp_match_table),
	//.plat_probe = ilitek_hid_plat_probe,
	.pm = &tp_pm_ops,
};

//static int __init ilitek_hid_plat_dev_init(void)
int ilitek_hid_plat_dev_init(struct i2c_client *client)
{
	ILI_INFO("ILITEK TP driver init for MTK\n");
	if (ili_hid_dev_init(&hwif, client) < 0) {
		ILI_ERR("Failed to register i2c/spi bus driver\n");
		return -ENODEV;
	}
	return 0;
}

void ilitek_hid_plat_dev_exit(void)
{
	ILI_INFO("remove plat dev\n");
	ili_hid_dev_remove(ENABLE);
}

#if 0
#ifdef __DRM_PANEL_H__
late_initcall(ilitek_hid_plat_dev_init);
#else
module_init(ilitek_hid_plat_dev_init);
#endif
module_exit(ilitek_hid_plat_dev_exit);
MODULE_AUTHOR("ILITEK");
MODULE_LICENSE("GPL");
#endif
