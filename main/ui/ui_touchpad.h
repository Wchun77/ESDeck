#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "app_config.h"     /* CFG_BG_LEN */
#include "ui_settings.h"    /* ui_settings_appearance_t */

/* -----------------------------------------------------------------------
 * Touchpad mode -- use the content area as a mouse touchpad for the PC.
 *
 * Layout (content area = SCREEN_W - SIDEBAR_W wide):
 *   - top strip     : Left / Right mouse buttons
 *   - rest          : touchpad surface (single finger, relative motion)
 * Sidebar (above the shared gear button from ui.c):
 *   - top button    : opens the existing mini keyboard (ui_keyboard_show())
 *   - middle strip  : scroll wheel (drag up/down)
 *
 * UI-only for now: every input is just logged (ESP_LOGI, tag "TOUCHPAD").
 * HID reporting to the PC is a later step.
 * ----------------------------------------------------------------------- */

/* Reserved appearance config -- same idea as ui_media_config_t, intended
 * to come from config/touchpad/<name>.json later. Not loaded from SD yet:
 * ui_touchpad_enter() uses an all-zero instance, which means
 * "no bg image, default overlay, default keyboard glyph". */
typedef struct {
    char     bg_image[CFG_BG_LEN];                   /* content bg, under SD_PATH_ASSETS_BG; empty = flat color */
    uint32_t overlay_color;                          /* 0xRRGGBB, translucent mask drawn over bg_image */
    uint8_t  overlay_opa;                            /* 0 = no mask */
    char     keyboard_icon[UI_SETTINGS_SIDE_ICON_LEN]; /* sidebar keyboard button icon; empty = LV_SYMBOL_KEYBOARD */
    ui_settings_appearance_t settings;               /* Settings overlay's own bg_image + side_icon */
} ui_touchpad_config_t;

/* Build Touchpad mode's sidebar widgets + content page. Must run on the
 * LVGL thread (same as ui_media_enter()). */
void ui_touchpad_enter(lv_obj_t *sidebar);

/* Tear down everything ui_touchpad_enter() created and restore the touch
 * read period. Safe to call when not entered. */
void ui_touchpad_exit(void);

/* Hide / show the content page while the Settings panel is up --
 * same contract as ui_media_deselect_current()/reselect_current(). */
void ui_touchpad_deselect_current(void);
void ui_touchpad_reselect_current(void);
