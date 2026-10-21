#include "ui_touchpad.h"
#include "ui.h"
#include "ui_settings.h"
#include "ui_keyboard.h"
#include "usb/usb_hid.h"
#include "lvgl_port.h"       /* lvgl_port_set_touch_hook(), multi-touch points */
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
/* "Hide Buttons" preference: pad takes the whole content area. */
#define TP_PAD_FULL_Y    TP_MARGIN
#define TP_PAD_FULL_H    (CONTENT_H - TP_MARGIN * 2)

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
#define TP_FLUSH_PERIOD_MS  10    /* HID send period (one report per flush) */
#define TP_TAP_MAX_MS       200   /* press->release shorter than this ... */
#define TP_TAP_MAX_TRAVEL   10    /* ... and moved less than this (px) = tap */
#define TP_DOUBLE_TAP_MS    250   /* window after a tap for tap-and-drag / double tap */
/* Tap-and-drag (tap, then touch and move = left held). Off: dragging /
 * selecting is "hold the Left button + move on the pad" only, and a tap
 * clicks immediately instead of waiting TP_DOUBLE_TAP_MS. Set to 1 to
 * bring it back -- it only ever applies while Hide Buttons is on. */
#define TP_TAP_DRAG         0
#define TP_TWO_TAP_MAX_MS   250   /* two-finger tap (right click): shorter than this ... */
#define TP_TWO_TAP_MAX_TRAVEL 15  /* ... and scrolled less than this (px) */
#define TP_MATCH_MAX_PX     150   /* moved less than this between reads = same finger */
#define TP_AXIS_LOCK_PX     10    /* two-finger travel before locking vertical / horizontal */
#define TP_SWIPE_TRIGGER_PX 80    /* two-finger horizontal travel for one Back / Forward */
/* Soft start: GT911 holds the coordinate for ~150 ms after touch-down,
 * then catches up in ~40 ms -- sent as-is that burst reads as a flick and
 * Windows' pointer acceleration throws the cursor. The first
 * TP_SOFT_START_PX of each touch ramp from TP_SOFT_START_GAIN up to full. */
#define TP_SOFT_START_PX    20
#define TP_SOFT_START_GAIN  0.3f
#define WHEEL_NOTCH_PX      24    /* grip line spacing (visual only) */
/* Scroll direction / speed / inertia / tap-to-click are runtime preferences,
 * see "Preferences" below (Settings > Custom > Touchpad). */
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

/* Preferences (Settings > Custom > Touchpad), loaded from NVS once. */
#define PREF_SPEED_MIN   50
#define PREF_SPEED_MAX   200
#define PREF_SPEED_STEP  25

static bool    s_prefs_loaded  = false;
static int     s_cursor_pct    = 100;
static int     s_scroll_pct    = 100;
static bool    s_natural       = true;
static bool    s_inertia       = true;
static bool    s_tap_click     = true;
static float   s_move_rem_x    = 0;    /* sub-pixel remainders after speed scaling */
static float   s_move_rem_y    = 0;
static float   s_wheel_rem     = 0;
static void    prefs_load(void);
static lv_obj_t *s_pad        = NULL;

/* More preferences (see "Preferences" below) */
static bool    s_hide_buttons  = false;   /* pad covers the whole content area */
static int     s_swipe_mode    = TP_SWIPE_HSCROLL;
static int     s_input_mode    = TP_INPUT_ESDECKPC;
static int     s_sidebar_mode  = TP_SIDEBAR_HOTKEYS;
static lv_obj_t *s_sidebar     = NULL;    /* ui.c's sidebar, kept to rebuild the middle area */

static lv_obj_t *s_kb_cell    = NULL;       /* sidebar: keyboard button cell */
static lv_obj_t *s_wheel      = NULL;       /* sidebar: wheel strip (Sidebar = Wheel) */
static lv_obj_t *s_hk_cont    = NULL;       /* sidebar: hotkey buttons (Sidebar = Hotkeys) */
static int       s_hk_down    = -1;         /* hotkey currently held, -1 = none */
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

/* Two-finger state for the current touch:
 *   two finger move = scroll (same smooth wheel + inertia as the strip)
 *   two finger tap  = right click */
static bool     s_pad_two        = false;
static bool     s_pad_two_frozen = false;  /* a finger lifted mid-gesture: ignore the rest */
static uint32_t s_pad_two_travel = 0;
static int      s_two_axis       = 0;      /* 0 undecided, 1 vertical, 2 horizontal */
static int32_t  s_two_sum_x      = 0;      /* travel before the axis is locked */
static int32_t  s_two_sum_y      = 0;
static bool     s_swipe_done     = false;  /* Back/Forward already fired this gesture */

/* Pad gesture lifetime (first pad finger down .. last pad finger up) */
static bool     s_gesture        = false;
static bool     s_gesture_btn    = false;  /* a L/R button was held during it: no taps */

/* Mouse button state from the L/R buttons */
static bool     s_left_held      = false;
static bool     s_right_held     = false;

