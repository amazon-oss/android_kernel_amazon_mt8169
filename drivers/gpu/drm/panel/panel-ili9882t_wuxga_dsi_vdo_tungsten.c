// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2022 MediaTek Inc.
 */
#include <linux/backlight.h>
#include <linux/delay.h>
#include <linux/gpio/consumer.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/regulator/consumer.h>
#include <linux/i2c.h>
#include "bias_i2c.h"

#include <drm/drm_connector.h>
#include <drm/drm_crtc.h>
#include <drm/drm_mipi_dsi.h>
#include <drm/drm_panel.h>

#include <video/mipi_display.h>
#include "panel_common.h"

#define CONFIG_MTK_PANEL_EXT
#if defined(CONFIG_MTK_PANEL_EXT)
#include "../mediatek/mediatek_v2/mtk_panel_ext.h"
#include "../mediatek/mediatek_v2/mtk_log.h"
#include "../mediatek/mediatek_v2/mtk_drm_graphics_base.h"
#endif

#define FRAME_WIDTH           (1200)
#define FRAME_HEIGHT          (1920)
/* KD INX panel porch */
#define KD_INX_HFP (80)    // params->dsi.horizontal_frontporch
#define KD_INX_HSA (80)    // params->dsi.horizontal_sync_active
#define KD_INX_HBP (80)    // params->dsi.horizontal_backporch
#define KD_INX_VFP (64)    // params->dsi.vertical_frontporch
#define KD_INX_VSA (4)     // params->dsi.vertical_sync_active
#define KD_INX_VBP (34)    // params->dsi.vertical_backporch

#define TG_HSD_HFP (80)    // params->dsi.horizontal_frontporch
#define TG_HSD_HSA (82)    // params->dsi.horizontal_sync_active
#define TG_HSD_HBP (80)    // params->dsi.horizontal_backporch
#define TG_HSD_VFP (58)    // params->dsi.vertical_frontporch
#define TG_HSD_VSA (2)     // params->dsi.vertical_sync_active
#define TG_HSD_VBP (40)    // params->dsi.vertical_backporch

#define STARRY_BOE_HFP (80)    // params->dsi.horizontal_frontporch
#define STARRY_BOE_HSA (83)    // params->dsi.horizontal_sync_active
#define STARRY_BOE_HBP (80)    // params->dsi.horizontal_backporch
#define STARRY_BOE_VFP (54)    // params->dsi.vertical_frontporch
#define STARRY_BOE_VSA (2)     // params->dsi.vertical_sync_active
#define STARRY_BOE_VBP (44)    // params->dsi.vertical_backporch

#define PHYSICAL_WIDTH			(135)
#define PHYSICAL_HEIGHT			(216)

static const struct drm_display_mode kd_inx_mode = {
	.clock = (FRAME_WIDTH + KD_INX_HFP + KD_INX_HSA + KD_INX_HBP) *
		(FRAME_HEIGHT + KD_INX_VFP + KD_INX_VSA + KD_INX_VBP) *
		60 / 1000,
	.hdisplay = FRAME_WIDTH,
	.hsync_start = FRAME_WIDTH + KD_INX_HFP,
	.hsync_end = FRAME_WIDTH + KD_INX_HFP + KD_INX_HSA,
	.htotal = FRAME_WIDTH + KD_INX_HFP + KD_INX_HSA + KD_INX_HBP,
	.vdisplay = FRAME_HEIGHT,
	.vsync_start = FRAME_HEIGHT + KD_INX_VFP,
	.vsync_end = FRAME_HEIGHT + KD_INX_VFP + KD_INX_VSA,
	.vtotal = FRAME_HEIGHT + KD_INX_VFP + KD_INX_VSA + KD_INX_VBP,
};

static const struct drm_display_mode tg_hsd_mode = {
	.clock = (FRAME_WIDTH + TG_HSD_HFP + TG_HSD_HSA + TG_HSD_HBP) *
		(FRAME_HEIGHT + TG_HSD_VFP + TG_HSD_VSA + TG_HSD_VBP) *
		60 / 1000,
	.hdisplay = FRAME_WIDTH,
	.hsync_start = FRAME_WIDTH + TG_HSD_HFP,
	.hsync_end = FRAME_WIDTH + TG_HSD_HFP + TG_HSD_HSA,
	.htotal = FRAME_WIDTH + TG_HSD_HFP + TG_HSD_HSA + TG_HSD_HBP,
	.vdisplay = FRAME_HEIGHT,
	.vsync_start = FRAME_HEIGHT + TG_HSD_VFP,
	.vsync_end = FRAME_HEIGHT + TG_HSD_VFP + TG_HSD_VSA,
	.vtotal = FRAME_HEIGHT + TG_HSD_VFP + TG_HSD_VSA + TG_HSD_VBP,
};

static const struct drm_display_mode starry_boe_mode = {
	.clock = (FRAME_WIDTH + STARRY_BOE_HFP + STARRY_BOE_HSA + STARRY_BOE_HBP) *
		(FRAME_HEIGHT + STARRY_BOE_VFP + STARRY_BOE_VSA + STARRY_BOE_VBP) *
		60 / 1000,
	.hdisplay = FRAME_WIDTH,
	.hsync_start = FRAME_WIDTH + STARRY_BOE_HFP,
	.hsync_end = FRAME_WIDTH + STARRY_BOE_HFP + STARRY_BOE_HSA,
	.htotal = FRAME_WIDTH + STARRY_BOE_HFP + STARRY_BOE_HSA + STARRY_BOE_HBP,
	.vdisplay = FRAME_HEIGHT,
	.vsync_start = FRAME_HEIGHT + STARRY_BOE_VFP,
	.vsync_end = FRAME_HEIGHT + STARRY_BOE_VFP + STARRY_BOE_VSA,
	.vtotal = FRAME_HEIGHT + STARRY_BOE_VFP + STARRY_BOE_VSA + STARRY_BOE_VBP,
};

static atomic_t gesture_status = ATOMIC_INIT(0);
static atomic_t touch_upgrade_status = ATOMIC_INIT(0);
static unsigned char vendor_id = 0xFF;
static int old_state = LCM_OFF;
static atomic_t g_power_request = ATOMIC_INIT(0);
static struct panel_info *share_info;

static atomic_t g_power_rebooting = ATOMIC_INIT(0);

static void get_lcm_id(void);
static bool panel_check_gesture_mode(void);
static bool panel_check_touch_upgrade_mode(void);
static void lcm_power_manager_lock(struct panel_info *panel, int pwr_state);
static void lcm_power_manager(struct panel_info *panel, int new_state);
#if IS_ENABLED(CONFIG_IDME)
extern unsigned int idme_get_lcmid_info(void);
#endif

static DEFINE_MUTEX(ili9882t_share_power_mutex);
/* trans_table[old_state][new_state]; */
static int trans_table[][5] = {
/*            LCM_OFF         LCM_ON         LCM_ON1        LCM_ON2               LCM_DSB   */
/* LCM_OFF */{TRS_KEEP_CUR,   TRS_OFF_TO_ON, TRS_ON_PHASE1, TRS_NOT_PSB,    TRS_NOT_PSB},
/* LCM_ON  */{TRS_OFF,        TRS_KEEP_CUR,  TRS_NOT_PSB,   TRS_KEEP_CUR,   TRS_DSB},
/* LCM_ON1 */{TRS_NOT_PSB,    TRS_NOT_PSB,   TRS_NOT_PSB,   TRS_ON_PHASE2,  TRS_NOT_PSB},
/* LCM_ON2 */{TRS_OFF,        TRS_KEEP_CUR,  TRS_NOT_PSB,   TRS_KEEP_CUR,   TRS_DSB},
/* LCM_DSB */{TRS_DSB_TO_OFF, TRS_DSB_TO_ON, TRS_KEEP_CUR,  TRS_DSB_TO_ON,  TRS_KEEP_CUR}
};

/*
 * different states:position in power on/off and suspend/resume sequence
 * LCM_OFF: fully powered off, disable all power supplies of the panel,include vsim1/avdd/avee
 * LCM_ON:  fully powered on, the state of the first boot to the kernel
 * LCM_ON2: execute the power on timing of LCM and TP reset, and send init code
 * LCM_ON1: enable vsim1, avdd and avee
 * LCM_DSB: send display-off/sleep-in code, pull down LCM/TP reset pin(no gesture mode)
 *==================================================================================================
 * first power on    power off      normal suspend   normal resume  gesture suspend   gesture resume
 *
 *  -LCM_OFF-    -LCM_ON/LCM_ON2-  -LCM_ON/LCM_ON2-    -LCM_OFF-    -LCM_ON/LCM_ON2-     -LCM_DSB-
 *     ||               ||               ||               ||               ||                ||
 *     \/               \/               \/               \/               \/                \/
 *  -LCM_ON-         -LCM_DSB-        -LCM_DSB-        -LCM_ON1-        -LCM_DSB-         -LCM_ON2-
 *                      ||               ||               ||
 *                      \/               \/               \/
 *                   -LCM_OFF-        -LCM_OFF-        -LCM_ON2-
 *==================================================================================================
 */
static char *lcm_state_str[] = {
	"LCM_OFF",
	"LCM_ON",
	"LCM_ON1",
	"LCM_ON2",
	"LCM_DSB",
};

static int ili9882t_reboot(struct notifier_block *notifier, unsigned long val, void *v)
{
	pr_notice("[KERNEL/LCM]: panel ili9882t rebooting\n");
	atomic_set(&g_power_rebooting, 1);
	return NOTIFY_OK;
}

static struct notifier_block ili9882t_reboot_notifier = {
	.notifier_call = ili9882t_reboot,
};

