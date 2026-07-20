#include "ui_media.h"
#include "ui.h"
#include "ui_settings.h"
#include "ui_media_config.h"
#include "ui_config.h"
#include "usb/usb_hid.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG  "MEDIA"

/* Content area -- same convention as ui_monitor.c's CONTENT_W/H. */
#define CONTENT_W   (SCREEN_W - SIDEBAR_W)
#define CONTENT_H   SCREEN_H

/* -----------------------------------------------------------------------
 * Media mode is entirely HID-driven now -- no more local fake/mock data.
 * Independent HID channels feed this page while subscribed (page=0xFE,
 * see usb_hid.h):
 *   - CMD_NOWPLAYING_PROGRESS(0x06): position/duration/playing, drives the
 *     progress bar, time labels and play/pause icon.
 *   - CMD_AUDIO_LEVEL(0x07): sidebar VU-meter bar value.
 *   - CMD_NOWPLAYING_IMG_START/CHUNK/END(0x08-0x0A): cover art + a
 *     PC-rendered title/artist strip (no CJK font on-device), chunked JPEG
 *     reassembled and decoded on the ESP side (usb_hid.c), delivered here
 *     as a ready RGB565 buffer via ui_media_on_hid_img(). See
 *     apply_cover_image()/apply_info_image() and their clear_* pairs.
 * Progress/level use a queue-plus-~3s-timeout convention, same as
 * ui_monitor.c's s_data_queue: while nothing has arrived yet (or the PC
 * stops sending), the UI shows a disabled/"None" state rather than making
 * anything up. Images follow s_real_data_received's same disconnect signal
 * (cleared alongside progress/level, see apply_connected_state()) but
 * don't have their own staleness timeout -- a cover/title doesn't need to
 * refresh anywhere near as often as position does.
 *
 * Transport buttons (prev/play-pause/next) are one-way remote control --
 * pressing one sends a command to the PC (usb_hid_media_play_pause() etc,
 * see usb_hid.h) and does NOT touch local state; the resulting play/pause
 * icon or track change comes back through the normal progress packets like
 * any other change, same as pressing the button in the PC's own player.
 * They're only clickable while s_real_data_received is true (see
 * apply_connected_state()).
 * ----------------------------------------------------------------------- */

/* -----------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */
static lv_obj_t   *s_sidebar_bar_cont = NULL;  /* sidebar sub-region, child of shared sidebar */
static lv_obj_t   *s_level_bar        = NULL;  /* vertical VU meter, HID-driven */

static lv_obj_t   *s_page          = NULL;     /* content area root */
static lv_obj_t   *s_bg_img        = NULL;     /* page child0, only created when config bg_image is set */
static lv_obj_t   *s_bg_mask       = NULL;     /* page child1, black 50% opa over s_bg_img */
static lv_img_dsc_t s_bg_dsc;
static uint8_t     *s_bg_data      = NULL;

static ui_media_config_t s_media_cfg;          /* loaded once in ui_media_enter(), see ui_media_config.h */

static lv_obj_t   *s_cover         = NULL;
static lv_obj_t   *s_title_lbl     = NULL;
static lv_obj_t   *s_artist_lbl    = NULL;
static lv_obj_t   *s_progress_bar  = NULL;
static lv_obj_t   *s_time_elapsed_lbl = NULL;
static lv_obj_t   *s_time_total_lbl   = NULL;
static lv_obj_t   *s_prev_btn      = NULL;
static lv_obj_t   *s_play_btn      = NULL;
static lv_obj_t   *s_next_btn      = NULL;
static lv_obj_t   *s_play_icon_lbl = NULL;

static lv_timer_t *s_media_timer = NULL;

static bool s_seeking = false;   /* true while user is dragging the progress slider */
static bool s_ui_enabled = false; /* last-applied connected/enabled UI state, see apply_connected_state() */

/* Seek confirmation grace window -- after sending HID_MEDIA_BTN_SEEK on
 * release, media_timer_cb would otherwise immediately overwrite the slider
 * with the stale s_real_progress from before the seek (visible snap-back),
 * then jump again once the PC's confirming packet arrives. Freeze display
 * at the target instead until either a progress packet close to that target
 * shows up, or a timeout elapses (in case the seek command / confirmation
 * gets lost) -- same idea as s_seeking but spanning the round trip. */
static bool s_seek_pending       = false;
static int  s_seek_target_sec    = 0;
static int  s_seek_pending_ticks = 0;
#define SEEK_PENDING_TIMEOUT_TICKS 25   /* ~2s at 80ms/tick */

/* -----------------------------------------------------------------------
 * Real Now Playing progress -- pushed from usb_hid's TinyUSB task via
 * ui_media_push_progress(), drained on the LVGL side by media_timer_cb().
 * Same queue-plus-timeout convention as ui_monitor.c's s_data_queue.
 * ----------------------------------------------------------------------- */
typedef struct {
    uint32_t position_ms;
    uint32_t duration_ms;
    bool     playing;
} media_progress_t;

