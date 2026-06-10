#include "ui.h"
#include "ui_img_pool.h"
#include "ui_deck.h"
#include "ui_settings.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

static lv_obj_t *s_sidebar       = NULL;
static lv_obj_t *s_context_panel = NULL;

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
void ui_show_switching_screen(void)
{
    lv_obj_t *cover = lv_obj_create(lv_scr_act());
    lv_obj_set_size(cover, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(cover, 0, 0);
    lv_obj_set_style_bg_color(cover, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_radius(cover, 0, 0);
    lv_obj_clear_flag(cover, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *lbl = lv_label_create(cover);
    lv_label_set_text(lbl, "Switching config...");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl);
}

/* -----------------------------------------------------------------------
 * Gear button callback
 * ----------------------------------------------------------------------- */
static void settings_btn_cb(lv_event_t *e)
{
    ui_settings_toggle();
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
    lv_obj_t *settings_label = lv_label_create(settings_btn);
    lv_label_set_text(settings_label, LV_SYMBOL_SETTINGS);
    lv_obj_set_style_text_font(settings_label, &lv_font_montserrat_24, 0);
    lv_obj_center(settings_label);

    /* Context panel (gear menu) */
    s_context_panel = ui_settings_build(scr);
}

/* -----------------------------------------------------------------------
 * Entry point
 * ----------------------------------------------------------------------- */
void my_ui_init(void)
{
    ui_build_static();

    deck_cfg_t *cfg = ui_img_pool_take_preload_cfg();
    ui_deck_build(s_sidebar, cfg);

    ESP_LOGI("UI", "init done - PSRAM free: %d B",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI("UI", "init done - internal free: %d B",
             heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}
