#include "ui_settings.h"
#include "ui_msc.h"
#include "ui_config_dialog.h"
#include "ui_monitor_config.h"
#include "ui_keyboard.h"
#include "ui_monitor.h"
#include "ui_deck.h"
#include "ui_media.h"
#include "ui_img_pool.h"
#include "ui_log_view.h"
#include "ui.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "app_config.h"
#include "nvs_manager/nvs_manager.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SCREEN_W   800
#define SCREEN_H   480
#define SIDEBAR_W   80

/* -----------------------------------------------------------------------
 * Settings menu tree
 *
 * The settings page is a small nested-menu tree instead of one flat list,
 * so it can keep growing (System > Select Config / MSC / Info, and so on)
 * without the top level ever getting taller than a handful of rows. Each
 * node is either an action (does something, page stays put underneath) or
 * a submenu (pushes a child array onto the nav stack). Both Deck and
 * Monitor mode share the exact same tree; mode_mask decides which nodes
 * are visible in which mode, filtered at render time.
 * ----------------------------------------------------------------------- */
typedef enum {
    SETMASK_DECK    = 1 << 0,
    SETMASK_MONITOR = 1 << 1,
    SETMASK_MEDIA   = 1 << 2,
    SETMASK_BOTH    = SETMASK_DECK | SETMASK_MONITOR,
    SETMASK_ALL     = SETMASK_DECK | SETMASK_MONITOR | SETMASK_MEDIA,
} setting_mask_t;

typedef enum {
    SETTING_ACTION,
    SETTING_SUBMENU,
} setting_node_type_t;

typedef struct setting_node_s {
    const char                  *label;
    setting_node_type_t          type;
    uint8_t                       mode_mask;
    lv_event_cb_t                 action_cb;   /* used when type == SETTING_ACTION */
    const struct setting_node_s  *children;    /* used when type == SETTING_SUBMENU */
    int                            child_count;
} setting_node_t;

#define SETTINGS_STACK_MAX 4

/* Current UI mode */
static ui_mode_t  s_mode = UI_MODE_DECK;

/* Settings page widgets */
static lv_obj_t *s_panel          = NULL;  /* the Settings "page" shell (right of sidebar, zero padding -- bg/mask attach directly to this, like ui_deck.c's page/btn_cont split) */
static lv_obj_t *s_content        = NULL;  /* padded flex column inside s_panel, holds header + s_list */
static lv_obj_t *s_list           = NULL;  /* scrollable item list inside the page */
static lv_obj_t *s_back_btn       = NULL;
static lv_obj_t *s_back_lbl       = NULL;  /* back_btn's glyph -- swaps LV_SYMBOL_LEFT/CLOSE with menu depth */
static lv_obj_t *s_breadcrumb_lbl = NULL;
static lv_obj_t *s_gear_btn       = NULL;  /* sidebar gear button -- highlighted like a page button */
static lv_obj_t *s_gear_glyph     = NULL;  /* LV_SYMBOL_SETTINGS label, gear_btn's child 0 -- hidden/shown as side_icon comes and goes */
static bool       s_gear_has_icon = false; /* true = gear shows side_icon image, selection uses outline instead of bg_color */

/* Settings page appearance -- set via ui_settings_apply_appearance(), called
 * by ui_deck_build()/ui_monitor_enter() with the active Deck/Monitor
 * config's own "settings" object, so this always reflects whichever config
 * is currently active rather than being loaded once from a standalone file.
 * Both fields optional; empty just means "off" (gear shows its glyph, page
 * has a plain bg). */
static char s_side_icon[UI_SETTINGS_SIDE_ICON_LEN];
static char s_bg_image[UI_SETTINGS_BG_LEN];

/* Settings page bg image -- decoded lazily the first time the page is
 * selected after a (re)load (see settings_lazy_bg_set()).
 *
 * Settings shares whichever lazy/LRU pool is currently active instead of
 * holding its own always-resident buffer that doesn't compete/cooperate
 * with page images for the same PSRAM budget:
 *   - Deck mode and Monitor mode both borrow a slot in the same
 *     ui_img_pool (marked as bg so it's eligible for LRU eviction
 *     like any page background) -- one pool, one code path, since
 *     Monitor's per-page backgrounds joined this same pool directly
 *     (see ui_monitor.c's monitor_lazy_bg_set()).
 *   - Anything else (Media today, or any future mode with no pool of its
 *     own): falls back to a private PSRAM buffer (s_bg_dsc) that
 *     Settings decodes and frees itself, same as before this existed.
 *
 * s_bg_dsc_ptr points at whichever of the above is currently backing the
 * displayed image; s_bg_owns_buf is only true for the standalone/private
 * case, since that's the only one where Settings itself is responsible
 * for freeing the pixel buffer -- the pool case owns its own buffer and
 * can free it out from under Settings via LRU eviction or mode exit, see
 * ui_settings_bg_widget_remove().
 *
 * s_bg_dsc is a fixed static address reused across config switches (only
 * its contents change) in the standalone case, so ui_settings_apply_
 * appearance() must invalidate LVGL's image cache when swapping it out --
 * the pool case has no equivalent need since ui_img_pool's storage
 * is freed and freshly reallocated every mode entry/exit, not a reused
 * static address. */