/* Wheel state */
static int32_t  s_wheel_offset   = 0;   /* visual grip offset, wraps at WHEEL_NOTCH_PX */
static float    s_wheel_vel      = 0;   /* smoothed finger speed, px per 10 ms */
static uint32_t s_wheel_last_tick = 0;  /* last PRESSING with motion */
static float    s_wheel_coast    = 0;   /* inertia speed after release, px per tick */
static float    s_wheel_coast_rem = 0;  /* sub-pixel remainder of coasting */
static int      s_scroll_axis    = 0;   /* 0 vertical, 1 horizontal -- current scroll / coast */

/* -----------------------------------------------------------------------
 * Finger tracking
 *
 * The content area (L/R buttons + pad) is driven from the raw multi-touch
 * points (lvgl_port touch hook, tp_touch_hook()), not LVGL events -- LVGL
 * only sees the first finger, which made "hold Left with one hand, move
 * on the pad with the other" impossible. Each finger keeps the region it
 * went down in for its whole life (like LVGL's press-lock); identity across
 * reads is by nearest position since the controller's point order isn't
 * stable. Sidebar / Settings / keyboard overlay stay on LVGL events.
 * ----------------------------------------------------------------------- */
typedef enum { REG_NONE = 0, REG_LEFT, REG_RIGHT, REG_PAD } tp_region_t;

typedef struct {
    bool        active;
    tp_region_t reg;
    int16_t     x, y;      /* last position (screen coordinates) */
    int16_t     dx, dy;    /* motion since the previous read */
} tp_contact_t;

static tp_contact_t s_ct[LVGL_PORT_TOUCH_MAX_POINTS];

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

static void click_right(void)
{
    uint8_t m = current_mask();
    usb_hid_tp_buttons(m | HID_TP_BTN_RIGHT);
    usb_hid_tp_buttons(m & (uint8_t)~HID_TP_BTN_RIGHT);
}

/* 10 ms: release a pending tap once its double-tap window has passed,
 * then send whatever is staged. */
/* Finger pixels -> wheel (+ = scroll up, Windows sign). */
static void send_wheel_px(int finger_px)
{
    float f = (float)finger_px * (float)s_scroll_pct / 100.0f + s_wheel_rem;
    int   px = (int)f;
    s_wheel_rem = f - (float)px;
    if (px == 0) return;

    /* Natural: content follows the finger. Vertical: finger down = wheel
     * up (+). Horizontal: finger right = view moves left = hwheel (-). */
    if (s_scroll_axis == 0) usb_hid_tp_wheel(s_natural ? px : -px);
    else                    usb_hid_tp_hwheel(s_natural ? -px : px);
}

/* Pad finger motion -> cursor, with the Cursor Speed preference. */
/* k = extra gain on top of Cursor Speed (soft start), 1.0 = none. */
static void send_move(int dx, int dy, float k)
{
    float g  = (float)s_cursor_pct / 100.0f * k;
    float fx = (float)dx * g + s_move_rem_x;
    float fy = (float)dy * g + s_move_rem_y;
    int ix = (int)fx, iy = (int)fy;
    s_move_rem_x = fx - (float)ix;
    s_move_rem_y = fy - (float)iy;
    if (ix || iy) usb_hid_tp_move(ix, iy);
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

/* Shared smooth-scroll tracking -- used by the sidebar wheel strip and by
 * two-finger scrolling on the pad. begin on touch down, track per move,
 * end on release (starts inertia coasting after a flick). */
static void scroll_begin(int axis)
{
    s_scroll_axis     = axis;
    s_wheel_rem       = 0;
    s_wheel_coast     = 0;   /* new touch stops any coasting */
    s_wheel_coast_rem = 0;
    s_wheel_vel       = 0;
    s_wheel_last_tick = lv_tick_get();
}

static void scroll_track(int dy)
{
    if (dy == 0) return;
    send_wheel_px(dy);

    /* Speed estimate for the flick, normalised to px per 10 ms so it
     * doesn't depend on the actual touch read interval. */
    uint32_t dt = lv_tick_elaps(s_wheel_last_tick);
    if (dt == 0) dt = 1;
    float inst = (float)dy * 10.0f / (float)dt;
    s_wheel_vel = s_wheel_vel * 0.5f + inst * 0.5f;
    s_wheel_last_tick = lv_tick_get();
}

static void scroll_end(bool allow_flick)
{
#if WHEEL_INERTIA
    if (allow_flick && s_inertia &&
        lv_tick_elaps(s_wheel_last_tick) < WHEEL_FLICK_IDLE_MS &&
        (s_wheel_vel > WHEEL_FLICK_MIN || s_wheel_vel < -WHEEL_FLICK_MIN)) {
        s_wheel_coast     = s_wheel_vel;
        s_wheel_coast_rem = 0;
    }
#else
    (void)allow_flick;
#endif
    s_wheel_vel = 0;
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
    usb_hid_kb_flush();   /* sidebar hotkeys go out on the keyboard report path */
}

/* Effective tap-to-click: forced on while the L/R buttons are hidden,
 * otherwise there would be no way left to click at all. */
static bool tap_enabled(void)
{
    return s_tap_click || s_hide_buttons;
}

/* See TP_TAP_DRAG. */
static bool tap_drag_enabled(void)
{
    return TP_TAP_DRAG && s_hide_buttons && tap_enabled();
}

static void click_x(uint8_t btn)
{
    uint8_t m = current_mask();
    usb_hid_tp_buttons(m | btn);
    usb_hid_tp_buttons(m & (uint8_t)~btn);
}

/* L/R buttons visible or pad stretched over the whole content area. */
static void apply_layout(void)
{
    if (!s_pad || !s_btn_left || !s_btn_right) return;

    if (s_hide_buttons) {
        lv_obj_add_flag(s_btn_left,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_btn_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_pad, TP_PAD_X, TP_PAD_FULL_Y);
        lv_obj_set_size(s_pad, TP_PAD_W, TP_PAD_FULL_H);
    } else {
        lv_obj_clear_flag(s_btn_left,  LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_btn_right, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_pad, TP_PAD_X, TP_PAD_Y);
        lv_obj_set_size(s_pad, TP_PAD_W, TP_PAD_H);
    }
    lv_obj_update_layout(s_pad);   /* region_at() reads the coords right away */
}

/* Pad / buttons only take input while they're actually what the user
 * is touching: not under Settings, not under the keyboard overlay. */
static bool input_enabled(void)
{
    return s_page && !lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN) && !ui_keyboard_is_open();
}