static void get_lcm_id(void)
{
	unsigned int lcm_id = 0xFF;
#if IS_ENABLED(CONFIG_IDME)
	lcm_id = idme_get_lcmid_info();
	if (lcm_id == 0xFF)
		pr_err("get lcm_id failed.\n");
#else
	pr_notice("%s, idme is not ready.\n", __func__);
#endif
	vendor_id = (unsigned char)lcm_id;
	pr_notice("[ili9882t] %s, vendor id = 0x%x\n", __func__, vendor_id);
}

static struct LCM_setting_table lcm_suspend_setting[] = {
	{REGFLAG_DELAY, 1, {}},
	{0xFF, 3, {0x98, 0x82, 0x00}},
	{0x28, 0, {}},
	{REGFLAG_DELAY, 20, {}},
	{0x10, 0, {}},
	{REGFLAG_DELAY, 85, {}},
	{0x00, 0, {}},
};

static struct LCM_setting_table gesture_mode_sleep_cmd[] = {
	{0xFF, 3, {0x98, 0x82, 0x06}},
	{0xD0, 1, {0x0A}},
	{0xD1, 1, {0x02}},
	{0x03, 1, {0xF0}},
	{REGFLAG_DELAY, 1, {}},
	{0xD1, 1, {0x00}},
	{0xFF, 3, {0x98, 0x82, 0x00}},
	{0x00, 0, {}},
};

static struct LCM_setting_table lcm_init_ili9882t_kd_inx[] = {
	{0xFF, 3, {0x98, 0x82, 0x01}},
	{0x00, 1, {0xC2}},
	{0x01, 1, {0x33}},
	{0x02, 1, {0x00}},
	{0x03, 1, {0x00}},
	{0x04, 1, {0x02}},
	{0x05, 1, {0x33}},
	{0x06, 1, {0x00}},
	{0x07, 1, {0x00}},
	{0x24, 1, {0xC8}},
	{0x25, 1, {0x9A}},
	{0x26, 1, {0x00}},
	{0x27, 1, {0x00}},
	{0x08, 1, {0x82}},
	{0x09, 1, {0x83}},
	{0x0A, 1, {0xB3}},
	{0x0B, 1, {0x00}},
	{0x0C, 1, {0x0A}},
	{0x0E, 1, {0x00}},
	{0x16, 1, {0x82}},
	{0x17, 1, {0x83}},
	{0x18, 1, {0x33}},
	{0x19, 1, {0x00}},
	{0x1A, 1, {0x0A}},
	{0x1C, 1, {0x00}},
	{0x28, 1, {0x86}},
	{0x29, 1, {0x89}},
	{0x2A, 1, {0x87}},
	{0x2B, 1, {0x8A}},
	{0xEE, 1, {0x14}},
	{0x2C, 1, {0x34}},
	{0x2D, 1, {0x43}},
	{0x31, 1, {0x07}},
	{0x32, 1, {0x07}},
	{0x33, 1, {0x07}},
	{0x34, 1, {0x24}},
	{0x35, 1, {0x00}},
	{0x36, 1, {0x00}},
	{0x37, 1, {0x23}},
	{0x38, 1, {0x29}},
	{0x39, 1, {0x28}},
	{0x3A, 1, {0x09}},
	{0x3B, 1, {0x0B}},
	{0x3C, 1, {0x0D}},
	{0x3D, 1, {0x0F}},
	{0x3E, 1, {0x11}},
	{0x3F, 1, {0x13}},
	{0x40, 1, {0x15}},
	{0x41, 1, {0x17}},
	{0x42, 1, {0x19}},
	{0x43, 1, {0x1B}},
	{0x44, 1, {0x21}},
	{0x45, 1, {0x07}},
	{0x46, 1, {0x07}},
	{0x47, 1, {0x07}},
	{0x48, 1, {0x07}},
	{0x49, 1, {0x07}},
	{0x4A, 1, {0x24}},
	{0x4B, 1, {0x00}},
	{0x4C, 1, {0x00}},
	{0x4D, 1, {0x22}},
	{0x4E, 1, {0x29}},
	{0x4F, 1, {0x28}},
	{0x50, 1, {0x08}},
	{0x51, 1, {0x0A}},
	{0x52, 1, {0x0C}},
	{0x53, 1, {0x0E}},
	{0x54, 1, {0x10}},
	{0x55, 1, {0x12}},
	{0x56, 1, {0x14}},
	{0x57, 1, {0x16}},
	{0x58, 1, {0x18}},
	{0x59, 1, {0x1A}},
	{0x5A, 1, {0x20}},
	{0x5B, 1, {0x07}},
	{0x5C, 1, {0x07}},
	{0x61, 1, {0x07}},
	{0x62, 1, {0x07}},
	{0x63, 1, {0x07}},
	{0x64, 1, {0x24}},
	{0x65, 1, {0x00}},
	{0x66, 1, {0x00}},
	{0x67, 1, {0x23}},
	{0x68, 1, {0x29}},
	{0x69, 1, {0x28}},
	{0x6A, 1, {0x09}},
	{0x6B, 1, {0x0B}},
	{0x6C, 1, {0x0D}},
	{0x6D, 1, {0x0F}},
	{0x6E, 1, {0x11}},
	{0x6F, 1, {0x13}},
	{0x70, 1, {0x15}},
	{0x71, 1, {0x17}},
	{0x72, 1, {0x19}},
	{0x73, 1, {0x1B}},
	{0x74, 1, {0x21}},
	{0x75, 1, {0x07}},
	{0x76, 1, {0x07}},
	{0x77, 1, {0x07}},
	{0x78, 1, {0x07}},
	{0x79, 1, {0x07}},
	{0x7A, 1, {0x24}},
	{0x7B, 1, {0x00}},
	{0x7C, 1, {0x00}},
	{0x7D, 1, {0x22}},
	{0x7E, 1, {0x29}},
	{0x7F, 1, {0x28}},
	{0x80, 1, {0x08}},
	{0x81, 1, {0x0A}},
	{0x82, 1, {0x0C}},
	{0x83, 1, {0x0E}},
	{0x84, 1, {0x10}},
	{0x85, 1, {0x12}},
	{0x86, 1, {0x14}},
	{0x87, 1, {0x16}},
	{0x88, 1, {0x18}},
	{0x89, 1, {0x1A}},
	{0x8A, 1, {0x20}},
	{0x8B, 1, {0x07}},
	{0x8C, 1, {0x07}},
	{0xB9, 1, {0x40}},
	{0xC0, 1, {0x1C}},
	{0xC1, 1, {0x2A}},
	{0xC2, 1, {0x00}},
	{0xC5, 1, {0x0B}},
	{0xC6, 1, {0x6F}},
	{0xC7, 1, {0x00}},
	{0xCE, 1, {0x71}},
	{0xB0, 1, {0x33}},
	{0xB1, 1, {0x33}},
	{0xB2, 1, {0x00}},
	{0xD1, 1, {0x11}},
	{0xE6, 1, {0x33}},
	{0xD3, 1, {0x01}},
	{0xD6, 1, {0x55}},
	{0xD7, 1, {0x54}},
	{0xD8, 1, {0x10}},
	{0xDA, 1, {0xAA}},
	{0xDB, 1, {0x0A}},
	{0xDC, 1, {0x28}},
	{0xDE, 1, {0x25}},
	{0xDD, 1, {0x08}},
	{0xDF, 1, {0x26}},
	{0xE0, 1, {0x95}},
	{0xE2, 1, {0x02}},
	{0xF7, 1, {0x71}},
	{0xD0, 1, {0x01}},
	{0xE3, 1, {0x3F}},
	{0xE4, 1, {0x36}},
	{0xE5, 1, {0x80}},
	{0xFF, 3, {0x98, 0x82, 0x02}},
	{0x46, 1, {0xF0}},
	{0x29, 1, {0x3A}},
	{0x2A, 1, {0x3B}},
	{0x3A, 1, {0x24}},
	{0x3B, 1, {0x40}},
	{0x39, 1, {0x11}},
	{0x3C, 1, {0x07}},
	{0x3D, 1, {0x01}},
	{0x06, 1, {0xFF}},
	{0x07, 1, {0x01}},
	{0x08, 1, {0x24}},
	{0x09, 1, {0x40}},
	{0x53, 1, {0x1F}},
	{0x5E, 1, {0x40}},
	{0x84, 1, {0x00}},
	{0xFF, 3, {0x98, 0x82, 0x03}},
	{0x20, 1, {0x01}},
	{0x21, 1, {0x3C}},
	{0x22, 1, {0xFA}},
	{0xFF, 3, {0x98, 0x82, 0x12}},
	{0x87, 1, {0x2C}},
	{0xFF, 3, {0x98, 0x82, 0x05}},
	{0x7F, 1, {0xBB}},
	{0x79, 1, {0x7C}},
	{0x69, 1, {0x92}},
	{0x6A, 1, {0x92}},
	{0x5F, 1, {0x01}},
	{0x62, 1, {0x3F}},
	{0x4F, 1, {0x2A}},
	{0xA7, 1, {0xFD}},
	{0x5D, 1, {0x02}},
	{0x73, 1, {0x95}},
	{0x74, 1, {0x95}},
	{0x75, 1, {0x95}},
	{0x76, 1, {0x95}},
	{0x77, 1, {0x17}},
	{0x6D, 1, {0x7E}},
	{0x6E, 1, {0x7E}},
	{0x6F, 1, {0x7E}},
	{0x70, 1, {0x7E}},
	{0x71, 1, {0x3D}},
	{0x9C, 1, {0x02}},
	{0x86, 1, {0x87}},
	{0xFF, 3, {0x98, 0x82, 0x06}},
	{0xC0, 1, {0x80}},
	{0xC1, 1, {0x07}},
	{0xCA, 1, {0x58}},
	{0xCB, 1, {0x02}},
	{0xCE, 1, {0x58}},
	{0xCF, 1, {0x02}},
	{0x10, 1, {0x00}},
	{0xD6, 1, {0x55}},
	{0xDC, 1, {0x38}},
	{0xD3, 1, {0x08}},
	{0x94, 1, {0xB0}},
	{0x92, 1, {0x22}},
	{0xFF, 3, {0x98, 0x82, 0x07}},
	{0x64, 1, {0xBB}},
	{0x84, 1, {0x03}},
	{0xC0, 1, {0x43}},
	{0xCB, 1, {0xC9}},
	{0xCC, 1, {0xB6}},
	{0xCD, 1, {0x98}},
	{0xCE, 1, {0x79}},
	{0xFF, 3, {0x98, 0x82, 0x08}},
	{0xE0, 23, {0x00, 0x0B, 0x1F, 0x3D, 0x4E, 0x43, 0x56, 0x59, 0x66, 0x66, 0x85, 0x8F, 0x97, 0xAF, 0xAA, 0xB2, 0xBB, 0xCE, 0xC7, 0xC0, 0xD9, 0xE5, 0xDF}},
	{0xE1, 23, {0x00, 0x0B, 0x1F, 0x3D, 0x4E, 0x43, 0x56, 0x59, 0x66, 0x66, 0x85, 0x8F, 0x97, 0xAF, 0xAA, 0xB2, 0xBB, 0xCE, 0xC7, 0xC0, 0xD9, 0xE5, 0xF0}},
	{0xFF, 3, {0x98, 0x82, 0x15}},
	{0x31, 1, {0xBB}},
	{0x33, 1, {0xBB}},
	{0xFF, 3, {0x98, 0x82, 0x04}},
	{0xBA, 1, {0x81}},
	{0xFF, 3, {0x98, 0x82, 0x0C}},
	{0x00, 1, {0x16}},
	{0x01, 1, {0x00}},
	{0x02, 1, {0x16}},
	{0x03, 1, {0x01}},
	{0x04, 1, {0x16}},
	{0x05, 1, {0x02}},
	{0x06, 1, {0x15}},
	{0x07, 1, {0x03}},
	{0x08, 1, {0x15}},
	{0x09, 1, {0x04}},
	{0x0A, 1, {0x16}},
	{0x0B, 1, {0x05}},
	{0x0C, 1, {0x16}},
	{0x0D, 1, {0x06}},
	{0x0E, 1, {0x16}},
	{0x0F, 1, {0x07}},
	{0x10, 1, {0x15}},
	{0x11, 1, {0x08}},
	{0x12, 1, {0x16}},
	{0x13, 1, {0x09}},
	{0x14, 1, {0x16}},
	{0x15, 1, {0x0A}},
	{0x16, 1, {0x16}},
	{0x17, 1, {0x0B}},
	{0x18, 1, {0x15}},
	{0x19, 1, {0x0C}},
	{0x1A, 1, {0x15}},
	{0x1B, 1, {0x0D}},
	{0x1C, 1, {0x16}},
	{0x1D, 1, {0x0E}},
	{0x1E, 1, {0x16}},
	{0x1F, 1, {0x0F}},
	{0x20, 1, {0x16}},
	{0x21, 1, {0x10}},
	{0x22, 1, {0x15}},
	{0x23, 1, {0x11}},
	{0x24, 1, {0x16}},
	{0x25, 1, {0x12}},
	{0x26, 1, {0x16}},
	{0x27, 1, {0x13}},
	{0x28, 1, {0x16}},
	{0x29, 1, {0x14}},
	{0x2A, 1, {0x16}},
	{0x2B, 1, {0x15}},
	{0x2C, 1, {0x15}},
	{0x2D, 1, {0x16}},
	{0x2E, 1, {0x16}},
	{0x2F, 1, {0x17}},
	{0x30, 1, {0x16}},
	{0x31, 1, {0x18}},
	{0x32, 1, {0x16}},
	{0x33, 1, {0x19}},
	{0x34, 1, {0x15}},
	{0x35, 1, {0x1A}},
	{0x36, 1, {0x15}},
	{0x37, 1, {0x1B}},
	{0x38, 1, {0x16}},
	{0x39, 1, {0x1C}},
	{0x3A, 1, {0x16}},
	{0x3B, 1, {0x1D}},
	{0x3C, 1, {0x16}},
	{0x3D, 1, {0x1E}},
	{0x3E, 1, {0x15}},
	{0x3F, 1, {0x1F}},
	{0xFF, 3, {0x98, 0x82, 0x04}},
	{0xBA, 1, {0x01}},
	{0xFF, 3, {0x98, 0x82, 0x0E}},
	{0x02, 1, {0x14}},
	{0x20, 1, {0x0E}},
	{0x25, 1, {0x15}},
	{0x26, 1, {0x00}},
	{0x27, 1, {0x00}},
	{0x29, 1, {0x75}},
	{0x2A, 1, {0x85}},
	{0xB0, 1, {0x01}},
	{0xB1, 1, {0xD0}},
	{0xC0, 1, {0x14}},
	{0xC2, 1, {0xD0}},
	{0xC3, 1, {0xD0}},
	{0xC4, 1, {0xD0}},
	{0xC5, 1, {0xD0}},
	{0xC6, 1, {0xD0}},
	{0xC7, 1, {0xD0}},
	{0xC8, 1, {0xD0}},
	{0xC9, 1, {0xD0}},
	{0x2D, 1, {0xCC}},
	{0x30, 1, {0x00}},
	{0x00, 1, {0x81}},
	{0x07, 1, {0x21}},
	{0x2B, 1, {0x1F}},
	{0x33, 1, {0x98}},
	{0x34, 1, {0x00}},
	{0x35, 1, {0x61}},
	{0x36, 1, {0xE0}},
	{0x80, 1, {0x3E}},
	{0x81, 1, {0xA0}},
	{0x38, 1, {0x75}},
	{0x04, 1, {0x10}},
	{0x31, 1, {0x06}},
	{0x32, 1, {0x9F}},
	{0xFF, 3, {0x98, 0x82, 0x1E}},
	{0x60, 1, {0x00}},
	{0x64, 1, {0x00}},
	{0x6D, 1, {0x00}},
	{0xFF, 3, {0x98, 0x82, 0x0B}},
	{0xA6, 1, {0x87}},
	{0xA7, 1, {0xEF}},
	{0xA8, 1, {0x06}},
	{0xA9, 1, {0x06}},
	{0xAA, 1, {0x88}},
	{0xAB, 1, {0x88}},
	{0xAC, 1, {0x07}},
	{0xBD, 1, {0xA2}},
	{0xBE, 1, {0xA1}},
	{0xFF, 3, {0x98, 0x82, 0x00}},
	{0x11, 0, {}},
	{REGFLAG_DELAY, 70, {}},
	{0x29, 0, {}},
	{REGFLAG_DELAY, 5, {}},
	{0x00, 0, {}},
};

