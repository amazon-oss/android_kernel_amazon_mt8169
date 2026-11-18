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
/* TG BOE panel porch */
#define TG_BOE_HFP (81)    // params->dsi.horizontal_frontporch
#define TG_BOE_HSA (4)     // params->dsi.horizontal_sync_active
#define TG_BOE_HBP (100)   // params->dsi.horizontal_backporch
#define TG_BOE_VFP (30)    // params->dsi.vertical_frontporch
#define TG_BOE_VSA (2)     // params->dsi.vertical_sync_active
#define TG_BOE_VBP (148)   // params->dsi.vertical_backporch

#define PHYSICAL_WIDTH			(135)
#define PHYSICAL_HEIGHT			(216)

static const struct drm_display_mode tg_boe_mode = {
	.clock = (FRAME_WIDTH + TG_BOE_HFP + TG_BOE_HSA + TG_BOE_HBP) *
		(FRAME_HEIGHT + TG_BOE_VFP + TG_BOE_VSA + TG_BOE_VBP) *
		60 / 1000,
	.hdisplay = FRAME_WIDTH,
	.hsync_start = FRAME_WIDTH + TG_BOE_HFP,
	.hsync_end = FRAME_WIDTH + TG_BOE_HFP + TG_BOE_HSA,
	.htotal = FRAME_WIDTH + TG_BOE_HFP + TG_BOE_HSA + TG_BOE_HBP,
	.vdisplay = FRAME_HEIGHT,
	.vsync_start = FRAME_HEIGHT + TG_BOE_VFP,
	.vsync_end = FRAME_HEIGHT + TG_BOE_VFP + TG_BOE_VSA,
	.vtotal = FRAME_HEIGHT + TG_BOE_VFP + TG_BOE_VSA + TG_BOE_VBP,
	.vrefresh = 60,
};

static atomic_t gesture_status = ATOMIC_INIT(0);
static atomic_t touch_upgrade_status = ATOMIC_INIT(0);
static unsigned char vendor_id = 0xFF;
static int old_state = LCM_OFF;
static atomic_t g_power_request = ATOMIC_INIT(0);
static struct panel_info *share_info;

static atomic_t nt36523n_rebooting = ATOMIC_INIT(0);

static void get_lcm_id(void);
static bool panel_check_gesture_mode(void);
static bool panel_check_touch_upgrade_mode(void);
static void lcm_power_manager_lock(struct panel_info *panel, int pwr_state);
static void lcm_power_manager(struct panel_info *panel, int new_state);
#if IS_ENABLED(CONFIG_IDME)
extern unsigned int idme_get_lcmid_info(void);
#endif

static DEFINE_MUTEX(nt36523n_share_power_mutex);
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

static int nt36523n_reboot(struct notifier_block *notifier, unsigned long val, void *v)
{
	pr_notice("[KERNEL/LCM]: panel nt36523n rebooting\n");
	atomic_set(&nt36523n_rebooting, 1);
	return NOTIFY_OK;
}

static struct notifier_block nt36523n_reboot_notifier = {
	.notifier_call = nt36523n_reboot,
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
	pr_notice("[nt36523n] %s, vendor id = 0x%x\n", __func__, vendor_id);
}