static lv_img_dsc_t  s_bg_dsc;
static lv_img_dsc_t *s_bg_dsc_ptr  = NULL;  /* NULL until a decode has succeeded */
static bool          s_bg_owns_buf = false; /* true only for the standalone/private-buffer case */
static bool          s_bg_applied  = false; /* true once settings_lazy_bg_set() has run for the current s_bg_image (even if it failed) -- guards against re-running every select */

/* Navigation stack -- index 0 is always the root menu. */
static const setting_node_t *s_menu_stack[SETTINGS_STACK_MAX];
static int                   s_menu_stack_count[SETTINGS_STACK_MAX];
static const char           *s_menu_stack_label[SETTINGS_STACK_MAX];
static int                   s_stack_depth = 0;

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
 * Deck -> Monitor background task
 * No image preload needed for monitor (no bg images yet),
 * but go through the same async pattern for future-proofing and
 * so the switching screen is visible before monitor builds.
 * ----------------------------------------------------------------------- */
static void on_enter_monitor_done(void *arg)
{
    ui_monitor_enter(ui_get_sidebar());
    ui_hide_switching_screen();
}

static void enter_monitor_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    lv_async_call(on_enter_monitor_done, NULL);
    vTaskDelete(NULL);
}

/* -----------------------------------------------------------------------
 * Deck/Monitor -> Media background task
 * Media is UI-mock-only right now (no bg image / icon loading yet), but
 * goes through the same async pattern as Deck<->Monitor for
 * future-proofing -- once it grows real SD-loaded assets, this is already
 * the right shape, and the switching screen is visible for the same
 * reason it is for Monitor: a beat of "something is loading" feedback
 * instead of an instant, jarring cut.
 * ----------------------------------------------------------------------- */
static void on_enter_media_done(void *arg)
{
    ui_media_enter(ui_get_sidebar());
    ui_hide_switching_screen();
}

static void enter_media_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    lv_async_call(on_enter_media_done, NULL);
    vTaskDelete(NULL);
}

/* -----------------------------------------------------------------------
 * Monitor -> Deck background task
 * ----------------------------------------------------------------------- */
static void on_back_to_deck_done(void *arg)
{
    ui_deck_build(ui_get_sidebar(), &s_back_cfg);
    ui_hide_switching_screen();
}

static void back_to_deck_task(void *arg)
{
    /* Invalidate LVGL image cache so stale entries from Monitor do not
     * render with freed buffer addresses on the first Deck frame. */
    lv_img_cache_invalidate_src(NULL);
    ui_deck_preload_icons(&s_back_cfg);
    /* Ensure at least 1 second on the switching screen. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    lv_async_call(on_back_to_deck_done, NULL);
    vTaskDelete(NULL);
}

void ui_settings_monitor_reload(void)
{
    /* Same reasoning as item_mode_cb(): rebuilding pages (here, Monitor's
     * sidebar) unconditionally re-highlights its own page 0, so leave
     * Settings first or the gear stays selected alongside it. */
    ui_settings_deselect();
    ui_monitor_exit();
    ui_show_switching_screen("Applying config...");
    lv_refr_now(NULL);
    xTaskCreate(enter_monitor_task, "mon_reload", 4096, NULL, 3, NULL);
}

void ui_settings_media_reload(void)
{
    /* Same shape as ui_settings_monitor_reload() above -- Media's
     * exit/enter pair is already the lightweight kind (no page/button
     * rebuild), so reuse enter_media_task() as-is instead of a dedicated
     * preload task like Deck's config switch needs. */
    ui_settings_deselect();
    ui_media_exit();
    ui_show_switching_screen("Applying config...");
    lv_refr_now(NULL);
    xTaskCreate(enter_media_task, "media_reload", 4096, NULL, 3, NULL);
}

/* -----------------------------------------------------------------------
 * Item callbacks
 *
 * None of these hide the Settings page anymore -- they only open a popup
 * dialog on top of it (or, for item_mode_cb, actually leave the page).
 * When a dialog is dismissed, the still-selected/highlighted Settings
 * page underneath is exactly as the user left it.
 * ----------------------------------------------------------------------- */
static void item_msc_cb(lv_event_t *e)
{
    ui_msc_show_confirm_dialog();
}

static void item_config_cb(lv_event_t *e)
{
    if (s_mode == UI_MODE_MONITOR)
        ui_monitor_config_dialog_show();
    else if (s_mode == UI_MODE_MEDIA)
        ui_media_config_dialog_show();
    else
        ui_config_dialog_show();
}

static void item_keyboard_cb(lv_event_t *e)
{
    ui_keyboard_show();
}

static void item_mode_cb(lv_event_t *e)
{
    /* This one really does leave the Settings page (mode switch replaces
     * the whole page set), unlike the other items above. */
    ui_settings_deselect();

    if (s_mode == UI_MODE_DECK) {
        /* Deck -> Monitor */
        ui_deck_destroy();
        s_mode = UI_MODE_MONITOR;
        ui_show_switching_screen("Entering Monitor Mode...");
        xTaskCreate(enter_monitor_task, "enter_mon", 4096, NULL, 3, NULL);
    } else {
        /* Monitor -> Deck
         * Show the switching screen FIRST so LVGL renders a clean frame
         * before ui_monitor_exit() frees the background image buffer.
         * Without this, the last Monitor frame may render with a freed buffer. */
        s_mode = UI_MODE_DECK;

        bool cfg_ok = ui_deck_config_load(&s_back_cfg);
        if (!cfg_ok || s_back_cfg.page_count == 0) {
            s_back_cfg.page_count = 1;
            s_back_cfg.pages      = calloc(1, sizeof(page_cfg_t));
            snprintf(s_back_cfg.pages[0].name, UI_DECK_CONFIG_NAME_LEN, "Main");
            s_back_cfg.pages[0].button_count = 0;
            s_back_cfg.pages[0].buttons      = NULL;
        }

        ui_show_switching_screen("Entering Deck Mode...");
        lv_refr_now(NULL);   /* flush switching screen before freeing monitor buffers */
        ui_monitor_exit();
        xTaskCreate(back_to_deck_task, "back_deck", 8192, NULL, 3, NULL);
    }
}