static struct LCM_setting_table lcm_init_ili9882t_tg_hsd[] = {
	{0xFF, 3, {0x98, 0x82, 0x01}},
	{0x00, 1, {0x42}},
	{0x01, 1, {0x11}},
	{0x02, 1, {0x00}},
	{0x03, 1, {0x00}},
	{0x04, 1, {0x01}},
	{0x05, 1, {0x11}},
	{0x06, 1, {0x00}},
	{0x07, 1, {0x00}},
	{0x08, 1, {0x80}},
	{0x09, 1, {0x81}},
	{0x0A, 1, {0x71}},
	{0x0B, 1, {0x00}},
	{0x0C, 1, {0x00}},
	{0x0E, 1, {0x10}},
	{0x24, 1, {0x00}},
	{0x25, 1, {0x00}},
	{0x26, 1, {0x00}},
	{0x27, 1, {0x00}},
	{0x2C, 1, {0xD4}},
	{0xB9, 1, {0x40}},
	{0xB0, 1, {0x11}},
	{0xE6, 1, {0x32}},
	{0xD1, 1, {0x30}},
	{0xD6, 1, {0x55}},
	{0xD0, 1, {0x01}},
	{0xE3, 1, {0x93}},
	{0xE4, 1, {0x00}},
	{0xE5, 1, {0x80}},
	{0x31, 1, {0x07}},
	{0x32, 1, {0x07}},
	{0x33, 1, {0x07}},
	{0x34, 1, {0x07}},
	{0x35, 1, {0x07}},
	{0x36, 1, {0x01}},
	{0x37, 1, {0x00}},
	{0x38, 1, {0x28}},
	{0x39, 1, {0x29}},
	{0x3A, 1, {0x11}},
	{0x3B, 1, {0x13}},
	{0x3C, 1, {0x15}},
	{0x3D, 1, {0x17}},
	{0x3E, 1, {0x09}},
	{0x3F, 1, {0x0D}},
	{0x40, 1, {0x02}},
	{0x41, 1, {0x02}},
	{0x42, 1, {0x02}},
	{0x43, 1, {0x02}},
	{0x44, 1, {0x02}},
	{0x45, 1, {0x02}},
	{0x46, 1, {0x02}},
	{0x47, 1, {0x07}},
	{0x48, 1, {0x07}},
	{0x49, 1, {0x07}},
	{0x4A, 1, {0x07}},
	{0x4B, 1, {0x07}},
	{0x4C, 1, {0x01}},
	{0x4D, 1, {0x00}},
	{0x4E, 1, {0x28}},
	{0x4F, 1, {0x29}},
	{0x50, 1, {0x10}},
	{0x51, 1, {0x12}},
	{0x52, 1, {0x14}},
	{0x53, 1, {0x16}},
	{0x54, 1, {0x08}},
	{0x55, 1, {0x0C}},
	{0x56, 1, {0x02}},
	{0x57, 1, {0x02}},
	{0x58, 1, {0x02}},
	{0x59, 1, {0x02}},
	{0x5A, 1, {0x02}},
	{0x5B, 1, {0x02}},
	{0x5C, 1, {0x02}},
	{0x61, 1, {0x07}},
	{0x62, 1, {0x07}},
	{0x63, 1, {0x07}},
	{0x64, 1, {0x07}},
	{0x65, 1, {0x07}},
	{0x66, 1, {0x01}},
	{0x67, 1, {0x00}},
	{0x68, 1, {0x28}},
	{0x69, 1, {0x29}},
	{0x6A, 1, {0x16}},
	{0x6B, 1, {0x14}},
	{0x6C, 1, {0x12}},
	{0x6D, 1, {0x10}},
	{0x6E, 1, {0x0C}},
	{0x6F, 1, {0x08}},
	{0x70, 1, {0x02}},
	{0x71, 1, {0x02}},
	{0x72, 1, {0x02}},
	{0x73, 1, {0x02}},
	{0x74, 1, {0x02}},
	{0x75, 1, {0x02}},
	{0x76, 1, {0x02}},
	{0x77, 1, {0x07}},
	{0x78, 1, {0x07}},
	{0x79, 1, {0x07}},
	{0x7A, 1, {0x07}},
	{0x7B, 1, {0x07}},
	{0x7C, 1, {0x01}},
	{0x7D, 1, {0x00}},
	{0x7E, 1, {0x28}},
	{0x7F, 1, {0x29}},
	{0x80, 1, {0x17}},
	{0x81, 1, {0x15}},
	{0x82, 1, {0x13}},
	{0x83, 1, {0x11}},
	{0x84, 1, {0x0D}},
	{0x85, 1, {0x09}},
	{0x86, 1, {0x02}},
	{0x87, 1, {0x07}},
	{0x88, 1, {0x07}},
	{0x89, 1, {0x07}},
	{0x8A, 1, {0x07}},
	{0x8B, 1, {0x07}},
	{0x8C, 1, {0x07}},
	{0xFF, 3, {0x98, 0x82, 0x02}},
	{0x29, 1, {0x3A}},
	{0x2A, 1, {0x3B}},
	{0x06, 1, {0xFF}},
	{0x07, 1, {0x00}},
	{0x08, 1, {0x2A}},
	{0x09, 1, {0x3A}},
	{0x3A, 1, {0x2A}},
	{0x3B, 1, {0x3A}},
	{0x39, 1, {0x11}},
	{0x3C, 1, {0x07}},
	{0x53, 1, {0x1F}},
	{0x5E, 1, {0x40}},
	{0x84, 1, {0x00}},
	{0xFF, 3, {0x98, 0x82, 0x03}},
	{0x20, 1, {0x01}},
	{0x21, 1, {0x3C}},
	{0x22, 1, {0xFA}},
	{0xFF, 3, {0x98, 0x82, 0x12}},
	{0x87, 1, {0x2C}},
	{0xFF, 3, {0x98, 0x82, 0x05}},
	{0x73, 1, {0xC7}},
	{0x7F, 1, {0x6B}},
	{0x6D, 1, {0x95}},
	{0x79, 1, {0x54}},
	{0x69, 1, {0x9C}},
	{0x6A, 1, {0x9C}},
	{0xA5, 1, {0x3F}},
	{0x61, 1, {0xDA}},
	{0xA7, 1, {0xF1}},
	{0x5F, 1, {0x01}},
	{0x62, 1, {0x3F}},
	{0x1D, 1, {0x90}},
	{0x86, 1, {0x87}},
	{0xFF, 3, {0x98, 0x82, 0x06}},
	{0x92, 1, {0x22}},
	{0xC0, 1, {0x80}},
	{0xC1, 1, {0x07}},
	{0xCA, 1, {0x58}},
	{0xCB, 1, {0x02}},
	{0xCE, 1, {0x58}},
	{0xCF, 1, {0x02}},
	{0x67, 1, {0x60}},
	{0x10, 1, {0x07}},
	{0xD6, 1, {0x55}},
	{0xDC, 1, {0x38}},
	{0xD3, 1, {0x08}},
	{0xFF, 3, {0x98, 0x82, 0x08}},
	{0xE0, 23, {0x00, 0x22, 0x4E, 0x79, 0x8B, 0x83, 0x95, 0x94, 0x9F, 0x9A, 0xB0, 0xB2, 0xB3, 0xC6, 0xBF, 0xC4, 0xCB, 0xDA, 0xD1, 0xC6, 0xDA, 0xE9, 0xE8}},
	{0xE1, 23, {0x00, 0x22, 0x4E, 0x79, 0x8B, 0x83, 0x95, 0x94, 0x9F, 0x9A, 0xB0, 0xB2, 0xB3, 0xC6, 0xBF, 0xC4, 0xCB, 0xDA, 0xD1, 0xC6, 0xDA, 0xE9, 0xE8}},
	{0xFF, 3, {0x98, 0x82, 0x15}},
	{0x31, 1, {0xA5}},
	{0x33, 1, {0xA5}},
	{0xFF, 3, {0x98, 0x82, 0x04}},
	{0xBA, 1, {0x81}},
	{0xFF, 3, {0x98, 0x82, 0x0C}},
	{0x00, 1, {0x0C}},
	{0x01, 1, {0x00}},
	{0x02, 1, {0x0C}},
	{0x03, 1, {0x01}},
	{0x04, 1, {0x0C}},
	{0x05, 1, {0x02}},
	{0x06, 1, {0x0B}},
	{0x07, 1, {0x03}},
	{0x08, 1, {0x0C}},
	{0x09, 1, {0x04}},
	{0x0A, 1, {0x0C}},
	{0x0B, 1, {0x05}},
	{0x0C, 1, {0x0C}},
	{0x0D, 1, {0x06}},
	{0x0E, 1, {0x0B}},
	{0x0F, 1, {0x07}},
	{0x10, 1, {0x0B}},
	{0x11, 1, {0x08}},
	{0x12, 1, {0x0C}},
	{0x13, 1, {0x09}},
	{0x14, 1, {0x0C}},
	{0x15, 1, {0x0A}},
	{0x16, 1, {0x0C}},
	{0x17, 1, {0x0B}},
	{0x18, 1, {0x0B}},
	{0x19, 1, {0x0C}},
	{0x1A, 1, {0x0C}},
	{0x1B, 1, {0x0D}},
	{0x1C, 1, {0x0C}},
	{0x1D, 1, {0x0E}},
	{0x1E, 1, {0x0C}},
	{0x1F, 1, {0x0F}},
	{0x20, 1, {0x0B}},
	{0x21, 1, {0x10}},
	{0x22, 1, {0x0B}},
	{0x23, 1, {0x11}},
	{0x24, 1, {0x0C}},
	{0x25, 1, {0x12}},
	{0x26, 1, {0x0C}},
	{0x27, 1, {0x13}},
	{0x28, 1, {0x0C}},
	{0x29, 1, {0x14}},
	{0x2A, 1, {0x0B}},
	{0x2B, 1, {0x15}},
	{0x2C, 1, {0x0C}},
	{0x2D, 1, {0x16}},
	{0x2E, 1, {0x0C}},
	{0x2F, 1, {0x17}},
	{0x30, 1, {0x0C}},
	{0x31, 1, {0x18}},
	{0x32, 1, {0x0B}},
	{0x33, 1, {0x19}},
	{0x34, 1, {0x0B}},
	{0x35, 1, {0x1A}},
	{0x36, 1, {0x0C}},
	{0x37, 1, {0x1B}},
	{0x38, 1, {0x0C}},
	{0x39, 1, {0x1C}},
	{0x3A, 1, {0x0C}},
	{0x3B, 1, {0x1D}},
	{0x3C, 1, {0x0B}},
	{0x3D, 1, {0x1E}},
	{0x3E, 1, {0x0C}},
	{0x3F, 1, {0x1F}},
	{0xFF, 3, {0x98, 0x82, 0x04}},
	{0xBA, 1, {0x01}},
	{0xFF, 3, {0x98, 0x82, 0x0E}},
	{0x02, 1, {0x10}},
	{0x20, 1, {0x0E}},
	{0x25, 1, {0x16}},
	{0x26, 1, {0xA0}},
	{0x27, 1, {0x00}},
	{0x29, 1, {0x74}},
	{0x2A, 1, {0x8B}},
	{0xB0, 1, {0x01}},
	{0xB1, 1, {0xCB}},
	{0xC0, 1, {0x14}},
	{0xC2, 1, {0xCB}},
	{0xC3, 1, {0xCB}},
	{0xC4, 1, {0xCB}},
	{0xC5, 1, {0xCB}},
	{0xC6, 1, {0xCB}},
	{0xC7, 1, {0xCB}},
	{0xC8, 1, {0xCB}},
	{0xC9, 1, {0xCB}},
	{0x2D, 1, {0xCA}},
	{0x30, 1, {0x00}},
	{0x00, 1, {0x81}},
	{0x08, 1, {0x0C}},
	{0x09, 1, {0x00}},
	{0x07, 1, {0x21}},
	{0x2B, 1, {0x1F}},
	{0x33, 1, {0x98}},
	{0x34, 1, {0x00}},
	{0x35, 1, {0x60}},
	{0x36, 1, {0x40}},
	{0x80, 1, {0x3E}},
	{0x81, 1, {0xA0}},
	{0x38, 1, {0x71}},
	{0x04, 1, {0x10}},
	{0x31, 1, {0x06}},
	{0x32, 1, {0x9F}},
	{0xFF, 3, {0x98, 0x82, 0x1E}},
	{0x60, 1, {0x00}},
	{0x64, 1, {0x00}},
	{0x6D, 1, {0x00}},
	{0xFF, 3, {0x98, 0x82, 0x0B}},
	{0xA6, 1, {0x88}},
	{0xA7, 1, {0x40}},
	{0xA8, 1, {0x06}},
	{0xA9, 1, {0x06}},
	{0xAA, 1, {0x8D}},
	{0xAB, 1, {0x8D}},
	{0xAC, 1, {0x07}},
	{0xBD, 1, {0xA2}},
	{0xBE, 1, {0xA1}},
	{0xFF, 3, {0x98, 0x82, 0x00}},
	{0x11, 0, {}},
	{REGFLAG_DELAY, 70, {}},
	{0x29, 0, {}},
	{REGFLAG_DELAY, 5, {}},
	{0x00, 0, {}},
};

