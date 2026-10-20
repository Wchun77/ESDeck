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
 *   - top button    : opens the mini keyboard (ui_keyboard_show())
 *   - middle area   : per config sidebar_mode -- empty (default), scroll
 *                     wheel, or up to TP_HOTKEY_MAX hotkey buttons
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

/* "Hide Labels" Settings toggle -- hides the Left/Right/Touchpad text.
 * Persisted in NVS (CFG_NVS_KEY_TPAD_HIDE_LBL); set applies immediately
 * if Touchpad mode is currently built. LVGL thread only. */
bool ui_touchpad_get_hide_labels(void);
void ui_touchpad_set_hide_labels(bool hide);

/* Settings > Custom > Touchpad preferences, persisted in NVS
 * (CFG_NVS_KEY_TPAD_*), take effect immediately. LVGL thread only.
 *   Cursor / Scroll Speed : 0.50x .. 2.00x in 0.25 steps (default 1.00x),
 *                           applied on the ESP before sending
 *   Natural Scroll        : finger down = scroll up (default on)
 *   Scroll Inertia        : coast after a flick (default on)
 *   Tap to Click          : tap = left click, two-finger tap = right click,
 *                           tap-and-drag (default on) */
const char *ui_touchpad_cursor_speed_text(void);
void        ui_touchpad_cursor_speed_step(int dir);
const char *ui_touchpad_scroll_speed_text(void);
void        ui_touchpad_scroll_speed_step(int dir);
bool ui_touchpad_get_natural_scroll(void);
void ui_touchpad_set_natural_scroll(bool on);
bool ui_touchpad_get_inertia(void);
void ui_touchpad_set_inertia(bool on);
bool ui_touchpad_get_tap_click(void);
void ui_touchpad_set_tap_click(bool on);