static QueueHandle_t    s_progress_queue      = NULL;
static media_progress_t s_real_progress       = { 0 };
static bool             s_real_data_received  = false;
static int              s_real_data_timeout   = 0;   /* media_timer_cb ticks since last real packet */

static void ui_media_on_hid_progress(uint32_t position_ms, uint32_t duration_ms, bool playing);

/* Real sidebar audio level -- same convention, independent of the progress
 * queue above (audio level and Now Playing arrive as separate HID cmds and
 * can go stale independently). */
static QueueHandle_t s_level_queue         = NULL;
static uint8_t       s_real_level          = 0;
static bool          s_real_level_received = false;
static int           s_real_level_timeout  = 0;

static void ui_media_on_hid_level(uint8_t level);

/* -----------------------------------------------------------------------
 * Cover art / title-artist image -- pushed from usb_hid's TinyUSB task via
 * ui_media_on_hid_img() once a JPEG chunk transfer finishes reassembly +
 * decode (see usb_hid.c's img_recv_end()). Ownership of the decoded
 * RGB565 buffer transfers through the queue; s_cover_data/s_info_data
 * track whichever buffer is currently live so it can be freed once
 * replaced or on ui_media_exit(). Unlike progress/level, these arrive
 * rarely (on track change) so a plain (non-overwrite) queue is fine --
 * depth 4 gives slack for both kinds arriving close together without
 * blocking the TinyUSB task.
 * ----------------------------------------------------------------------- */
typedef struct {
    uint8_t  kind;
    uint8_t *rgb565_data;   /* MALLOC_CAP_SPIRAM, ownership transferred */
    uint16_t w;
    uint16_t h;
} media_img_msg_t;

static QueueHandle_t s_img_queue = NULL;

static lv_obj_t     *s_cover_img  = NULL;   /* shown instead of s_cover once a cover arrives */
static lv_img_dsc_t  s_cover_dsc;
static uint8_t       *s_cover_data = NULL;   /* currently displayed buffer, owned here */

static lv_obj_t     *s_info_img   = NULL;   /* shown instead of title/artist labels once an info strip arrives */
static lv_img_dsc_t  s_info_dsc;
static uint8_t       *s_info_data = NULL;

static void ui_media_on_hid_img(uint8_t kind, uint8_t *rgb565_data, uint16_t w, uint16_t h);

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
static void format_time(int sec, char *buf, size_t buf_len)
{
    if (sec < 0) sec = 0;
    snprintf(buf, buf_len, "%d:%02d", sec / 60, sec % 60);
}

/* Dim + block input on a widget as a unit (LVGL renders an object with
 * opa<255 -- and its children -- as one blended layer, so this dims icon
 * labels inside buttons too, not just the button background). Clearing
 * CLICKABLE is the actual input block; DISABLED state is set alongside for
 * any default-theme/semantic behaviour that keys off it. */
static void set_widget_enabled(lv_obj_t *obj, bool enabled)
{
    if (!obj) return;

    if (enabled) {
        lv_obj_clear_state(obj, LV_STATE_DISABLED);
        lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(obj, LV_OPA_COVER, 0);
    } else {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(obj, LV_OPA_50, 0);
    }
}

/* Hides the cover image (if shown) and frees its buffer, reverting to the
 * placeholder glyph box (s_cover). Safe to call with nothing currently
 * shown. */
static void clear_cover_image(void)
{
    if (s_cover_img) lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
    if (s_cover)     lv_obj_clear_flag(s_cover, LV_OBJ_FLAG_HIDDEN);

    if (s_cover_data) {
        heap_caps_free(s_cover_data);
        s_cover_data = NULL;
    }
}

/* Same idea for the title/artist info strip -- reverts to the "None" text
 * labels. */
static void clear_info_image(void)
{
    if (s_info_img)  lv_obj_add_flag(s_info_img, LV_OBJ_FLAG_HIDDEN);
    if (s_title_lbl)  lv_obj_clear_flag(s_title_lbl, LV_OBJ_FLAG_HIDDEN);
    if (s_artist_lbl) lv_obj_clear_flag(s_artist_lbl, LV_OBJ_FLAG_HIDDEN);

    if (s_info_data) {
        heap_caps_free(s_info_data);
        s_info_data = NULL;
    }
}

/* Swaps in a freshly decoded RGB565 buffer as the cover image, freeing
 * whatever was shown before and hiding the placeholder glyph box. Takes
 * ownership of rgb565_data. */
static void apply_cover_image(uint8_t *rgb565_data, uint16_t w, uint16_t h)
{
    if (s_cover_data) heap_caps_free(s_cover_data);
    s_cover_data = rgb565_data;

    s_cover_dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
    s_cover_dsc.header.always_zero = 0;
    s_cover_dsc.header.w           = w;
    s_cover_dsc.header.h           = h;
    s_cover_dsc.data_size          = (uint32_t)w * h * 2;
    s_cover_dsc.data               = rgb565_data;

    lv_img_cache_invalidate_src(&s_cover_dsc);
    lv_img_set_src(s_cover_img, &s_cover_dsc);
    lv_obj_clear_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
}