static struct LCM_setting_table lcm_init_ili9882t_starry_boe[] = {
	{0xFF, 3, {0x98, 0x82, 0x01}},
	{0x00, 1, {0x48}},
	{0x01, 1, {0x13}},
	{0x02, 1, {0x00}},
	{0x03, 1, {0x00}},
	{0x04, 1, {0xCD}},
	{0x05, 1, {0x13}},
	{0x06, 1, {0x00}},
	{0x07, 1, {0x00}},
	{0x08, 1, {0x85}},
	{0x09, 1, {0x0E}},
	{0x0A, 1, {0x72}},
	{0x0B, 1, {0x00}},
	{0x0C, 1, {0x5E}},
	{0x0D, 1, {0x00}},
	{0x0E, 1, {0x1A}},
	{0x0F, 1, {0x00}},
	{0x28, 1, {0x4E}},
	{0x29, 1, {0x98}},
	{0x2A, 1, {0x4D}},
	{0x2B, 1, {0x97}},
	{0xEE, 1, {0x10}},
	{0xED, 1, {0x93}},
	{0xDA, 1, {0x0A}},
	{0xDB, 1, {0x82}},
	{0xDC, 1, {0x94}},
	{0xDF, 1, {0x26}},
	{0xE0, 1, {0x45}},
	{0xE1, 1, {0x41}},
	{0xCE, 1, {0x80}},
	{0x31, 1, {0x2A}},
	{0x32, 1, {0x2A}},
	{0x33, 1, {0x25}},
	{0x34, 1, {0x08}},
	{0x35, 1, {0x0C}},
	{0x36, 1, {0x2A}},
	{0x37, 1, {0x23}},
	{0x38, 1, {0x22}},
	{0x39, 1, {0x12}},
	{0x3A, 1, {0x12}},
	{0x3B, 1, {0x10}},
	{0x3C, 1, {0x10}},
	{0x3D, 1, {0x16}},
	{0x3E, 1, {0x16}},
	{0x3F, 1, {0x14}},
	{0x40, 1, {0x14}},
	{0x41, 1, {0x07}},
	{0x42, 1, {0x07}},
	{0x43, 1, {0x07}},
	{0x44, 1, {0x07}},
	{0x45, 1, {0x07}},
	{0x46, 1, {0x07}},
	{0x47, 1, {0x2A}},
	{0x48, 1, {0x2A}},
	{0x49, 1, {0x25}},
	{0x4A, 1, {0x09}},
	{0x4B, 1, {0x0D}},
	{0x4C, 1, {0x2A}},
	{0x4D, 1, {0x23}},
	{0x4E, 1, {0x22}},
	{0x4F, 1, {0x13}},
	{0x50, 1, {0x13}},
	{0x51, 1, {0x11}},
	{0x52, 1, {0x11}},
	{0x53, 1, {0x17}},
	{0x54, 1, {0x17}},
	{0x55, 1, {0x15}},
	{0x56, 1, {0x15}},
	{0x57, 1, {0x07}},
	{0x58, 1, {0x07}},
	{0x59, 1, {0x07}},
	{0x5A, 1, {0x07}},
	{0x5B, 1, {0x07}},
	{0x5C, 1, {0x07}},
	{0x61, 1, {0x2A}},
	{0x62, 1, {0x2A}},
	{0x63, 1, {0x2A}},
	{0x64, 1, {0x09}},
	{0x65, 1, {0x0D}},
	{0x66, 1, {0x25}},
	{0x67, 1, {0x23}},
	{0x68, 1, {0x22}},
	{0x69, 1, {0x15}},
	{0x6A, 1, {0x15}},
	{0x6B, 1, {0x17}},
	{0x6C, 1, {0x17}},
	{0x6D, 1, {0x11}},
	{0x6E, 1, {0x11}},
	{0x6F, 1, {0x13}},
	{0x70, 1, {0x13}},
	{0x71, 1, {0x07}},
	{0x72, 1, {0x07}},
	{0x73, 1, {0x07}},
	{0x74, 1, {0x07}},
	{0x75, 1, {0x07}},
	{0x76, 1, {0x07}},
	{0x77, 1, {0x2A}},
	{0x78, 1, {0x2A}},
	{0x79, 1, {0x2A}},
	{0x7A, 1, {0x08}},
	{0x7B, 1, {0x0C}},
	{0x7C, 1, {0x25}},
	{0x7D, 1, {0x23}},
	{0x7E, 1, {0x22}},
	{0x7F, 1, {0x14}},
	{0x80, 1, {0x14}},
	{0x81, 1, {0x16}},
	{0x82, 1, {0x16}},
	{0x83, 1, {0x10}},
	{0x84, 1, {0x10}},
	{0x85, 1, {0x12}},
	{0x86, 1, {0x12}},
	{0x87, 1, {0x07}},
	{0x88, 1, {0x07}},
	{0x89, 1, {0x07}},
	{0x8A, 1, {0x07}},
	{0x8B, 1, {0x07}},
	{0x8C, 1, {0x07}},
	{0xB0, 1, {0x22}},
	{0xB2, 1, {0x00}},
	{0xB9, 1, {0x10}},
	{0xBA, 1, {0x04}},
	{0xFF, 3, {0x98, 0x82, 0x02}},
	{0x46, 1, {0xF0}},
	{0x29, 1, {0x3A}},
	{0x2A, 1, {0x3B}},
	{0x3A, 1, {0x2E}},
	{0x3B, 1, {0x36}},
	{0x39, 1, {0x11}},
	{0x3C, 1, {0x08}},
	{0x53, 1, {0x1F}},
	{0x5E, 1, {0x40}},
	{0x84, 1, {0x00}},
	{0xFF, 3, {0x98, 0x82, 0x03}},
	{0x20, 1, {0x01}},
	{0x21, 1, {0x3C}},
	{0x22, 1, {0xFA}},
	{0xFF, 3, {0x98, 0x82, 0x12}},
	{0x87, 1, {0x2C}},
	{0xFF, 3, {0x98, 0x82, 0x05}},
	{0x73, 1, {0xF9}},
	{0x7F, 1, {0x7F}},
	{0x6D, 1, {0xAE}},
	{0x79, 1, {0x5E}},
	{0x69, 1, {0x8E}},
	{0x6A, 1, {0x8E}},
	{0x61, 1, {0xEC}},
	{0xA7, 1, {0xF1}},
	{0x5F, 1, {0x01}},
	{0x62, 1, {0x3F}},
	{0x1D, 1, {0x90}},
	{0x86, 1, {0x87}},
	{0x9C, 1, {0x02}},
	{0xFF, 3, {0x98, 0x82, 0x06}},
	{0xC0, 1, {0x80}},
	{0xC1, 1, {0x07}},
	{0xCA, 1, {0x58}},
	{0xCB, 1, {0x02}},
	{0xCE, 1, {0x58}},
	{0xCF, 1, {0x02}},
	{0x10, 1, {0x00}},
	{0xD6, 1, {0x55}},
	{0xDC, 1, {0x38}},
	{0xD3, 1, {0x08}},
	{0x92, 1, {0x22}},
	{0xFF, 3, {0x98, 0x82, 0x08}},
	{0xE0, 23, {0x00, 0x00, 0x1C, 0x3A, 0x4B, 0x40, 0x55, 0x5A, 0x68, 0x68, 0x88, 0x94, 0x9E, 0xB7, 0xB4, 0xBB, 0xC3, 0xD4, 0xCC, 0xC3, 0xDA, 0xEB, 0xFF}},
	{0xE1, 23, {0x00, 0x00, 0x1C, 0x3A, 0x4B, 0x40, 0x55, 0x5A, 0x68, 0x68, 0x88, 0x94, 0x9E, 0xB7, 0xB4, 0xBB, 0xC3, 0xD4, 0xCC, 0xC3, 0xDA, 0xEB, 0xFF}},
	{0xFF, 3, {0x98, 0x82, 0x15}},
	{0xFF, 3, {0x98, 0x82, 0x04}},
	{0xBA, 1, {0x81}},
	{0xFF, 3, {0x98, 0x82, 0x0C}},
	{0x00, 1, {0x09}},
	{0x01, 1, {0x00}},
	{0x02, 1, {0x09}},
	{0x03, 1, {0x01}},
	{0x04, 1, {0x09}},
	{0x05, 1, {0x02}},
	{0x06, 1, {0x09}},
	{0x07, 1, {0x03}},
	{0x08, 1, {0x0A}},
	{0x09, 1, {0x04}},
	{0x0A, 1, {0x09}},
	{0x0B, 1, {0x05}},
	{0x0C, 1, {0x09}},
	{0x0D, 1, {0x06}},
	{0x0E, 1, {0x09}},
	{0x0F, 1, {0x07}},
	{0x10, 1, {0x09}},
	{0x11, 1, {0x08}},
	{0x12, 1, {0x0A}},
	{0x13, 1, {0x09}},
	{0x14, 1, {0x09}},
	{0x15, 1, {0x0A}},
	{0x16, 1, {0x09}},
	{0x17, 1, {0x0B}},
	{0x18, 1, {0x09}},
	{0x19, 1, {0x0C}},
	{0x1A, 1, {0x0A}},
	{0x1B, 1, {0x0D}},
	{0x1C, 1, {0x0A}},
	{0x1D, 1, {0x0E}},
	{0x1E, 1, {0x09}},
	{0x1F, 1, {0x0F}},
	{0x20, 1, {0x09}},
	{0x21, 1, {0x10}},
	{0x22, 1, {0x09}},
	{0x23, 1, {0x11}},
	{0x24, 1, {0x0A}},
	{0x25, 1, {0x12}},
	{0x26, 1, {0x0A}},
	{0x27, 1, {0x13}},
	{0x28, 1, {0x09}},
	{0x29, 1, {0x14}},
	{0x2A, 1, {0x09}},
	{0x2B, 1, {0x15}},
	{0x2C, 1, {0x09}},
	{0x2D, 1, {0x16}},
	{0x2E, 1, {0x0A}},
	{0x2F, 1, {0x17}},
	{0x30, 1, {0x0A}},
	{0x31, 1, {0x18}},
	{0x32, 1, {0x09}},
	{0x33, 1, {0x19}},
	{0x34, 1, {0x09}},
	{0x35, 1, {0x1A}},
	{0x36, 1, {0x09}},
	{0x37, 1, {0x1B}},
	{0x38, 1, {0x0A}},
	{0x39, 1, {0x1C}},
	{0x3A, 1, {0x09}},
	{0x3B, 1, {0x1D}},
	{0x3C, 1, {0x09}},
	{0x3D, 1, {0x1E}},
	{0x3E, 1, {0x09}},
	{0x3F, 1, {0x1F}},
	{0xFF, 3, {0x98, 0x82, 0x04}},
	{0xBA, 1, {0x01}},
	{0xFF, 3, {0x98, 0x82, 0x0E}},
	{0x02, 1, {0x10}},
	{0x20, 1, {0x0E}},
	{0x25, 1, {0x15}},
	{0x26, 1, {0x00}},
	{0x27, 1, {0x00}},
	{0x29, 1, {0x74}},
	{0xB0, 1, {0x01}},
	{0xB1, 1, {0xC0}},
	{0xC0, 1, {0x23}},
	{0xC2, 1, {0xC0}},
	{0xC3, 1, {0xC0}},
	{0xC4, 1, {0xC0}},
	{0xC5, 1, {0xC0}},
	{0xC6, 1, {0xC0}},
	{0xC7, 1, {0xC0}},
	{0xC8, 1, {0xC0}},
	{0xC9, 1, {0xC0}},
	{0xCB, 1, {0xA8}},
	{0xCC, 1, {0xA8}},
	{0xCD, 1, {0xA8}},
	{0xCE, 1, {0xA8}},
	{0xCF, 1, {0xA8}},
	{0xD0, 1, {0xA8}},
	{0xD1, 1, {0xA8}},
	{0xD2, 1, {0xA8}},
	{0x2D, 1, {0xCE}},
	{0x30, 1, {0x00}},
	{0x00, 1, {0x81}},
	{0x07, 1, {0x21}},
	{0x2B, 1, {0x1F}},
	{0x33, 1, {0x98}},
	{0x34, 1, {0x00}},
	{0x35, 1, {0x61}},
	{0x36, 1, {0xE0}},
	{0x80, 1, {0x3E}},
	{0x81, 1, {0xA0}},
	{0x2A, 1, {0x88}},
	{0x38, 1, {0x72}},
	{0x04, 1, {0x10}},
	{0x31, 1, {0x06}},
	{0x32, 1, {0x9F}},
	{0xFF, 3, {0x98, 0x82, 0x1E}},
	{0x60, 1, {0x00}},
	{0x64, 1, {0x00}},
	{0x6D, 1, {0x00}},
	{0xFF, 3, {0x98, 0x82, 0x0B}},
	{0xA6, 1, {0x88}},
	{0xA7, 1, {0x40}},
	{0xA8, 1, {0x06}},
	{0xA9, 1, {0x06}},
	{0xAA, 1, {0x8D}},
	{0xAB, 1, {0x8D}},
	{0xAC, 1, {0x07}},
	{0xBD, 1, {0xA2}},
	{0xBE, 1, {0xA1}},
	{0xFF, 3, {0x98, 0x82, 0x00}},
	{0x11, 0, {}},
	{REGFLAG_DELAY, 70, {}},
	{0x29, 0, {}},
	{REGFLAG_DELAY, 5, {}},
	{0x00, 0, {}},
};

