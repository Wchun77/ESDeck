#include "ui_settings.h"
#include "ui_msc.h"
#include "ui_config_dialog.h"
#include "ui_keyboard.h"
#include "ui_monitor.h"
#include "ui_deck.h"
#include "ui_img_pool.h"
#include "ui.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W  800
#define SCREEN_H  480

/* Current UI mode */
static ui_mode_t  s_mode    = UI_MODE_DECK;
static lv_obj_t  *s_panel   = NULL;
static lv_obj_t  *s_overlay = NULL;

/* Item for mode switch — label changes depending on current mode */
static lv_obj_t  *s_mode_item_lbl = NULL;

/* Config used during Monitor -> Deck background preload task */
static deck_cfg_t s_back_cfg;

/* -----------------------------------------------------------------------
 * Accessors
 * ----------------------------------------------------------------------- */
ui_mode_t ui_settings_get_mode(void)
{
    return s_mode;
}

/* -----------------------------------------------------------------------
 * Internal helpers
 * ----------------------------------------------------------------------- */
static void hide_menu(void)
{
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
}

static void update_mode_label(void)
{
    if (!s_mode_item_lbl) return;
    lv_label_set_text(s_mode_item_lbl,
                      s_mode == UI_MODE_DECK ? "Switch to Monitor"
                                             : "Switch to Deck");
}

/* -----------------------------------------------------------------------
 * Deck -> Monitor background task
 * No image preload needed for monitor (no bg images yet),
 * but go through the same async pattern for future-proofing and
 * so the switching screen is visible before monitor builds.
 * ----------------------------------------------------------------------- */
static void on_enter_monitor_done(void *arg)
{
    ui_monitor_enter(ui_get_sidebar());
}

static void enter_monitor_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    lv_async_call(on_enter_monitor_done, NULL);
    vTaskDelete(NULL);
}

/* -----------------------------------------------------------------------
 * Monitor -> Deck background task
 * ----------------------------------------------------------------------- */
static void on_back_to_deck_done(void *arg)
{
    ui_deck_build(ui_get_sidebar(), &s_back_cfg);
}

static void back_to_deck_task(void *arg)
{
    /* Invalidate LVGL image cache so stale entries from Monitor do not
     * render with freed buffer addresses on the first Deck frame. */
    lv_img_cache_invalidate_src(NULL);
    ui_img_pool_load(&s_back_cfg);
    /* Ensure at least 1 second on the switching screen. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    lv_async_call(on_back_to_deck_done, NULL);
    vTaskDelete(NULL);
}

/* -----------------------------------------------------------------------
 * Item callbacks
 * ----------------------------------------------------------------------- */
static void item_msc_cb(lv_event_t *e)
{
    hide_menu();
    ui_msc_show_confirm_dialog();
}

static void item_config_cb(lv_event_t *e)
{
    hide_menu();
    ui_config_dialog_show();
}

static void item_keyboard_cb(lv_event_t *e)
{
    hide_menu();
    ui_keyboard_show();
}

static void item_mode_cb(lv_event_t *e)
{
    hide_menu();

    if (s_mode == UI_MODE_DECK) {
        /* Deck -> Monitor */
        ui_deck_destroy();
        s_mode = UI_MODE_MONITOR;
        update_mode_label();
        ui_show_switching_screen("Entering Monitor Mode...");
        xTaskCreate(enter_monitor_task, "enter_mon", 4096, NULL, 3, NULL);
    } else {
        /* Monitor -> Deck
         * Show the switching screen FIRST so LVGL renders a clean frame
         * before ui_monitor_exit() frees the background image buffer.
         * Without this, the last Monitor frame may render with a freed buffer. */
        s_mode = UI_MODE_DECK;
        update_mode_label();

        bool cfg_ok = ui_config_load(&s_back_cfg);
        if (!cfg_ok || s_back_cfg.page_count == 0) {
            s_back_cfg.page_count = 1;
            s_back_cfg.pages      = calloc(1, sizeof(page_cfg_t));
            snprintf(s_back_cfg.pages[0].name, UI_CONFIG_NAME_LEN, "Main");
            s_back_cfg.pages[0].button_count = 0;
            s_back_cfg.pages[0].buttons      = NULL;
        }

        ui_show_switching_screen("Returning to Deck...");
        lv_refr_now(NULL);   /* flush switching screen before freeing monitor buffers */
        ui_monitor_exit();
        xTaskCreate(back_to_deck_task, "back_deck", 8192, NULL, 3, NULL);
    }
}

static void overlay_cb(lv_event_t *e)
{
    hide_menu();
}

/* -----------------------------------------------------------------------
 * Helper: create a single menu item button
 * ----------------------------------------------------------------------- */
static lv_obj_t *add_item(lv_obj_t *panel, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *item = lv_btn_create(panel);
    lv_obj_set_width(item, LV_PCT(100));
    lv_obj_set_style_bg_color(item, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_bg_color(item, lv_color_hex(0x3a3a3a), LV_STATE_PRESSED);
    lv_obj_set_style_radius(item, 4, 0);
    lv_obj_add_event_cb(item, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(item);
    lv_label_set_text(lbl, text);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    return lbl;
}

/* -----------------------------------------------------------------------
 * Public
 * ----------------------------------------------------------------------- */
lv_obj_t *ui_settings_build(lv_obj_t *scr)
{
    s_panel = lv_obj_create(scr);
    lv_obj_set_size(s_panel, 220, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(s_panel, 8, 0);
    lv_obj_set_style_pad_all(s_panel, 8, 0);
    lv_obj_set_layout(s_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);

    /* Deck-only items */
    add_item(s_panel, "Keyboard Mode", item_keyboard_cb);

    /* Shared items */
    add_item(s_panel, "Select Config",      item_config_cb);
    add_item(s_panel, "Switch to MSC mode", item_msc_cb);

    /* Mode switch item — label updates dynamically */
    s_mode_item_lbl = add_item(s_panel, "", item_mode_cb);
    update_mode_label();

    lv_obj_update_layout(s_panel);
    lv_obj_set_pos(s_panel,
                   80 + 8,
                   SCREEN_H - lv_obj_get_height(s_panel) - 8);
    return s_panel;
}

void ui_settings_toggle(void)
{
    if (lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN)) {
        /* Hide keyboard item when in Monitor mode */
        lv_obj_t *keyboard_item = lv_obj_get_child(s_panel, 0);
        if (s_mode == UI_MODE_MONITOR)
            lv_obj_add_flag(keyboard_item, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_clear_flag(keyboard_item, LV_OBJ_FLAG_HIDDEN);

        /* Recalculate y position after show/hide of items */
        lv_obj_update_layout(s_panel);
        lv_obj_set_pos(s_panel,
                       80 + 8,
                       SCREEN_H - lv_obj_get_height(s_panel) - 8);

        lv_obj_t *scr = lv_scr_act();
        s_overlay = lv_obj_create(scr);
        lv_obj_set_size(s_overlay, SCREEN_W, SCREEN_H);
        lv_obj_set_pos(s_overlay, 0, 0);
        lv_obj_set_style_bg_opa(s_overlay, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_overlay, 0, 0);
        lv_obj_add_event_cb(s_overlay, overlay_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_move_foreground(s_panel);
        lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        hide_menu();
    }
}