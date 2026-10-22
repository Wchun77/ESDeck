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
 *   - middle area   : per the Sidebar preference -- hotkey buttons from
 *                     the config (default; none = empty), scroll wheel,
 *                     or empty
 *
 * Pad gestures: tap = left click, tap + tap = double click, two-finger
 * move = scroll (vertical; horizontal per "2-Finger Swipe"), two-finger
 * tap = right click. Drag / select = hold the Left button + move on the pad
 * (fingers are tracked per region, see tp_touch_hook()); tap-and-drag is
 * compiled out (TP_TAP_DRAG).
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

/* Hide Buttons: no Left/Right strip, the pad fills the content area. Tap to
 * Click is treated as on while hidden (otherwise nothing could click). */
bool ui_touchpad_get_hide_buttons(void);
void ui_touchpad_set_hide_buttons(bool on);

/* 2-Finger Swipe: what a horizontal two-finger move does. */
typedef enum {
    TP_SWIPE_OFF = 0,
    TP_SWIPE_HSCROLL,     /* horizontal scroll (default) */
    TP_SWIPE_BACK_FWD,    /* one Back / Forward (mouse buttons 4/5) per swipe */
    TP_SWIPE_COUNT,
} tp_swipe_mode_t;

const char *const *ui_touchpad_swipe_mode_names(void);   /* TP_SWIPE_COUNT entries */
int  ui_touchpad_get_swipe_mode(void);
void ui_touchpad_set_swipe_mode(int mode);

/* Input Mode: how touchpad + mini keyboard input reaches the PC.
 *   TP_INPUT_ESDECKPC -- vendor reports, ESDeckPC injects them (default)
 *   TP_INPUT_USB_HID  -- the device's own USB keyboard + mouse interface;
 *                        no ESDeckPC needed, works where injected input
 *                        doesn't (games with anti-cheat, elevated windows) */
typedef enum {
    TP_INPUT_ESDECKPC = 0,
    TP_INPUT_USB_HID,
    TP_INPUT_COUNT,
} tp_input_mode_t;

const char *const *ui_touchpad_input_mode_names(void);   /* TP_INPUT_COUNT entries */
int  ui_touchpad_get_input_mode(void);
void ui_touchpad_set_input_mode(int mode);

/* Sidebar: what fills the sidebar between the keyboard button and the gear.
 * Hotkeys come from the selected config (none configured = empty strip).
 * Setting it rebuilds the sidebar right away when Touchpad mode is up. */
typedef enum {
    TP_SIDEBAR_HOTKEYS = 0,   /* default */
    TP_SIDEBAR_WHEEL,
    TP_SIDEBAR_EMPTY,
    TP_SIDEBAR_COUNT,
} tp_sidebar_mode_t;

const char *const *ui_touchpad_sidebar_mode_names(void); /* TP_SIDEBAR_COUNT entries */
int  ui_touchpad_get_sidebar_mode(void);
void ui_touchpad_set_sidebar_mode(int mode);

/* Top Left / Top Right Button: what each of the two top buttons does while
 * held -- a mouse button, or a modifier key held down (Ctrl + tap on the
 * pad = Ctrl+click). The button shows the function's name. */
typedef enum {
    TP_BTNF_LEFT = 0,    /* left mouse button (top-left default) */
    TP_BTNF_RIGHT,       /* right mouse button (top-right default) */
    TP_BTNF_CTRL,
    TP_BTNF_SHIFT,
    TP_BTNF_ALT,
    TP_BTNF_MIDDLE,      /* middle mouse button (appended: values are stored in NVS) */
    TP_BTNF_COUNT,
} tp_btn_func_t;

const char *const *ui_touchpad_btn_func_names(void);     /* TP_BTNF_COUNT entries */
int  ui_touchpad_get_btn_left_func(void);
void ui_touchpad_set_btn_left_func(int func);
int  ui_touchpad_get_btn_right_func(void);
void ui_touchpad_set_btn_right_func(int func);

/* True when tapping is the only way left to click -- buttons hidden, or
 * neither top button is Left Click -- so Tap to Click is on regardless of
 * its own setting (Settings shows the toggle locked on). */
bool ui_touchpad_tap_click_forced(void);