static inline struct panel_info *to_info_panel(struct drm_panel *panel)
{
	return container_of(panel, struct panel_info, base);
}

static void lcm_init_push_table(struct panel_info *info, struct LCM_setting_table *table,
			unsigned int count)
{
	unsigned int i;
	unsigned int cmd;
	struct mipi_dsi_device *dsi = info->dsi;
	int ret;

	for (i = 0; i < count; i++) {
		cmd = table[i].cmd;

		switch (cmd) {
		case REGFLAG_DELAY:
			mdelay(table[i].count);
			break;
		case REGFLAG_END_OF_TABLE:
			break;
		default:
			ret = mipi_dsi_dcs_write(dsi, cmd, table[i].para_list,
						table[i].count);
			if (ret < 0)
				pr_err("mipi_dsi_dcs_write failed!"
					"ret = %d, i = %d, cmd = 0x%x.\n", ret, i, cmd);
			break;
		}
	}
}

static void lcm_panel_init(struct panel_info *info)
{
	switch (vendor_id) {
	case LCM_ILI9882T_KD_INX:
		lcm_init_push_table(info, lcm_init_ili9882t_kd_inx,
			ARRAY_SIZE(lcm_init_ili9882t_kd_inx));
		break;
	case LCM_ILI9882T_TG_HSD:
		lcm_init_push_table(info, lcm_init_ili9882t_tg_hsd,
			ARRAY_SIZE(lcm_init_ili9882t_tg_hsd));
		break;
	case LCM_ILI9882T_STARRY_BOE:
		lcm_init_push_table(info, lcm_init_ili9882t_starry_boe,
			ARRAY_SIZE(lcm_init_ili9882t_starry_boe));
		break;
	default:
		pr_notice("Not match correct ID, loading default init code...\n");
		lcm_init_push_table(info, lcm_init_ili9882t_kd_inx,
			ARRAY_SIZE(lcm_init_ili9882t_kd_inx));
		break;
	}
}