static bool obj_hit(lv_obj_t *obj, int16_t x, int16_t y)
{
    if (!obj || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

/* Region a finger went down in (screen coordinates). */
static tp_region_t region_at(int16_t x, int16_t y)
{
    if (!input_enabled()) return REG_NONE;
    if (obj_hit(s_btn_left, x, y))  return REG_LEFT;
    if (obj_hit(s_btn_right, x, y)) return REG_RIGHT;
    if (obj_hit(s_pad, x, y))       return REG_PAD;
    return REG_NONE;   /* sidebar, margins -- LVGL's business or nothing */
}

/* Buttons held = any finger that went down on them is still down. */
static void update_buttons(void)
{
    bool l = false, r = false;
    for (int i = 0; i < LVGL_PORT_TOUCH_MAX_POINTS; i++) {
        if (!s_ct[i].active) continue;
        if (s_ct[i].reg == REG_LEFT)  l = true;
        if (s_ct[i].reg == REG_RIGHT) r = true;
    }
    if (l == s_left_held && r == s_right_held) return;

    if (l != s_left_held && s_btn_left) {
        if (l) lv_obj_add_state(s_btn_left, LV_STATE_PRESSED);
        else   lv_obj_clear_state(s_btn_left, LV_STATE_PRESSED);
        ESP_LOGI(TAG, "L %s", l ? "down" : "up");
    }
    if (r != s_right_held && s_btn_right) {
        if (r) lv_obj_add_state(s_btn_right, LV_STATE_PRESSED);
        else   lv_obj_clear_state(s_btn_right, LV_STATE_PRESSED);
        ESP_LOGI(TAG, "R %s", r ? "down" : "up");
    }
    s_left_held  = l;
    s_right_held = r;
    if (l || r) s_gesture_btn = true;
    push_buttons();
}

/* -----------------------------------------------------------------------
 * Pad gesture (fingers whose region is REG_PAD)
 *
 *   one finger   : move; tap = left click (after the double-tap window),
 *                  tap + tap = double click, tap + touch-and-move = drag
 *   two fingers  : axis locks on the first TP_AXIS_LOCK_PX of travel
 *                  vertical   -> scroll (smooth + inertia)
 *                  horizontal -> per "2-Finger Swipe": horizontal scroll,
 *                                Back / Forward (once per gesture), or off
 *                  quick tap  -> right click
 *   a held L/R button plus a pad finger is just "button + move" -- that's
 *   how click-and-drag selection works; taps are suppressed meanwhile.
 * ----------------------------------------------------------------------- */
static void gesture_begin(void)
{
    s_gesture        = true;
    s_gesture_btn    = s_left_held || s_right_held;
    s_pad_press_tick = lv_tick_get();
    s_pad_travel     = 0;
    s_pad_two        = false;
    s_pad_two_frozen = false;
    s_pad_two_travel = 0;

    /* Touch landed inside the double-tap window: hold the pending tap
     * back until we know whether this is a drag or a second tap. */
    s_second_touch = s_tap_pending && tap_drag_enabled();
    s_tap_pending  = false;
}

static void two_begin(void)
{
    s_pad_two    = true;
    s_two_axis   = 0;
    s_two_sum_x  = 0;
    s_two_sum_y  = 0;
    s_swipe_done = false;
    if (s_second_touch) {
        /* A tap was held back for tap-and-drag -- it was a click after all. */
        click_left();
        s_second_touch = false;
    }
    s_wheel_coast = 0;   /* a new two-finger touch stops any coasting */
    ESP_LOGI(TAG, "two-finger gesture");
}

static void two_end(bool flick)
{
    if (s_two_axis == 1 || (s_two_axis == 2 && s_swipe_mode == TP_SWIPE_HSCROLL))
        scroll_end(flick);
}

static void two_move(int dx, int dy)
{
    s_pad_two_travel += (uint32_t)(abs(dx) + abs(dy));

    if (s_two_axis == 0) {
        s_two_sum_x += dx;
        s_two_sum_y += dy;
        int ax = abs(s_two_sum_x), ay = abs(s_two_sum_y);
        if (ax < TP_AXIS_LOCK_PX && ay < TP_AXIS_LOCK_PX) return;

        s_two_axis = (ay >= ax) ? 1 : 2;
        if (s_two_axis == 1) {
            scroll_begin(0);
            scroll_track(s_two_sum_y);   /* don't lose the travel used to decide */
        } else if (s_swipe_mode == TP_SWIPE_HSCROLL) {
            scroll_begin(1);
            scroll_track(s_two_sum_x);
        }
        ESP_LOGI(TAG, "two-finger %s", s_two_axis == 1 ? "vertical" : "horizontal");
        return;
    }

    if (s_two_axis == 1) {
        scroll_track(dy);
        return;
    }

    /* horizontal */
    if (s_swipe_mode == TP_SWIPE_HSCROLL) {
        scroll_track(dx);
    } else if (s_swipe_mode == TP_SWIPE_BACK_FWD && !s_swipe_done) {
        s_two_sum_x += dx;
        if (s_two_sum_x >= TP_SWIPE_TRIGGER_PX || s_two_sum_x <= -TP_SWIPE_TRIGGER_PX) {
            /* Same as browsers / macOS: swipe right = back, left = forward. */
            bool back = s_two_sum_x > 0;
            click_x(back ? HID_TP_BTN_BACK : HID_TP_BTN_FORWARD);
            s_swipe_done = true;
            ESP_LOGI(TAG, "swipe -> %s", back ? "back" : "forward");
        }
    }
}

static void gesture_end(void)
{
    uint32_t held = lv_tick_elaps(s_pad_press_tick);
    s_gesture = false;

    if (s_pad_two) {
        if (!s_pad_two_frozen) two_end(true);
        if (tap_enabled() && !s_gesture_btn &&
            held < TP_TWO_TAP_MAX_MS && s_pad_two_travel < TP_TWO_TAP_MAX_TRAVEL) {
            click_right();
            ESP_LOGI(TAG, "two-finger tap -> right click");
        }
        s_pad_two      = false;
        s_second_touch = false;
        return;
    }

    bool is_tap = tap_enabled() && !s_gesture_btn &&
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
    } else if (is_tap && tap_drag_enabled()) {
        /* Wait out the double-tap window: a touch inside it may still
         * become a drag (flush_timer_cb() clicks if it doesn't). */
        s_tap_pending = true;
        s_tap_tick    = lv_tick_get();
    } else if (is_tap) {
        click_left();   /* no tap-and-drag: click now; two taps = double click */
        ESP_LOGI(TAG, "click");
    }
    s_second_touch = false;
}

/* One read's worth of pad fingers. */
static void gesture_update(int pad_count, int dx_sum, int dy_sum, int moved)
{
    if (!s_gesture) {
        if (pad_count == 0) return;
        gesture_begin();
    }
    if (pad_count == 0) {
        gesture_end();
        return;
    }

    if (!s_pad_two && pad_count >= 2 && !s_dragging) two_begin();

    if (s_pad_two) {
        if (s_pad_two_frozen) return;
        if (pad_count < 2) {
            /* One finger lifted: stop here, a fast lift still flicks. */
            s_pad_two_frozen = true;
            two_end(true);
            return;
        }
        if (moved) two_move(dx_sum / moved, dy_sum / moved);   /* average of both fingers */
        return;
    }

    if (dx_sum == 0 && dy_sum == 0) return;
    float k = 1.0f;
    if (s_pad_travel < TP_SOFT_START_PX)
        k = TP_SOFT_START_GAIN + (1.0f - TP_SOFT_START_GAIN) * (float)s_pad_travel / (float)TP_SOFT_START_PX;
    send_move(dx_sum, dy_sum, k);
    s_pad_travel += (uint32_t)(abs(dx_sum) + abs(dy_sum));

    if (s_second_touch && !s_dragging && tap_drag_enabled() && s_pad_travel >= TP_TAP_MAX_TRAVEL) {
        s_dragging = true;
        push_buttons();
        ESP_LOGI(TAG, "drag start");
    }
}

/* Drop every finger and gesture (Settings / keyboard came up over the
 * pad): buttons up, no taps fired. Fingers still down stay tracked as
 * REG_NONE so they can't turn into pad input when the overlay closes. */
static void input_cancel(void)
{
    for (int i = 0; i < LVGL_PORT_TOUCH_MAX_POINTS; i++) s_ct[i].reg = REG_NONE;
    if (s_gesture) {
        if (s_pad_two && !s_pad_two_frozen) two_end(false);
        s_gesture = s_pad_two = s_dragging = s_second_touch = false;
        push_buttons();
    }
    update_buttons();
}

/* lvgl_port touch hook -- every touch read, LVGL task. */
static void tp_touch_hook(const lvgl_port_touch_t *t)
{
    int n = t->count;
    bool taken[LVGL_PORT_TOUCH_MAX_POINTS] = { false };
    bool still[LVGL_PORT_TOUCH_MAX_POINTS] = { false };

    /* 1. Match existing fingers to new points, nearest first. */
    for (;;) {
        int best_c = -1, best_p = -1;
        int32_t best_d = (int32_t)TP_MATCH_MAX_PX * TP_MATCH_MAX_PX;
        for (int c = 0; c < LVGL_PORT_TOUCH_MAX_POINTS; c++) {
            if (!s_ct[c].active || still[c]) continue;
            for (int p = 0; p < n; p++) {
                if (taken[p]) continue;
                int32_t ddx = (int32_t)t->x[p] - s_ct[c].x;
                int32_t ddy = (int32_t)t->y[p] - s_ct[c].y;
                int32_t d = ddx * ddx + ddy * ddy;
                if (d <= best_d) { best_d = d; best_c = c; best_p = p; }
            }
        }
        if (best_c < 0) break;
        tp_contact_t *ct = &s_ct[best_c];
        ct->dx = (int16_t)(t->x[best_p] - ct->x);
        ct->dy = (int16_t)(t->y[best_p] - ct->y);
        ct->x  = (int16_t)t->x[best_p];
        ct->y  = (int16_t)t->y[best_p];
        still[best_c] = true;
        taken[best_p] = true;
    }

    /* 2. Unmatched fingers lifted. */
    for (int c = 0; c < LVGL_PORT_TOUCH_MAX_POINTS; c++)
        if (s_ct[c].active && !still[c]) s_ct[c].active = false;

    /* 3. Unmatched points are new fingers: region fixed at touch-down. */
    for (int p = 0; p < n; p++) {
        if (taken[p]) continue;
        for (int c = 0; c < LVGL_PORT_TOUCH_MAX_POINTS; c++) {
            if (s_ct[c].active) continue;
            s_ct[c].active = true;
            s_ct[c].x  = (int16_t)t->x[p];
            s_ct[c].y  = (int16_t)t->y[p];
            s_ct[c].dx = s_ct[c].dy = 0;
            s_ct[c].reg = region_at(s_ct[c].x, s_ct[c].y);
            break;
        }
    }

    if (!input_enabled()) {
        input_cancel();
        return;
    }

    /* 4. Buttons, then the pad gesture. */
    update_buttons();

    int pad_count = 0, moved = 0, dx_sum = 0, dy_sum = 0;
    for (int c = 0; c < LVGL_PORT_TOUCH_MAX_POINTS; c++) {
        if (!s_ct[c].active || s_ct[c].reg != REG_PAD) continue;
        pad_count++;
        if (still[c]) {   /* new fingers have no motion yet */
            dx_sum += s_ct[c].dx;
            dy_sum += s_ct[c].dy;
            moved++;
        }
    }
    gesture_update(pad_count, dx_sum, dy_sum, moved);
}

static void wheel_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        leave_settings();
        scroll_begin(0);
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

        scroll_track(v.y);
    }
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        lv_obj_set_style_bg_color(s_wheel, lv_color_hex(COL_SURFACE), 0);
        scroll_end(code == LV_EVENT_RELEASED);
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