/* -----------------------------------------------------------------------
 * Media mode transitions -- UI PROTOTYPE ONLY (see ui_media.h).
 *
 * Uses the same show-switching-screen + xTaskCreate pattern as
 * item_mode_cb() above (see enter_media_task()) so entering Media already
 * has room for background image / icon loading once that's real, instead
 * of needing this rewired later.
 * ----------------------------------------------------------------------- */
static void item_mode_to_media_cb(lv_event_t *e)
{
    ui_settings_deselect();

    if (s_mode == UI_MODE_DECK)
        ui_deck_destroy();
    else if (s_mode == UI_MODE_MONITOR)
        ui_monitor_exit();

    s_mode = UI_MODE_MEDIA;
    ui_show_switching_screen("Entering Media Mode...");
    xTaskCreate(enter_media_task, "enter_media", 4096, NULL, 3, NULL);
}

static void item_mode_from_media_cb(lv_event_t *e)
{
    ui_settings_deselect();
    ui_media_exit();
    s_mode = UI_MODE_DECK;

    bool cfg_ok = ui_deck_config_load(&s_back_cfg);
    if (!cfg_ok || s_back_cfg.page_count == 0) {
        s_back_cfg.page_count = 1;
        s_back_cfg.pages      = calloc(1, sizeof(page_cfg_t));
        snprintf(s_back_cfg.pages[0].name, UI_DECK_CONFIG_NAME_LEN, "Main");
        s_back_cfg.pages[0].button_count = 0;
        s_back_cfg.pages[0].buttons      = NULL;
    }

    ui_show_switching_screen("Entering Deck Mode...");
    lv_refr_now(NULL);
    xTaskCreate(back_to_deck_task, "back_deck", 8192, NULL, 3, NULL);
}

/* Media -> Monitor. Was left out originally (only Deck was wired as the
 * one way back out of Media, to keep the first pass small) -- added now
 * that it's clearly wanted. Reuses enter_monitor_task(), same as the
 * existing Deck -> Monitor path. */
static void item_mode_from_media_to_monitor_cb(lv_event_t *e)
{
    ui_settings_deselect();
    ui_media_exit();
    s_mode = UI_MODE_MONITOR;
    ui_show_switching_screen("Entering Monitor Mode...");
    xTaskCreate(enter_monitor_task, "enter_mon", 4096, NULL, 3, NULL);
}

static void info_dismiss_cb(lv_event_t *e)
{
    lv_obj_del(lv_event_get_target(e));
}

/* -----------------------------------------------------------------------
 * Hidden log viewer entry point -- press and hold the "ESDeck"/project
 * name label in the Info dialog for 5 seconds to open ui_log_view.c.
 * Deliberately not a normal LV_EVENT_LONG_PRESSED (that's a global
 * indev setting, shared by every clickable widget in the app, and its
 * default threshold is much shorter than the 5s wanted here) -- instead
 * a one-shot lv_timer is started on PRESSED and cancelled on RELEASED/
 * PRESS_LOST, so only this one label gets the long hold behavior.
 * ----------------------------------------------------------------------- */
static lv_timer_t *s_log_hold_timer = NULL;

static void info_log_hold_cb(lv_timer_t *t)
{
    lv_obj_t *info_root = (lv_obj_t *)t->user_data;
    s_log_hold_timer = NULL;   /* one-shot: LVGL deletes it after this call returns */

    lv_obj_del(info_root);     /* close the Info dialog underneath */
    ui_log_view_show();
}

static void info_name_press_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_obj_t *info_root = (lv_obj_t *)lv_event_get_user_data(e);

    if (code == LV_EVENT_PRESSED) {
        s_log_hold_timer = lv_timer_create(info_log_hold_cb, 5000, info_root);
        lv_timer_set_repeat_count(s_log_hold_timer, 1);
    } else {
        /* RELEASED or PRESS_LOST -- released early, cancel the hold */
        if (s_log_hold_timer) {
            lv_timer_del(s_log_hold_timer);
            s_log_hold_timer = NULL;
        }
    }
}

