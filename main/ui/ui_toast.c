#include "ui_toast.h"
#include "ui.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

#define TOAST_QUEUE_MAX   8
#define TOAST_LABEL_LEN   32
#define TOAST_KEY_LEN     32

#define TOAST_W           360
#define TOAST_H           64
#define TOAST_Y_HIDDEN    (-TOAST_H)
#define TOAST_Y_SHOWN     12

#define TOAST_ANIM_MS     220
#define TOAST_HOLD_MS     2200

typedef struct {
    char label[TOAST_LABEL_LEN];
    char merge_key[TOAST_KEY_LEN];   /* empty string == never merges */
    int  count;
} toast_entry_t;

/* Overlay widgets -- created once in ui_toast_init(), reused for every
 * banner. Positioned off-screen (TOAST_Y_HIDDEN) while idle instead of
 * hidden/deleted, so there's nothing to (re)create per push. */
static lv_obj_t *s_toast     = NULL;
static lv_obj_t *s_toast_lbl = NULL;

/* Pending queue -- ring buffer over TOAST_QUEUE_MAX, oldest at s_queue_head. */
static toast_entry_t s_queue[TOAST_QUEUE_MAX];
static int           s_queue_head  = 0;
static int           s_queue_count = 0;

/* Entry currently on screen (valid only while s_showing is true). */
static toast_entry_t s_current;
static bool          s_showing = false;

static lv_timer_t *s_hold_timer = NULL;

static void toast_try_show_next(void);
static void toast_start_slide_out(void);

/* lv_anim_exec_xcb_t is void(*)(void *, int32_t) -- lv_obj_set_y takes a
 * plain lv_coord_t (int16_t in this build's sdkconfig, LV_USE_LARGE_COORD
 * is off), so casting lv_obj_set_y directly to lv_anim_exec_xcb_t and
 * calling through that pointer would be undefined behavior (mismatched
 * parameter width). Every LVGL example wraps instead -- do the same. */
static void toast_set_y(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, (lv_coord_t)v);
}

/* -----------------------------------------------------------------------
 * Label text
 * ----------------------------------------------------------------------- */
static void toast_refresh_label(void)
{
    char buf[TOAST_LABEL_LEN + 16];
    if (s_current.count > 1) {
        snprintf(buf, sizeof(buf), "%s  x%d", s_current.label, s_current.count);
    } else {
        snprintf(buf, sizeof(buf), "%s", s_current.label);
    }
    lv_label_set_text(s_toast_lbl, buf);
}

/* -----------------------------------------------------------------------
 * Animations
 * ----------------------------------------------------------------------- */
static void hold_timer_cb(lv_timer_t *t)
{
    (void)t;
    s_hold_timer = NULL;
    toast_start_slide_out();
}

static void slide_in_ready_cb(lv_anim_t *a)
{
    (void)a;
    /* Cancel any leftover timer (shouldn't normally happen) before arming
     * a fresh one. */
    if (s_hold_timer) {
        lv_timer_del(s_hold_timer);
        s_hold_timer = NULL;
    }
    s_hold_timer = lv_timer_create(hold_timer_cb, TOAST_HOLD_MS, NULL);
    lv_timer_set_repeat_count(s_hold_timer, 1);
}

static void slide_out_ready_cb(lv_anim_t *a)
{
    (void)a;
    s_showing = false;
    memset(&s_current, 0, sizeof(s_current));
    toast_try_show_next();
}

static void toast_start_slide_in(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_values(&a, TOAST_Y_HIDDEN, TOAST_Y_SHOWN);
    lv_anim_set_time(&a, TOAST_ANIM_MS);
    lv_anim_set_exec_cb(&a, toast_set_y);
    lv_anim_set_ready_cb(&a, slide_in_ready_cb);
    lv_anim_start(&a);
}

static void toast_start_slide_out(void)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_toast);
    lv_anim_set_values(&a, lv_obj_get_y(s_toast), TOAST_Y_HIDDEN);
    lv_anim_set_time(&a, TOAST_ANIM_MS);
    lv_anim_set_exec_cb(&a, toast_set_y);
    lv_anim_set_ready_cb(&a, slide_out_ready_cb);
    lv_anim_start(&a);
}

