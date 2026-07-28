#include "ui_toast.h"
#include "ui.h"
#include "lvgl.h"
#include <string.h>
#include <stdio.h>

#define TOAST_QUEUE_MAX   8
/* 32 was fine for our own short, fixed labels ("BLE Connected"); real ANCS
 * title/message text needs real headroom -- 96 bytes covers a short CJK
 * sentence (3 bytes/glyph in UTF-8) with room to spare. Actual display
 * width is still capped by TOAST_W regardless -- this is the data-level
 * cap (memory + never showing an unbounded string), separate from
 * whatever long-mode/wrap behavior the label ends up needing once real
 * message content is wired in. */
#define TOAST_LABEL_LEN   96
#define TOAST_KEY_LEN     32

#define TOAST_W           440
/* Height is dynamic now (see toast_refresh_label()) -- clamped between
 * these two rather than fixed, so a short "BLE Connected" banner stays
 * compact while a long ANCS message (app name + title + message, up to
 * three wrapped lines or more) gets enough room to actually show instead
 * of being clipped through the middle by a box too short for its own
 * content. TOAST_Y_HIDDEN is pinned to -TOAST_MAX_H (not -TOAST_H, which
 * no longer exists) so the off-screen parking position is always fully
 * off-screen no matter which height is currently in use. */
#define TOAST_H_MIN       64
#define TOAST_MAX_H       240
#define TOAST_Y_HIDDEN    (-TOAST_MAX_H)
#define TOAST_Y_SHOWN     12

#define TOAST_ANIM_MS     220
#define TOAST_HOLD_MS     2200

typedef struct {
    char label[TOAST_LABEL_LEN];
    char merge_key[TOAST_KEY_LEN];   /* empty string == never merges */
    int  count;
    const lv_font_t *font;           /* NULL == default (montserrat_20) */
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

/* Copy src into dst (dst_size bytes total, nul included), truncating at
 * TOAST_LABEL_LEN if needed. snprintf("%s") alone would also stay in
 * bounds, but it can slice a multi-byte UTF-8 sequence in half at the cut
 * point -- fine for our own ASCII labels, not fine once real ANCS text
 * (CJK, 3 bytes/glyph) flows through here, since a chopped sequence can
 * render as a mangled/garbage glyph. This backs off to the nearest UTF-8
 * character boundary instead, and appends "..." so a truncated string is
 * visibly truncated rather than silently cut off. */
static void toast_copy_truncated(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) return;

    size_t src_len = strlen(src);
    if (src_len < dst_size) {
        memcpy(dst, src, src_len + 1);
        return;
    }

    static const char ellipsis[] = "...";
    size_t ellipsis_len = sizeof(ellipsis) - 1;

    if (dst_size <= ellipsis_len + 1) {
        /* Degenerate buffer, no room for the ellipsis -- hard truncate. */
        size_t n = dst_size - 1;
        memcpy(dst, src, n);
        dst[n] = '\0';
        return;
    }

    size_t max_copy = dst_size - 1 - ellipsis_len;
    /* UTF-8 continuation bytes are 10xxxxxx -- back off until we land on
     * a lead byte (or ASCII byte), never mid-sequence. */
    while (max_copy > 0 && (src[max_copy] & 0xC0) == 0x80) {
        max_copy--;
    }

    memcpy(dst, src, max_copy);
    memcpy(dst + max_copy, ellipsis, ellipsis_len);
    dst[max_copy + ellipsis_len] = '\0';
}

