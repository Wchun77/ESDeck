#include "ui_media.h"
#include "ui.h"
#include "ui_settings.h"
#include "esp_log.h"
#include "esp_random.h"
#include <math.h>
#include <stdio.h>

#define TAG  "MEDIA"

/* Content area -- same convention as ui_monitor.c's CONTENT_W/H. */
#define CONTENT_W   (SCREEN_W - SIDEBAR_W)
#define CONTENT_H   SCREEN_H

/* -----------------------------------------------------------------------
 * UI PROTOTYPE ONLY -- everything below is fake data driven by a local
 * timer. No PC/HID wiring yet (see ui_media.h). The level bar uses a sine
 * wave + jitter to look alive; the player card cycles through a small
 * built-in track list so title length / marquee behaviour can be eyeballed
 * before the real HID protocol (image-chunked cover art + title strip,
 * see project notes) is designed.
 *
 * The title label uses LVGL's native LV_LABEL_LONG_SCROLL_CIRCULAR as a
 * stand-in marquee. This only works here because the mock titles are
 * plain ASCII (Montserrat has the glyphs) -- the real device has no CJK
 * font, so the production version will replace this label with a
 * PC-rendered scrolling image strip and animate its x-offset the same way
 * (see discussion: text-as-image, same chunk pipeline as cover art).
 * ----------------------------------------------------------------------- */

typedef struct {
    const char *title;
    const char *artist;
    int         duration_sec;
} mock_track_t;

static const mock_track_t s_mock_tracks[] = {
    { "Clair de Lune",                                                    "Debussy",                      257 },
    { "Bohemian Rhapsody",                                                "Queen",                         355 },
    { "A Very Long Song Title That Should Definitely Scroll On This Screen", "Some Artist With A Long Name", 217 },
};
#define MOCK_TRACK_COUNT  (sizeof(s_mock_tracks) / sizeof(s_mock_tracks[0]))

/* -----------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */
static lv_obj_t   *s_sidebar_bar_cont = NULL;  /* sidebar sub-region, child of shared sidebar */
static lv_obj_t   *s_level_bar        = NULL;  /* vertical fake VU meter */

static lv_obj_t   *s_page          = NULL;     /* content area root */
static lv_obj_t   *s_cover         = NULL;
static lv_obj_t   *s_title_lbl     = NULL;
static lv_obj_t   *s_artist_lbl    = NULL;
static lv_obj_t   *s_progress_bar  = NULL;
static lv_obj_t   *s_time_elapsed_lbl = NULL;
static lv_obj_t   *s_time_total_lbl   = NULL;
static lv_obj_t   *s_play_btn      = NULL;
static lv_obj_t   *s_play_icon_lbl = NULL;

static lv_timer_t *s_fake_timer = NULL;

static int  s_track_idx    = 0;
static int  s_progress_sec = 0;
static bool s_playing      = true;
static bool s_seeking      = false;  /* true while user is dragging the progress slider */
static float s_level_phase = 0.0f;
static int  s_tick_count   = 0;   /* ticks since last 1s progress step */

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
static void format_time(int sec, char *buf, size_t buf_len)
{
    if (sec < 0) sec = 0;
    snprintf(buf, buf_len, "%d:%02d", sec / 60, sec % 60);
}

static void load_track(int idx)
{
    const mock_track_t *t = &s_mock_tracks[idx];
    lv_label_set_text(s_title_lbl, t->title);
    lv_label_set_text(s_artist_lbl, t->artist);
    s_progress_sec = 0;
    lv_slider_set_value(s_progress_bar, 0, LV_ANIM_OFF);

    char buf[16];
    format_time(0, buf, sizeof(buf));
    lv_label_set_text(s_time_elapsed_lbl, buf);
    format_time(t->duration_sec, buf, sizeof(buf));
    lv_label_set_text(s_time_total_lbl, buf);
}

/* -----------------------------------------------------------------------
 * Transport button callbacks -- mock only, local state, no PC action.
 * ----------------------------------------------------------------------- */