/* Same idea for the title/artist info strip. */
static void apply_info_image(uint8_t *rgb565_data, uint16_t w, uint16_t h)
{
    if (s_info_data) heap_caps_free(s_info_data);
    s_info_data = rgb565_data;

    /* TRUE_COLOR_ALPHA (3 bytes/px: RGB565 LE + alpha), not plain
     * TRUE_COLOR -- the whole point of sending this as a PNG (see
     * usb_hid.c's decode_png_to_rgb565()) is a real transparent
     * background so the text doesn't sit on a solid rectangle once Media
     * has an actual background image behind it; LVGL alpha-blends this
     * against s_page (or later, the bg image) at render time. */
    s_info_dsc.header.cf          = LV_IMG_CF_TRUE_COLOR_ALPHA;
    s_info_dsc.header.always_zero = 0;
    s_info_dsc.header.w           = w;
    s_info_dsc.header.h           = h;
    s_info_dsc.data_size          = (uint32_t)w * h * 3;
    s_info_dsc.data               = rgb565_data;

    lv_img_cache_invalidate_src(&s_info_dsc);
    lv_img_set_src(s_info_img, &s_info_dsc);
    lv_obj_clear_flag(s_info_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_title_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_artist_lbl, LV_OBJ_FLAG_HIDDEN);
}

/* Applies (or re-applies) the connected/disconnected look. Cheap to call
 * unconditionally every tick, but media_timer_cb only calls it on actual
 * transitions to avoid needlessly re-touching LVGL styles every 80ms. */
static void apply_connected_state(bool connected)
{
    s_ui_enabled = connected;

    set_widget_enabled(s_progress_bar, connected);
    set_widget_enabled(s_prev_btn, connected);
    set_widget_enabled(s_play_btn, connected);
    set_widget_enabled(s_next_btn, connected);

    if (!connected) {
        lv_label_set_text(s_title_lbl, "None");
        lv_label_set_text(s_artist_lbl, "None");
        lv_label_set_text(s_time_elapsed_lbl, "-:--");
        lv_label_set_text(s_time_total_lbl, "-:--");
        lv_label_set_text(s_play_icon_lbl, LV_SYMBOL_PLAY);
        lv_slider_set_value(s_progress_bar, 0, LV_ANIM_OFF);

        /* Drop any cover/info image and fall back to the placeholder
         * glyph / "None" labels -- a stale image from whatever was
         * playing before disconnect shouldn't linger on screen. */
        clear_cover_image();
        clear_info_image();
    }
}

/* -----------------------------------------------------------------------
 * Transport button callbacks -- one-way remote control, see file header.
 * Guarded on s_real_data_received even though the buttons are already
 * un-clickable while disconnected (set_widget_enabled), as a defensive
 * belt-and-suspenders against any stray event.
 * ----------------------------------------------------------------------- */
static void play_pause_cb(lv_event_t *e)
{
    if (!s_real_data_received) return;
    usb_hid_media_play_pause();
}

static void prev_cb(lv_event_t *e)
{
    if (!s_real_data_received) return;
    usb_hid_media_prev();
}

static void next_cb(lv_event_t *e)
{
    if (!s_real_data_received) return;
    usb_hid_media_next();
}

/* Tapping the sidebar bar is how the user leaves the Settings page and
 * comes back to the player card -- Media has no per-page sidebar buttons
 * for this like Deck/Monitor do, so the bar itself doubles as the "go
 * back to Media" tap target. Mirrors sidebar_btn_cb() in ui_deck.c /
 * ui_monitor.c. */
static void sidebar_bar_click_cb(lv_event_t *e)
{
    ui_settings_deselect();
    ui_media_reselect_current();
}

/* Progress slider -- drag to preview a seek position locally.
 * LV_EVENT_PRESSED/RELEASED bracket the drag so media_timer_cb() stops
 * overwriting the slider value out from under the user's finger;
 * VALUE_CHANGED fires continuously while dragging so the time label tracks
 * live. On release, sends HID_MEDIA_BTN_SEEK with the target position and
 * holds the display at that target (s_seek_pending) until confirmed by a
 * real progress packet or a timeout -- see media_timer_cb(). */
static void progress_seek_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        s_seeking = true;
        return;
    }

    if (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_RELEASED) {
        int pct = lv_slider_get_value(s_progress_bar);
        uint32_t duration_sec = s_real_progress.duration_ms / 1000;
        int preview_sec = (int)(pct * (int)duration_sec / 100);

        char buf[16];
        format_time(preview_sec, buf, sizeof(buf));
        lv_label_set_text(s_time_elapsed_lbl, buf);

        if (code == LV_EVENT_RELEASED && s_real_data_received) {
            uint32_t target_ms = (uint32_t)pct * s_real_progress.duration_ms / 100;
            usb_hid_media_seek(target_ms);
            s_seek_pending       = true;
            s_seek_target_sec    = preview_sec;
            s_seek_pending_ticks = 0;
        }
    }

    if (code == LV_EVENT_RELEASED) {
        s_seeking = false;
    }
}