static void toast_refresh_label(void)
{
    char buf[TOAST_LABEL_LEN + 16];
    if (s_current.count > 1) {
        snprintf(buf, sizeof(buf), "%s  x%d", s_current.label, s_current.count);
    } else {
        snprintf(buf, sizeof(buf), "%s", s_current.label);
    }
    lv_obj_set_style_text_font(s_toast_lbl, s_current.font ? s_current.font : &lv_font_montserrat_20, 0);
    lv_label_set_text(s_toast_lbl, buf);

    /* Resize the box to fit this content -- s_toast_lbl's own height
     * already auto-fits its (possibly multi-line, wrapped) text since no
     * explicit height was ever set on it, only width (which is what
     * forces the wrap); lv_obj_update_layout() forces LVGL to actually
     * recompute that before we read it back, since label text was just
     * changed this same tick and layout recalculation is normally
     * deferred. Clamped to TOAST_H_MIN..TOAST_MAX_H rather than growing
     * unbounded -- see those macros' comment. */
    lv_obj_update_layout(s_toast_lbl);
    lv_coord_t content_h = lv_obj_get_height(s_toast_lbl);
    lv_coord_t box_h = content_h + 24;   /* 12px worth of margin top and bottom around the (now vertically-centered) label */
    if (box_h < TOAST_H_MIN) box_h = TOAST_H_MIN;
    if (box_h > TOAST_MAX_H) box_h = TOAST_MAX_H;
    lv_obj_set_height(s_toast, box_h);
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

void ui_toast_push(const char *label, int count, const char *merge_key,
                    const lv_font_t *font)
{
    if (!s_toast) return;   /* ui_toast_init() not called yet */
    if (count < 1) count = 1;

    bool has_key = (merge_key && merge_key[0] != '\0');

    if (has_key) {
        if (s_showing && strcmp(s_current.merge_key, merge_key) == 0) {
            toast_copy_truncated(s_current.label, sizeof(s_current.label), label);
            s_current.count = count;
            s_current.font  = font;
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
                toast_copy_truncated(s_queue[idx].label, sizeof(s_queue[idx].label), label);
                s_queue[idx].count = count;
                s_queue[idx].font  = font;
                return;
            }
        }
    }

    if (s_queue_count >= TOAST_QUEUE_MAX) return;   /* drop -- queue full */

    int idx = (s_queue_head + s_queue_count) % TOAST_QUEUE_MAX;
    toast_copy_truncated(s_queue[idx].label, sizeof(s_queue[idx].label), label);
    snprintf(s_queue[idx].merge_key, sizeof(s_queue[idx].merge_key), "%s", has_key ? merge_key : "");
    s_queue[idx].count = count;
    s_queue[idx].font  = font;
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
    lv_obj_set_size(s_toast, TOAST_W, TOAST_H_MIN);   /* height grows per-push, see toast_refresh_label() */
    lv_obj_set_pos(s_toast, (SCREEN_W - TOAST_W) / 2, TOAST_Y_HIDDEN);
    /* Zeroed explicitly -- lv_obj_create() picks up whatever padding the
     * active theme defaults to for a plain container, which is otherwise
     * an unknown extra inset stacking on top of the label's own (16, 0)
     * alignment offset below. With this at 0, that offset is the only
     * spacing in play, so the box's actual size/position math (here and
     * in toast_refresh_label()) means what it says. */
    lv_obj_set_style_pad_all(s_toast, 0, 0);
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
    /* Fixed width + wrap -- needed now that real (potentially multi-word,
     * CJK) ANCS text flows through here instead of just our own short
     * fixed ASCII labels. s_toast's height now tracks this label's
     * wrapped content height (see toast_refresh_label()), clamped to
     * TOAST_MAX_H -- only content that still doesn't fit even at that cap
     * would get clipped, which shouldn't happen in practice given
     * TOAST_LABEL_LEN's byte-level cap upstream. Left-aligned, anchored
     * horizontally-left, vertically-centered rather than fully centered --
     * multi-line centered text (an app name over a wrapped message) read
     * as "broken/misaligned" rather than intentional; left alignment
     * reads as a normal notification banner instead. LEFT_MID rather
     * than TOP_LEFT specifically: box height is dynamic (see
     * toast_refresh_label()) and only ever sized to content + a fixed
     * margin, so vertical centering and top-anchoring end up looking
     * almost identical for a long wrapped message (little slack either
     * way) -- but for a short single-line message, which clamps to
     * TOAST_H_MIN, TOP_LEFT left a visibly lopsided gap underneath the
     * text with nothing above it, while LEFT_MID balances that gap
     * evenly top and bottom instead. */
    lv_obj_set_width(s_toast_lbl, TOAST_W - 32);
    lv_label_set_long_mode(s_toast_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_toast_lbl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(s_toast_lbl, LV_ALIGN_LEFT_MID, 16, 0);

    s_queue_head  = 0;
    s_queue_count = 0;
    s_showing     = false;
    memset(&s_current, 0, sizeof(s_current));
}