static struct LCM_setting_table lcm_init_nt36523n_tg_boe[] = {
	{0xFF, 1, {0x20}},
	{0xFB, 1, {0x01}},
	{0x05, 1, {0xD1}},
	{0x07, 1, {0x8C}},
	{0x08, 1, {0x46}},
	{0x0D, 1, {0x63}},
	{0x0E, 1, {0xAC}},
	{0x0F, 1, {0x5F}},
	{0x58, 1, {0x43}},
	{0x6E, 1, {0x00}},
	{0x75, 1, {0xA2}},
	{0x77, 1, {0xB3}},
	{0x95, 1, {0xFF}},
	{0x96, 1, {0xFF}},
	{0x9D, 1, {0x00}},
	{0x9E, 1, {0x00}},
	{0xFF, 1, {0x20}},
	{0xFB, 1, {0x01}},
	{0xB0, 16, {0x00, 0x08, 0x00, 0x21, 0x00, 0x4A, 0x00, 0x6B, 0x00, 0x87, 0x00, 0x9F, 0x00, 0xB5, 0x00, 0xC8}},
	{0xB1, 16, {0x00, 0xD9, 0x01, 0x10, 0x01, 0x37, 0x01, 0x79, 0x01, 0xA9, 0x01, 0xF5, 0x02, 0x30, 0x02, 0x32}},
	{0xB2, 16, {0x02, 0x6C, 0x02, 0xAC, 0x02, 0xD4, 0x03, 0x09, 0x03, 0x2A, 0x03, 0x53, 0x03, 0x5F, 0x03, 0x6E}},
	{0xB3, 12, {0x03, 0x7D, 0x03, 0x90, 0x03, 0xA6, 0x03, 0xBD, 0x03, 0xFC, 0x03, 0xFF}},
	{0xB4, 16, {0x00, 0x08, 0x00, 0x21, 0x00, 0x4A, 0x00, 0x6B, 0x00, 0x87, 0x00, 0x9F, 0x00, 0xB5, 0x00, 0xC8}},
	{0xB5, 16, {0x00, 0xD9, 0x01, 0x10, 0x01, 0x37, 0x01, 0x79, 0x01, 0xA9, 0x01, 0xF5, 0x02, 0x30, 0x02, 0x32}},
	{0xB6, 16, {0x02, 0x6C, 0x02, 0xAC, 0x02, 0xD4, 0x03, 0x09, 0x03, 0x2A, 0x03, 0x53, 0x03, 0x5F, 0x03, 0x6E}},
	{0xB7, 12, {0x03, 0x7D, 0x03, 0x90, 0x03, 0xA6, 0x03, 0xBD, 0x03, 0xFC, 0x03, 0xFF}},
	{0xB8, 16, {0x00, 0x08, 0x00, 0x21, 0x00, 0x4A, 0x00, 0x6B, 0x00, 0x87, 0x00, 0x9F, 0x00, 0xB5, 0x00, 0xC8}},
	{0xB9, 16, {0x00, 0xD9, 0x01, 0x10, 0x01, 0x37, 0x01, 0x79, 0x01, 0xA9, 0x01, 0xF5, 0x02, 0x30, 0x02, 0x32}},
	{0xBA, 16, {0x02, 0x6C, 0x02, 0xAC, 0x02, 0xD4, 0x03, 0x09, 0x03, 0x2A, 0x03, 0x53, 0x03, 0x5F, 0x03, 0x6E}},
	{0xBB, 12, {0x03, 0x7D, 0x03, 0x90, 0x03, 0xA6, 0x03, 0xBD, 0x03, 0xFC, 0x03, 0xFF}},
	{0xC6, 1, {0x32}},
	{0xC7, 1, {0x10}},
	{0xC8, 1, {0x41}},
	{0xC9, 1, {0x20}},
	{0xCA, 1, {0x10}},
	{0xCB, 1, {0x32}},
	{0xCC, 1, {0x10}},
	{0xCD, 1, {0x41}},
	{0xCE, 1, {0x20}},
	{0xCF, 1, {0x10}},
	{0xD0, 1, {0x32}},
	{0xD1, 1, {0x10}},
	{0xD2, 1, {0x41}},
	{0xD3, 1, {0x20}},
	{0xD4, 1, {0x10}},
	{0xD5, 1, {0x32}},
	{0xD6, 1, {0x10}},
	{0xD7, 1, {0x41}},
	{0xD8, 1, {0x20}},
	{0xD9, 1, {0x10}},
	{0xDA, 1, {0x32}},
	{0xDB, 1, {0x10}},
	{0xDC, 1, {0x41}},
	{0xDD, 1, {0x20}},
	{0xDE, 1, {0x10}},
	{0xDF, 1, {0x32}},
	{0xE0, 1, {0x10}},
	{0xE1, 1, {0x41}},
	{0xE2, 1, {0x20}},
	{0xE3, 1, {0x10}},
	{0xFF, 1, {0x21}},
	{0xFB, 1, {0x01}},
	{0xB0, 16, {0x00, 0x00, 0x00, 0x19, 0x00, 0x42, 0x00, 0x63, 0x00, 0x7F, 0x00, 0x97, 0x00, 0xAD, 0x00, 0xC0}},
	{0xB1, 16, {0x00, 0xD1, 0x01, 0x08, 0x01, 0x29, 0x01, 0x71, 0x01, 0xA1, 0x01, 0xED, 0x02, 0x28, 0x02, 0x29}},
	{0xB2, 16, {0x02, 0x64, 0x02, 0xA4, 0x02, 0xCC, 0x03, 0x01, 0x03, 0x22, 0x03, 0x4B, 0x03, 0x57, 0x03, 0x66}},
	{0xB3, 12, {0x03, 0x75, 0x03, 0x88, 0x03, 0x9E, 0x03, 0xB5, 0x03, 0xF4, 0x03, 0xF7}},
	{0xB4, 16, {0x00, 0x00, 0x00, 0x19, 0x00, 0x42, 0x00, 0x63, 0x00, 0x7F, 0x00, 0x97, 0x00, 0xAD, 0x00, 0xC0}},
	{0xB5, 16, {0x00, 0xD1, 0x01, 0x08, 0x01, 0x29, 0x01, 0x71, 0x01, 0xA1, 0x01, 0xED, 0x02, 0x28, 0x02, 0x29}},
	{0xB6, 16, {0x02, 0x64, 0x02, 0xA4, 0x02, 0xCC, 0x03, 0x01, 0x03, 0x22, 0x03, 0x4B, 0x03, 0x57, 0x03, 0x66}},
	{0xB7, 12, {0x03, 0x75, 0x03, 0x88, 0x03, 0x9E, 0x03, 0xB5, 0x03, 0xF4, 0x03, 0xF7}},
	{0xB8, 16, {0x00, 0x00, 0x00, 0x19, 0x00, 0x42, 0x00, 0x63, 0x00, 0x7F, 0x00, 0x97, 0x00, 0xAD, 0x00, 0xC0}},
	{0xB9, 16, {0x00, 0xD1, 0x01, 0x08, 0x01, 0x29, 0x01, 0x71, 0x01, 0xA1, 0x01, 0xED, 0x02, 0x28, 0x02, 0x29}},
	{0xBA, 16, {0x02, 0x64, 0x02, 0xA4, 0x02, 0xCC, 0x03, 0x01, 0x03, 0x22, 0x03, 0x4B, 0x03, 0x57, 0x03, 0x66}},
	{0xBB, 12, {0x03, 0x75, 0x03, 0x88, 0x03, 0x9E, 0x03, 0xB5, 0x03, 0xF4, 0x03, 0xF7}},
	{0xFF, 1, {0x23}},
	{0xFB, 1, {0x01}},
	{0x11, 1, {0x01}},
	{0x12, 1, {0x68}},
	{0x15, 1, {0xE9}},
	{0x16, 1, {0x0C}},
	{0xFF, 1, {0x24}},
	{0xFB, 1, {0x01}},
	{0x00, 1, {0x00}},
	{0x01, 1, {0x00}},
	{0x02, 1, {0x22}},
	{0x03, 1, {0x08}},
	{0x04, 1, {0x04}},
	{0x05, 1, {0x23}},
	{0x06, 1, {0x21}},
	{0x07, 1, {0x22}},
	{0x08, 1, {0x0F}},
	{0x09, 1, {0x0F}},
	{0x0A, 1, {0x0E}},
	{0x0B, 1, {0x0E}},
	{0x0C, 1, {0x0D}},
	{0x0D, 1, {0x0D}},
	{0x0E, 1, {0x0C}},
	{0x0F, 1, {0x0C}},
	{0x10, 1, {0x00}},
	{0x11, 1, {0x00}},
	{0x12, 1, {0x00}},
	{0x13, 1, {0x00}},
	{0x14, 1, {0x00}},
	{0x15, 1, {0x00}},
	{0x16, 1, {0x00}},
	{0x17, 1, {0x00}},
	{0x18, 1, {0x22}},
	{0x19, 1, {0x08}},
	{0x1A, 1, {0x04}},
	{0x1B, 1, {0x23}},
	{0x1C, 1, {0x21}},
	{0x1D, 1, {0x22}},
	{0x1E, 1, {0x0F}},
	{0x1F, 1, {0x0F}},
	{0x20, 1, {0x0E}},
	{0x21, 1, {0x0E}},
	{0x22, 1, {0x0D}},
	{0x23, 1, {0x0D}},
	{0x24, 1, {0x0C}},
	{0x25, 1, {0x0C}},
	{0x26, 1, {0x00}},
	{0x27, 1, {0x00}},
	{0x28, 1, {0x00}},
	{0x29, 1, {0x00}},
	{0x2A, 1, {0x00}},
	{0x2B, 1, {0x00}},
	{0x2D, 1, {0x20}},
	{0x2F, 1, {0x54}},
	{0x30, 1, {0x0D}},
	{0x33, 1, {0x54}},
	{0x34, 1, {0x0D}},
	{0x37, 1, {0x44}},
	{0x3A, 1, {0x9A}},
	{0x3B, 1, {0xA0}},
	{0x3D, 1, {0x92}},
	{0x3F, 1, {0x08}},
	{0x43, 1, {0x08}},
	{0x47, 1, {0x44}},
	{0x4A, 1, {0x9A}},
	{0x4B, 1, {0xA0}},
	{0x4C, 1, {0x91}},
	{0x4D, 1, {0x43}},
	{0x4E, 1, {0x21}},
	{0x51, 1, {0x12}},
	{0x52, 1, {0x34}},
	{0x55, 2, {0x87, 0x07}},
	{0x56, 1, {0x04}},
	{0x58, 1, {0x21}},
	{0x59, 1, {0x40}},
	{0x5A, 1, {0xBA}},
	{0x5B, 1, {0x96}},
	{0x5C, 1, {0x00}},
	{0x5D, 1, {0x00}},
	{0x5E, 2, {0x00, 0x13}},
	{0x5F, 1, {0x00}},
	{0x60, 1, {0x96}},
	{0x61, 1, {0x80}},
	{0x63, 1, {0x70}},
	{0x8D, 1, {0x88}},
	{0x8E, 1, {0x08}},
	{0x91, 1, {0x40}},
	{0x92, 1, {0xB7}},
	{0x94, 1, {0xAA}},
	{0xAD, 1, {0x00}},
	{0xAE, 1, {0x01}},
	{0xAF, 1, {0x0C}},
	{0xB0, 1, {0x16}},
	{0xB1, 1, {0x0D}},
	{0xB2, 1, {0x19}},
	{0xB3, 1, {0x80}},
	{0xB6, 12, {0x05, 0x00, 0x05, 0x00, 0x00, 0x00, 0x05, 0x05, 0x05, 0x05, 0x00, 0x00}},
	{0xBB, 1, {0x80}},
	{0xBC, 5, {0x00, 0x00, 0x40, 0x00, 0x00}},
	{0xC2, 1, {0xCA}},
	{0xC3, 1, {0x8A}},
	{0xC4, 1, {0x20}},
	{0xC5, 1, {0x03}},
	{0xC7, 1, {0x0D}},
	{0xC8, 1, {0x19}},
	{0xD7, 1, {0x55}},
	{0xDA, 1, {0x0A}},
	{0xDB, 1, {0x05}},
	{0xDC, 1, {0xDF}},
	{0xDD, 1, {0x22}},
	{0xDE, 1, {0x08}},
	{0xDF, 1, {0x05}},
	{0xE0, 1, {0xDF}},
	{0xE1, 1, {0x05}},
	{0xE2, 1, {0xDF}},
	{0xE3, 1, {0x05}},
	{0xE4, 1, {0xDF}},
	{0xE5, 1, {0x05}},
	{0xE6, 1, {0xDF}},
	{0xFF, 1, {0x25}},
	{0xFB, 1, {0x01}},
	{0x05, 1, {0x00}},
	{0x13, 1, {0x04}},
	{0x14, 1, {0x77}},
	{0x17, 1, {0xCF}},
	{0x19, 1, {0x0F}},
	{0x1B, 1, {0x5B}},
	{0x1F, 1, {0xBA}},
	{0x20, 1, {0x96}},
	{0x26, 1, {0xBA}},
	{0x27, 1, {0x96}},
	{0x33, 1, {0xBA}},
	{0x34, 1, {0x96}},
	{0x3F, 1, {0xE0}},
	{0x40, 1, {0x00}},
	{0x44, 1, {0x00}},
	{0x45, 1, {0x40}},
	{0x48, 1, {0xBA}},
	{0x49, 1, {0x96}},
	{0x5B, 1, {0x00}},
	{0x5C, 1, {0x00}},
	{0x5D, 1, {0x00}},
	{0x5E, 1, {0xD0}},
	{0x61, 1, {0xBA}},
	{0x62, 1, {0x96}},
	{0xDB, 1, {0x03}},
	{0xDC, 1, {0x84}},
	{0xFF, 1, {0x26}},
	{0xFB, 1, {0x01}},
	{0x00, 1, {0xA1}},
	{0x04, 1, {0x50}},
	{0x06, 1, {0x30}},
	{0x0A, 1, {0xF4}},
	{0x0C, 1, {0x16}},
	{0x0D, 1, {0x0B}},
	{0x0F, 1, {0x00}},
	{0x11, 1, {0x00}},
	{0x12, 1, {0x50}},
	{0x13, 1, {0x4D}},
	{0x14, 1, {0x54}},
	{0x15, 1, {0x00}},
	{0x16, 1, {0x10}},
	{0x19, 1, {0x1A}},
	{0x1A, 1, {0x5B}},
	{0x1B, 1, {0x18}},
	{0x1C, 1, {0x6E}},
	{0x1D, 1, {0x00}},
	{0x1E, 1, {0xC6}},
	{0x1F, 1, {0xC6}},
	{0x22, 1, {0x00}},
	{0x23, 1, {0x00}},
	{0x24, 1, {0x00}},
	{0x25, 1, {0xC6}},
	{0x2A, 1, {0x1A}},
	{0x2B, 1, {0x5B}},
	{0x2F, 1, {0x04}},
	{0x30, 1, {0xC6}},
	{0x33, 1, {0x89}},
	{0x34, 1, {0x67}},
	{0x39, 1, {0x00}},
	{0x3A, 1, {0xDF}},
	{0xA9, 1, {0x56}},
	{0xAA, 1, {0x5F}},
	{0xAB, 1, {0x4B}},
	{0xAC, 1, {0x51}},
	{0xAD, 1, {0x5C}},
	{0xAE, 1, {0x58}},
	{0xAF, 1, {0x5D}},
	{0xB0, 1, {0x50}},
	{0xB1, 1, {0x47}},
	{0xB2, 1, {0x49}},
	{0xB3, 1, {0x45}},
	{0xB4, 1, {0x4C}},
	{0xB5, 1, {0x53}},
	{0xB6, 1, {0x5A}},
	{0xB7, 1, {0x57}},
	{0xB8, 1, {0x46}},
	{0xB9, 1, {0x55}},
	{0xBA, 1, {0x61}},
	{0xBB, 1, {0x5E}},
	{0xBC, 1, {0x4A}},
	{0xBD, 1, {0x5B}},
	{0xBE, 1, {0x60}},
	{0xBF, 1, {0x59}},
	{0xC0, 1, {0x48}},
	{0xC1, 1, {0x4F}},
	{0xC2, 1, {0x54}},
	{0xC3, 1, {0x52}},
	{0xC4, 1, {0x4E}},
	{0xC5, 1, {0x62}},
	{0xC8, 1, {0x04}},
	{0xC9, 1, {0x9E}},
	{0xCA, 1, {0x4E}},
	{0xCB, 1, {0x00}},
	{0xCD, 2, {0x4E, 0x41}},
	{0xCE, 2, {0x4D, 0x41}},
	{0xCF, 3, {0x34, 0x00, 0x20}},
	{0xD2, 1, {0x3F}},
	{0xFF, 1, {0x27}},
	{0xFB, 1, {0x01}},
	{0x00, 1, {0x00}},
	{0x58, 1, {0x80}},
	{0x59, 1, {0x00}},
	{0x5A, 1, {0x00}},
	{0x5B, 1, {0x8F}},
	{0x5C, 1, {0x00}},
	{0x5D, 1, {0x27}},
	{0x5E, 1, {0x14}},
	{0x5F, 1, {0x08}},
	{0x60, 1, {0x20}},
	{0x61, 1, {0x41}},
	{0x62, 1, {0x42}},
	{0x63, 1, {0x00}},
	{0x64, 1, {0x12}},
	{0x65, 1, {0x40}},
	{0x66, 1, {0x42}},
	{0x67, 1, {0x00}},
	{0x68, 1, {0x12}},
	{0xC0, 1, {0x18}},
	{0xC1, 1, {0x00}},
	{0xC2, 1, {0x00}},
	{0xC3, 1, {0x00}},
	{0xD0, 1, {0x13}},
	{0xD1, 1, {0x59}},
	{0xDE, 1, {0x43}},
	{0xDF, 1, {0x02}},
	{0xFF, 1, {0x2A}},
	{0xFB, 1, {0x01}},
	{0x22, 1, {0x0F}},
	{0x23, 1, {0x08}},
	{0x24, 1, {0x00}},
	{0x25, 1, {0xC6}},
	{0x26, 1, {0xF8}},
	{0x27, 1, {0x00}},
	{0x28, 1, {0x1E}},
	{0x29, 1, {0x00}},
	{0x2A, 1, {0x1E}},
	{0x2B, 1, {0x00}},
	{0x2D, 1, {0x1E}},
	{0x64, 1, {0x16}},
	{0x67, 1, {0x16}},
	{0x6A, 1, {0x16}},
	{0x73, 1, {0x9E}},
	{0x76, 1, {0x16}},
	{0x79, 1, {0x9E}},
	{0x7C, 1, {0x16}},
	{0x97, 1, {0x3C}},
	{0x98, 1, {0x02}},
	{0x99, 1, {0x95}},
	{0x9A, 1, {0x06}},
	{0x9B, 1, {0x00}},
	{0x9C, 1, {0x0B}},
	{0x9D, 1, {0x0A}},
	{0x9E, 1, {0x90}},
	{0xA2, 1, {0xF3}},
	{0xA3, 1, {0xFF}},
	{0xA4, 1, {0xFF}},
	{0xA5, 1, {0xFF}},
	{0xD6, 1, {0x08}},
	{0xE8, 1, {0x00}},
	{0xF1, 1, {0x03}},
	{0xF2, 1, {0x3C}},
	{0xF3, 1, {0x02}},
	{0xF4, 1, {0x95}},
	{0xF5, 1, {0x06}},
	{0xF6, 1, {0x00}},
	{0xF7, 1, {0x0B}},
	{0xF8, 1, {0x0A}},
	{0xF9, 1, {0x90}},
	{0xB9, 9, {0x14, 0x00, 0x95, 0x00, 0xE0, 0x31, 0xA0, 0x70, 0x40}},
	{0xBC, 1, {0x33}},
	{0xFF, 1, {0xD0}},
	{0xFB, 1, {0x01}},
	{0x09, 1, {0xAE}},
	{0xFF, 1, {0x10}},
	{0xFB, 1, {0x01}},
	{0x35, 1, {0x00}},
	{0x3B, 5, {0x03, 0x96, 0x1E, 0x04, 0x04}},
	{0xB2, 1, {0x91}},
	{0xB3, 1, {0x00}},
	{0x11, 0, {}},
	{REGFLAG_DELAY, 110, {}},
	{0x29, 0, {}},
	{REGFLAG_DELAY, 45, {}},
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
	case LCM_NT36523N_TG_BOE:
		lcm_init_push_table(info, lcm_init_nt36523n_tg_boe,
			ARRAY_SIZE(lcm_init_nt36523n_tg_boe));
		break;
	default:
		pr_notice("Not match correct ID, loading default init code...\n");
		lcm_init_push_table(info, lcm_init_nt36523n_tg_boe,
			ARRAY_SIZE(lcm_init_nt36523n_tg_boe));
		break;
	}
}