static void play_pause_cb(lv_event_t *e)
{
    s_playing = !s_playing;
    lv_label_set_text(s_play_icon_lbl, s_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static void prev_cb(lv_event_t *e)
{
    s_track_idx = (s_track_idx - 1 + MOCK_TRACK_COUNT) % MOCK_TRACK_COUNT;
    load_track(s_track_idx);
}

static void next_cb(lv_event_t *e)
{
    s_track_idx = (s_track_idx + 1) % MOCK_TRACK_COUNT;
    load_track(s_track_idx);
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

/* Progress slider -- drag to seek. LV_EVENT_PRESSED/RELEASED bracket the
 * drag so fake_timer_cb() stops overwriting the slider value out from
 * under the user's finger; VALUE_CHANGED fires continuously while
 * dragging (and once on a plain tap) so the time label tracks live. */
static void progress_seek_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);

    if (code == LV_EVENT_PRESSED) {
        s_seeking = true;
        return;
    }

    if (code == LV_EVENT_VALUE_CHANGED || code == LV_EVENT_RELEASED) {
        int pct = lv_slider_get_value(s_progress_bar);
        const mock_track_t *t = &s_mock_tracks[s_track_idx];
        s_progress_sec = pct * t->duration_sec / 100;

        char buf[16];
        format_time(s_progress_sec, buf, sizeof(buf));
        lv_label_set_text(s_time_elapsed_lbl, buf);
    }

    if (code == LV_EVENT_RELEASED) {
        s_seeking = false;
        s_tick_count = 0;   /* re-sync the ~1s cadence to the moment the user let go */
    }
}

/* -----------------------------------------------------------------------
 * Fake data timer -- 80ms tick.
 *   - level bar updates every tick (smooth wobble)
 *   - progress advances once per ~1s (12 ticks), loops track on end
 * ----------------------------------------------------------------------- */
static void fake_timer_cb(lv_timer_t *timer)
{
    /* Sine wave + jitter, clamped 0-100 -- just needs to look alive. */
    s_level_phase += 0.35f;
    float wave   = (sinf(s_level_phase) + 1.0f) * 0.5f;        /* 0..1 */
    int   jitter = (int)(esp_random() % 20) - 10;              /* -10..+9 */
    int   level  = (int)(wave * 80.0f) + 15 + jitter;
    if (level < 0)   level = 0;
    if (level > 100) level = 100;
    lv_bar_set_value(s_level_bar, level, LV_ANIM_OFF);

    if (!s_playing || s_seeking) return;

    s_tick_count++;
    if (s_tick_count < 12) return;   /* ~12 * 80ms = ~1s */
    s_tick_count = 0;

    s_progress_sec++;
    const mock_track_t *t = &s_mock_tracks[s_track_idx];
    if (s_progress_sec >= t->duration_sec) {
        next_cb(NULL);
        return;
    }

    int pct = (t->duration_sec > 0) ? (s_progress_sec * 100 / t->duration_sec) : 0;
    lv_slider_set_value(s_progress_bar, pct, LV_ANIM_OFF);

    char buf[16];
    format_time(s_progress_sec, buf, sizeof(buf));
    lv_label_set_text(s_time_elapsed_lbl, buf);
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
    lv_obj_set_style_bg_color(s_level_bar, lv_color_hex(0x1a1a1a), LV_PART_MAIN);
    lv_obj_set_style_radius(s_level_bar, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_level_bar, lv_color_hex(0x00d4ff), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_level_bar, 0, LV_PART_INDICATOR);
}

/* -----------------------------------------------------------------------
 * Content area: mock player card
 * ----------------------------------------------------------------------- */
static void build_player_card(lv_obj_t *scr)
{
    s_page = lv_obj_create(scr);
    lv_obj_set_size(s_page, CONTENT_W, CONTENT_H);
    lv_obj_set_pos(s_page, SIDEBAR_W, 0);
    lv_obj_set_style_bg_color(s_page, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);

    /* TODO: once Media has a real bg_image (SD-loaded, like Deck/Monitor),
     * add a semi-transparent mask layer between the bg and the card
     * widgets below -- same bg(child0)+mask(child1) convention as
     * ui_deck.c's create_page()/ui_monitor.c's make_page(), so the player
     * card stays readable over a busy background image. Flat color only
     * for now, no bg image support yet. */

    /* Cover placeholder */
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

    /* Title -- native LVGL marquee stand-in, see file header comment. */
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

    /* Progress bar -- lv_slider, not lv_bar: lv_bar is display-only and
     * cannot be dragged, lv_slider is the draggable counterpart. Knob is
     * kept small (visually close to a plain bar) but its hit-area is
     * padded out so it's still easy to grab on a touchscreen. */
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

    /* Transport buttons: prev / play-pause / next */
    lv_obj_t *prev_btn = lv_btn_create(s_page);
    lv_obj_set_size(prev_btn, 56, 56);
    lv_obj_set_style_radius(prev_btn, 28, 0);
    lv_obj_set_style_bg_color(prev_btn, lv_color_hex(0x2a2a2a), 0);
    lv_obj_align(prev_btn, LV_ALIGN_TOP_MID, -76, 400);
    lv_obj_add_event_cb(prev_btn, prev_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(prev_btn, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_t *prev_lbl = lv_label_create(prev_btn);
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
    lv_label_set_text(s_play_icon_lbl, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_font(s_play_icon_lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(s_play_icon_lbl);

    lv_obj_t *next_btn = lv_btn_create(s_page);
    lv_obj_set_size(next_btn, 56, 56);
    lv_obj_set_style_radius(next_btn, 28, 0);
    lv_obj_set_style_bg_color(next_btn, lv_color_hex(0x2a2a2a), 0);
    lv_obj_align(next_btn, LV_ALIGN_TOP_MID, 76, 400);
    lv_obj_add_event_cb(next_btn, next_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(next_btn, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_t *next_lbl = lv_label_create(next_btn);
    lv_label_set_text(next_lbl, LV_SYMBOL_NEXT);
    lv_obj_center(next_lbl);
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void ui_media_enter(lv_obj_t *sidebar)
{
    lv_obj_t *scr = lv_scr_act();

    /* No Media config file yet -- plain background + default gear glyph. */
    ui_settings_apply_appearance(NULL);

    build_sidebar_bar(sidebar);
    build_player_card(scr);

    s_track_idx    = 0;
    s_progress_sec = 0;
    s_playing      = true;
    s_seeking      = false;
    s_tick_count   = 0;
    s_level_phase  = 0.0f;
    load_track(s_track_idx);

    s_fake_timer = lv_timer_create(fake_timer_cb, 80, NULL);
    lv_timer_ready(s_fake_timer);

    ESP_LOGI(TAG, "entered media mode (UI mock, no PC data)");
}

void ui_media_exit(void)
{
    if (s_fake_timer) {
        lv_timer_del(s_fake_timer);
        s_fake_timer = NULL;
    }

    if (s_page) {
        lv_obj_del(s_page);
        s_page = NULL;
    }
    s_cover = s_title_lbl = s_artist_lbl = NULL;
    s_progress_bar = s_time_elapsed_lbl = s_time_total_lbl = NULL;
    s_play_btn = s_play_icon_lbl = NULL;

    if (s_sidebar_bar_cont) {
        lv_obj_del(s_sidebar_bar_cont);
        s_sidebar_bar_cont = NULL;
    }
    s_level_bar = NULL;

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