/* Put assets/side_icons/<file> on btn if the file exists (same handling as
 * the gear's side_icon in ui_settings_apply_appearance()). */
static bool add_side_icon(lv_obj_t *btn, const char *file)
{
    if (!file || !file[0]) return false;

    char icon_path[sizeof("S:") + sizeof(SD_PATH_ASSETS_SIDE_ICON) + 1 + UI_SETTINGS_SIDE_ICON_LEN];
    snprintf(icon_path, sizeof(icon_path), "S:%s/%s", SD_PATH_ASSETS_SIDE_ICON, file);

    FILE *f = fopen(icon_path + 2, "r");   /* skip the "S:" LVGL drive letter */
    if (!f) {
        ESP_LOGW(TAG, "icon not found: %s (falling back to text)", icon_path);
        return false;
    }
    fclose(f);

    lv_obj_t *img = lv_img_create(btn);
    lv_img_set_src(img, icon_path);
    lv_obj_center(img);
    lv_obj_set_style_clip_corner(btn, true, 0);
    return true;
}

static void hotkey_release(void)
{
    if (s_hk_down < 0) return;
    const tp_hotkey_t *hk = &s_cfg.hotkeys[s_hk_down];
    usb_hid_kb_key(false, hk->vk, hk->mods);
    usb_hid_kb_flush();
    s_hk_down = -1;
}