static void panel_enter_sleep_mode(struct panel_info *info)
{
	struct mipi_dsi_device *dsi = info->dsi;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	lcm_init_push_table(info, lcm_suspend_setting,
		ARRAY_SIZE(lcm_suspend_setting));
	mdelay(65);

	if (true == panel_check_gesture_mode() && atomic_read(&g_power_rebooting) == 0)
		lcm_init_push_table(info, gesture_mode_sleep_cmd,
			ARRAY_SIZE(gesture_mode_sleep_cmd));
}

void panel_gesture_mode_set_by_ili9882t(bool gst_mode)
{
	atomic_set(&gesture_status, gst_mode);
}
EXPORT_SYMBOL_GPL(panel_gesture_mode_set_by_ili9882t);

static bool panel_check_gesture_mode(void)
{
	bool ret = false;

	ret = atomic_read(&gesture_status);

	return ret;
}

void panel_touch_upgrade_status_set_by_ili9882t(bool upg_mode)
{
	atomic_set(&touch_upgrade_status, upg_mode);
}
EXPORT_SYMBOL_GPL(panel_touch_upgrade_status_set_by_ili9882t);

static bool panel_check_touch_upgrade_mode(void)
{
	bool ret = false;

	ret = atomic_read(&touch_upgrade_status);

	return ret;
}

void ili9882t_share_powerpin_on(void)
{
	mutex_lock(&ili9882t_share_power_mutex);
	/*LCM resume power with ON1->ON2, touch resume with ON.*/
	if (atomic_read(&g_power_request) == 0)
		LCM_ERROR("%s Touch poweron should NOT before LCM poweron", __func__);
	lcm_power_manager(share_info, LCM_ON);
	mutex_unlock(&ili9882t_share_power_mutex);
}
EXPORT_SYMBOL(ili9882t_share_powerpin_on);

void ili9882t_share_powerpin_off(void)
{
	mutex_lock(&ili9882t_share_power_mutex);
	lcm_power_manager(share_info, LCM_OFF);
	mutex_unlock(&ili9882t_share_power_mutex);
}
EXPORT_SYMBOL(ili9882t_share_powerpin_off);

void ili9882t_share_tp_resetpin_control(int value)
{
	mutex_lock(&ili9882t_share_power_mutex);
	if (atomic_read(&g_power_request) <= 0) {
		LCM_ERROR("%s skip, bceause res_cnt(%d) <= 0",
				__func__, atomic_read(&g_power_request));
	} else {
		gpiod_set_value(share_info->ctp_rst_gpio, value);
	}
	mutex_unlock(&ili9882t_share_power_mutex);
}
EXPORT_SYMBOL(ili9882t_share_tp_resetpin_control);