static void item_info_cb(lv_event_t *e)
{
    esp_app_desc_t desc;
    bool ok = (esp_ota_get_partition_description(esp_ota_get_running_partition(), &desc) == ESP_OK);

    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_set_size(root, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_60, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, info_dismiss_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *box = lv_obj_create(root);
    lv_obj_set_size(box, 420, 240);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1e1e1e), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 28, 0);
    lv_obj_set_layout(box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 16, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name_lbl = lv_label_create(box);
    lv_label_set_text(name_lbl, ok ? desc.project_name : "ESDeck");
    lv_obj_set_style_text_color(name_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(name_lbl, &lv_font_montserrat_24, 0);

    /* Hidden log viewer entry point -- see info_name_press_cb() above.
     * Labels aren't clickable by default, need it explicitly for PRESSED/
     * RELEASED to fire at all. user_data is this dialog's root so the
     * 5s-hold callback can close it before opening the log viewer. */
    lv_obj_add_flag(name_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(name_lbl, info_name_press_cb, LV_EVENT_PRESSED, root);
    lv_obj_add_event_cb(name_lbl, info_name_press_cb, LV_EVENT_RELEASED, root);
    lv_obj_add_event_cb(name_lbl, info_name_press_cb, LV_EVENT_PRESS_LOST, root);

    char ver_buf[64];
    char built_buf[64];
    if (ok) {
        snprintf(ver_buf, sizeof(ver_buf), "Version %s", desc.version);
        snprintf(built_buf, sizeof(built_buf), "Built %s %s", desc.date, desc.time);
    } else {
        snprintf(ver_buf, sizeof(ver_buf), "Version info unavailable");
        built_buf[0] = '\0';
    }

    lv_obj_t *ver_lbl = lv_label_create(box);
    lv_label_set_text(ver_lbl, ver_buf);
    lv_obj_set_style_text_color(ver_lbl, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_text_font(ver_lbl, &lv_font_montserrat_20, 0);

    if (built_buf[0] != '\0') {
        lv_obj_t *built_lbl = lv_label_create(box);
        lv_label_set_text(built_lbl, built_buf);
        lv_obj_set_style_text_color(built_lbl, lv_color_hex(0x888888), 0);
        lv_obj_set_style_text_font(built_lbl, &lv_font_montserrat_16, 0);
    }
}

/* -----------------------------------------------------------------------
 * Boot animation picker
 *
 * Each animation set lives in its own subfolder under SD_PATH_ASSETS_BOOT
 * (e.g. assets/boot/xmas/frame_0000.jpg...). The chosen folder name is
 * stored in NVS (CFG_NVS_KEY_BOOT_ANIM); "none" disables the custom
 * animation and falls back to the built-in one (see boot_anim.c).
 * ----------------------------------------------------------------------- */
#define BOOT_ANIM_LIST_MAX  12
#define BOOT_ANIM_NAME_LEN  32

static char s_boot_anim_names[BOOT_ANIM_LIST_MAX][BOOT_ANIM_NAME_LEN];
static int  s_boot_anim_count = 0;

static void scan_boot_anim_dirs(void)
{
    s_boot_anim_count = 0;

    DIR *dir = opendir(SD_PATH_ASSETS_BOOT);
    if (!dir) return;

    struct dirent *de;
    while ((de = readdir(dir)) != NULL && s_boot_anim_count < BOOT_ANIM_LIST_MAX) {
        if (de->d_type != DT_DIR) continue;
        if (de->d_name[0] == '.') continue;   /* skip "." / ".." */
        snprintf(s_boot_anim_names[s_boot_anim_count], BOOT_ANIM_NAME_LEN, "%.31s", de->d_name);
        s_boot_anim_count++;
    }
    closedir(dir);
}

static void boot_anim_pick_cb(lv_event_t *e)
{
    const char *value = (const char *)lv_event_get_user_data(e);
    nvs_manager_set_str(CFG_NVS_NAMESPACE, CFG_NVS_KEY_BOOT_ANIM, value);

    /* dialog hierarchy: root -> box -> this button */
    lv_obj_t *btn  = lv_event_get_target(e);
    lv_obj_t *box  = lv_obj_get_parent(btn);
    lv_obj_t *root = lv_obj_get_parent(box);
    lv_obj_del(root);
}

/* value must stay valid for the app's lifetime (string literal, or a
 * pointer into s_boot_anim_names[], both satisfy this -- no heap alloc). */
static void add_pick_item(lv_obj_t *box, const char *label,
                           const char *selected_name, const char *value)
{
    lv_obj_t *item = lv_btn_create(box);
    lv_obj_set_width(item, LV_PCT(100));
    bool is_selected = (strcmp(selected_name, value) == 0);
    lv_obj_set_style_bg_color(item, is_selected ? lv_color_hex(0x2a5a8a) : lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_bg_color(item, lv_color_hex(0x3a3a3a), LV_STATE_PRESSED);
    lv_obj_set_style_radius(item, 4, 0);
    lv_obj_clear_flag(item, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(item, boot_anim_pick_cb, LV_EVENT_CLICKED, (void *)value);

    lv_obj_t *lbl = lv_label_create(item);
    lv_label_set_text(lbl, label);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
}

static void item_boot_anim_cb(lv_event_t *e)
{
    scan_boot_anim_dirs();

    char selected[BOOT_ANIM_NAME_LEN];
    if (!nvs_manager_get_str(CFG_NVS_NAMESPACE, CFG_NVS_KEY_BOOT_ANIM, selected, sizeof(selected)) ||
        selected[0] == '\0') {
        snprintf(selected, sizeof(selected), "none");
    }

    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *root = lv_obj_create(scr);
    lv_obj_set_size(root, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_60, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, info_dismiss_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *box = lv_obj_create(root);
    lv_obj_set_size(box, 320, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(box, 380, 0);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1e1e1e), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 16, 0);
    lv_obj_set_layout(box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 8, 0);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "Boot Animation");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);

    add_pick_item(box, "None", selected, "none");
    for (int i = 0; i < s_boot_anim_count; i++) {
        add_pick_item(box, s_boot_anim_names[i], selected, s_boot_anim_names[i]);
    }
}

/* -----------------------------------------------------------------------
 * Settings menu tree definition
 *
 * Shared verbatim between Deck and Monitor mode. mode_mask decides
 * whether a node is shown while in that mode; nothing else about the
 * tree changes between modes. "Monitor Mode" / "Deck Mode" are two
 * separate nodes (rather than one node with a dynamically updated label)
 * purely for simplicity -- if a genuinely dynamic label is ever needed
 * elsewhere, add an optional label_fn to setting_node_t instead of
 * growing this pattern.
 * ----------------------------------------------------------------------- */
static const setting_node_t s_system_menu[] = {
    { "Select Config", SETTING_ACTION, SETMASK_ALL, item_config_cb, NULL, 0 },
    { "MSC Mode",       SETTING_ACTION, SETMASK_ALL,  item_msc_cb,    NULL, 0 },
    { "Info",            SETTING_ACTION, SETMASK_ALL,  item_info_cb,  NULL, 0 },
};

static const setting_node_t s_root_menu[] = {
    { "System",          SETTING_SUBMENU, SETMASK_ALL,    NULL,              s_system_menu,  3 },
    { "Boot Animation", SETTING_ACTION, SETMASK_ALL,     item_boot_anim_cb, NULL,           0 },
    { "Keyboard Mode", SETTING_ACTION, SETMASK_DECK,    item_keyboard_cb,  NULL,           0 },
    { "Monitor Mode",  SETTING_ACTION, SETMASK_DECK,    item_mode_cb,      NULL,           0 },
    { "Deck Mode",      SETTING_ACTION, SETMASK_MONITOR, item_mode_cb,      NULL,           0 },
    { "Media Mode",     SETTING_ACTION, SETMASK_BOTH,    item_mode_to_media_cb,   NULL,     0 },
    { "Monitor Mode",   SETTING_ACTION, SETMASK_MEDIA,   item_mode_from_media_to_monitor_cb, NULL, 0 },
    { "Deck Mode",      SETTING_ACTION, SETMASK_MEDIA,   item_mode_from_media_cb, NULL,     0 },
};

/* -----------------------------------------------------------------------
 * Menu navigation / rendering
 * ----------------------------------------------------------------------- */
static void render_current_level(void);

static void generic_item_cb(lv_event_t *e)
{
    const setting_node_t *node = (const setting_node_t *)lv_event_get_user_data(e);

    if (node->type == SETTING_SUBMENU) {
        if (s_stack_depth + 1 >= SETTINGS_STACK_MAX) return;
        s_stack_depth++;
        s_menu_stack[s_stack_depth]       = node->children;
        s_menu_stack_count[s_stack_depth] = node->child_count;
        s_menu_stack_label[s_stack_depth] = node->label;
        render_current_level();
    } else {
        if (node->action_cb) node->action_cb(e);
    }
}

/* Close Settings entirely and resume whatever mode is active -- the
 * counterpart to the deselect_current() call in ui_settings_select().
 * Called from the root-level back button (shown as an X, see
 * render_current_level()) so there is always a way out of Settings, even
 * for modes like Media that have no page list to tap instead. */
static void close_settings(void)
{
    ui_settings_deselect();

    if (s_mode == UI_MODE_DECK)
        ui_deck_reselect_current();
    else if (s_mode == UI_MODE_MONITOR)
        ui_monitor_reselect_current();
    else
        ui_media_reselect_current();
}

static void back_btn_cb(lv_event_t *e)
{
    if (s_stack_depth == 0) {
        close_settings();
        return;
    }
    s_stack_depth--;
    render_current_level();
}

static void render_current_level(void)
{
    lv_obj_clean(s_list);

    uint8_t mode_bit = (uint8_t)(1u << s_mode);

    const setting_node_t *arr   = s_menu_stack[s_stack_depth];
    int                   count = s_menu_stack_count[s_stack_depth];

    for (int i = 0; i < count; i++) {
        const setting_node_t *node = &arr[i];
        if (!(node->mode_mask & mode_bit)) continue;

        lv_obj_t *item = lv_btn_create(s_list);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_height(item, 48);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x2a2a2a), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x3a3a3a), LV_STATE_PRESSED);
        lv_obj_set_style_bg_grad_dir(item, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_shadow_width(item, 0, 0);
        lv_obj_set_style_outline_width(item, 0, 0);
        lv_obj_set_style_radius(item, 6, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_add_event_cb(item, generic_item_cb, LV_EVENT_CLICKED, (void *)node);

        lv_obj_t *lbl = lv_label_create(item);
        lv_label_set_text(lbl, node->label);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 12, 0);

        if (node->type == SETTING_SUBMENU) {
            lv_obj_t *chevron = lv_label_create(item);
            lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
            lv_obj_set_style_text_color(chevron, lv_color_hex(0x888888), 0);
            lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -12, 0);
        }
    }

    /* Root level: back_btn doubles as a close button (X) instead of being
     * hidden -- Media has no page list to pick from to leave Settings the
     * old way, so there needs to be an explicit close everywhere. */
    if (s_stack_depth == 0) {
        lv_label_set_text(s_breadcrumb_lbl, "Settings");
        lv_label_set_text(s_back_lbl, LV_SYMBOL_CLOSE);
    } else {
        lv_label_set_text(s_breadcrumb_lbl, s_menu_stack_label[s_stack_depth]);
        lv_label_set_text(s_back_lbl, LV_SYMBOL_LEFT);
    }
    lv_obj_clear_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
}