/* Hotkey button: real key down on press, up on release (modifiers ride
 * along, ESDeckPC wraps them around the key). No auto-repeat. */
static void hotkey_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_cfg.hotkey_count) return;

    if (code == LV_EVENT_PRESSED) {
        leave_settings();
        hotkey_release();
        const tp_hotkey_t *hk = &s_cfg.hotkeys[idx];
        usb_hid_kb_key(true, hk->vk, hk->mods);
        usb_hid_kb_flush();
        s_hk_down = idx;
        ESP_LOGI(TAG, "hotkey %d \"%s\" vk=0x%02X mods=0x%02X", idx, hk->label, hk->vk, hk->mods);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        hotkey_release();
    }
}

static void build_hotkeys(lv_obj_t *sidebar)
{
    if (s_cfg.hotkey_count == 0) return;

    /* Same 80 px cells as the keyboard / gear buttons, stacked between them. */
    s_hk_cont = make_plain_obj(sidebar);
    lv_obj_set_size(s_hk_cont, SB_CELL, SCREEN_H - SB_CELL * 2);
    lv_obj_set_pos(s_hk_cont, 0, SB_CELL);
    lv_obj_set_style_bg_opa(s_hk_cont, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(s_hk_cont, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < s_cfg.hotkey_count; i++) {
        const tp_hotkey_t *hk = &s_cfg.hotkeys[i];

        lv_obj_t *btn = lv_btn_create(s_hk_cont);
        lv_obj_set_size(btn, SB_BTN, SB_BTN);
        lv_obj_set_pos(btn, SB_PAD, i * SB_CELL + SB_PAD);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COL_BTN), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COL_BTN_PRESSED), LV_STATE_PRESSED);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_shadow_width(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 2, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_add_event_cb(btn, hotkey_cb, LV_EVENT_ALL, (void *)(intptr_t)i);

        if (!add_side_icon(btn, hk->icon)) {
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, hk->label[0] ? hk->label : "?");
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(lbl, SB_BTN - 4);
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(COL_TEXT), 0);
            lv_obj_center(lbl);
        }
    }
}

