#include "ui_touchpad.h"
#include "ui.h"
#include "ui_settings.h"
#include "ui_keyboard.h"
#include "usb/usb_hid.h"
#include "app_config.h"     /* SD_PATH_ASSETS_BG, CFG_NVS_* */
#include "nvs_manager/nvs_manager.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdio.h>
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
/* Finger-position dot. Off for now: every dot move forces an LVGL redraw,
 * which (AVOID_TEAR_MODE_3 waits for VSYNC) appears to slow down and jitter
 * the touch read itself. Set to 1 to bring it back. */
#define TP_SHOW_DOT      0

/* Sidebar: keyboard button cell on top (same 80x80 cell / 64x64 button as
 * the gear cell in ui.c), wheel strip between it and the gear cell. */
#define SB_CELL          80
#define SB_BTN           64
#define SB_PAD           ((SB_CELL - SB_BTN) / 2)
#define WHEEL_X          SB_PAD
#define WHEEL_Y          (SB_CELL + SB_PAD)
#define WHEEL_W          SB_BTN
#define WHEEL_H          (SCREEN_H - SB_CELL - SB_CELL - SB_PAD * 2)

/* Wheel track: grip-line area between the up/down arrows, clipped so the
 * lines never run under the arrows or leave a gap at either end. */
#define WHEEL_ARROW_H    32
#define WHEEL_TRACK_Y    WHEEL_ARROW_H
#define WHEEL_TRACK_H    (WHEEL_H - WHEEL_ARROW_H * 2)

/* Input tuning */
#define TP_READ_PERIOD_MS   10    /* touch read period while in this mode (LVGL default here is 30) */
#define TP_FLUSH_PERIOD_MS  10    /* HID send period = IN endpoint bInterval */
#define TP_TAP_MAX_MS       200   /* press->release shorter than this ... */
#define TP_TAP_MAX_TRAVEL   10    /* ... and moved less than this (px) = tap */
#define TP_DOUBLE_TAP_MS    250   /* window after a tap for tap-and-drag / double tap */
#define WHEEL_NOTCH_PX      24    /* grip line spacing (visual only) */
#define WHEEL_NATURAL       1     /* 1: finger down = scroll up (phone-style) */
/* Smooth scrolling: raw finger pixels go to the PC (ESDeckPC scales them
 * to fractional WHEEL_DELTA units), instead of whole 120-unit notches. */
#define WHEEL_ANIM          0     /* grip lines follow the finger; off = no redraw while scrolling */
#define WHEEL_INERTIA       1     /* keep scrolling after a flick, decaying */
#define WHEEL_FRICTION      0.92f /* coast velocity kept per 10 ms tick */
#define WHEEL_COAST_MIN     0.3f  /* px per tick below which coasting stops */
#define WHEEL_FLICK_MIN     1.0f  /* px per tick needed at release to start coasting */
#define WHEEL_FLICK_IDLE_MS 50    /* finger held still longer than this before release = no flick */

/* Colors -- same palette as the other modes */
#define COL_PAGE_BG     0x222222
#define COL_SURFACE     0x1a1a1a
#define COL_BTN         0x2a2a2a
#define COL_BTN_PRESSED 0x0055cc
#define COL_BORDER      0x333333
#define COL_HINT        0x444444
#define COL_TEXT        0xcccccc
#define COL_ACCENT      0x00d4ff

/* With a bg image, surfaces go translucent so the image shows through
 * (image itself already sits under a 50% black mask, same as Media). */
#define OPA_SURFACE_ON_BG  LV_OPA_50
#define OPA_BTN_ON_BG      LV_OPA_70

/* -----------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */
static ui_touchpad_config_t s_cfg;          /* loaded in ui_touchpad_enter() */