static int panel_enter_sleep_mode(struct panel_info *info)
{
	struct mipi_dsi_device *dsi = info->dsi;
	int ret = 0xFF;

	dsi->mode_flags &= ~MIPI_DSI_MODE_LPM;

	ret = mipi_dsi_dcs_set_display_off(dsi);
	if (ret < 0)
		return ret;

	ret = mipi_dsi_dcs_enter_sleep_mode(dsi);
	if (ret < 0)
		return ret;

	return 0;
}

void panel_gesture_mode_set_by_nt36523n(bool gst_mode)
{
	atomic_set(&gesture_status, gst_mode);
}
EXPORT_SYMBOL_GPL(panel_gesture_mode_set_by_nt36523n);

static bool panel_check_gesture_mode(void)
{
	bool ret = false;

	ret = atomic_read(&gesture_status);

	return ret;
}

void panel_touch_upgrade_status_set_by_nt36523n(bool upg_mode)
{
	atomic_set(&touch_upgrade_status, upg_mode);
}
EXPORT_SYMBOL_GPL(panel_touch_upgrade_status_set_by_nt36523n);

static bool panel_check_touch_upgrade_mode(void)
{
	bool ret = false;

	ret = atomic_read(&touch_upgrade_status);

	return ret;
}

void nt36523n_share_tp_resetpin_control(int value)
{
	mutex_lock(&nt36523n_share_power_mutex);
	if (atomic_read(&g_power_request) <= 0) {
		LCM_ERROR("%s skip, bceause res_cnt(%d) <= 0",
				__func__, atomic_read(&g_power_request));
	} else {
		gpiod_set_value(share_info->ctp_rst_gpio, value);
	}
	mutex_unlock(&nt36523n_share_power_mutex);
}
EXPORT_SYMBOL(nt36523n_share_tp_resetpin_control);