static void build_wheel(lv_obj_t *sidebar);

/* Middle area per the Sidebar preference. */
static void build_sidebar_middle(lv_obj_t *sidebar)
{
    if (s_sidebar_mode == TP_SIDEBAR_HOTKEYS)    build_hotkeys(sidebar);
    else if (s_sidebar_mode == TP_SIDEBAR_WHEEL) build_wheel(sidebar);
}

static void destroy_sidebar_middle(void)
{
    hotkey_release();   /* never leave a hotkey combo held on the PC */
    s_wheel_coast = 0;
    if (s_wheel) {
        lv_obj_del(s_wheel);  /* also deletes grip lines */
        s_wheel = NULL;
    }
    if (s_hk_cont) {
        lv_obj_del(s_hk_cont);
        s_hk_cont = NULL;
    }
    s_wheel_grip = NULL;
}

static void build_sidebar(lv_obj_t *sidebar)
{
    s_sidebar = sidebar;

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

    /* keyboard_icon from config, same handling as the gear's side_icon
     * (ui_settings_apply_appearance()): image if the file exists, else
     * the keyboard glyph. */
    if (!add_side_icon(kb_btn, s_cfg.keyboard_icon)) {
        lv_obj_t *kb_lbl = lv_label_create(kb_btn);
        lv_label_set_text(kb_lbl, LV_SYMBOL_KEYBOARD);
        lv_obj_set_style_text_font(kb_lbl, &lv_font_montserrat_24, 0);
        lv_obj_center(kb_lbl);
    }

    build_sidebar_middle(sidebar);
}