static lv_obj_t *s_page       = NULL;       /* content page, child of screen */
static lv_obj_t *s_bg_img     = NULL;       /* page child0, only when bg_image is set */
static lv_obj_t *s_bg_mask    = NULL;       /* page child1, black 50% over s_bg_img */
static lv_img_dsc_t s_bg_dsc;
static uint8_t  *s_bg_data    = NULL;       /* PSRAM pixels behind s_bg_dsc, owned here */
static lv_obj_t *s_btn_left   = NULL;
static lv_obj_t *s_btn_right  = NULL;
static lv_obj_t *s_lbl_left   = NULL;       /* text labels, hidden by the "Hide Labels" toggle */
static lv_obj_t *s_lbl_right  = NULL;
static lv_obj_t *s_lbl_hint   = NULL;

static bool s_hide_labels        = false;
static bool s_hide_labels_loaded = false;   /* NVS read once, then cached */
static void apply_hide_labels(void);
static lv_obj_t *s_pad        = NULL;
static lv_obj_t *s_dot        = NULL;       /* finger position indicator */

static lv_obj_t *s_kb_cell    = NULL;       /* sidebar: keyboard button cell */
static lv_obj_t *s_wheel      = NULL;       /* sidebar: wheel strip */
static lv_obj_t *s_wheel_grip = NULL;       /* moving grip lines inside the wheel track */

static lv_indev_t *s_touch_indev   = NULL;
static uint32_t    s_prev_read_ms  = 0;
static lv_timer_t *s_flush_timer   = NULL;

/* Pad gesture state
 *   tap                 : click, delayed by TP_DOUBLE_TAP_MS so a second
 *                         touch can still turn it into a drag
 *   tap, then tap       : double click
 *   tap, then touch+move: drag (left held until the finger lifts) */
static uint32_t s_pad_press_tick = 0;
static uint32_t s_pad_travel     = 0;
static bool     s_tap_pending    = false;  /* a tap is waiting out the double-tap window */
static uint32_t s_tap_tick       = 0;
static bool     s_second_touch   = false;  /* current touch started inside that window */
static bool     s_dragging       = false;

/* Mouse button state from the L/R buttons */
static bool     s_left_held      = false;
static bool     s_right_held     = false;

/* Wheel state */
static int32_t  s_wheel_offset   = 0;   /* visual grip offset, wraps at WHEEL_NOTCH_PX */
static float    s_wheel_vel      = 0;   /* smoothed finger speed, px per 10 ms */
static uint32_t s_wheel_last_tick = 0;  /* last PRESSING with motion */
static float    s_wheel_coast    = 0;   /* inertia speed after release, px per tick */
static float    s_wheel_coast_rem = 0;  /* sub-pixel remainder of coasting */

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

static uint8_t current_mask(void)
{
    uint8_t m = 0;
    if (s_left_held || s_dragging) m |= HID_TP_BTN_LEFT;
    if (s_right_held)              m |= HID_TP_BTN_RIGHT;
    return m;
}

static void push_buttons(void)
{
    usb_hid_tp_buttons(current_mask());
}

static void click_left(void)
{
    uint8_t m = current_mask();
    usb_hid_tp_buttons(m | HID_TP_BTN_LEFT);
    usb_hid_tp_buttons(m & (uint8_t)~HID_TP_BTN_LEFT);
}

/* 10 ms: release a pending tap once its double-tap window has passed,
 * then send whatever is staged. */
/* Finger pixels -> wheel (+ = scroll up, Windows sign). */
static void send_wheel_px(int finger_px)
{
    usb_hid_tp_wheel(WHEEL_NATURAL ? finger_px : -finger_px);
}

static void wheel_coast_step(void)
{
    if (s_wheel_coast == 0) return;

    s_wheel_coast_rem += s_wheel_coast;
    int step = (int)s_wheel_coast_rem;
    s_wheel_coast_rem -= (float)step;
    if (step != 0) send_wheel_px(step);

    s_wheel_coast *= WHEEL_FRICTION;
    if (s_wheel_coast < WHEEL_COAST_MIN && s_wheel_coast > -WHEEL_COAST_MIN) {
        s_wheel_coast     = 0;
        s_wheel_coast_rem = 0;
    }
}

