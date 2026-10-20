#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "ui_touchpad_config.h"

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
 * Pad gestures: tap = left click (sent after a 250 ms double-tap window),
 * tap + tap = double click, tap + touch-and-move = drag.
 * Input goes to the PC as HID_TP_PAGE reports (usb_hid.h); ESDeckPC turns
 * them into SendInput mouse events.
 * ----------------------------------------------------------------------- */

/* Appearance config (bg_image + Settings appearance) lives in
 * ui_touchpad_config.h, loaded from config/touchpad/<name>.json. */

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