/* -----------------------------------------------------------------------
 * Media update timer -- 80ms tick. Drains both HID queues, applies
 * connected/disconnected UI state on transitions, and while connected
 * drives the progress bar / time labels / play-pause icon / level bar
 * straight from the latest real data.
 * ----------------------------------------------------------------------- */
static void media_timer_cb(lv_timer_t *timer)
{
    /* Drain real progress queue -- keep only the latest entry. ~36 ticks
     * (36 * 80ms ≈ 3s) of silence after having received real data drops
     * back to the disconnected state, same idea as ui_monitor.c's timeout. */
    media_progress_t prog;
    bool got_progress = false;
    while (s_progress_queue && xQueueReceive(s_progress_queue, &prog, 0) == pdTRUE)
        got_progress = true;

    if (got_progress) {
        s_real_progress      = prog;
        s_real_data_received = true;
        s_real_data_timeout  = 0;
    } else if (s_real_data_received) {
        s_real_data_timeout++;
        if (s_real_data_timeout >= 36) {
            s_real_data_received = false;
            s_real_data_timeout  = 0;
        }
    }

    if (s_real_data_received != s_ui_enabled)
        apply_connected_state(s_real_data_received);

    /* Drain cover/info image queue -- rare compared to progress/level, so
     * just apply every message in arrival order (each apply_*_image() call
     * frees whatever it's replacing). Only meaningful while connected; if
     * a message somehow arrives after disconnect (race with the ~3s
     * progress timeout) just free it instead of showing a stale image. */
    media_img_msg_t img_msg;
    while (s_img_queue && xQueueReceive(s_img_queue, &img_msg, 0) == pdTRUE) {
        /* rgb565_data == NULL is the "clear" sentinel (see img_recv_start()
         * in usb_hid.c) -- PC has no image for this kind right now (e.g.
         * Now Playing lost focus/session), revert to the placeholder
         * instead of applying a buffer. */
        if (!s_real_data_received) {
            if (img_msg.rgb565_data) heap_caps_free(img_msg.rgb565_data);
        } else if (img_msg.kind == HID_MEDIA_IMG_KIND_COVER) {
            if (img_msg.rgb565_data) apply_cover_image(img_msg.rgb565_data, img_msg.w, img_msg.h);
            else clear_cover_image();
        } else if (img_msg.kind == HID_MEDIA_IMG_KIND_INFO) {
            if (img_msg.rgb565_data) apply_info_image(img_msg.rgb565_data, img_msg.w, img_msg.h);
            else clear_info_image();
        } else if (img_msg.rgb565_data) {
            heap_caps_free(img_msg.rgb565_data);
        }
    }

    /* Drain real audio level queue -- keep only the latest entry, same
     * ~3s timeout convention as the progress queue above. Independent of
     * s_real_data_received: audio level and Now Playing are separate HID
     * cmds and can go stale on their own schedules. */
    uint8_t lvl;
    bool got_level = false;
    while (s_level_queue && xQueueReceive(s_level_queue, &lvl, 0) == pdTRUE)
        got_level = true;

    if (got_level) {
        s_real_level          = lvl;
        s_real_level_received = true;
        s_real_level_timeout  = 0;
        /* LV_ANIM_ON: blend towards the new value instead of snapping,
         * so the bar reads as fluid motion rather than a rigid step every
         * time a new sample arrives. */
        lv_bar_set_value(s_level_bar, s_real_level, LV_ANIM_ON);
    } else if (s_real_level_received) {
        s_real_level_timeout++;
        if (s_real_level_timeout >= 36) {
            s_real_level_received = false;
            s_real_level_timeout  = 0;
            lv_bar_set_value(s_level_bar, 0, LV_ANIM_OFF);
        }
    }

    if (!s_real_data_received) {
        s_seek_pending = false;
        return;
    }

    lv_label_set_text(s_play_icon_lbl,
                      s_real_progress.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);

    if (s_seek_pending) {
        int position_sec = (int)(s_real_progress.position_ms / 1000);
        bool confirmed = got_progress && abs(position_sec - s_seek_target_sec) <= 2;

        s_seek_pending_ticks++;
        if (confirmed || s_seek_pending_ticks >= SEEK_PENDING_TIMEOUT_TICKS)
            s_seek_pending = false;
    }

    if (!s_seeking && !s_seek_pending) {
        uint32_t position_sec = s_real_progress.position_ms / 1000;
        uint32_t duration_sec = s_real_progress.duration_ms / 1000;
        int pct = (duration_sec > 0) ? (int)(position_sec * 100 / duration_sec) : 0;
        if (pct > 100) pct = 100;
        lv_slider_set_value(s_progress_bar, pct, LV_ANIM_OFF);

        char buf[16];
        format_time((int)position_sec, buf, sizeof(buf));
        lv_label_set_text(s_time_elapsed_lbl, buf);
        format_time((int)duration_sec, buf, sizeof(buf));
        lv_label_set_text(s_time_total_lbl, buf);
    }
}