/* -----------------------------------------------------------------------
 * Queue
 * ----------------------------------------------------------------------- */
static void toast_try_show_next(void)
{
    if (s_showing) return;
    if (s_queue_count == 0) return;

    s_current = s_queue[s_queue_head];
    s_queue_head = (s_queue_head + 1) % TOAST_QUEUE_MAX;
    s_queue_count--;

    s_showing = true;
    toast_refresh_label();

    /* Always on top -- later overlays (Settings panel, the "switching
     * config" cover) are created/re-shown well after ui_toast_init(), so
     * without this the banner could end up buried under them. */
    lv_obj_move_foreground(s_toast);
    toast_start_slide_in();
}

void ui_toast_push(const char *label, int count, const char *merge_key)
{
    if (!s_toast) return;   /* ui_toast_init() not called yet */
    if (count < 1) count = 1;

    bool has_key = (merge_key && merge_key[0] != '\0');

    if (has_key) {
        if (s_showing && strcmp(s_current.merge_key, merge_key) == 0) {
            snprintf(s_current.label, sizeof(s_current.label), "%s", label);
            s_current.count = count;
            toast_refresh_label();
            /* Restart the hold window so the update gets its own full
             * read-time instead of vanishing right after. */
            if (s_hold_timer) {
                lv_timer_del(s_hold_timer);
                s_hold_timer = NULL;
            }
            s_hold_timer = lv_timer_create(hold_timer_cb, TOAST_HOLD_MS, NULL);
            lv_timer_set_repeat_count(s_hold_timer, 1);
            return;
        }

        for (int i = 0; i < s_queue_count; i++) {
            int idx = (s_queue_head + i) % TOAST_QUEUE_MAX;
            if (strcmp(s_queue[idx].merge_key, merge_key) == 0) {
                snprintf(s_queue[idx].label, sizeof(s_queue[idx].label), "%s", label);
                s_queue[idx].count = count;
                return;
            }
        }
    }

    if (s_queue_count >= TOAST_QUEUE_MAX) return;   /* drop -- queue full */

    int idx = (s_queue_head + s_queue_count) % TOAST_QUEUE_MAX;
    snprintf(s_queue[idx].label, sizeof(s_queue[idx].label), "%s", label);
    snprintf(s_queue[idx].merge_key, sizeof(s_queue[idx].merge_key), "%s", has_key ? merge_key : "");
    s_queue[idx].count = count;
    s_queue_count++;

    toast_try_show_next();
}

/* -----------------------------------------------------------------------
 * Swipe-up-to-dismiss
 * ----------------------------------------------------------------------- */
static void toast_gesture_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;
    if (lv_indev_get_gesture_dir(indev) != LV_DIR_TOP) return;
    if (!s_showing) return;

    if (s_hold_timer) {
        lv_timer_del(s_hold_timer);
        s_hold_timer = NULL;
    }
    toast_start_slide_out();
}

/* -----------------------------------------------------------------------
 * Public
 * ----------------------------------------------------------------------- */
void ui_toast_init(void)
{
    lv_obj_t *scr = lv_scr_act();

    s_toast = lv_obj_create(scr);
    lv_obj_set_size(s_toast, TOAST_W, TOAST_H);
    lv_obj_set_pos(s_toast, (SCREEN_W - TOAST_W) / 2, TOAST_Y_HIDDEN);
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x1e1e1e), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_90, 0);
    lv_obj_set_style_border_color(s_toast, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(s_toast, 1, 0);
    lv_obj_set_style_radius(s_toast, 12, 0);
    lv_obj_set_style_shadow_width(s_toast, 16, 0);
    lv_obj_set_style_shadow_opa(s_toast, LV_OPA_40, 0);
    lv_obj_clear_flag(s_toast, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_toast, toast_gesture_cb, LV_EVENT_GESTURE, NULL);

    s_toast_lbl = lv_label_create(s_toast);
    lv_obj_set_style_text_color(s_toast_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_toast_lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(s_toast_lbl);

    s_queue_head  = 0;
    s_queue_count = 0;
    s_showing     = false;
    memset(&s_current, 0, sizeof(s_current));
}