/* Standalone decode path -- used only when there's no lazy/LRU pool to
 * join for the current mode (Media today). Manual JPEG/PNG decode into
 * a caller-owned PSRAM buffer; caller is responsible for freeing it
 * later (heap_caps_free(out->data)). Returns false (and *out untouched)
 * on any failure. */
static bool settings_decode_standalone(const char *bg_path, lv_img_dsc_t *out)
{
    lv_img_decoder_dsc_t dec;
    memset(&dec, 0, sizeof(dec));
    if (lv_img_decoder_open(&dec, bg_path, lv_color_white(), 0) != LV_RES_OK) {
        ESP_LOGW("SETTINGS", "bg decode open failed: %s", bg_path);
        return false;
    }

    uint32_t    w  = dec.header.w;
    uint32_t    h  = dec.header.h;
    lv_img_cf_t cf = dec.header.cf;
    uint8_t     px = lv_img_cf_get_px_size(cf) / 8;
    if (px == 0) { cf = LV_IMG_CF_TRUE_COLOR; px = sizeof(lv_color_t); }   /* JPEG reports 0 */

    size_t   sz  = (size_t)w * h * px;
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    bool ok = (buf != NULL);
    if (ok) {
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
    }
    lv_img_decoder_close(&dec);

    if (!ok) {
        ESP_LOGE("SETTINGS", "bg decode failed: %s", bg_path);
        if (buf) heap_caps_free(buf);
        return false;
    }

    out->header.cf          = cf;
    out->header.always_zero = 0;
    out->header.reserved    = 0;
    out->header.w           = w;
    out->header.h           = h;
    out->data_size          = sz;
    out->data               = buf;

    ESP_LOGI("SETTINGS", "cached %s [%lux%lu %lu KB]", bg_path,
             (unsigned long)w, (unsigned long)h, (unsigned long)(sz / 1024));
    return true;
}