static void build_wheel(lv_obj_t *sidebar)
{
    s_wheel = make_plain_obj(sidebar);
    lv_obj_set_size(s_wheel, WHEEL_W, WHEEL_H);
    lv_obj_set_pos(s_wheel, WHEEL_X, WHEEL_Y);
    lv_obj_set_style_bg_color(s_wheel, lv_color_hex(COL_SURFACE), 0);
    lv_obj_set_style_radius(s_wheel, 8, 0);
    /* No clip_corner: the grip lines sit in an inner track that never
     * reaches the rounded corners, and clip_corner makes every redraw of
     * the strip (press highlight) noticeably more expensive. */
    lv_obj_add_flag(s_wheel, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_PRESS_LOCK);
    /* Never let a vertical swipe get picked up as a parent scroll (would
     * steal the press with PRESS_LOST). */
    lv_obj_clear_flag(s_wheel, LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_SCROLL_CHAIN);
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
    /* Driven by the touch engine (finger tracking), not LVGL events --
     * the pressed look is set by hand in update_buttons(). */
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    (void)name;

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
    /* Content area input comes from the touch engine (tp_touch_hook()).
     * The page stays clickable on purpose but does nothing: LVGL's own
     * press (first finger) has to land somewhere, and landing here -- with
     * scroll chaining / gesture bubbling cut -- keeps a drag on the pad
     * from ever being picked up as a scroll of the screen behind it. */
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLL_CHAIN | LV_OBJ_FLAG_GESTURE_BUBBLE);

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
    lv_obj_clear_flag(s_pad, LV_OBJ_FLAG_CLICKABLE);   /* touch engine, see tp_touch_hook() */
    if (has_bg) lv_obj_set_style_bg_opa(s_pad, OPA_SURFACE_ON_BG, 0);

    lv_obj_t *hint = lv_label_create(s_pad);
    s_lbl_hint = hint;
    lv_label_set_text(hint, "Touchpad");
    lv_obj_set_style_text_color(hint, lv_color_hex(has_bg ? 0x777777 : COL_HINT), 0);
    lv_obj_set_style_text_font(hint, &lv_font_montserrat_20, 0);
    lv_obj_center(hint);

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

    prefs_load();                    /* before the sidebar: Sidebar pref picks its middle */
    build_sidebar(sidebar);
    build_page(lv_scr_act());
    ui_touchpad_get_hide_labels();   /* load from NVS on first enter */
    usb_hid_set_native_input(s_input_mode == TP_INPUT_USB_HID);
    s_move_rem_x = s_move_rem_y = s_wheel_rem = 0;
    apply_hide_labels();
    apply_layout();

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

    memset(s_ct, 0, sizeof(s_ct));
    s_gesture = false;
    lvgl_port_set_touch_hook(tp_touch_hook);

    set_fast_touch_read(true);
    s_flush_timer = lv_timer_create(flush_timer_cb, TP_FLUSH_PERIOD_MS, NULL);

    ESP_LOGI(TAG, "entered touchpad mode (read period %d ms)", TP_READ_PERIOD_MS);
}