static int panel_unprepare_power(struct drm_panel *panel)
{
	struct panel_info *info = to_info_panel(panel);

	if (!info->prepared_power)
		return 0;

	if ((false == panel_check_gesture_mode() && false == panel_check_touch_upgrade_mode()) ||
		atomic_read(&nt36523n_rebooting) == 1) {
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
		ret = panel_enter_sleep_mode(info);
		if (ret < 0) {
			dev_err(info->dev, "failed to set panel off: %d\n",
					ret);
			return ret;
		}
		if ((false == panel_check_gesture_mode() && false == panel_check_touch_upgrade_mode()) ||
			atomic_read(&nt36523n_rebooting) == 1) {
			mdelay(66);
			gpiod_set_value(info->lcm_rst_gpio, 0);
		}
		break;
	case TRS_DSB_TO_ON:
		//gesture mode resume
		gpiod_set_value(info->lcm_rst_gpio, 0);
		mdelay(1);
		gpiod_set_value(info->lcm_rst_gpio, 1);
		mdelay(1);
		gpiod_set_value(info->lcm_rst_gpio, 0);
		mdelay(1);
		gpiod_set_value(info->lcm_rst_gpio, 1);
		mdelay(12);
		lcm_panel_init(info);
		break;
	case TRS_DSB_TO_OFF:
		//normal suspend
		mdelay(5);
		bias_off(info->avdd, info->avee);
		mdelay(12);
		gpiod_set_value(info->ctp_rst_gpio, 0);
		if (info->vsim1)
			regulator_disable(info->vsim1);
		break;
	case TRS_ON_PHASE1:
		//normal resume phase1
		gpiod_set_value(info->lcm_rst_gpio, 0);
		gpiod_set_value(info->ctp_rst_gpio, 0);
		mdelay(1);

		if (info->vsim1) {
			ret = regulator_enable(info->vsim1);
			if (ret < 0)
				dev_err(info->dev, "Enable vsim1 fail, %ld\n", ret);
			mdelay(3);
		}

		gpiod_set_value(info->ctp_rst_gpio, 1);
		mdelay(5);
		bias_on(info->avdd, info->avee);
		mdelay(12);
		break;
	case TRS_ON_PHASE2:
		//normal resume phase2
		gpiod_set_value(info->lcm_rst_gpio, 1);
		mdelay(1);
		gpiod_set_value(info->lcm_rst_gpio, 0);
		mdelay(1);
		gpiod_set_value(info->lcm_rst_gpio, 1);
		mdelay(12);
		lcm_panel_init(info);
		break;
	case TRS_OFF_TO_ON:
		//normal resume phase1
		gpiod_set_value(info->lcm_rst_gpio, 0);
		gpiod_set_value(info->ctp_rst_gpio, 0);
		mdelay(1);

		if (info->vsim1) {
			ret = regulator_enable(info->vsim1);
			if (ret < 0)
				dev_err(info->dev, "Enable vsim1 fail, %ld\n", ret);
			mdelay(3);
		}

		gpiod_set_value(info->ctp_rst_gpio, 1);
		mdelay(5);
		bias_on(info->avdd, info->avee);
		mdelay(12);

		//normal resume phase2
		gpiod_set_value(info->lcm_rst_gpio, 1);
		mdelay(1);
		gpiod_set_value(info->lcm_rst_gpio, 0);
		mdelay(1);
		gpiod_set_value(info->lcm_rst_gpio, 1);
		mdelay(12);
		lcm_panel_init(info);
		break;
	case TRS_OFF:
		ret = panel_enter_sleep_mode(info);
		if (ret < 0) {
			dev_err(info->dev, "failed to set panel off: %d\n",
					ret);
			return ret;
		}
		mdelay(66);
		gpiod_set_value(info->lcm_rst_gpio, 0);
		mdelay(5);
		bias_off(info->avdd, info->avee);
		mdelay(12);
		gpiod_set_value(info->ctp_rst_gpio, 0);
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
	mutex_lock(&nt36523n_share_power_mutex);
	lcm_power_manager(info, pwr_state);
	mutex_unlock(&nt36523n_share_power_mutex);
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
		LCM_INFO("%s,no match state", __func__);
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

static struct mtk_panel_params ext_tg_boe_params = {
	.pll_clk = 555,
	.phy_timcon = {
		.clk_trail = 12,
	},
	.esd_check_enable = 1,
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
	case LCM_NT36523N_TG_BOE:
		mode = drm_mode_duplicate(connector->dev, &tg_boe_mode);
		if (!mode) {
			dev_err(panel->dev, "failed to add mode %ux%ux@%u\n",
				tg_boe_mode.hdisplay, tg_boe_mode.vdisplay,
				drm_mode_vrefresh(&tg_boe_mode));
			return -ENOMEM;
		}
		break;
	default:
		mode = drm_mode_duplicate(connector->dev, &tg_boe_mode);
		if (!mode) {
			dev_err(panel->dev, "failed to add mode %ux%ux@%u\n",
				tg_boe_mode.hdisplay, tg_boe_mode.vdisplay,
				drm_mode_vrefresh(&tg_boe_mode));
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

	ret = register_reboot_notifier(&nt36523n_reboot_notifier);
	if (ret < 0) {
		dev_err(dev, "Failed to register nt36523n reboot notifier %d\n", ret);
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
	case LCM_NT36523N_TG_BOE:
		pr_notice("[KERNEL/LCM] nt36523n panel, vendor_id = %d loading.\n",
				vendor_id);
		break;
	default:
		pr_notice("[KERNEL/LCM] It's not nt36523n panel, exit.\n");
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
	case LCM_NT36523N_TG_BOE:
		pr_notice("[KERNEL/LCM] nt36523n tg boe, pll_clock=%d.\n",
				ext_tg_boe_params.pll_clk);
		ret = mtk_panel_ext_create(dev, &ext_tg_boe_params,
						&ext_funcs, &info->base);
		break;
	default:
		pr_notice("[KERNEL/LCM] It's default panel nt36523n tg boe,pll_clock=%d.\n",
				ext_tg_boe_params.pll_clk);
		ret = mtk_panel_ext_create(dev, &ext_tg_boe_params,
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

	unregister_reboot_notifier(&nt36523n_reboot_notifier);

	return ret;
}

static const struct of_device_id panel_of_match[] = {
	{ .compatible = "panel,nt36523n_wuxga_dsi_vdo" },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, panel_of_match);

static struct mipi_dsi_driver panel_driver = {
	.driver = {
		.name = "panel-nt36523n_wuxga_dsi_vdo_tungsten",
		.of_match_table = panel_of_match,
	},
	.probe = panel_probe,
	.remove = panel_remove,
};
module_mipi_dsi_driver(panel_driver);

MODULE_DESCRIPTION("TG nt36523n_wuxga_dsi_vdo_tungsten 1200x1980 video mode panel driver");
MODULE_LICENSE("GPL v2");
