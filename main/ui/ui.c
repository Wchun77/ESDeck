#include "ui.h"
#include "ui_deck.h"
#include "ui_settings.h"
#include "ui_toast.h"
#include "ui_font_cjk.h"
#include "sys_clock.h"
#include "usb/usb_hid.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static lv_obj_t *s_sidebar       = NULL;
static lv_obj_t *s_context_panel = NULL;
static lv_obj_t *s_switching_cover = NULL;

/* -----------------------------------------------------------------------
 * Mode query callback — called from TinyUSB task via usb_hid.
 * Returns the raw ui_mode_t value (0=deck, 1=monitor, 2=media) -- usb_hid.c
 * intentionally doesn't know about ui_mode_t, see usb_hid_reply_mode().
 * ----------------------------------------------------------------------- */
static uint8_t mode_query_cb(void)
{
    return (uint8_t)ui_settings_get_mode();
}

/* -----------------------------------------------------------------------
 * Cross-module accessors
 * ----------------------------------------------------------------------- */
lv_obj_t *ui_get_sidebar(void)
{
    return s_sidebar;
}

lv_obj_t *ui_get_context_panel(void)
{
    return s_context_panel;
}

/* -----------------------------------------------------------------------
 * Switching screen
 * ----------------------------------------------------------------------- */
void ui_show_switching_screen(const char *msg)
{
    /* Only one cover should ever exist -- a leftover from a previous
     * transition would otherwise sit buried in the object tree forever,
     * ready to show through as a stuck screen if a later page-visibility
     * bug ever leaves a gap above it (see sidebar_btn_cb early-return
     * fix). */
    ui_hide_switching_screen();

    lv_obj_t *cover = lv_obj_create(lv_scr_act());
    lv_obj_set_size(cover, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(cover, 0, 0);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_radius(cover, 0, 0);
    lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(cover);
    lv_label_set_text(lbl, msg);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl);

    s_switching_cover = cover;
}

void ui_hide_switching_screen(void)
{
    if (!s_switching_cover) return;
    lv_obj_del(s_switching_cover);
    s_switching_cover = NULL;
}

/* -----------------------------------------------------------------------
 * Gear button callback
 * ----------------------------------------------------------------------- */
static void settings_btn_cb(lv_event_t *e)
{
    ui_settings_select();
}

/* -----------------------------------------------------------------------
 * Static UI: sidebar frame + gear button + context panel
 * ----------------------------------------------------------------------- */
static void ui_build_static(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x111111), 0);

    /* Sidebar strip */
    s_sidebar = lv_obj_create(scr);
    lv_obj_set_size(s_sidebar, SIDEBAR_W, SCREEN_H);
    lv_obj_set_pos(s_sidebar, 0, 0);
    lv_obj_set_style_bg_color(s_sidebar, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(s_sidebar, 0, 0);
    lv_obj_set_style_radius(s_sidebar, 0, 0);
    lv_obj_set_style_pad_all(s_sidebar, 0, 0);
    lv_obj_clear_flag(s_sidebar, LV_OBJ_FLAG_SCROLLABLE);

    /* Bottom area of sidebar: holds the gear button */
    lv_obj_t *sidebar_bottom = lv_obj_create(s_sidebar);
    lv_obj_set_size(sidebar_bottom, SIDEBAR_W, 80);
    lv_obj_set_pos(sidebar_bottom, 0, SCREEN_H - 80);
    lv_obj_set_style_bg_opa(sidebar_bottom, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(sidebar_bottom, 0, 0);
    lv_obj_set_style_pad_all(sidebar_bottom, 8, 0);
    lv_obj_clear_flag(sidebar_bottom, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *settings_btn = lv_btn_create(sidebar_bottom);
    lv_obj_set_size(settings_btn, 64, 64);
    lv_obj_set_style_bg_color(settings_btn, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_radius(settings_btn, 8, 0);
    lv_obj_add_event_cb(settings_btn, settings_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(settings_btn, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_t *settings_label = lv_label_create(settings_btn);
    lv_label_set_text(settings_label, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(settings_label, &lv_font_montserrat_24, 0);
    lv_obj_center(settings_label);

    /* Settings page (gear button takes you here, like selecting any other page) */
    s_context_panel = ui_settings_build(scr, settings_btn);
}

/* -----------------------------------------------------------------------
 * CJK notification font preload -- warms ui_font_cjk_get()'s cache on a
 * background task at boot, so the *first* real ANCS notification doesn't
 * stall the LVGL task waiting on a synchronous font load inside
 * ancs_toast_push_cb() (see ble_manager.c). ui_font_cjk_get() caches the
 * loaded font after its first call (s_font/s_tried in ui_font_cjk.c), so
 * every call after this one -- including the real one from
 * ble_manager.c -- is effectively free.
 *
 * No stack-depth risk here (unlike the earlier FreeType attempt --
 * lv_font_load() just parses LVGL's own pre-converted bitmap font
 * format, no rasterization engine involved) -- but it IS slow: measured
 * ~13s to load a ~5500-glyph common-Hanzi .bin from the SD card.
 * lv_font_load() does two full passes over every glyph (metadata, then
 * bitmap data), each doing an lv_fs_seek() + several small reads, and
 * SD random-access seeks are the expensive part -- this is roughly
 * linear in glyph count, not file size. Runs on its own task rather than
 * inline in my_ui_init() purely so it doesn't block reaching the main
 * screen for that same ~13s -- no LVGL tree access needed here at all
 * (unlike the earlier version of this task, back when it also drew a
 * visible on-screen test string -- removed now that real ANCS text
 * exercises this same path end to end, see ble_manager.c). */
static void cjk_font_preload_task(void *arg)
{
    (void)arg;
    ui_font_cjk_get();
    vTaskDelete(NULL);
}

/* -----------------------------------------------------------------------
 * Entry point
 * ----------------------------------------------------------------------- */
void my_ui_init(void)
{
    usb_hid_set_mode_query_cb(mode_query_cb);

    /* Registered once here, not per UI mode -- the PC may send CMD_TIME
     * while any mode is active, and the clock keeps ticking on its own
     * across mode switches. See sys_clock.h. */
    sys_clock_init();
    usb_hid_set_time_cb(sys_clock_push_hid_time);

    ui_build_static();
    ui_toast_init();

    xTaskCreate(cjk_font_preload_task, "cjk_font_preload", 4096, NULL, 3, NULL);

    /* Temporary checkpoint -- isolates ui_build_static()+ui_toast_init()'s
     * own PSRAM cost from ui_deck_build()'s (logged separately below and
     * in ui_deck.c), to track down where a reported PSRAM regression is
     * actually coming from instead of guessing. Remove once that's
     * settled. */
    ESP_LOGI("UI", "before deck build - PSRAM free: %d B",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    deck_cfg_t *cfg = ui_deck_preload_take_cfg();
    ui_deck_build(s_sidebar, cfg);

    ESP_LOGI("UI", "init done - PSRAM free: %d B",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI("UI", "init done - internal free: %d B",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}