static int panel_unprepare_power(struct drm_panel *panel)
{
	struct panel_info *info = to_info_panel(panel);

	if (!info->prepared_power)
		return 0;

	if ((false == panel_check_gesture_mode() && false == panel_check_touch_upgrade_mode()) ||
		atomic_read(&g_power_rebooting) != 0) {
		lcm_power_manager_lock(info, LCM_OFF);
		info->prepared_power = false;
	}

	return 0;
}

static int panel_unprepare(struct drm_panel *panel)
{
	struct panel_info *info = to_info_panel(panel);

	if (!info->prepared)
		return 0;

	lcm_power_manager_lock(info, LCM_DSB);
	info->prepared = false;

	return 0;
}

static int panel_prepare_power(struct drm_panel *panel)
{
	struct panel_info *info = to_info_panel(panel);

	if (info->prepared_power)
		return 0;

	lcm_power_manager_lock(info, LCM_ON1);
	info->prepared_power = true;

	return 0;
}

static int panel_prepare(struct drm_panel *panel)
{
	struct panel_info *info = to_info_panel(panel);

	if (info->prepared)
		return 0;

	lcm_power_manager_lock(info, LCM_ON2);
	info->prepared = true;

#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_tch_rst(panel);
#endif

	return 0;
}

static int panel_enable(struct drm_panel *panel)
{
	struct panel_info *info = to_info_panel(panel);

	if (info->enabled)
		return 0;

	if (info->backlight) {
		info->backlight->props.state &= ~BL_CORE_FBBLANK;
		info->backlight->props.power = FB_BLANK_UNBLANK;
		backlight_update_status(info->backlight);
	}

	info->enabled = true;

	return 0;
}

static int panel_disable(struct drm_panel *panel)
{
	struct panel_info *info = to_info_panel(panel);

	if (!info->enabled)
		return 0;

	if (info->backlight) {
		info->backlight->props.power = FB_BLANK_POWERDOWN;
		info->backlight->props.state |= BL_CORE_FBBLANK;
		backlight_update_status(info->backlight);
	}

	info->enabled = false;

	return 0;
}

#if defined(CONFIG_MTK_PANEL_EXT)
static int panel_ext_reset(struct drm_panel *panel, int on)
{
	struct panel_info *info = to_info_panel(panel);

	gpiod_set_value(info->lcm_rst_gpio, on);
	devm_gpiod_put(info->dev, info->lcm_rst_gpio);

	return 0;
}

static int panel_ata_check(struct drm_panel *panel)
{
	return 1;
}

static int lcm_power_controller(struct panel_info *info, int state)
{
	int ret = 0;

	switch (state) {
	case TRS_DSB:
		//deep sleep or gesture mode suspend
		panel_enter_sleep_mode(info);
		if ((false == panel_check_gesture_mode() && false == panel_check_touch_upgrade_mode()) ||
			atomic_read(&g_power_rebooting) != 0) {
			mdelay(65);
			gpiod_set_value(info->lcm_rst_gpio, 0);
			mdelay(1);
			gpiod_set_value(info->ctp_rst_gpio, 0);
		}
		break;
	case TRS_DSB_TO_ON:
		//gesture mode resume
		gpiod_set_value(info->lcm_rst_gpio, 0);
		mdelay(1);
		gpiod_set_value(info->lcm_rst_gpio, 1);
		mdelay(1);
		gpiod_set_value(info->ctp_rst_gpio, 0);
		mdelay(1);
		gpiod_set_value(info->ctp_rst_gpio, 1);
		mdelay(12);
		lcm_panel_init(info);
		break;
	case TRS_DSB_TO_OFF:
		//normal suspend
		mdelay(3);
		bias_off(info->avdd, info->avee);
		mdelay(10);
		if (info->vsim1)
			regulator_disable(info->vsim1);
		break;
	case TRS_ON_PHASE1:
		//normal resume phase1
		if (info->vsim1) {
			ret = regulator_enable(info->vsim1);
			if (ret < 0)
				dev_err(info->dev, "Enable vsim1 fail, %ld\n", ret);
			mdelay(2);
		}

		bias_on(info->avdd, info->avee);
		mdelay(2);
		break;
	case TRS_ON_PHASE2:
		//normal resume phase2
		gpiod_set_value(info->lcm_rst_gpio, 1);
		mdelay(1);
		gpiod_set_value(info->ctp_rst_gpio, 1);
		mdelay(12);
		lcm_panel_init(info);
		break;
	case TRS_OFF_TO_ON:
		//normal resume phase1
		if (info->vsim1) {
			ret = regulator_enable(info->vsim1);
			if (ret < 0)
				dev_err(info->dev, "Enable vsim1 fail, %ld\n", ret);
			mdelay(2);
		}

		bias_on(info->avdd, info->avee);
		mdelay(2);

		//normal resume phase2
		gpiod_set_value(info->lcm_rst_gpio, 1);
		mdelay(1);
		gpiod_set_value(info->ctp_rst_gpio, 1);
		mdelay(12);
		lcm_panel_init(info);
		break;
	case TRS_OFF:
		panel_enter_sleep_mode(info);
		mdelay(65);
		gpiod_set_value(info->lcm_rst_gpio, 0);
		mdelay(1);
		gpiod_set_value(info->ctp_rst_gpio, 0);
		mdelay(5);
		bias_off(info->avdd, info->avee);
		mdelay(10);
		if (info->vsim1)
			regulator_disable(info->vsim1);
		break;
	case TRS_KEEP_CUR:
		ret = -1;
		dev_info(info->dev, "keep current state\n");
		break;
	case TRS_NOT_PSB:
		ret = -1;
		dev_err(info->dev, "Unexpected state change\n");
		break;
	default:
		ret = -1;
		dev_err(info->dev, "%s undefined state (%d)\n", __func__, state);
		break;
	}

	return ret;
}

static void lcm_power_manager_lock(struct panel_info *info, int pwr_state)
{
	mutex_lock(&ili9882t_share_power_mutex);
	lcm_power_manager(info, pwr_state);
	mutex_unlock(&ili9882t_share_power_mutex);
}

static void lcm_power_manager(struct panel_info *info, int new_state)
{
	int tmp_state = LCM_ON;
	int new_trns;
	int ret = 0;

	tmp_state = old_state;

	switch (new_state) {
	case LCM_ON:
	case LCM_ON1:
		atomic_inc(&g_power_request);
		break;
	case LCM_ON2:
	case LCM_DSB:
		break;
	case LCM_OFF:
		atomic_dec(&g_power_request);
		break;
	default:
		LCM_INFO("%s, no match state", __func__);
		break;
	}

	new_trns = trans_table[old_state][new_state];

	if (new_trns == TRS_KEEP_CUR || new_trns == TRS_NOT_PSB) {
		if (new_state == LCM_ON || new_state == LCM_ON1)
			atomic_dec(&g_power_request);
		if (new_state == LCM_OFF)
			atomic_inc(&g_power_request);
	}

	ret = lcm_power_controller(info, new_trns);
	if (ret == 0)
		old_state = new_state;

	LCM_INFO("%s requesting %s, changed from %s to %s, count=%d",
		__func__, lcm_state_str[new_state],
		lcm_state_str[tmp_state], lcm_state_str[old_state],
		atomic_read(&g_power_request));
}

static struct mtk_panel_params ext_ili9882t_kd_inx_params = {
	.pll_clk = 555,
	.phy_timcon = {
		.clk_trail = 12,
	},
	.cust_esd_check = 1,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
		.ic_cmd_list[0] = 0x10FD1500,
	},
	.lcm_esd_check_table[1] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
		.ic_cmd_list[0] = 0x00FD1500,
	},
};

static struct mtk_panel_params ext_ili9882t_tg_hsd_params = {
	.pll_clk = 555,
	.phy_timcon = {
		.clk_trail = 12,
	},
	.cust_esd_check = 1,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
		.ic_cmd_list[0] = 0x10FD1500,
	},
	.lcm_esd_check_table[1] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
		.ic_cmd_list[0] = 0x00FD1500,
	},
};

static struct mtk_panel_params ext_ili9882t_starry_boe_params = {
	.pll_clk = 555,
	.phy_timcon = {
		.clk_trail = 12,
	},
	.cust_esd_check = 1,
	.esd_check_enable = 1,
	.lcm_esd_check_table[0] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
		.ic_cmd_list[0] = 0x10FD1500,
	},
	.lcm_esd_check_table[1] = {
		.cmd = 0x0a,
		.count = 1,
		.para_list[0] = 0x9c,
		.ic_cmd_list[0] = 0x00FD1500,
	},
};

static struct mtk_panel_funcs ext_funcs = {
	.reset = panel_ext_reset,
	.ata_check = panel_ata_check,
};
#endif