/* Lazily decode + apply the Settings page background the first time the
 * page is selected -- same "decode on first use, skip after" shape as
 * ui_deck_lazy_bg_set(). Which backing store it decodes into depends on
 * the active mode (see s_bg_dsc_ptr comment above). Inserted at child
 * index 0/1 of s_panel, same trick ui_deck.c uses to slide bg+mask behind
 * already-existing content (s_content here, btn_cont there). */
static void settings_lazy_bg_set(void)
{
    if (s_bg_applied || s_bg_image[0] == '\0') return;

    char bg_path[sizeof("S:") + sizeof(SD_PATH_ASSETS_BG) + 1 + UI_SETTINGS_BG_LEN];
    snprintf(bg_path, sizeof(bg_path), "S:%s/%s", SD_PATH_ASSETS_BG, s_bg_image);

    if (!s_bg_dsc_ptr) {
        if (s_mode == UI_MODE_DECK || s_mode == UI_MODE_MONITOR) {
            /* Both modes share the exact same pool now (Monitor's own
             * per-page backgrounds join it directly too, see
             * ui_monitor.c's monitor_lazy_bg_set()) -- one code path,
             * shares the same PSRAM budget and LRU eviction as whichever
             * mode's page backgrounds, in both directions (see
             * ui_img_pool.c's eviction match loop). */
            lv_img_dsc_t *dsc = ui_img_pool_decode(bg_path);
            if (dsc) {
                ui_img_pool_mark_bg(bg_path);
                s_bg_dsc_ptr  = dsc;
                s_bg_owns_buf = false;
            }
        } else {
            /* No lazy/LRU pool exists for this mode (Media today) --
             * fall back to a private buffer like before this existed. */
            if (settings_decode_standalone(bg_path, &s_bg_dsc)) {
                s_bg_dsc_ptr  = &s_bg_dsc;
                s_bg_owns_buf = true;
            }
        }

        if (!s_bg_dsc_ptr) {
            s_bg_applied = true;   /* don't retry every select */
            return;
        }
    }

    int page_w = SCREEN_W - SIDEBAR_W;
    int page_h = SCREEN_H;

    uint32_t zoom_x   = (uint32_t)page_w * 256 / s_bg_dsc_ptr->header.w;
    uint32_t zoom_y   = (uint32_t)page_h * 256 / s_bg_dsc_ptr->header.h;
    uint32_t zoom     = (zoom_x > zoom_y) ? zoom_x : zoom_y;
    int32_t  scaled_w = (int32_t)s_bg_dsc_ptr->header.w * (int32_t)zoom / 256;
    int32_t  scaled_h = (int32_t)s_bg_dsc_ptr->header.h * (int32_t)zoom / 256;
    int32_t  off_x    = ((int32_t)page_w - scaled_w) / 2;
    int32_t  off_y    = ((int32_t)page_h - scaled_h) / 2;

    lv_obj_t *bg = lv_img_create(s_panel);
    lv_obj_move_to_index(bg, 0);
    lv_img_set_src(bg, s_bg_dsc_ptr);
    lv_img_set_pivot(bg, 0, 0);
    lv_obj_set_pos(bg, off_x, off_y);
    lv_obj_set_size(bg, page_w, page_h);
    lv_img_set_zoom(bg, (uint16_t)zoom);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *mask = lv_obj_create(s_panel);
    lv_obj_move_to_index(mask, 1);
    lv_obj_set_size(mask, page_w, page_h);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_50, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_set_style_radius(mask, 0, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_bg_applied = true;
}

/* Called externally (extern, no header decl -- same convention as
 * ui_deck.c's ui_deck_lazy_bg_remove_widgets()) by ui_img_pool.c
 * when Settings' borrowed pool entry gets LRU-evicted, or when that whole
 * pool gets torn down on a mode exit (Deck or Monitor). The buffer
 * itself is already freed by the caller (whichever pool owned it) --
 * this only tears down the now-stale widgets and forgets the reference,
 * so the next select decodes fresh (and re-competes for a pool slot
 * then). Must never free anything: s_bg_owns_buf is always false
 * whenever this can fire, since the standalone case has no pool to be
 * evicted from. Safe to call even when Settings has nothing attached. */
void ui_settings_bg_widget_remove(void)
{
    if (!s_bg_dsc_ptr) return;

    lv_obj_t *bg = lv_obj_get_child(s_panel, 0);
    if (bg) lv_obj_del(bg);
    lv_obj_t *mask = lv_obj_get_child(s_panel, 0);   /* shifted into index 0 */
    if (mask) lv_obj_del(mask);

    s_bg_dsc_ptr  = NULL;
    s_bg_owns_buf = false;
    s_bg_applied  = false;   /* force re-decode next select */

    lv_img_cache_invalidate_src(NULL);
}

/* Exposes the full LVGL-FS path Settings' bg currently uses, so
 * ui_img_pool.c's LRU eviction can recognize when the pool entry
 * it's about to evict is actually Settings' own image (which isn't one
 * of Deck's or Monitor's own configured page backgrounds, so the usual
 * page-match loops won't find it). Writes "" if Settings has no bg
 * configured. */
void ui_settings_current_bg_path(char *out, size_t out_size)
{
    if (s_bg_image[0] == '\0') { out[0] = '\0'; return; }
    snprintf(out, out_size, "S:%s/%s", SD_PATH_ASSETS_BG, s_bg_image);
}

/* Drop the currently-attached bg + mask (if any) and free the decoded
 * buffer *if Settings owns it*, so the next settings_lazy_bg_set() call
 * starts clean for a different image. Only ever called while s_panel is
 * hidden (Settings is always deselected before a Deck/Monitor rebuild --
 * see ui_settings_apply_appearance() callers), so there's no visible
 * flicker from deleting live content. */
static void settings_bg_release(void)
{
    if (s_bg_dsc_ptr) {
        lv_obj_t *bg = lv_obj_get_child(s_panel, 0);
        if (bg) lv_obj_del(bg);
        lv_obj_t *mask = lv_obj_get_child(s_panel, 0);   /* shifted into index 0 */
        if (mask) lv_obj_del(mask);
    }
    if (s_bg_owns_buf && s_bg_dsc.data) {
        heap_caps_free((void *)s_bg_dsc.data);
        s_bg_dsc.data = NULL;
    }
    /* Pool-borrowed cases: don't free anything here and don't touch the
     * pool entry either -- that pool owns its own lifecycle (LRU
     * eviction, or a mode-exit free_all/pool_free that already calls
     * ui_settings_bg_widget_remove() itself). Just forget our reference. */
    s_bg_dsc_ptr  = NULL;
    s_bg_owns_buf = false;
    s_bg_applied  = false;

    /* s_bg_dsc lives at a fixed static address reused across config
     * switches -- LVGL's image cache keys decoded entries by that pointer,
     * not its contents, so a stale cache hit here would render the new
     * image's buffer against the old cached header/pixels (same tearing
     * bug ui_media.c's own bg teardown documents/handles for its own
     * static s_cover_dsc/s_info_dsc/bg buffers). */
    lv_img_cache_invalidate_src(NULL);
}

/* Update Settings' background image and gear sidebar icon to match the
 * currently active Deck/Monitor config. See ui_settings.h. */
void ui_settings_apply_appearance(const ui_settings_appearance_t *appearance)
{
    static const ui_settings_appearance_t empty = { 0 };
    if (!appearance) appearance = &empty;

    /* ---- gear sidebar icon ---- */
    if (strcmp(appearance->side_icon, s_side_icon) != 0) {
        if (s_gear_has_icon) {
            /* Custom icon is gear_btn's child 1 (child 0 is the glyph
             * label created once in ui_settings_build()). */
            lv_obj_t *old_img = lv_obj_get_child(s_gear_btn, 1);
            if (old_img) lv_obj_del(old_img);
            s_gear_has_icon = false;
        }

        snprintf(s_side_icon, sizeof(s_side_icon), "%s", appearance->side_icon);

        if (s_side_icon[0] != '\0') {
            char icon_path[sizeof("S:") + sizeof(SD_PATH_ASSETS_SIDE_ICON) + 1 + UI_SETTINGS_SIDE_ICON_LEN];
            snprintf(icon_path, sizeof(icon_path), "S:%s/%s", SD_PATH_ASSETS_SIDE_ICON, s_side_icon);

            FILE *f = fopen(icon_path + 2, "r");
            if (f) {
                fclose(f);
                if (s_gear_glyph) lv_obj_add_flag(s_gear_glyph, LV_OBJ_FLAG_HIDDEN);

                lv_obj_t *img = lv_img_create(s_gear_btn);
                lv_img_set_src(img, icon_path);
                lv_obj_center(img);
                lv_obj_set_style_clip_corner(s_gear_btn, true, 0);
                s_gear_has_icon = true;
            } else {
                ESP_LOGW("SETTINGS", "side_icon set but not found: %s (falling back to glyph)", icon_path);
            }
        }

        if (!s_gear_has_icon && s_gear_glyph)
            lv_obj_clear_flag(s_gear_glyph, LV_OBJ_FLAG_HIDDEN);
    }

    /* ---- background image ---- */
    if (strcmp(appearance->bg_image, s_bg_image) != 0) {
        settings_bg_release();
        snprintf(s_bg_image, sizeof(s_bg_image), "%s", appearance->bg_image);
        /* Actual decode stays lazy -- happens on next ui_settings_select()
         * via settings_lazy_bg_set(), same as before. */
    }
}

/* -----------------------------------------------------------------------
 * Public
 * ----------------------------------------------------------------------- */
lv_obj_t *ui_settings_build(lv_obj_t *scr, lv_obj_t *gear_btn)
{
    s_gear_btn   = gear_btn;
    s_gear_glyph = lv_obj_get_child(s_gear_btn, 0);   /* LV_SYMBOL_SETTINGS label, created by ui.c before this call */

    /* No appearance yet -- ui_deck_build()/ui_monitor_enter() apply the
     * active config's own "settings" object via ui_settings_apply_appearance()
     * right after this returns (see my_ui_init()), so this just starts
     * from the plain-glyph/no-bg default. */
    s_side_icon[0] = '\0';
    s_bg_image[0]  = '\0';

    /* Page shell: same size/position/base color as create_page() in
     * ui_deck.c, zero padding so a lazily-added bg image + mask (see
     * settings_lazy_bg_set()) can sit flush with the edges exactly like
     * a normal Deck/Monitor page. Content (header + list) lives in a
     * separate padded flex child below instead of being padded directly
     * on the shell -- same page/btn_cont split ui_deck.c uses. */
    s_panel = lv_obj_create(scr);
    lv_obj_set_size(s_panel, SCREEN_W - SIDEBAR_W, SCREEN_H);
    lv_obj_set_pos(s_panel, SIDEBAR_W, 0);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);

    s_content = lv_obj_create(s_panel);
    lv_obj_set_size(s_content, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(s_content, 0, 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_radius(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 16, 0);
    lv_obj_set_layout(s_content, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_content, 12, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_content, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* Header: back button + centered breadcrumb. The back button is always
     * visible once a level has been rendered -- it shows LV_SYMBOL_LEFT to
     * go up one menu level, or LV_SYMBOL_CLOSE at the root to close
     * Settings entirely (see render_current_level() / close_settings()). */
    lv_obj_t *header = lv_obj_create(s_content);
    lv_obj_set_size(header, LV_PCT(100), 40);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    s_back_btn = lv_btn_create(header);
    lv_obj_set_size(s_back_btn, 36, 36);
    lv_obj_align(s_back_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_back_btn, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_bg_opa(s_back_btn, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(s_back_btn, lv_color_hex(0x3a3a3a), LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_dir(s_back_btn, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_shadow_width(s_back_btn, 0, 0);
    lv_obj_set_style_outline_width(s_back_btn, 0, 0);
    lv_obj_set_style_radius(s_back_btn, 6, 0);
    lv_obj_clear_flag(s_back_btn, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(s_back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
    s_back_lbl = lv_label_create(s_back_btn);
    lv_label_set_text(s_back_lbl, LV_SYMBOL_LEFT);
    lv_obj_center(s_back_lbl);

    /* Centered on the page rather than balanced against the back button's
     * width -- simpler, and reads fine whether or not the back button is
     * showing. */
    s_breadcrumb_lbl = lv_label_create(header);
    lv_label_set_text(s_breadcrumb_lbl, "Settings");
    lv_obj_set_style_text_color(s_breadcrumb_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_breadcrumb_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(s_breadcrumb_lbl, LV_ALIGN_CENTER, 0, 0);

    /* Scrollable item list -- grows to fill remaining page height */
    s_list = lv_obj_create(s_content);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_layout(s_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_AUTO);

    s_menu_stack[0]       = s_root_menu;
    s_menu_stack_count[0] = (int)(sizeof(s_root_menu) / sizeof(s_root_menu[0]));
    s_stack_depth         = 0;

    lv_obj_set_style_outline_color(s_gear_btn, lv_color_hex(0x0055cc), 0);
    lv_obj_set_style_outline_width(s_gear_btn, 0, 0);

    return s_panel;
}

void ui_settings_select(void)
{
    if (s_mode == UI_MODE_DECK)
        ui_deck_deselect_current();
    else if (s_mode == UI_MODE_MONITOR)
        ui_monitor_deselect_current();
    else
        ui_media_deselect_current();

    s_stack_depth = 0;
    render_current_level();
    settings_lazy_bg_set();

    /* Icon buttons are covered edge-to-edge by the image, so a bg_color
     * swap would be invisible -- use an outline ring instead, same as
     * Deck/Monitor's own side_icon page buttons. */
    if (s_gear_has_icon) {
        lv_obj_set_style_outline_width(s_gear_btn, 3, 0);
    } else {
        lv_obj_set_style_bg_color(s_gear_btn, lv_color_hex(0x0055cc), 0);
    }

    lv_obj_move_foreground(s_panel);
    lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
}

void ui_settings_deselect(void)
{
    if (lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN)) return;

    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_gear_btn, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_outline_width(s_gear_btn, 0, 0);
    s_stack_depth = 0;
}