static void flush_timer_cb(lv_timer_t *t)
{
    (void)t;
    wheel_coast_step();
    if (s_tap_pending && lv_tick_elaps(s_tap_tick) >= TP_DOUBLE_TAP_MS) {
        s_tap_pending = false;
        click_left();
        ESP_LOGI(TAG, "click");
    }
    usb_hid_tp_flush();
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

    bool *held = (name[0] == 'L') ? &s_left_held : &s_right_held;

    if (code == LV_EVENT_PRESSED) {
        *held = true;
        push_buttons();
        ESP_LOGI(TAG, "%s down", name);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        /* PRESS_LOST counts as up too -- a button must never be left
         * logically held once the finger is gone. */
        if (!*held) return;
        *held = false;
        push_buttons();
        ESP_LOGI(TAG, "%s up", name);
    }
}

static void pad_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        s_pad_press_tick = lv_tick_get();
        s_pad_travel     = 0;

        /* Touch landed inside the double-tap window: hold the pending tap
         * back until we know whether this is a drag or a second tap. */
        s_second_touch = s_tap_pending;
        s_tap_pending  = false;

        pad_move_dot();
        if (s_dot) lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
    }
    else if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_get_act();
        if (!indev) return;

        lv_point_t v;
        lv_indev_get_vect(indev, &v);
        if (v.x == 0 && v.y == 0) return;

        usb_hid_tp_move(v.x, v.y);
        s_pad_travel += (uint32_t)(abs(v.x) + abs(v.y));
        pad_move_dot();

        if (s_second_touch && !s_dragging && s_pad_travel >= TP_TAP_MAX_TRAVEL) {
            s_dragging = true;
            push_buttons();
            ESP_LOGI(TAG, "drag start");
        }
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (s_dot) lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);

        uint32_t held  = lv_tick_elaps(s_pad_press_tick);
        bool     is_tap = (code == LV_EVENT_RELEASED) &&
                          held < TP_TAP_MAX_MS && s_pad_travel < TP_TAP_MAX_TRAVEL;

        if (s_dragging) {
            s_dragging = false;
            push_buttons();
            ESP_LOGI(TAG, "drag end");
        } else if (s_second_touch) {
            /* First tap was held back; this touch didn't become a drag. */
            click_left();
            if (is_tap) {
                click_left();
                ESP_LOGI(TAG, "double click");
            } else {
                ESP_LOGI(TAG, "click");
            }
        } else if (is_tap) {
            s_tap_pending = true;
            s_tap_tick    = lv_tick_get();
        }
        s_second_touch = false;
    }
}