static int panel_get_modes(struct drm_panel *panel)
{
	struct drm_connector *connector = panel->connector;
	struct drm_display_mode *mode;

	switch (vendor_id) {
	case LCM_ILI9882T_KD_INX:
		mode = drm_mode_duplicate(connector->dev, &kd_inx_mode);
		if (!mode) {
			dev_err(panel->dev, "failed to add mode %ux%ux@%u\n",
				kd_inx_mode.hdisplay, kd_inx_mode.vdisplay,
				drm_mode_vrefresh(&kd_inx_mode));
			return -ENOMEM;
		}
		break;
	case LCM_ILI9882T_TG_HSD:
		mode = drm_mode_duplicate(connector->dev, &tg_hsd_mode);
		if (!mode) {
			dev_err(panel->dev, "failed to add mode %ux%ux@%u\n",
				tg_hsd_mode.hdisplay, tg_hsd_mode.vdisplay,
				drm_mode_vrefresh(&tg_hsd_mode));
			return -ENOMEM;
		}
		break;
	case LCM_ILI9882T_STARRY_BOE:
		mode = drm_mode_duplicate(connector->dev, &starry_boe_mode);
		if (!mode) {
			dev_err(panel->dev, "failed to add mode %ux%ux@%u\n",
				starry_boe_mode.hdisplay, starry_boe_mode.vdisplay,
				drm_mode_vrefresh(&starry_boe_mode));
			return -ENOMEM;
		}
		break;
	default:
		mode = drm_mode_duplicate(connector->dev, &kd_inx_mode);
		if (!mode) {
			dev_err(panel->dev, "failed to add mode %ux%ux@%u\n",
				kd_inx_mode.hdisplay, kd_inx_mode.vdisplay,
				drm_mode_vrefresh(&kd_inx_mode));
			return -ENOMEM;
		}
		break;
	}

	mode->type = DRM_MODE_TYPE_DRIVER | DRM_MODE_TYPE_PREFERRED;
	drm_mode_set_name(mode);
	drm_mode_probed_add(connector, mode);

	connector->display_info.width_mm = PHYSICAL_WIDTH;
	connector->display_info.height_mm = PHYSICAL_HEIGHT;
	connector->display_info.bpc = 8;

	return 1;
}

static const struct drm_panel_funcs panel_funcs = {
	.unprepare = panel_unprepare,
	.prepare = panel_prepare,
	.unprepare_power = panel_unprepare_power,
	.prepare_power = panel_prepare_power,
	.disable = panel_disable,
	.enable = panel_enable,
	.get_modes = panel_get_modes,
};

static int panel_add(struct panel_info *info)
{
	struct device *dev = &info->dsi->dev;
	struct device_node *backlight;
	int ret = 0xFF;

	info->vsim1 = devm_regulator_get(dev, "vsim1");
	if (IS_ERR(info->vsim1)) {
		dev_err(dev, "cannot get vsim1 1.8v regulator %ld\n",
			PTR_ERR(info->vsim1));
		return PTR_ERR(info->vsim1);
	} else if (regulator_is_enabled(info->vsim1)) {
		ret = regulator_enable(info->vsim1);
		if (ret < 0)
			dev_err(dev, "Enable vsim1 fail, %ld\n", ret);
	}

	info->lcm_rst_gpio = devm_gpiod_get(dev, "lcm-rst", GPIOD_ASIS);
	if (IS_ERR(info->lcm_rst_gpio)) {
		dev_err(dev, "cannot get lcm-rst-gpios %ld\n",
			PTR_ERR(info->lcm_rst_gpio));
		return PTR_ERR(info->lcm_rst_gpio);
	}

	info->ctp_rst_gpio = devm_gpiod_get(dev, "ctp-rst", GPIOD_ASIS);
	if (IS_ERR(info->ctp_rst_gpio)) {
		dev_err(dev, "cannot get ctp-rst-gpios %ld\n",
			PTR_ERR(info->ctp_rst_gpio));
		return PTR_ERR(info->ctp_rst_gpio);
	}

	info->avdd = devm_gpiod_get(dev, "avdd", GPIOD_ASIS);
	if (IS_ERR(info->avdd)) {
		dev_err(dev, "cannot get avdd-gpios %ld\n",
			PTR_ERR(info->avdd));
		return PTR_ERR(info->avdd);
	}

	info->avee = devm_gpiod_get(dev, "avee", GPIOD_ASIS);
	if (IS_ERR(info->avee)) {
		dev_err(dev, "cannot get avee-gpios %ld\n",
			PTR_ERR(info->avee));
		return PTR_ERR(info->avee);
	}

	drm_panel_init(&info->base);

	info->prepared = true;
	info->enabled = true;
	info->prepared_power = true;

	info->base.funcs = &panel_funcs;
	info->base.dev = &info->dsi->dev;

	backlight = of_parse_phandle(dev->of_node, "backlight", 0);
	if (backlight) {
		info->backlight = of_find_backlight_by_node(backlight);
		of_node_put(backlight);

		if (!info->backlight)
			return -EPROBE_DEFER;
	}

	drm_panel_add(&info->base);

	ret = register_reboot_notifier(&ili9882t_reboot_notifier);
	if (ret < 0) {
		dev_err(dev, "Failed to register ili9882t reboot notifier %d\n", ret);
		return -EPROBE_DEFER;
	}

	return 0;
}

static void panel_resources_release(struct panel_info *info)
{
	struct device *dev = &info->dsi->dev;

	if (!IS_ERR(info->vsim1))
		devm_regulator_put(info->vsim1);

	if (!IS_ERR(info->lcm_rst_gpio))
		devm_gpiod_put(dev, info->lcm_rst_gpio);

	if (!IS_ERR(info->ctp_rst_gpio))
		devm_gpiod_put(dev, info->ctp_rst_gpio);

	if (!IS_ERR(info->avdd))
		devm_gpiod_put(dev, info->avdd);

	if (!IS_ERR(info->avee))
		devm_gpiod_put(dev, info->avee);
}

static int panel_probe(struct mipi_dsi_device *dsi)
{
	struct panel_info *info;
	struct device *dev = &dsi->dev;
	int ret = 0xFF;

	if (vendor_id == 0xFF)
		get_lcm_id();

	switch (vendor_id) {
	case LCM_ILI9882T_KD_INX:
		pr_notice("[KERNEL/LCM] ili9882t kd inx panel, vendor_id = %d loading.\n",
				vendor_id);
		break;
	case LCM_ILI9882T_TG_HSD:
		pr_notice("[KERNEL/LCM] ili9882t tg hsd panel, vendor_id = %d loading.\n",
				vendor_id);
		break;
	case LCM_ILI9882T_STARRY_BOE:
		pr_notice("[KERNEL/LCM] ili9882t starry boe panel, vendor_id = %d loading.\n",
				vendor_id);
		break;
	default:
		pr_notice("[KERNEL/LCM] It's not ili9882t panel, exit.\n");
		return -ENODEV;
	}

	info = devm_kzalloc(&dsi->dev, sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;

	info->dev = dev;
	dsi->lanes = 4;
	dsi->format = MIPI_DSI_FMT_RGB888;
	dsi->mode_flags = MIPI_DSI_MODE_VIDEO | MIPI_DSI_MODE_VIDEO_SYNC_PULSE | MIPI_DSI_MODE_LPM;
	info->dsi = dsi;
	ret = panel_add(info);
	if (ret < 0) {
		panel_resources_release(info);
		devm_kfree(&dsi->dev, info);
		return ret;
	}

	mipi_dsi_set_drvdata(dsi, info);

	ret = mipi_dsi_attach(dsi);
	if (ret) {
		panel_resources_release(info);
		drm_panel_remove(&info->base);
		devm_kfree(&dsi->dev, info);
		return ret;
	}

#if defined(CONFIG_MTK_PANEL_EXT)
	mtk_panel_tch_handle_reg(&info->base);
	switch (vendor_id) {
	case LCM_ILI9882T_KD_INX:
		pr_notice("[KERNEL/LCM] ili9882t kd inx, pll_clock=%d.\n",
				ext_ili9882t_kd_inx_params.pll_clk);
		ret = mtk_panel_ext_create(dev, &ext_ili9882t_kd_inx_params,
						&ext_funcs, &info->base);
		break;
	case LCM_ILI9882T_TG_HSD:
		pr_notice("[KERNEL/LCM] ili9882t tg hsd, pll_clock=%d.\n",
				ext_ili9882t_tg_hsd_params.pll_clk);
		ret = mtk_panel_ext_create(dev, &ext_ili9882t_tg_hsd_params,
						&ext_funcs, &info->base);
		break;
	case LCM_ILI9882T_STARRY_BOE:
		pr_notice("[KERNEL/LCM] ili9882t starry boe, pll_clock=%d.\n",
				ext_ili9882t_starry_boe_params.pll_clk);
		ret = mtk_panel_ext_create(dev, &ext_ili9882t_starry_boe_params,
						&ext_funcs, &info->base);
		break;
	default:
		pr_notice("[KERNEL/LCM] It's default panel ili9882t kd inx, pll_clock=%d.\n",
				ext_ili9882t_kd_inx_params.pll_clk);
		ret = mtk_panel_ext_create(dev, &ext_ili9882t_kd_inx_params,
						&ext_funcs, &info->base);
		break;
	}
#endif
	if (ret < 0) {
		panel_resources_release(info);
		mipi_dsi_detach(dsi);
		drm_panel_remove(&info->base);
		devm_kfree(&dsi->dev, info);
		return ret;
	}
	share_info = info;
	old_state = LCM_ON;
	atomic_inc(&g_power_request);

	return ret;
}

static int panel_remove(struct mipi_dsi_device *dsi)
{
	struct panel_info *info = mipi_dsi_get_drvdata(dsi);
	int ret = 0xFF;

	ret = mipi_dsi_detach(dsi);
	if (ret < 0)
		dev_err(&dsi->dev, "failed to detach from DSI host: %d\n", ret);

	if (info->base.dev)
		drm_panel_remove(&info->base);

	unregister_reboot_notifier(&ili9882t_reboot_notifier);

	return 0;
}

static const struct of_device_id panel_of_match[] = {
	{ .compatible = "panel,ili9882t_wuxga_dsi_vdo" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, panel_of_match);

static struct mipi_dsi_driver panel_driver = {
	.driver = {
		.name = "panel-ili9882t_wuxga_dsi-vdo_tungsten",
		.of_match_table = panel_of_match,
	},
	.probe = panel_probe,
	.remove = panel_remove,
};
module_mipi_dsi_driver(panel_driver);

MODULE_DESCRIPTION("KD ili9882t_wuxga_dsi_vdo_tungsten 1200x2000 video mode panel driver");
MODULE_LICENSE("GPL v2");
