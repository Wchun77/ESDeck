#include "ui_touchpad.h"
#include "ui.h"
#include "ui_settings.h"
#include "ui_keyboard.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdlib.h>
#include <string.h>

static const char *TAG = "TOUCHPAD";

/* Content area -- same convention as ui_media.c / ui_monitor.c. */
#define CONTENT_W   (SCREEN_W - SIDEBAR_W)
#define CONTENT_H   SCREEN_H

/* Layout */
#define TP_MARGIN        12
#define TP_BTN_BAR_H     100                                   /* top strip incl. margins */
#define TP_BTN_H         (TP_BTN_BAR_H - TP_MARGIN * 2)
#define TP_BTN_W         ((CONTENT_W - TP_MARGIN * 3) / 2)
#define TP_PAD_X         TP_MARGIN
#define TP_PAD_Y         TP_BTN_BAR_H
#define TP_PAD_W         (CONTENT_W - TP_MARGIN * 2)
#define TP_PAD_H         (CONTENT_H - TP_BTN_BAR_H - TP_MARGIN)
#define TP_DOT_SIZE      28

/* Sidebar: keyboard button cell on top (same 80x80 cell / 64x64 button as
 * the gear cell in ui.c), wheel strip between it and the gear cell. */
#define SB_CELL          80
#define SB_BTN           64
#define SB_PAD           ((SB_CELL - SB_BTN) / 2)
#define WHEEL_X          SB_PAD
#define WHEEL_Y          (SB_CELL + SB_PAD)
#define WHEEL_W          SB_BTN
#define WHEEL_H          (SCREEN_H - SB_CELL - SB_CELL - SB_PAD * 2)

/* Input tuning */
#define TP_READ_PERIOD_MS   10    /* touch read period while in this mode (LVGL default here is 30) */
#define TP_LOG_PERIOD_MS    100   /* throttle for motion logs */
#define TP_TAP_MAX_MS       200   /* press->release shorter than this ... */
#define TP_TAP_MAX_TRAVEL   10    /* ... and moved less than this (px) = tap */
#define WHEEL_NOTCH_PX      24    /* finger travel per wheel notch */
#define WHEEL_GRIP_LINES    9
#define WHEEL_NATURAL       1     /* 1: finger up = scroll down (phone-style) */

/* Colors -- same palette as the other modes */
#define COL_PAGE_BG     0x222222
#define COL_SURFACE     0x1a1a1a
#define COL_BTN         0x2a2a2a
#define COL_BTN_PRESSED 0x0055cc
#define COL_BORDER      0x333333
#define COL_HINT        0x444444
#define COL_TEXT        0xcccccc
#define COL_ACCENT      0x00d4ff

/* -----------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */
static ui_touchpad_config_t s_cfg;          /* reserved, all-zero until config loading exists */

static lv_obj_t *s_page       = NULL;       /* content page, child of screen */
static lv_obj_t *s_bg_mask    = NULL;       /* reserved: translucent overlay over a future bg image */
static lv_obj_t *s_btn_left   = NULL;
static lv_obj_t *s_btn_right  = NULL;
static lv_obj_t *s_pad        = NULL;
static lv_obj_t *s_dot        = NULL;       /* finger position indicator */

static lv_obj_t *s_kb_cell    = NULL;       /* sidebar: keyboard button cell */
static lv_obj_t *s_wheel      = NULL;       /* sidebar: wheel strip */
static lv_obj_t *s_wheel_grip = NULL;       /* moving grip lines inside the wheel */

static lv_indev_t *s_touch_indev   = NULL;
static uint32_t    s_prev_read_ms  = 0;

/* Pad gesture state */
static uint32_t s_pad_press_tick = 0;
static uint32_t s_pad_travel     = 0;
static int32_t  s_pad_acc_dx     = 0;
static int32_t  s_pad_acc_dy     = 0;
static uint32_t s_pad_log_tick   = 0;

/* Wheel state */
static int32_t  s_wheel_acc_px   = 0;   /* finger travel not yet turned into a notch */
static int32_t  s_wheel_offset   = 0;   /* visual grip offset, wraps at WHEEL_NOTCH_PX */

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */

/* Any sidebar interaction while Settings is up leaves Settings first,
 * same as Media's sidebar_bar_click_cb(). ui_settings_deselect() is a
 * no-op when Settings isn't shown. */
static void leave_settings(void)
{
    ui_settings_deselect();
    ui_touchpad_reselect_current();
}