void ui_touchpad_exit(void)
{
    /* Release anything held and try to get it out now. If the endpoint is
     * busy the release stays queued and goes out on the next enter; the
     * PC side also releases everything on disconnect. */
    lvgl_port_set_touch_hook(NULL);
    memset(s_ct, 0, sizeof(s_ct));
    s_gesture = false;

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
    s_bg_img = s_bg_mask = s_btn_left = s_btn_right = s_pad = NULL;
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
    destroy_sidebar_middle();   /* also releases a held hotkey */
    s_sidebar = NULL;

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

/* -----------------------------------------------------------------------
 * Preferences
 * ----------------------------------------------------------------------- */
static int nvs_get_int(const char *key, int def)
{
    char v[8] = "";
    if (!nvs_manager_get_str(CFG_NVS_NAMESPACE, key, v, sizeof(v)) || !v[0]) return def;
    return atoi(v);
}

static void nvs_set_int(const char *key, int val)
{
    char v[8];
    snprintf(v, sizeof(v), "%d", val);
    if (!nvs_manager_set_str(CFG_NVS_NAMESPACE, key, v))
        ESP_LOGW(TAG, "NVS save failed: %s", key);
}

static int clamp_speed(int pct)
{
    if (pct < PREF_SPEED_MIN) return PREF_SPEED_MIN;
    if (pct > PREF_SPEED_MAX) return PREF_SPEED_MAX;
    return pct;
}

static void prefs_load(void)
{
    if (s_prefs_loaded) return;
    s_cursor_pct = clamp_speed(nvs_get_int(CFG_NVS_KEY_TPAD_CURSOR, 100));
    s_scroll_pct = clamp_speed(nvs_get_int(CFG_NVS_KEY_TPAD_SCROLL, 100));
    s_natural    = nvs_get_int(CFG_NVS_KEY_TPAD_NATURAL, 1) != 0;
    s_inertia    = nvs_get_int(CFG_NVS_KEY_TPAD_INERTIA, 1) != 0;
    s_tap_click  = nvs_get_int(CFG_NVS_KEY_TPAD_TAP, 1) != 0;
    s_hide_buttons = nvs_get_int(CFG_NVS_KEY_TPAD_HIDE_BTN, 0) != 0;
    s_swipe_mode = nvs_get_int(CFG_NVS_KEY_TPAD_SWIPE, TP_SWIPE_HSCROLL);
    if (s_swipe_mode < 0 || s_swipe_mode >= TP_SWIPE_COUNT) s_swipe_mode = TP_SWIPE_HSCROLL;
    s_input_mode = nvs_get_int(CFG_NVS_KEY_TPAD_INPUT, TP_INPUT_ESDECKPC);
    s_sidebar_mode = nvs_get_int(CFG_NVS_KEY_TPAD_SIDEBAR, TP_SIDEBAR_HOTKEYS);
    if (s_sidebar_mode < 0 || s_sidebar_mode >= TP_SIDEBAR_COUNT) s_sidebar_mode = TP_SIDEBAR_HOTKEYS;
    if (s_input_mode < 0 || s_input_mode >= TP_INPUT_COUNT) s_input_mode = TP_INPUT_ESDECKPC;
    s_prefs_loaded = true;
}

static const char *speed_text(int pct)
{
    /* 16 = worst case GCC assumes for an unbounded int ("-21474836.-47x"),
     * so -Wformat-truncation stays quiet; real values are 0.50x .. 2.00x. */
    static char buf[16];
    snprintf(buf, sizeof(buf), "%d.%02dx", pct / 100, pct % 100);
    return buf;
}

const char *ui_touchpad_cursor_speed_text(void) { prefs_load(); return speed_text(s_cursor_pct); }
const char *ui_touchpad_scroll_speed_text(void) { prefs_load(); return speed_text(s_scroll_pct); }

void ui_touchpad_cursor_speed_step(int dir)
{
    prefs_load();
    s_cursor_pct = clamp_speed(s_cursor_pct + dir * PREF_SPEED_STEP);
    nvs_set_int(CFG_NVS_KEY_TPAD_CURSOR, s_cursor_pct);
}

void ui_touchpad_scroll_speed_step(int dir)
{
    prefs_load();
    s_scroll_pct = clamp_speed(s_scroll_pct + dir * PREF_SPEED_STEP);
    nvs_set_int(CFG_NVS_KEY_TPAD_SCROLL, s_scroll_pct);
}

bool ui_touchpad_get_natural_scroll(void) { prefs_load(); return s_natural; }
bool ui_touchpad_get_inertia(void)        { prefs_load(); return s_inertia; }
bool ui_touchpad_get_tap_click(void)      { prefs_load(); return s_tap_click; }

void ui_touchpad_set_natural_scroll(bool on)
{
    prefs_load();
    s_natural = on;
    nvs_set_int(CFG_NVS_KEY_TPAD_NATURAL, on);
}

void ui_touchpad_set_inertia(bool on)
{
    prefs_load();
    s_inertia = on;
    if (!on) s_wheel_coast = 0;
    nvs_set_int(CFG_NVS_KEY_TPAD_INERTIA, on);
}

void ui_touchpad_set_tap_click(bool on)
{
    prefs_load();
    s_tap_click = on;
    if (!on) s_tap_pending = false;   /* drop a tap still waiting out its window */
    nvs_set_int(CFG_NVS_KEY_TPAD_TAP, on);
}

bool ui_touchpad_get_hide_buttons(void) { prefs_load(); return s_hide_buttons; }

void ui_touchpad_set_hide_buttons(bool on)
{
    prefs_load();
    s_hide_buttons = on;
    nvs_set_int(CFG_NVS_KEY_TPAD_HIDE_BTN, on);
    apply_layout();
}

static const char *const s_swipe_names[TP_SWIPE_COUNT] = {
    "Off", "Horizontal Scroll", "Back / Forward",
};

const char *const *ui_touchpad_swipe_mode_names(void) { return s_swipe_names; }
int  ui_touchpad_get_swipe_mode(void) { prefs_load(); return s_swipe_mode; }

void ui_touchpad_set_swipe_mode(int mode)
{
    if (mode < 0 || mode >= TP_SWIPE_COUNT) return;
    prefs_load();
    s_swipe_mode = mode;
    nvs_set_int(CFG_NVS_KEY_TPAD_SWIPE, mode);
}

static const char *const s_input_names[TP_INPUT_COUNT] = {
    "ESDeckPC", "USB HID",
};

const char *const *ui_touchpad_input_mode_names(void) { return s_input_names; }
int  ui_touchpad_get_input_mode(void) { prefs_load(); return s_input_mode; }

void ui_touchpad_set_input_mode(int mode)
{
    if (mode < 0 || mode >= TP_INPUT_COUNT) return;
    prefs_load();
    s_input_mode = mode;
    nvs_set_int(CFG_NVS_KEY_TPAD_INPUT, mode);
    usb_hid_set_native_input(mode == TP_INPUT_USB_HID);
}

static const char *const s_sidebar_names[TP_SIDEBAR_COUNT] = {
    "Hotkeys", "Wheel", "Empty",
};

const char *const *ui_touchpad_sidebar_mode_names(void) { return s_sidebar_names; }
int  ui_touchpad_get_sidebar_mode(void) { prefs_load(); return s_sidebar_mode; }

void ui_touchpad_set_sidebar_mode(int mode)
{
    if (mode < 0 || mode >= TP_SIDEBAR_COUNT) return;
    prefs_load();
    if (mode == s_sidebar_mode) return;
    s_sidebar_mode = mode;
    nvs_set_int(CFG_NVS_KEY_TPAD_SIDEBAR, mode);
    if (s_sidebar) {   /* Touchpad mode is up: swap the middle area now */
        destroy_sidebar_middle();
        build_sidebar_middle(s_sidebar);
    }
}

void ui_touchpad_deselect_current(void)
{
    if (!s_page) return;
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
}

void ui_touchpad_reselect_current(void)
{
    /* Only when actually hidden: lv_obj_clear_flag(HIDDEN) invalidates the
     * whole object even if it was already visible, and leave_settings()
     * calls this on every wheel / keyboard-button press -- an unconditional
     * clear meant a full 720x480 page redraw (VSYNC-locked) right at touch
     * down, so the wheel ignored the first part of a quick swipe. */
    if (!s_page || !lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN)) return;
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_HIDDEN);
}