static void wheel_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        leave_settings();
        s_wheel_coast     = 0;   /* touching the wheel stops any coasting */
        s_wheel_coast_rem = 0;
        s_wheel_vel       = 0;
        s_wheel_last_tick = lv_tick_get();
        lv_obj_set_style_bg_color(s_wheel, lv_color_hex(COL_BTN), 0);
    }
    else if (code == LV_EVENT_PRESSING) {
        lv_indev_t *indev = lv_indev_get_act();
        if (!indev) return;

        lv_point_t v;
        lv_indev_get_vect(indev, &v);
        if (v.y == 0) return;

#if WHEEL_ANIM
        /* Grip lines follow the finger, wrapping every notch, so the strip
         * reads as a physical wheel turning. Costs a redraw per move. */
        s_wheel_offset = (s_wheel_offset + v.y) % WHEEL_NOTCH_PX;
        if (s_wheel_offset < 0) s_wheel_offset += WHEEL_NOTCH_PX;
        lv_obj_set_y(s_wheel_grip, s_wheel_offset - WHEEL_NOTCH_PX);
#endif

        send_wheel_px(v.y);

        /* Speed estimate for the flick, normalised to px per 10 ms so it
         * doesn't depend on the actual touch read interval. */
        uint32_t dt = lv_tick_elaps(s_wheel_last_tick);
        if (dt == 0) dt = 1;
        float inst = (float)v.y * 10.0f / (float)dt;
        s_wheel_vel = s_wheel_vel * 0.5f + inst * 0.5f;
        s_wheel_last_tick = lv_tick_get();
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_set_style_bg_color(s_wheel, lv_color_hex(COL_SURFACE), 0);
#if WHEEL_INERTIA
        if (code == LV_EVENT_RELEASED &&
            lv_tick_elaps(s_wheel_last_tick) < WHEEL_FLICK_IDLE_MS &&
            (s_wheel_vel > WHEEL_FLICK_MIN || s_wheel_vel < -WHEEL_FLICK_MIN)) {
            s_wheel_coast     = s_wheel_vel;
            s_wheel_coast_rem = 0;
        }
#endif
        s_wheel_vel = 0;
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

    /* Grip lines live in a clipped track between the arrows. The grip
     * container is one notch taller than the track and starts one notch
     * above it, so shifting it by 0..NOTCH-1 px always keeps the track
     * fully covered. Nothing in here is clickable -- presses land on
     * s_wheel. */
    lv_obj_t *track = make_plain_obj(s_wheel);
    lv_obj_set_size(track, WHEEL_W, WHEEL_TRACK_H);
    lv_obj_set_pos(track, 0, WHEEL_TRACK_Y);
    lv_obj_set_style_bg_opa(track, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(track, LV_OBJ_FLAG_CLICKABLE);

    int grip_h     = WHEEL_TRACK_H + WHEEL_NOTCH_PX;
    int line_count = grip_h / WHEEL_NOTCH_PX + 1;

    s_wheel_grip = make_plain_obj(track);
    lv_obj_set_size(s_wheel_grip, WHEEL_W, grip_h);
    lv_obj_set_pos(s_wheel_grip, 0, -WHEEL_NOTCH_PX);
    lv_obj_set_style_bg_opa(s_wheel_grip, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_wheel_grip, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < line_count; i++) {
        lv_obj_t *line = make_plain_obj(s_wheel_grip);
        lv_obj_set_size(line, 28, 3);
        lv_obj_set_pos(line, (WHEEL_W - 28) / 2, WHEEL_NOTCH_PX / 2 + i * WHEEL_NOTCH_PX);
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

/* Decode bg_image straight into a PSRAM buffer -- copy of ui_media.c's
 * decode_bg_to_psram() (Media/Touchpad have no shared LRU pool to join,
 * same reasoning as there). Runs on the LVGL thread. */
static bool decode_bg_to_psram(const char *path, lv_img_dsc_t *dsc)
{
    lv_img_decoder_dsc_t dec;
    memset(&dec, 0, sizeof(dec));

    if (lv_img_decoder_open(&dec, path, lv_color_white(), 0) != LV_RES_OK) {
        ESP_LOGW(TAG, "bg decode open failed: %s", path);
        return false;
    }

    uint32_t    w  = dec.header.w;
    uint32_t    h  = dec.header.h;
    lv_img_cf_t cf = dec.header.cf;
    uint8_t     px = lv_img_cf_get_px_size(cf) / 8;

    /* JPEG reports px_size == 0 -- fix up to TRUE_COLOR. */
    if (px == 0) {
        cf = LV_IMG_CF_TRUE_COLOR;
        px = sizeof(lv_color_t);
    }

    size_t   sz  = (size_t)w * h * px;
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGW(TAG, "bg PSRAM alloc failed (%lu KB): %s", (unsigned long)(sz / 1024), path);
        lv_img_decoder_close(&dec);
        return false;
    }

    bool ok = true;
    if (dec.img_data) {
        memcpy(buf, dec.img_data, sz);
    } else {
        size_t stride = (size_t)w * px;
        for (lv_coord_t y = 0; y < (lv_coord_t)h && ok; y++) {
            if (lv_img_decoder_read_line(&dec, 0, y, (lv_coord_t)w,
                                          buf + (size_t)y * stride) != LV_RES_OK) {
                ok = false;
            }
        }
    }
    lv_img_decoder_close(&dec);

    if (!ok) {
        heap_caps_free(buf);
        return false;
    }

    dsc->header.cf          = cf;
    dsc->header.always_zero = 0;
    dsc->header.w           = w;
    dsc->header.h           = h;
    dsc->data_size          = sz;
    dsc->data               = buf;
    return true;
}

/* bg(child0) + 50% black mask(child1) under every widget on s_page,
 * cover-fit + centered -- same as Media's build_bg(). No-op when
 * bg_image is empty (flat COL_PAGE_BG stays). Returns true if a bg image
 * is actually showing. */
static bool build_bg(void)
{
    if (!s_cfg.bg_image[0]) return false;

    char path[sizeof("S:") + sizeof(SD_PATH_ASSETS_BG) + 1 + CFG_BG_LEN];
    snprintf(path, sizeof(path), "S:%s/%s", SD_PATH_ASSETS_BG, s_cfg.bg_image);

    if (!decode_bg_to_psram(path, &s_bg_dsc)) return false;
    s_bg_data = (uint8_t *)s_bg_dsc.data;

    uint32_t zoom_x   = (uint32_t)CONTENT_W * 256 / s_bg_dsc.header.w;
    uint32_t zoom_y   = (uint32_t)CONTENT_H * 256 / s_bg_dsc.header.h;
    uint32_t zoom     = (zoom_x > zoom_y) ? zoom_x : zoom_y;
    int32_t  scaled_w = (int32_t)s_bg_dsc.header.w * (int32_t)zoom / 256;
    int32_t  scaled_h = (int32_t)s_bg_dsc.header.h * (int32_t)zoom / 256;
    int32_t  off_x    = ((int32_t)CONTENT_W - scaled_w) / 2;
    int32_t  off_y    = ((int32_t)CONTENT_H - scaled_h) / 2;

    s_bg_img = lv_img_create(s_page);
    lv_obj_move_to_index(s_bg_img, 0);
    lv_img_set_src(s_bg_img, &s_bg_dsc);
    lv_img_set_pivot(s_bg_img, 0, 0);
    lv_obj_set_pos(s_bg_img, off_x, off_y);
    lv_obj_set_size(s_bg_img, CONTENT_W, CONTENT_H);
    lv_img_set_zoom(s_bg_img, (uint16_t)zoom);
    lv_obj_clear_flag(s_bg_img, LV_OBJ_FLAG_CLICKABLE);

    s_bg_mask = make_plain_obj(s_page);
    lv_obj_move_to_index(s_bg_mask, 1);
    lv_obj_set_size(s_bg_mask, CONTENT_W, CONTENT_H);
    lv_obj_set_pos(s_bg_mask, 0, 0);
    lv_obj_set_style_bg_color(s_bg_mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_bg_mask, LV_OPA_50, 0);
    lv_obj_clear_flag(s_bg_mask, LV_OBJ_FLAG_CLICKABLE);

    ESP_LOGI(TAG, "bg image: %s", s_cfg.bg_image);
    return true;
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

    bool has_bg = build_bg();

    s_btn_left  = build_mouse_btn(TP_MARGIN,                     "Left",  "L");
    s_btn_right = build_mouse_btn(TP_MARGIN * 2 + TP_BTN_W,      "Right", "R");
    s_lbl_left  = lv_obj_get_child(s_btn_left, 0);
    s_lbl_right = lv_obj_get_child(s_btn_right, 0);
    if (has_bg) {
        lv_obj_set_style_bg_opa(s_btn_left,  OPA_BTN_ON_BG, 0);
        lv_obj_set_style_bg_opa(s_btn_right, OPA_BTN_ON_BG, 0);
    }

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
    if (has_bg) lv_obj_set_style_bg_opa(s_pad, OPA_SURFACE_ON_BG, 0);

    lv_obj_t *hint = lv_label_create(s_pad);
    s_lbl_hint = hint;
    lv_label_set_text(hint, "Touchpad");
    lv_obj_set_style_text_color(hint, lv_color_hex(has_bg ? 0x777777 : COL_HINT), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_20, 0);
    lv_obj_center(hint);

#if TP_SHOW_DOT
    s_dot = make_plain_obj(s_pad);
    lv_obj_set_size(s_dot, TP_DOT_SIZE, TP_DOT_SIZE);
    lv_obj_set_style_radius(s_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_dot, lv_color_hex(COL_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_dot, LV_OPA_60, 0);
    lv_obj_clear_flag(s_dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_dot, LV_OBJ_FLAG_HIDDEN);
#endif
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void ui_touchpad_enter(lv_obj_t *sidebar)
{
    /* Selected config/touchpad/<name>.json; none selected / missing just
     * leaves s_cfg all-empty (flat colors, default gear glyph). Every mode
     * re-applies Settings' appearance on enter, same as Media. */
    ui_touchpad_config_load(&s_cfg);
    ui_settings_apply_appearance(&s_cfg.settings);

    build_sidebar(sidebar);
    build_page(lv_scr_act());
    ui_touchpad_get_hide_labels();   /* load from NVS on first enter */
    apply_hide_labels();

    s_wheel_offset    = 0;
    s_wheel_vel       = 0;
    s_wheel_coast     = 0;
    s_wheel_coast_rem = 0;
    s_tap_pending  = false;
    s_second_touch = false;
    s_dragging     = false;
    s_left_held    = false;
    s_right_held   = false;

    usb_hid_tp_reset_motion();
    push_buttons();   /* state 0 -- no-op unless an old release is still queued */

    set_fast_touch_read(true);
    s_flush_timer = lv_timer_create(flush_timer_cb, TP_FLUSH_PERIOD_MS, NULL);

    ESP_LOGI(TAG, "entered touchpad mode (read period %d ms)", TP_READ_PERIOD_MS);
}

void ui_touchpad_exit(void)
{
    /* Release anything held and try to get it out now. If the endpoint is
     * busy the release stays queued and goes out on the next enter; the
     * PC side also releases everything on disconnect. */
    s_tap_pending = s_second_touch = s_dragging = false;
    s_left_held = s_right_held = false;
    s_wheel_coast = 0;
    push_buttons();
    usb_hid_tp_flush();

    if (s_flush_timer) {
        lv_timer_del(s_flush_timer);
        s_flush_timer = NULL;
    }
    set_fast_touch_read(false);

    /* LVGL's image cache keys on the source pointer (&s_bg_dsc, a static
     * that's reused on every enter) -- invalidate before freeing so a
     * stale cache hit can't serve the freed buffer next time. Same as
     * ui_media_exit(). */
    lv_img_cache_invalidate_src(NULL);

    if (s_page) {
        lv_obj_del(s_page);   /* also deletes bg img/mask, buttons, pad, dot */
        s_page = NULL;
    }
    s_bg_img = s_bg_mask = s_btn_left = s_btn_right = s_pad = s_dot = NULL;
    s_lbl_left = s_lbl_right = s_lbl_hint = NULL;

    if (s_bg_data) {
        heap_caps_free(s_bg_data);
        s_bg_data = NULL;
    }
    memset(&s_bg_dsc, 0, sizeof(s_bg_dsc));

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

/* -----------------------------------------------------------------------
 * Hide Labels toggle
 * ----------------------------------------------------------------------- */
static void apply_hide_labels(void)
{
    lv_obj_t *lbls[] = { s_lbl_left, s_lbl_right, s_lbl_hint };
    for (size_t i = 0; i < sizeof(lbls) / sizeof(lbls[0]); i++) {
        if (!lbls[i]) continue;
        if (s_hide_labels) lv_obj_add_flag(lbls[i], LV_OBJ_FLAG_HIDDEN);
        else               lv_obj_clear_flag(lbls[i], LV_OBJ_FLAG_HIDDEN);
    }
}

bool ui_touchpad_get_hide_labels(void)
{
    if (!s_hide_labels_loaded) {
        char v[4] = "";
        s_hide_labels = nvs_manager_get_str(CFG_NVS_NAMESPACE, CFG_NVS_KEY_TPAD_HIDE_LBL,
                                            v, sizeof(v)) && v[0] == '1';
        s_hide_labels_loaded = true;
    }
    return s_hide_labels;
}

void ui_touchpad_set_hide_labels(bool hide)
{
    s_hide_labels        = hide;
    s_hide_labels_loaded = true;
    if (!nvs_manager_set_str(CFG_NVS_NAMESPACE, CFG_NVS_KEY_TPAD_HIDE_LBL, hide ? "1" : "0"))
        ESP_LOGW(TAG, "failed to save hide-labels to NVS");
    apply_hide_labels();
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