static lv_indev_t *find_touch_indev(void)
{
    lv_indev_t *indev = lv_indev_get_next(NULL);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) return indev;
        indev = lv_indev_get_next(indev);
    }
    return NULL;
}

static void set_fast_touch_read(bool fast)
{
    if (fast) {
        s_touch_indev = find_touch_indev();
        if (!s_touch_indev || !s_touch_indev->driver->read_timer) {
            ESP_LOGW(TAG, "touch indev not found, read period unchanged");
            s_touch_indev = NULL;
            return;
        }
        s_prev_read_ms = s_touch_indev->driver->read_timer->period;
        lv_timer_set_period(s_touch_indev->driver->read_timer, TP_READ_PERIOD_MS);
    } else if (s_touch_indev) {
        lv_timer_set_period(s_touch_indev->driver->read_timer, s_prev_read_ms);
        s_touch_indev = NULL;
    }
}

static void pad_flush_motion_log(void)
{
    if (s_pad_acc_dx == 0 && s_pad_acc_dy == 0) return;
    ESP_LOGI(TAG, "move dx=%ld dy=%ld", (long)s_pad_acc_dx, (long)s_pad_acc_dy);
    s_pad_acc_dx = 0;
    s_pad_acc_dy = 0;
}

static void pad_move_dot(void)
{
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || !s_dot) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_area_t a;
    lv_obj_get_coords(s_pad, &a);

    lv_coord_t x = p.x - a.x1 - TP_DOT_SIZE / 2;
    lv_coord_t y = p.y - a.y1 - TP_DOT_SIZE / 2;
    lv_obj_set_pos(s_dot, x, y);
}

/* -----------------------------------------------------------------------
 * Event callbacks
 * ----------------------------------------------------------------------- */
static void mouse_btn_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    const char *name = (const char *)lv_event_get_user_data(e);

    if (code == LV_EVENT_PRESSED) {
        ESP_LOGI(TAG, "%s down", name);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        /* PRESS_LOST counts as up too -- a button must never be left
         * logically held once the finger is gone. */
        ESP_LOGI(TAG, "%s up", name);
    }
}

static void pad_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        s_pad_press_tick = lv_tick_get();
        s_pad_log_tick   = s_pad_press_tick;
        s_pad_travel     = 0;
        s_pad_acc_dx     = 0;
        s_pad_acc_dy     = 0;
        pad_move_dot();
        lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    }
    else if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_get_act();
        if (!indev) return;

        lv_point_t v;
        lv_indev_get_vect(indev, &v);
        if (v.x != 0 || v.y != 0) {
            s_pad_acc_dx += v.x;
            s_pad_acc_dy += v.y;
            s_pad_travel += (uint32_t)(abs(v.x) + abs(v.y));
            pad_move_dot();
        }

        if (lv_tick_elaps(s_pad_log_tick) >= TP_LOG_PERIOD_MS) {
            pad_flush_motion_log();
            s_pad_log_tick = lv_tick_get();
        }
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        pad_flush_motion_log();
        lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);

        uint32_t held = lv_tick_elaps(s_pad_press_tick);
        if (code == LV_EVENT_RELEASED &&
            held < TP_TAP_MAX_MS && s_pad_travel < TP_TAP_MAX_TRAVEL) {
            ESP_LOGI(TAG, "tap -> left click (%lu ms, %lu px)",
                     (unsigned long)held, (unsigned long)s_pad_travel);
        }
    }
}

static void wheel_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        leave_settings();
        s_wheel_acc_px = 0;
        lv_obj_set_style_bg_color(s_wheel, lv_color_hex(COL_BTN), 0);
    }
    else if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_get_act();
        if (!indev) return;

        lv_point_t v;
        lv_indev_get_vect(indev, &v);
        if (v.y == 0) return;

        /* Grip lines follow the finger, wrapping every notch, so the strip
         * reads as a physical wheel turning. */
        s_wheel_offset = (s_wheel_offset + v.y) % WHEEL_NOTCH_PX;
        if (s_wheel_offset < 0) s_wheel_offset += WHEEL_NOTCH_PX;
        lv_obj_set_y(s_wheel_grip, s_wheel_offset - WHEEL_NOTCH_PX);

        s_wheel_acc_px += v.y;
        while (s_wheel_acc_px >= WHEEL_NOTCH_PX || s_wheel_acc_px <= -WHEEL_NOTCH_PX) {
            int finger_down = (s_wheel_acc_px > 0) ? 1 : -1;
            s_wheel_acc_px -= finger_down * WHEEL_NOTCH_PX;

            /* Wheel delta sign follows Windows: + = scroll up. */
            int notch = WHEEL_NATURAL ? finger_down : -finger_down;
            ESP_LOGI(TAG, "wheel %+d (%s)", notch, notch > 0 ? "up" : "down");
        }
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        s_wheel_acc_px = 0;
        lv_obj_set_style_bg_color(s_wheel, lv_color_hex(COL_SURFACE), 0);
    }
}

