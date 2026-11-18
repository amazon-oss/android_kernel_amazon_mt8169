/* SPDX-License-Identifier: GPL-2.0 */
/*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*/
#include <linux/reboot.h>

#ifndef __LINUX_PANEL_COMMON_H__
#define __LINUX_PANEL_COMMON_H__

#define LCD_DEBUG_EN 1
#if LCD_DEBUG_EN
#define LCM_DEBUG(fmt, args...) do { \
    printk("[KERNEL/LCM]%s:"fmt"\n", __func__, ##args); \
} while (0)

#define LCM_FUNC_ENTER() do { \
    printk("[KERNEL/LCM]%s: Enter\n", __func__); \
} while (0)

#define LCM_FUNC_EXIT() do { \
    printk("[KERNEL/LCM]%s: Exit(%d)\n", __func__, __LINE__); \
} while (0)
#else /* #if LCD_DEBUG_EN*/
#define LCM_DEBUG(fmt, args...)
#define LCM_FUNC_ENTER()
#define LCM_FUNC_EXIT()
#endif

#define LCM_INFO(fmt, args...) do { \
    printk(KERN_INFO "[KERNEL/LCM]]%s:"fmt"\n", __func__, ##args); \
} while (0)

#define LCM_ERROR(fmt, args...) do { \
    printk(KERN_ERR "[[KERNEL/LCM]]%s:"fmt"\n", __func__, ##args); \
} while (0)

#define LCM_FT8205_STARRY_BOE       5
#define LCM_ILI9882T_TG_HSD         6
#define LCM_FT8205_KD_INX           9
#define LCM_ILI9882T_KD_INX         10
#define LCM_NT36523N_TG_BOE         12
#define LCM_ILI9882T_STARRY_BOE     14
#define LCM_VENDOR_ID_UNKNOWN       0xFF

/*
 * power on sequnce: vsim1/AVDD/AVEE -> LP11 -> RESET high
 * the LP11 will be enable after resume_power() and before resume()
 * so   set vsim1/AVDD/AVEE at resume_power() //TRS_ON_PHASE1
 * then set RESET at resume()                 //TRS_ON_PHASE2
*/
#define TRS_OFF_TO_ON        0
#define TRS_OFF              1
#define TRS_ON_PHASE1        2
#define TRS_ON_PHASE2        3
#define TRS_DSB              4
#define TRS_DSB_TO_ON        5
#define TRS_DSB_TO_OFF       6
#define TRS_KEEP_CUR         0xFD /* keep current state */
#define TRS_NOT_PSB          0xFE /* not possible */

#define LCM_OFF             0
#define LCM_ON              1
#define LCM_ON1             2
#define LCM_ON2             3
#define LCM_DSB             4

#define REGFLAG_DELAY               0xFFFC
#define REGFLAG_END_OF_TABLE        0xFFFD

struct LCM_setting_table {
	unsigned int cmd;
	unsigned char count;
	unsigned char para_list[64];
};

struct panel_info {
	struct drm_panel base;
	struct mipi_dsi_device *dsi;
	struct backlight_device *backlight;
	struct device *dev;
	const struct panel_desc *desc;
	bool enabled;

	enum drm_panel_orientation orientation;
	struct regulator *vsim1;
	struct gpio_desc *avee;
	struct gpio_desc *avdd;
	struct gpio_desc *lcm_rst_gpio;
	struct gpio_desc *ctp_rst_gpio;

	bool prepared_power;
	bool prepared;
};

#endif /* __LINUX_PANEL_COMMON_H__ */