/* -----------------------------------------------------------------------
 * Sidebar level bar (replaces the page-button column Deck/Monitor use)
 * ----------------------------------------------------------------------- */
static void build_sidebar_bar(lv_obj_t *sidebar)
{
    s_sidebar_bar_cont = lv_obj_create(sidebar);
    lv_obj_set_size(s_sidebar_bar_cont, SIDEBAR_W, SCREEN_H - 80);
    lv_obj_set_pos(s_sidebar_bar_cont, 0, 0);
    lv_obj_set_style_bg_opa(s_sidebar_bar_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_sidebar_bar_cont, 0, 0);
    lv_obj_set_style_pad_all(s_sidebar_bar_cont, 0, 0);
    lv_obj_clear_flag(s_sidebar_bar_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_sidebar_bar_cont, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_sidebar_bar_cont, sidebar_bar_click_cb, LV_EVENT_CLICKED, NULL);

    /* Narrow + tall -> LVGL renders lv_bar vertically, filling bottom-up.
     * Bottom-aligned with no radius (instead of centered + rounded) so it
     * reads as anchored/rooted against the sidebar's bottom black gear-
     * button strip, not as a floating pill in the middle of the column. */
    s_level_bar = lv_bar_create(s_sidebar_bar_cont);
    lv_obj_set_size(s_level_bar, 28, SCREEN_H - 80 - 20);
    lv_obj_align(s_level_bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_bar_set_range(s_level_bar, 0, 100);
    lv_bar_set_value(s_level_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_anim_time(s_level_bar, 120, LV_PART_MAIN);   /* blend duration for LV_ANIM_ON updates */
    lv_obj_set_style_bg_color(s_level_bar, lv_color_hex(0x1a1a1a), LV_PART_MAIN);
    lv_obj_set_style_radius(s_level_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_level_bar, lv_color_hex(0x00d4ff), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_level_bar, 0, LV_PART_INDICATOR);
}

/* -----------------------------------------------------------------------
 * Background image -- decodes s_media_cfg.bg_image (if set) straight into
 * a PSRAM buffer, same shape as ui_monitor_img.c's decode_to_psram() (own
 * small copy rather than a shared one, matching how ui_config.c/
 * ui_monitor_config.c already each keep their own read_file()/str_field()
 * rather than sharing). Runs on the LVGL thread already (called from
 * ui_media_enter(), itself reached via a Settings menu item click), so
 * unlike usb_hid.c's PNG decode this needs no lvgl_port_lock().
 * ----------------------------------------------------------------------- */
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

    /* JPEG reports px_size == 0 -- fix up to TRUE_COLOR, same as
     * ui_monitor_img.c's decode_to_psram(). */
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

/* Builds the bg(child0)+mask(child1) pair on top of s_page, same
 * cover-fit zoom/center math as ui_deck.c's ui_deck_lazy_bg_set() (see
 * there for the reasoning behind each line). No-op if bg_image is empty
 * -- s_page's flat 0x222222 fallback (set in build_player_card()) is
 * left showing as-is, matching Deck/Monitor's own "no bg selected" look. */
static void build_bg(void)
{
    if (!s_media_cfg.bg_image[0]) return;

    char path[sizeof("S:") + sizeof(UI_CONFIG_BG_PATH) + 1 + UI_MEDIA_CFG_BG_LEN];
    snprintf(path, sizeof(path), "S:%s/%s", UI_CONFIG_BG_PATH, s_media_cfg.bg_image);

    if (!decode_bg_to_psram(path, &s_bg_dsc)) return;
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
    lv_obj_add_flag(s_bg_img, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(s_bg_img, LV_OBJ_FLAG_CLICKABLE);

    s_bg_mask = lv_obj_create(s_page);
    lv_obj_move_to_index(s_bg_mask, 1);
    lv_obj_set_size(s_bg_mask, CONTENT_W, CONTENT_H);
    lv_obj_set_pos(s_bg_mask, 0, 0);
    lv_obj_set_style_bg_color(s_bg_mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_bg_mask, LV_OPA_50, 0);
    lv_obj_set_style_border_width(s_bg_mask, 0, 0);
    lv_obj_set_style_radius(s_bg_mask, 0, 0);
    lv_obj_add_flag(s_bg_mask, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(s_bg_mask, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

/* -----------------------------------------------------------------------
 * Content area: Now Playing player card
 * ----------------------------------------------------------------------- */
static void build_player_card(lv_obj_t *scr)
{
    s_page = lv_obj_create(scr);
    lv_obj_set_size(s_page, CONTENT_W, CONTENT_H);
    lv_obj_set_pos(s_page, SIDEBAR_W, 0);
    lv_obj_set_style_bg_color(s_page, lv_color_hex(0x222222), 0);   /* same fallback as ui_deck.c/ui_monitor.c's "no bg selected" flat color */
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    build_bg();   /* child0/child1 bg+mask, before any widgets below -- see build_bg() */

    /* Cover placeholder -- always shown as-is, no real cover art yet. */
    s_cover = lv_obj_create(s_page);
    lv_obj_set_size(s_cover, 220, 220);
    lv_obj_align(s_cover, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_color(s_cover, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_radius(s_cover, 12, 0);
    lv_obj_set_style_border_width(s_cover, 0, 0);
    lv_obj_clear_flag(s_cover, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *cover_glyph = lv_label_create(s_cover);
    lv_label_set_text(cover_glyph, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(cover_glyph, lv_color_hex(0x555555), 0);
    lv_obj_set_style_text_font(cover_glyph, &lv_font_montserrat_48, 0);
    lv_obj_center(cover_glyph);

    /* Cover image -- same box/position as the placeholder above, hidden
     * until a real cover arrives over CMD_NOWPLAYING_IMG_* (see
     * apply_cover_image()/clear_cover_image()). PC encodes to exactly
     * HID_MEDIA_IMG_COVER_W x H before sending. */
    s_cover_img = lv_img_create(s_page);
    lv_obj_set_size(s_cover_img, HID_MEDIA_IMG_COVER_W, HID_MEDIA_IMG_COVER_H);
    lv_obj_align(s_cover_img, LV_ALIGN_TOP_MID, 0, 36);
    lv_obj_set_style_bg_opa(s_cover_img, LV_OPA_TRANSP, 0);   /* harmless here (always opaque JPEG), kept for consistency with s_info_img */
    lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);

    /* Title -- shows "None" until a real cover/info image arrives. Kept as
     * a plain label for the disconnected/no-image state; once
     * CMD_NOWPLAYING_IMG_KIND_INFO delivers a PC-rendered strip (needed
     * for CJK text, no font on-device), s_info_img is shown over this
     * region instead and this label is hidden -- see
     * apply_info_image()/clear_info_image(). */
    s_title_lbl = lv_label_create(s_page);
    lv_obj_set_width(s_title_lbl, 480);
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_title_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_24, 0);
    lv_obj_align(s_title_lbl, LV_ALIGN_TOP_MID, 0, 274);

    /* Artist */
    s_artist_lbl = lv_label_create(s_page);
    lv_obj_set_width(s_artist_lbl, 480);
    lv_label_set_long_mode(s_artist_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_artist_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_artist_lbl, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(s_artist_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_artist_lbl, LV_ALIGN_TOP_MID, 0, 308);

    /* Info image -- spans the same region as the title+artist labels
     * above (title at y=274, artist at y=308), hidden until a real one
     * arrives. */
    s_info_img = lv_img_create(s_page);
    lv_obj_set_size(s_info_img, HID_MEDIA_IMG_INFO_W, HID_MEDIA_IMG_INFO_H);
    lv_obj_align(s_info_img, LV_ALIGN_TOP_MID, 0, 270);
    /* The theme applies a default LV_PART_MAIN bg fill to lv_img objects
     * like any other lv_obj -- invisible everywhere else in this file
     * because those images are always fully opaque and cover their whole
     * widget area, but this one has a real alpha channel (see
     * apply_info_image()), so without explicitly clearing it the widget's
     * own opaque background paints first and shows through as a solid
     * block behind the text instead of whatever's actually behind the
     * widget in the page (the flat color or bg image). */
    lv_obj_set_style_bg_opa(s_info_img, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_info_img, LV_OBJ_FLAG_HIDDEN);

    /* Progress bar -- lv_slider, not lv_bar: lv_bar is display-only and
     * cannot be dragged, lv_slider is the draggable counterpart. Knob is
     * kept small (visually close to a plain bar) but its hit-area is
     * padded out so it's still easy to grab on a touchscreen. Starts
     * disabled -- apply_connected_state(true) enables it once real data
     * arrives. */
    s_progress_bar = lv_slider_create(s_page);
    lv_obj_set_size(s_progress_bar, 480, 8);
    lv_obj_align(s_progress_bar, LV_ALIGN_TOP_MID, 0, 356);
    lv_slider_set_range(s_progress_bar, 0, 100);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0x2a2a2a), LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress_bar, 4, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0x00d4ff), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_progress_bar, 4, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(0xffffff), LV_PART_KNOB);
    lv_obj_set_style_radius(s_progress_bar, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_pad_all(s_progress_bar, 4, LV_PART_KNOB);   /* enlarge touch target a bit beyond the visible dot, not too far past the track ends */
    lv_obj_add_event_cb(s_progress_bar, progress_seek_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_progress_bar, progress_seek_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_progress_bar, progress_seek_cb, LV_EVENT_RELEASED, NULL);

    /* Time labels either side of the progress bar -- x lines up with the
     * track ends (480 wide, +-240) now that the knob is small enough
     * (pad 4) not to cover them; the extra vertical gap (y 384 vs the
     * track's y 356) is what keeps the knob clear instead. */
    s_time_elapsed_lbl = lv_label_create(s_page);
    lv_obj_set_style_text_color(s_time_elapsed_lbl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_time_elapsed_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(s_time_elapsed_lbl, LV_ALIGN_TOP_MID, -240, 384);

    s_time_total_lbl = lv_label_create(s_page);
    lv_obj_set_style_text_color(s_time_total_lbl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_time_total_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(s_time_total_lbl, LV_ALIGN_TOP_MID, 240, 384);

    /* Transport buttons: prev / play-pause / next -- one-way remote
     * control, see file header. Start disabled, same as the progress bar. */
    s_prev_btn = lv_btn_create(s_page);
    lv_obj_set_size(s_prev_btn, 56, 56);
    lv_obj_set_style_radius(s_prev_btn, 28, 0);
    lv_obj_set_style_bg_color(s_prev_btn, lv_color_hex(0x2a2a2a), 0);
    lv_obj_align(s_prev_btn, LV_ALIGN_TOP_MID, -76, 400);
    lv_obj_add_event_cb(s_prev_btn, prev_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(s_prev_btn, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_t *prev_lbl = lv_label_create(s_prev_btn);
    lv_label_set_text(prev_lbl, LV_SYMBOL_PREV);
    lv_obj_center(prev_lbl);

    s_play_btn = lv_btn_create(s_page);
    lv_obj_set_size(s_play_btn, 64, 64);
    lv_obj_set_style_radius(s_play_btn, 32, 0);
    lv_obj_set_style_bg_color(s_play_btn, lv_color_hex(0x00558a), 0);
    lv_obj_align(s_play_btn, LV_ALIGN_TOP_MID, 0, 396);
    lv_obj_add_event_cb(s_play_btn, play_pause_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(s_play_btn, LV_OBJ_FLAG_PRESS_LOCK);
    s_play_icon_lbl = lv_label_create(s_play_btn);
    lv_label_set_text(s_play_icon_lbl, LV_SYMBOL_PLAY);
    lv_obj_set_style_text_font(s_play_icon_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(s_play_icon_lbl);

    s_next_btn = lv_btn_create(s_page);
    lv_obj_set_size(s_next_btn, 56, 56);
    lv_obj_set_style_radius(s_next_btn, 28, 0);
    lv_obj_set_style_bg_color(s_next_btn, lv_color_hex(0x2a2a2a), 0);
    lv_obj_align(s_next_btn, LV_ALIGN_TOP_MID, 76, 400);
    lv_obj_add_event_cb(s_next_btn, next_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(s_next_btn, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_t *next_lbl = lv_label_create(s_next_btn);
    lv_label_set_text(next_lbl, LV_SYMBOL_NEXT);
    lv_obj_center(next_lbl);
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void ui_media_enter(lv_obj_t *sidebar)
{
    lv_obj_t *scr = lv_scr_act();

    /* Create queues before starting the timer or registering HID callbacks
     * -- same ordering as ui_monitor_enter(). */
    s_progress_queue = xQueueCreate(1, sizeof(media_progress_t));
    s_level_queue    = xQueueCreate(1, sizeof(uint8_t));
    s_img_queue      = xQueueCreate(4, sizeof(media_img_msg_t));

    /* Single fixed config file for now (config/media/settings.json, no
     * picker UI yet) -- see ui_media_config.h. Missing/invalid file just
     * means "no config": ui_media_config_load() leaves s_media_cfg all-
     * zeroed, so build_bg() below no-ops (empty bg_image) and
     * s_media_cfg.settings is already the same all-empty struct
     * ui_settings_apply_appearance(NULL) substitutes internally -- no
     * need to special-case NULL here, passing it unconditionally is
     * already equivalent. */
    ui_media_config_load(&s_media_cfg);
    ui_settings_apply_appearance(&s_media_cfg.settings);

    build_sidebar_bar(sidebar);
    build_player_card(scr);

    s_seeking             = false;
    s_seek_pending        = false;
    s_seek_pending_ticks  = 0;
    s_real_data_received  = false;
    s_real_data_timeout   = 0;
    s_real_level_received = false;
    s_real_level_timeout  = 0;

    /* Start in the disconnected look. apply_connected_state() always
     * applies unconditionally (the "only on transition" skip lives in
     * media_timer_cb, not here), so this unconditionally draws the
     * disabled/"None" state fresh for this session regardless of
     * whatever s_ui_enabled was left at from a previous session. */
    apply_connected_state(false);

    s_media_timer = lv_timer_create(media_timer_cb, 80, NULL);
    lv_timer_ready(s_media_timer);

    /* Register HID callbacks and notify PC to start sending Media data */
    usb_hid_set_nowplaying_cb(ui_media_on_hid_progress);
    usb_hid_set_audio_level_cb(ui_media_on_hid_level);
    usb_hid_set_nowplaying_img_cb(ui_media_on_hid_img);
    usb_hid_media_subscribe();

    ESP_LOGI(TAG, "entered media mode (progress + level + cover/info image HID-driven)");
}

void ui_media_exit(void)
{
    /* Notify PC to stop sending data and unregister callbacks first */
    usb_hid_media_unsubscribe();
    usb_hid_set_nowplaying_cb(NULL);
    usb_hid_set_audio_level_cb(NULL);
    usb_hid_set_nowplaying_img_cb(NULL);

    if (s_media_timer) {
        lv_timer_del(s_media_timer);
        s_media_timer = NULL;
    }

    if (s_progress_queue) {
        vQueueDelete(s_progress_queue);
        s_progress_queue = NULL;
    }
    if (s_level_queue) {
        vQueueDelete(s_level_queue);
        s_level_queue = NULL;
    }
    if (s_img_queue) {
        /* Drain and free any messages that never got applied -- otherwise
         * their RGB565 buffers leak (queue holds raw pointers, no LVGL
         * object owns them until apply_*_image() runs). */
        media_img_msg_t leftover;
        while (xQueueReceive(s_img_queue, &leftover, 0) == pdTRUE)
            heap_caps_free(leftover.rgb565_data);
        vQueueDelete(s_img_queue);
        s_img_queue = NULL;
    }

    if (s_cover_data) { heap_caps_free(s_cover_data); s_cover_data = NULL; }
    if (s_info_data)  { heap_caps_free(s_info_data);  s_info_data  = NULL; }
    if (s_bg_data)    { heap_caps_free(s_bg_data);    s_bg_data    = NULL; }

    /* s_page owns s_bg_img/s_bg_mask as children -- lv_obj_del(s_page)
     * below deletes them too. lv_img_cache_invalidate_src(NULL) still
     * needed first though: LVGL's image cache keys decoded entries by
     * source pointer (&s_bg_dsc), which is a static -- the SAME address
     * every time Media is re-entered, only its .data changes to a fresh
     * buffer -- so without this a stale cache hit could serve the just-
     * freed buffer above on next entry. Same issue ui_monitor_img.c's
     * ui_monitor_img_free_all() already documents/handles for Monitor. */
    lv_img_cache_invalidate_src(NULL);

    if (s_page) {
        lv_obj_del(s_page);
        s_page = NULL;
    }
    s_bg_img = s_bg_mask = NULL;
    s_cover = s_title_lbl = s_artist_lbl = NULL;
    s_cover_img = s_info_img = NULL;
    s_progress_bar = s_time_elapsed_lbl = s_time_total_lbl = NULL;
    s_prev_btn = s_play_btn = s_next_btn = s_play_icon_lbl = NULL;

    if (s_sidebar_bar_cont) {
        lv_obj_del(s_sidebar_bar_cont);
        s_sidebar_bar_cont = NULL;
    }
    s_level_bar = NULL;

    s_seeking             = false;
    s_seek_pending        = false;
    s_seek_pending_ticks  = 0;
    s_real_data_received  = false;
    s_real_data_timeout   = 0;
    s_real_level_received = false;
    s_real_level_timeout  = 0;
    s_ui_enabled           = false;

    ESP_LOGI(TAG, "exited media mode");
}

void ui_media_deselect_current(void)
{
    if (!s_page) return;
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
}

void ui_media_reselect_current(void)
{
    if (!s_page) return;
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_HIDDEN);
}

void ui_media_push_progress(uint32_t position_ms, uint32_t duration_ms, bool playing)
{
    if (!s_progress_queue) return;
    media_progress_t p = {
        .position_ms = position_ms,
        .duration_ms = duration_ms,
        .playing     = playing,
    };
    xQueueOverwrite(s_progress_queue, &p);
}

/* -----------------------------------------------------------------------
 * HID callbacks -- called from TinyUSB task context.
 * Only queue operations allowed here; no LVGL calls.
 * ----------------------------------------------------------------------- */
static void ui_media_on_hid_progress(uint32_t position_ms, uint32_t duration_ms, bool playing)
{
    ui_media_push_progress(position_ms, duration_ms, playing);
}

void ui_media_push_level(uint8_t level)
{
    if (!s_level_queue) return;
    if (level > 100) level = 100;
    xQueueOverwrite(s_level_queue, &level);
}

static void ui_media_on_hid_level(uint8_t level)
{
    ui_media_push_level(level);
}

/* usb_hid.c hands off ownership of rgb565_data here -- if the queue is
 * missing (not entered/already exited) or full, this frees it immediately
 * rather than leaking it. media_timer_cb() (LVGL thread) is the only
 * other place that frees an s_img_queue item, applying it via
 * apply_cover_image()/apply_info_image(). */
static void ui_media_on_hid_img(uint8_t kind, uint8_t *rgb565_data, uint16_t w, uint16_t h)
{
    if (!s_img_queue) {
        heap_caps_free(rgb565_data);
        return;
    }

    media_img_msg_t msg = { .kind = kind, .rgb565_data = rgb565_data, .w = w, .h = h };
    if (xQueueSend(s_img_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "img queue full, dropping kind %u", kind);
        heap_caps_free(rgb565_data);
    }
}