static void kb_btn_cb(lv_event_t *e)
{
    (void)e;
    leave_settings();
    ESP_LOGI(TAG, "keyboard open");
    ui_keyboard_show();
}

/* -----------------------------------------------------------------------
 * Builders
 * ----------------------------------------------------------------------- */
static lv_obj_t *make_plain_obj(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void build_sidebar(lv_obj_t *sidebar)
{
    /* Keyboard button cell -- mirrors the gear cell in ui.c. */
    s_kb_cell = make_plain_obj(sidebar);
    lv_obj_set_size(s_kb_cell, SB_CELL, SB_CELL);
    lv_obj_set_pos(s_kb_cell, 0, 0);
    lv_obj_set_style_bg_opa(s_kb_cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(s_kb_cell, SB_PAD, 0);

    lv_obj_t *kb_btn = lv_btn_create(s_kb_cell);
    lv_obj_set_size(kb_btn, SB_BTN, SB_BTN);
    lv_obj_set_style_bg_color(kb_btn, lv_color_hex(COL_BTN), 0);
    lv_obj_set_style_radius(kb_btn, 8, 0);
    lv_obj_clear_flag(kb_btn, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(kb_btn, kb_btn_cb, LV_EVENT_CLICKED, NULL);

    /* Reserved: s_cfg.keyboard_icon (image) replaces this glyph later. */
    lv_obj_t *kb_lbl = lv_label_create(kb_btn);
    lv_label_set_text(kb_lbl, LV_SYMBOL_KEYBOARD);
    lv_obj_set_style_text_font(kb_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(kb_lbl);

    /* Wheel strip */
    s_wheel = make_plain_obj(sidebar);
    lv_obj_set_size(s_wheel, WHEEL_W, WHEEL_H);
    lv_obj_set_pos(s_wheel, WHEEL_X, WHEEL_Y);
    lv_obj_set_style_bg_color(s_wheel, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_radius(s_wheel, 8, 0);
    lv_obj_set_style_clip_corner(s_wheel, true, 0);
    lv_obj_add_flag(s_wheel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_clear_flag(s_wheel, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_wheel, wheel_cb, LV_EVENT_ALL, NULL);

    /* Grip lines -- one notch taller than needed so shifting by up to one
     * notch never exposes a gap. Children are not clickable, so presses
     * still land on s_wheel. */
    s_wheel_grip = make_plain_obj(s_wheel);
    lv_obj_set_size(s_wheel_grip, WHEEL_W, WHEEL_GRIP_LINES * WHEEL_NOTCH_PX + WHEEL_NOTCH_PX);
    lv_obj_set_pos(s_wheel_grip, 0, -WHEEL_NOTCH_PX);
    lv_obj_set_style_bg_opa(s_wheel_grip, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_wheel_grip, LV_OBJ_FLAG_CLICKABLE);

    int grip_top = (WHEEL_H - (WHEEL_GRIP_LINES - 1) * WHEEL_NOTCH_PX) / 2;
    for (int i = 0; i <= WHEEL_GRIP_LINES; i++) {
        lv_obj_t *line = make_plain_obj(s_wheel_grip);
        lv_obj_set_size(line, 28, 3);
        lv_obj_set_pos(line, (WHEEL_W - 28) / 2, grip_top + i * WHEEL_NOTCH_PX);
        lv_obj_set_style_bg_color(line, lv_color_hex(0x3a3a3a), 0);
        lv_obj_set_style_radius(line, 1, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
    }

    lv_obj_t *up = lv_label_create(s_wheel);
    lv_label_set_text(up, LV_SYMBOL_UP);
    lv_obj_set_style_text_color(up, lv_color_hex(0x666666), 0);
    lv_obj_align(up, LV_ALIGN_TOP_MID, 0, 8);

    lv_obj_t *down = lv_label_create(s_wheel);
    lv_label_set_text(down, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_color(down, lv_color_hex(0x666666), 0);
    lv_obj_align(down, LV_ALIGN_BOTTOM_MID, 0, -8);
}

/* Reserved: bg image + translucent overlay from s_cfg. Kept as children
 * 0/1 of s_page (before any widget), same ordering trick as Media's
 * build_bg(). With the all-zero config nothing is created. */
static void build_bg(void)
{
    if (s_cfg.bg_image[0] != '\0') {
        ESP_LOGW(TAG, "bg_image '%s' set but bg loading not implemented yet", s_cfg.bg_image);
    }

    if (s_cfg.overlay_opa > 0) {
        s_bg_mask = make_plain_obj(s_page);
        lv_obj_set_size(s_bg_mask, CONTENT_W, CONTENT_H);
        lv_obj_set_pos(s_bg_mask, 0, 0);
        lv_obj_set_style_bg_color(s_bg_mask, lv_color_hex(s_cfg.overlay_color), 0);
        lv_obj_set_style_bg_opa(s_bg_mask, s_cfg.overlay_opa, 0);
        lv_obj_clear_flag(s_bg_mask, LV_OBJ_FLAG_CLICKABLE);
    }
}

static lv_obj_t *build_mouse_btn(lv_coord_t x, const char *text, const char *name)
{
    lv_obj_t *btn = lv_btn_create(s_page);
    lv_obj_set_size(btn, TP_BTN_W, TP_BTN_H);
    lv_obj_set_pos(btn, x, TP_MARGIN);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_BTN), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_BTN_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_add_event_cb(btn, mouse_btn_cb, LV_EVENT_ALL, (void *)name);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);
    return btn;
}

static void build_page(lv_obj_t *scr)
{
    s_page = make_plain_obj(scr);
    lv_obj_set_size(s_page, CONTENT_W, CONTENT_H);
    lv_obj_set_pos(s_page, SIDEBAR_W, 0);
    lv_obj_set_style_bg_color(s_page, lv_color_hex(COL_PAGE_BG), 0);

    build_bg();

    s_btn_left  = build_mouse_btn(TP_MARGIN,                     "Left",  "L");
    s_btn_right = build_mouse_btn(TP_MARGIN * 2 + TP_BTN_W,      "Right", "R");

    s_pad = make_plain_obj(s_page);
    lv_obj_set_size(s_pad, TP_PAD_W, TP_PAD_H);
    lv_obj_set_pos(s_pad, TP_PAD_X, TP_PAD_Y);
    lv_obj_set_style_bg_color(s_pad, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_radius(s_pad, 12, 0);
    lv_obj_set_style_border_width(s_pad, 1, 0);
    lv_obj_set_style_border_color(s_pad, lv_color_hex(COL_BORDER), 0);
    lv_obj_add_flag(s_pad, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_clear_flag(s_pad, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_pad, pad_cb, LV_EVENT_ALL, NULL);

    lv_obj_t *hint = lv_label_create(s_pad);
    lv_label_set_text(hint, "Touchpad");
    lv_obj_set_style_text_color(hint, lv_color_hex(COL_HINT), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_20, 0);
    lv_obj_center(hint);

    s_dot = make_plain_obj(s_pad);
    lv_obj_set_size(s_dot, TP_DOT_SIZE, TP_DOT_SIZE);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_60, 0);
    lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void ui_touchpad_enter(lv_obj_t *sidebar)
{
    memset(&s_cfg, 0, sizeof(s_cfg));

    /* Every mode re-applies Settings' appearance on enter; all-empty =
     * plain bg + default gear glyph (see ui_settings_apply_appearance()). */
    ui_settings_apply_appearance(&s_cfg.settings);

    build_sidebar(sidebar);
    build_page(lv_scr_act());

    s_wheel_acc_px = 0;
    s_wheel_offset = 0;

    set_fast_touch_read(true);

    ESP_LOGI(TAG, "entered touchpad mode (UI only, read period %d ms)", TP_READ_PERIOD_MS);
}

void ui_touchpad_exit(void)
{
    set_fast_touch_read(false);

    if (s_page) {
        lv_obj_del(s_page);   /* also deletes bg mask, buttons, pad, dot */
        s_page = NULL;
    }
    s_bg_mask = s_btn_left = s_btn_right = s_pad = s_dot = NULL;

    if (s_kb_cell) {
        lv_obj_del(s_kb_cell);
        s_kb_cell = NULL;
    }
    if (s_wheel) {
        lv_obj_del(s_wheel);  /* also deletes grip lines */
        s_wheel = NULL;
    }
    s_wheel_grip = NULL;

    ESP_LOGI(TAG, "exited touchpad mode");
}

void ui_touchpad_deselect_current(void)
{
    if (!s_page) return;
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
}

void ui_touchpad_reselect_current(void)
{
    if (!s_page) return;
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_HIDDEN);
}
