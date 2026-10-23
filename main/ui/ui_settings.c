#include "ui_settings.h"
#include "ui_msc.h"
#include "ui_config_dialog.h"
#include "ui_monitor_config.h"
#include "ui_monitor.h"
#include "ui_deck.h"
#include "ui_media.h"
#include "ui_touchpad.h"
#include "ui_canvas.h"
#include "ui_keyboard.h"
#include "ui_img_pool.h"
#include "ui_log_view.h"
#include "usb/usb_hid.h"
#include "ui_toast.h"
#include "ui.h"
#include "ble/ble_manager.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "app_config.h"
#include "nvs_manager/nvs_manager.h"
#include "lvgl_port.h"   /* screen rotation */
#include "esp_system.h"  /* esp_restart() */
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
    SETMASK_MEDIA    = 1 << 2,
    SETMASK_TOUCHPAD = 1 << 3,
    SETMASK_CANVAS   = 1 << 4,
    SETMASK_BOTH     = SETMASK_DECK | SETMASK_MONITOR,
    SETMASK_ALL      = SETMASK_DECK | SETMASK_MONITOR | SETMASK_MEDIA | SETMASK_TOUCHPAD |
                       SETMASK_CANVAS,
} setting_mask_t;

typedef enum {
    SETTING_ACTION,
    SETTING_SUBMENU,
    SETTING_TOGGLE,
    SETTING_LABEL,    /* informational row, not clickable -- see render_current_level() */
    SETTING_VALUE,    /* "[-] value [+]" stepper -- see render_current_level() */
    SETTING_CHOICE,   /* row shows current choice; tap = popup list, pick = close */
} setting_node_type_t;

typedef struct setting_node_s {
    const char                  *label;
    setting_node_type_t          type;
    uint8_t                       mode_mask;
    lv_event_cb_t                 action_cb;   /* used when type == SETTING_ACTION */
    const struct setting_node_s  *children;    /* used when type == SETTING_SUBMENU */
    int                            child_count;
    bool                        (*get_state_cb)(void);   /* used when type == SETTING_TOGGLE */
    void                        (*set_state_cb)(bool on); /* used when type == SETTING_TOGGLE */
    const char                 *(*value_text_cb)(void);   /* used when type == SETTING_VALUE */
    void                        (*value_step_cb)(int dir);/* used when type == SETTING_VALUE: -1 / +1 */
    const char *const          *(*choice_names_cb)(void); /* used when type == SETTING_CHOICE */
    int                           choice_count;
    int                         (*choice_get_cb)(void);
    void                        (*choice_set_cb)(int idx);
    bool                        (*locked_cb)(void);      /* SETTING_TOGGLE, optional: true = shown on and disabled */
    bool                        (*visible_cb)(void);     /* optional, any type: false = row hidden (on top of mode_mask) */
} setting_node_t;

/* Element count of a static array (menu tables, choice name lists). */
#define COUNT_OF(a) ((int)(sizeof(a) / sizeof((a)[0])))

#define SETTINGS_STACK_MAX 4

/* Current UI mode */
static ui_mode_t  s_mode = UI_MODE_DECK;

/* Settings page widgets */
static lv_obj_t *s_panel          = NULL;  /* the Settings "page" shell (right of sidebar, zero padding -- bg/mask attach directly to this, like ui_deck.c's page/btn_cont split) */
static lv_obj_t *s_content        = NULL;  /* padded flex column inside s_panel, holds header + s_list */
static lv_obj_t *s_list           = NULL;  /* item list inside the page (paged, see page_btn_cb) */
static lv_obj_t *s_pg_up_btn      = NULL;  /* header: previous / next page of s_list */
static lv_obj_t *s_pg_dn_btn      = NULL;
static lv_obj_t *s_close_btn      = NULL;  /* header, far left: always closes Settings */
static lv_obj_t *s_back_btn       = NULL;  /* header, next to it: up one menu level, hidden at the root */
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
static bool          s_bg_ext      = false; /* bg came from ui_settings_set_external_appearance(): caller's buffer, never decoded / freed here */
static bool          s_gear_ext    = false; /* gear icon came from ui_settings_set_external_appearance() */

/* Navigation stack -- index 0 is always the root menu. */
static const setting_node_t *s_menu_stack[SETTINGS_STACK_MAX];
static int                   s_menu_stack_count[SETTINGS_STACK_MAX];
static const char           *s_menu_stack_label[SETTINGS_STACK_MAX];
static int                   s_stack_depth = 0;

/* Deck config handed from the load (mode switch or config reload) to the
 * icon preload task and then to ui_deck_build(), which takes ownership. */
static deck_cfg_t s_deck_cfg;

/* -----------------------------------------------------------------------
 * Accessors
 * ----------------------------------------------------------------------- */
ui_mode_t ui_settings_get_mode(void)
{
    return s_mode;
}

/* -----------------------------------------------------------------------
 * Boot Mode
 * ----------------------------------------------------------------------- */
#define BOOT_MODE_AUTO 0

static int nvs_get_small_int(const char *key, int def)
{
    char v[8] = "";
    if (!nvs_manager_get_str(CFG_NVS_NAMESPACE, key, v, sizeof(v)) || !v[0]) return def;
    return atoi(v);
}

static void nvs_set_small_int(const char *key, int val)
{
    char v[8];
    snprintf(v, sizeof(v), "%d", val);
    if (!nvs_manager_set_str(CFG_NVS_NAMESPACE, key, v))
        ESP_LOGW("SETTINGS", "NVS save failed: %s", key);
}

static bool valid_mode(int m) { return m >= UI_MODE_DECK && m <= UI_MODE_CANVAS; }

int ui_settings_get_boot_mode_choice(void)
{
    int c = nvs_get_small_int(CFG_NVS_KEY_BOOT_MODE, BOOT_MODE_AUTO);
    return (c >= 0 && c < UI_BOOT_MODE_CHOICES) ? c : BOOT_MODE_AUTO;
}

void ui_settings_set_boot_mode_choice(int idx)
{
    if (idx < 0 || idx >= UI_BOOT_MODE_CHOICES) return;
    nvs_set_small_int(CFG_NVS_KEY_BOOT_MODE, idx);
    /* Switching to Auto: remember the mode we're in right now, so the next
     * boot already lands here even without another mode switch. */
    if (idx == BOOT_MODE_AUTO) nvs_set_small_int(CFG_NVS_KEY_LAST_MODE, s_mode);
}

static const char *const s_boot_mode_names[UI_BOOT_MODE_CHOICES] = {
    "Auto (last used)", "Deck", "Monitor", "Media", "Touchpad", "Canvas",
};

const char *const *ui_settings_boot_mode_names(void) { return s_boot_mode_names; }

ui_mode_t ui_settings_boot_mode(void)
{
    int c = ui_settings_get_boot_mode_choice();
    int m = (c == BOOT_MODE_AUTO) ? nvs_get_small_int(CFG_NVS_KEY_LAST_MODE, UI_MODE_DECK)
                                  : c - 1;
    return valid_mode(m) ? (ui_mode_t)m : UI_MODE_DECK;
}

void ui_settings_init_mode(ui_mode_t mode)
{
    s_mode = mode;
}

/* -----------------------------------------------------------------------
 * Enter tasks -- one per mode, used by both switch_to_mode() and the
 * ui_settings_*_reload() functions. Each runs after the switching screen
 * is up; Monitor/Media/Touchpad have nothing heavy to preload, the delay
 * just keeps the switching screen visible for a beat. Deck preloads its
 * icons (see enter_deck_task()).
 * ----------------------------------------------------------------------- */
static void on_enter_deck_done(void *arg)
{
    ui_deck_build(ui_get_sidebar(), &s_deck_cfg);
    ui_hide_switching_screen();
}

static void enter_deck_task(void *arg)
{
    /* Invalidate LVGL image cache so stale entries from the previous
     * mode/config don't render with freed buffer addresses on the first
     * Deck frame. */
    lv_img_cache_invalidate_src(NULL);
    ui_deck_preload_icons(&s_deck_cfg);
    /* Ensure at least 1 second on the switching screen. */
    vTaskDelay(pdMS_TO_TICKS(1000));
    lv_async_call(on_enter_deck_done, NULL);
    vTaskDelete(NULL);
}

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

static void on_enter_touchpad_done(void *arg)
{
    ui_touchpad_enter(ui_get_sidebar());
    ui_hide_switching_screen();
}

static void enter_touchpad_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    lv_async_call(on_enter_touchpad_done, NULL);
    vTaskDelete(NULL);
}

/* Canvas: build right away (it asks ESDeckPC for the page), then keep the
 * switching screen up until the first page has arrived -- the user never
 * sees an empty or half-drawn canvas. Gives up after a few seconds (no
 * ESDeckPC): the mode then shows "Waiting for ESDeckPC...". */
static volatile bool s_canvas_built = false;

static void on_enter_canvas_build(void *arg)
{
    ui_canvas_enter(ui_get_sidebar());
    s_canvas_built = true;
}

static void on_enter_canvas_done(void *arg)
{
    ui_hide_switching_screen();
}

static void enter_canvas_task(void *arg)
{
    s_canvas_built = false;
    lv_async_call(on_enter_canvas_build, NULL);
    for (int i = 0; i < 100 && !s_canvas_built; i++) vTaskDelay(pdMS_TO_TICKS(20));
    vTaskDelay(pdMS_TO_TICKS(300));
    ui_canvas_wait_first_frame(5000);
    lv_async_call(on_enter_canvas_done, NULL);
    vTaskDelete(NULL);
}

/* Load the selected Deck config (an empty "Main" page if there is none or
 * it fails to load), show the switching screen and build the deck in the
 * background. The previous deck/mode must already be torn down. Shared by
 * switch_to_mode() and ui_settings_deck_reload(). */
static void start_enter_deck(const char *msg)
{
    if (!ui_deck_config_load(&s_deck_cfg) || s_deck_cfg.page_count == 0) {
        s_deck_cfg.page_count = 1;
        s_deck_cfg.pages      = calloc(1, sizeof(page_cfg_t));
        snprintf(s_deck_cfg.pages[0].name, UI_DECK_CONFIG_NAME_LEN, "Main");
        s_deck_cfg.pages[0].button_count = 0;
        s_deck_cfg.pages[0].buttons      = NULL;
    }
    ui_show_switching_screen(msg);
    lv_refr_now(NULL);
    xTaskCreate(enter_deck_task, "enter_deck", 8192, NULL, 3, NULL);
}

void ui_settings_deck_reload(void)
{
    /* Same reasoning as switch_to_mode(): the rebuilt deck highlights its
     * own page 0, so leave Settings first or the gear stays selected. */
    ui_settings_deselect();
    ui_deck_destroy();
    start_enter_deck("Applying config...");
}

void ui_settings_monitor_reload(void)
{
    /* Same reasoning as switch_to_mode(): rebuilding pages (here, Monitor's
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

void ui_settings_touchpad_reload(void)
{
    /* Same shape as ui_settings_media_reload(). */
    ui_settings_deselect();
    ui_touchpad_exit();
    ui_show_switching_screen("Applying config...");
    lv_refr_now(NULL);
    xTaskCreate(enter_touchpad_task, "tpad_reload", 4096, NULL, 3, NULL);
}

/* -----------------------------------------------------------------------
 * Item callbacks
 *
 * None of these hide the Settings page anymore -- they only open a popup
 * dialog on top of it (or, for switch_to_mode(), actually leave the page).
 * When a dialog is dismissed, the still-selected/highlighted Settings
 * page underneath is exactly as the user left it.
 * ----------------------------------------------------------------------- */
static void item_msc_cb(lv_event_t *e)
{
    ui_msc_show_confirm_dialog();
}

#if ESDECK_ENABLE_BLE
/* Never actually invoked as a click handler -- SETTING_LABEL rows aren't
 * wired to generic_item_cb at all (see render_current_level()). This only
 * exists so the Bluetooth connection-status row can be identified the same
 * way item_hid_status_cb's row is: by comparing action_cb's identity in
 * render_current_level()'s label-formatting special case. */
static void item_bt_conn_status_marker(lv_event_t *e) { (void)e; }
#endif

static void item_config_cb(lv_event_t *e)
{
    if (s_mode == UI_MODE_MONITOR)
        ui_monitor_config_dialog_show();
    else if (s_mode == UI_MODE_MEDIA)
        ui_media_config_dialog_show();
    else if (s_mode == UI_MODE_TOUCHPAD)
        ui_touchpad_config_dialog_show();
    else if (s_mode == UI_MODE_CANVAS)
        ui_canvas_config_dialog_show();
    else
        ui_deck_config_dialog_show();
}

/* -----------------------------------------------------------------------
 * Mode switching -- one function, any mode to any mode.
 *
 * Replaces the old per-pair callbacks (item_mode_cb /
 * item_mode_to_media_cb / item_mode_from_media_cb /
 * item_mode_from_media_to_monitor_cb, see git history) that accreted when
 * Media was bolted onto an originally Deck<->Monitor-only design. Teardown
 * only depends on the mode being left; setup only depends on the mode
 * being entered -- so N modes need N teardowns + N setups, not N*N
 * transition callbacks.
 *
 * The old Monitor -> Deck path had an extra show-screen-then-lv_refr_now
 * step BEFORE ui_monitor_exit(), with a comment about LVGL rendering a
 * frame against a freed background buffer. That hazard lived inside
 * ui_monitor_exit() itself and has since been fixed there (it deletes
 * every LVGL object before ui_img_pool_free() -- see its own comments), so
 * exit-before-show is safe now; ui_settings_monitor_reload() has always
 * called ui_monitor_exit() first the same way. The lv_refr_now() after
 * ui_show_switching_screen() is kept (for every transition, not just
 * Deck's) so the switching screen is on-glass before any heavy setup work
 * starts, rather than a UAF guard.
 * ----------------------------------------------------------------------- */
static void switch_to_mode(ui_mode_t target)
{
    if (target == s_mode) return;

    /* This really does leave the Settings page (mode switch replaces the
     * whole page set), unlike the dialog-opening items above. */
    ui_settings_deselect();

    /* Teardown -- depends only on the mode being left. */
    switch (s_mode) {
    case UI_MODE_DECK:    ui_deck_destroy(); break;
    case UI_MODE_MONITOR: ui_monitor_exit(); break;
    case UI_MODE_MEDIA:    ui_media_exit();    break;
    case UI_MODE_TOUCHPAD: ui_touchpad_exit(); break;
    case UI_MODE_CANVAS:   ui_canvas_exit();   break;
    }

    s_mode = target;

    /* Boot Mode = Auto remembers the last mode -- one NVS write per switch,
     * only while Auto is selected. */
    if (ui_settings_get_boot_mode_choice() == BOOT_MODE_AUTO)
        nvs_set_small_int(CFG_NVS_KEY_LAST_MODE, target);

    /* Setup -- depends only on the mode being entered. */
    switch (target) {
    case UI_MODE_DECK:
        start_enter_deck("Entering Deck Mode...");
        break;

    case UI_MODE_MONITOR:
        ui_show_switching_screen("Entering Monitor Mode...");
        lv_refr_now(NULL);
        xTaskCreate(enter_monitor_task, "enter_mon", 4096, NULL, 3, NULL);
        break;

    case UI_MODE_MEDIA:
        ui_show_switching_screen("Entering Media Mode...");
        lv_refr_now(NULL);
        xTaskCreate(enter_media_task, "enter_media", 4096, NULL, 3, NULL);
        break;

    case UI_MODE_TOUCHPAD:
        ui_show_switching_screen("Entering Touchpad Mode...");
        lv_refr_now(NULL);
        xTaskCreate(enter_touchpad_task, "enter_tpad", 4096, NULL, 3, NULL);
        break;

    case UI_MODE_CANVAS:
        ui_show_switching_screen("Entering Canvas Mode...");
        lv_refr_now(NULL);
        xTaskCreate(enter_canvas_task, "enter_canvas", 4096, NULL, 3, NULL);
        break;
    }
}

static void item_mode_to_deck_cb(lv_event_t *e)    { (void)e; switch_to_mode(UI_MODE_DECK); }
static void item_mode_to_monitor_cb(lv_event_t *e) { (void)e; switch_to_mode(UI_MODE_MONITOR); }
static void item_mode_to_media_cb(lv_event_t *e)   { (void)e; switch_to_mode(UI_MODE_MEDIA); }
static void item_mode_to_touchpad_cb(lv_event_t *e) { (void)e; switch_to_mode(UI_MODE_TOUCHPAD); }
static void item_mode_to_canvas_cb(lv_event_t *e)   { (void)e; switch_to_mode(UI_MODE_CANVAS); }

/* Long press on the gear: pick a mode directly, without going through
 * Settings > Mode. Reuses the SETTING_CHOICE popup with a node of its own;
 * picking the current mode just closes it (switch_to_mode() no-op). */
static void choice_open(const setting_node_t *node);   /* SETTING_CHOICE popup, defined below */

static const char *const s_mode_pick_names[] = { "Deck", "Monitor", "Media", "Touchpad", "Canvas" };
static const char *const *mode_pick_names(void) { return s_mode_pick_names; }
static int  mode_pick_get(void)    { return (int)s_mode; }
static void mode_pick_set(int idx) { if (valid_mode(idx)) switch_to_mode((ui_mode_t)idx); }

static const setting_node_t s_mode_pick_node = {
    "Switch Mode", SETTING_CHOICE, SETMASK_ALL, NULL, NULL, 0, NULL, NULL, NULL, NULL,
    mode_pick_names, COUNT_OF(s_mode_pick_names),
    mode_pick_get, mode_pick_set,
};

void ui_settings_mode_picker_open(void)
{
    choice_open(&s_mode_pick_node);
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
 * HID Status row (System submenu) -- toggles the real USB state instead
 * of a single "reconnect" action: tap while Connected calls
 * usb_hid_force_disconnect() and stays disconnected (lets you manually
 * verify the PC side notices), tap again while Disconnected calls
 * usb_hid_force_connect(). The row's label/color is redrawn via
 * hid_conn_cb() -> render_current_level() whenever usb_hid.c's tracked
 * connection state changes (see the s_conn_state comment in usb_hid.c for
 * why it's tracked in software instead of read from tud_mounted()).
 * Disconnect updates that state immediately (nothing to wait for), so its
 * toast/log/label all land in the same frame as the tap. Connect only
 * re-enables the pull-up -- the label stays Disconnected until the real
 * TINYUSB_EVENT_ATTACHED lands (host actually finishes re-enumerating,
 * ~1-3s later), so the toast below is deliberately worded as pending. */
static void item_hid_status_cb(lv_event_t *e)
{
    (void)e;
    if (usb_hid_is_connected()) {
        ESP_LOGI("SETTINGS", "HID Status: user forced disconnect");
        usb_hid_force_disconnect();
        ui_toast_push("HID: Disconnected", 1, NULL, NULL);
    } else {
        ESP_LOGI("SETTINGS", "HID Status: user forced connect");
        usb_hid_force_connect();
        ui_toast_push("HID: Connecting...", 1, NULL, NULL);
    }
}

/* -----------------------------------------------------------------------
 * Boot animation picker
 *
 * Each animation set lives in its own subfolder under SD_PATH_ASSETS_BOOT
 * (e.g. assets/boot/xmas/frame_0000.jpg...). The chosen folder name is
 * stored in NVS (CFG_NVS_KEY_BOOT_ANIM); "default" (or unset) plays the
 * built-in animation, "none" plays nothing (see boot_anim.c).
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
        snprintf(selected, sizeof(selected), "default");
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
    add_pick_item(box, "Default", selected, "default");
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
/* Touchpad-only rows live in Custom too; mode_mask hides them elsewhere.
 *
 * Custom > Touchpad: Input Mode first -- it decides what the rest does.
 * With "Windows Touchpad" (PTP) Windows does pointer / taps / gestures, so
 * Pointer & Gestures is swapped for a pointer to Windows' own settings,
 * and Scrolling only stays while the sidebar wheel (which still uses it)
 * is shown. All persisted in NVS by ui_touchpad.c. */
static bool tp_not_ptp(void)  { return ui_touchpad_get_input_mode() != TP_INPUT_PTP; }
static bool tp_is_ptp(void)   { return ui_touchpad_get_input_mode() == TP_INPUT_PTP; }
static bool tp_scroll_used(void)
{
    return tp_not_ptp() || ui_touchpad_get_sidebar_mode() == TP_SIDEBAR_WHEEL;
}

static const setting_node_t s_tp_gesture_menu[] = {
    { "Cursor Speed",   SETTING_VALUE,  SETMASK_TOUCHPAD, NULL, NULL, 0, NULL, NULL,
      ui_touchpad_cursor_speed_text, ui_touchpad_cursor_speed_step },
    { "Tap to Click",   SETTING_TOGGLE, SETMASK_TOUCHPAD, NULL, NULL, 0,
      ui_touchpad_get_tap_click, ui_touchpad_set_tap_click,
      NULL, NULL, NULL, 0, NULL, NULL, ui_touchpad_tap_click_forced },
    { "2-Finger Swipe", SETTING_CHOICE, SETMASK_TOUCHPAD, NULL, NULL, 0, NULL, NULL, NULL, NULL,
      ui_touchpad_swipe_mode_names, TP_SWIPE_COUNT,
      ui_touchpad_get_swipe_mode, ui_touchpad_set_swipe_mode },
    { "3-Finger Swipe", SETTING_CHOICE, SETMASK_TOUCHPAD, NULL, NULL, 0, NULL, NULL, NULL, NULL,
      ui_touchpad_swipe3_mode_names, TP_SWIPE3_COUNT,
      ui_touchpad_get_swipe3_mode, ui_touchpad_set_swipe3_mode },
    { "Pinch to Zoom",  SETTING_TOGGLE, SETMASK_TOUCHPAD, NULL, NULL, 0,
      ui_touchpad_get_pinch_zoom, ui_touchpad_set_pinch_zoom },
};

/* Two-finger scrolling on the pad and the sidebar wheel strip. */
static const setting_node_t s_tp_scroll_menu[] = {
    { "Scroll Speed",   SETTING_VALUE,  SETMASK_TOUCHPAD, NULL, NULL, 0, NULL, NULL,
      ui_touchpad_scroll_speed_text, ui_touchpad_scroll_speed_step },
    { "Natural Scroll", SETTING_TOGGLE, SETMASK_TOUCHPAD, NULL, NULL, 0,
      ui_touchpad_get_natural_scroll, ui_touchpad_set_natural_scroll },
    { "Scroll Inertia", SETTING_TOGGLE, SETMASK_TOUCHPAD, NULL, NULL, 0,
      ui_touchpad_get_inertia, ui_touchpad_set_inertia },
};

static const setting_node_t s_tp_layout_menu[] = {
    { "Sidebar",        SETTING_CHOICE, SETMASK_TOUCHPAD, NULL, NULL, 0, NULL, NULL, NULL, NULL,
      ui_touchpad_sidebar_mode_names, TP_SIDEBAR_COUNT,
      ui_touchpad_get_sidebar_mode, ui_touchpad_set_sidebar_mode },
    { "Hide Buttons",   SETTING_TOGGLE, SETMASK_TOUCHPAD, NULL, NULL, 0,
      ui_touchpad_get_hide_buttons, ui_touchpad_set_hide_buttons },
    { "Top Left Button",  SETTING_CHOICE, SETMASK_TOUCHPAD, NULL, NULL, 0, NULL, NULL, NULL, NULL,
      ui_touchpad_btn_func_names, TP_BTNF_COUNT,
      ui_touchpad_get_btn_left_func, ui_touchpad_set_btn_left_func },
    { "Top Right Button", SETTING_CHOICE, SETMASK_TOUCHPAD, NULL, NULL, 0, NULL, NULL, NULL, NULL,
      ui_touchpad_btn_func_names, TP_BTNF_COUNT,
      ui_touchpad_get_btn_right_func, ui_touchpad_set_btn_right_func },
    { "Hide Labels",    SETTING_TOGGLE, SETMASK_TOUCHPAD, NULL, NULL, 0,
      ui_touchpad_get_hide_labels, ui_touchpad_set_hide_labels },
};

static const setting_node_t s_touchpad_menu[] = {
    { "Input Mode",     SETTING_CHOICE, SETMASK_TOUCHPAD, NULL, NULL, 0, NULL, NULL, NULL, NULL,
      ui_touchpad_input_mode_names, TP_INPUT_COUNT,
      ui_touchpad_get_input_mode, ui_touchpad_set_input_mode },
    { .label = "Pointer & Gestures", .type = SETTING_SUBMENU, .mode_mask = SETMASK_TOUCHPAD,
      .children = s_tp_gesture_menu, .child_count = COUNT_OF(s_tp_gesture_menu),
      .visible_cb = tp_not_ptp },
    { .label = "Gestures: Windows Settings", .type = SETTING_LABEL, .mode_mask = SETMASK_TOUCHPAD,
      .visible_cb = tp_is_ptp },
    { .label = "Scrolling", .type = SETTING_SUBMENU, .mode_mask = SETMASK_TOUCHPAD,
      .children = s_tp_scroll_menu, .child_count = COUNT_OF(s_tp_scroll_menu),
      .visible_cb = tp_scroll_used },
    { .label = "Layout", .type = SETTING_SUBMENU, .mode_mask = SETMASK_TOUCHPAD,
      .children = s_tp_layout_menu, .child_count = COUNT_OF(s_tp_layout_menu) },
};

/* Mini keyboard (Touchpad mode's sidebar keyboard button). */
static const setting_node_t s_keyboard_menu[] = {
    { "Show Zhuyin",    SETTING_TOGGLE, SETMASK_TOUCHPAD, NULL, NULL, 0,
      ui_keyboard_get_zhuyin, ui_keyboard_set_zhuyin },
};

static const setting_node_t s_custom_menu[] = {
    /* Canvas: the config lives on the PC, picked in ESDeckPC. */
    { "Select Config",  SETTING_ACTION, SETMASK_ALL, item_config_cb, NULL, 0 },
    { "Boot Mode",      SETTING_CHOICE, SETMASK_ALL, NULL, NULL, 0, NULL, NULL, NULL, NULL,
      ui_settings_boot_mode_names, UI_BOOT_MODE_CHOICES,
      ui_settings_get_boot_mode_choice, ui_settings_set_boot_mode_choice },
    { "Boot Animation", SETTING_ACTION, SETMASK_ALL, item_boot_anim_cb, NULL, 0 },
    { "Touchpad",       SETTING_SUBMENU, SETMASK_TOUCHPAD, NULL, s_touchpad_menu,
      COUNT_OF(s_touchpad_menu) },
    { "Keyboard",       SETTING_SUBMENU, SETMASK_TOUCHPAD, NULL, s_keyboard_menu,
      COUNT_OF(s_keyboard_menu) },
};

/* -----------------------------------------------------------------------
 * Screen Rotation (System) -- 0 / 180 degrees, for when the cable has to
 * come out the other side. Applied at boot (lvgl_port.h), so picking the
 * other one asks to restart; the row always shows the rotation in effect.
 * ----------------------------------------------------------------------- */
static const char *const s_rot_names[] = { "Normal", "Upside down" };
static const char *const *rot_names(void) { return s_rot_names; }
static int  rot_get(void) { return lvgl_port_rotation_180() ? 1 : 0; }

static lv_obj_t *s_rot_dlg = NULL;

static void rot_restart_timer_cb(lv_timer_t *t)
{
    (void)t;
    esp_restart();
}

static void rot_confirm_cb(lv_event_t *e)
{
    bool want = (bool)(intptr_t)lv_event_get_user_data(e);
    if (!lvgl_port_rotation_save(want)) {
        ESP_LOGE("SETTINGS", "saving screen rotation failed");
        if (s_rot_dlg) { lv_obj_del(s_rot_dlg); s_rot_dlg = NULL; }
        return;
    }
    /* Message on a plain black screen, then restart once it's drawn.
     * The box (this button's parent) is only hidden -- not deleted from
     * inside its own button's event. */
    lv_obj_add_flag(lv_obj_get_parent(lv_event_get_target(e)), LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(s_rot_dlg, LV_OPA_COVER, 0);
    lv_obj_t *lbl = lv_label_create(s_rot_dlg);
    lv_label_set_text(lbl, "Restarting...");
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
    lv_obj_center(lbl);
    lv_timer_t *t = lv_timer_create(rot_restart_timer_cb, 300, NULL);
    lv_timer_set_repeat_count(t, 1);
}

static void rot_cancel_cb(lv_event_t *e)
{
    (void)e;
    if (s_rot_dlg) { lv_obj_del(s_rot_dlg); s_rot_dlg = NULL; }
}

static lv_obj_t *rot_btn(lv_obj_t *parent, const char *text, uint32_t bg, lv_event_cb_t cb, void *ud)
{
    lv_obj_t *b = lv_btn_create(parent);
    lv_obj_set_size(b, 120, 40);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_color(l, lv_color_hex(0xffffff), 0);
    lv_obj_center(l);
    return b;
}

/* lv_async_call target: runs after the choice popup closed and the
 * settings list re-rendered, so the dialog ends up on top. */
static void rot_confirm_open(void *arg)
{
    bool want = (bool)(intptr_t)arg;
    if (s_rot_dlg) return;

    s_rot_dlg = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_rot_dlg, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_rot_dlg, 0, 0);
    lv_obj_set_style_bg_color(s_rot_dlg, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_rot_dlg, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_rot_dlg, 0, 0);
    lv_obj_set_style_radius(s_rot_dlg, 0, 0);
    lv_obj_clear_flag(s_rot_dlg, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(s_rot_dlg);
    lv_obj_set_size(box, 460, 200);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 24, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, want ? "Turn Screen Upside Down" : "Turn Screen Back to Normal");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_pos(title, 0, 0);

    lv_obj_t *body = lv_label_create(box);
    lv_label_set_text(body, "The device restarts to apply this.\nESDeckPC reconnects by itself.");
    lv_obj_set_style_text_color(body, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(body, 0, 36);

    lv_obj_t *cancel = rot_btn(box, "Cancel", 0x3a3a3a, rot_cancel_cb, NULL);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_RIGHT, -132, 0);
    lv_obj_t *ok = rot_btn(box, "Restart", 0x0055cc, rot_confirm_cb, (void *)(intptr_t)want);
    lv_obj_align(ok, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

static void rot_set(int idx)
{
    bool want = (idx == 1);
    if (want == lvgl_port_rotation_180()) return;
    lv_async_call(rot_confirm_open, (void *)(intptr_t)want);
}

static const setting_node_t s_system_menu[] = {
    { "HID Status", SETTING_ACTION, SETMASK_ALL, item_hid_status_cb, NULL, 0 },
    { "Toasts",     SETTING_TOGGLE, SETMASK_ALL, NULL, NULL, 0,
      ui_toast_get_enabled, ui_toast_set_enabled },
    { "Screen Rotation", SETTING_CHOICE, SETMASK_ALL, NULL, NULL, 0, NULL, NULL, NULL, NULL,
      rot_names, COUNT_OF(s_rot_names), rot_get, rot_set },
    { "Info",       SETTING_ACTION, SETMASK_ALL, item_info_cb,       NULL, 0 },
};

#if ESDECK_ENABLE_BLE
/* Bluetooth's own detail page -- the root-level "Bluetooth" row (see
 * s_root_menu below) only shows On/Off as a status string and isn't itself
 * a toggle; the actual switch lives in here alongside a connection-status
 * row. Keeping the switch off the root row avoids the "flick the switch by
 * accident while trying to tap into the page" ambiguity a combined
 * switch+navigate row would have. */
static const setting_node_t s_bluetooth_menu[] = {
    { "Bluetooth", SETTING_TOGGLE, SETMASK_ALL, NULL, NULL, 0, ble_manager_is_enabled, ble_manager_set_enabled },
    /* Device name isn't shown -- ble_manager_is_connected() only knows
     * *that* a phone is connected, not its name (see that function's
     * header comment). Connected/Not Connected is what's actually
     * available today. */
    { "Connection", SETTING_LABEL, SETMASK_ALL, item_bt_conn_status_marker, NULL, 0 },
};
#endif

/* Storage Mode first, then one row per target mode in fixed
 * Deck/Monitor/Media order -- switch_to_mode() handles any source mode,
 * so each destination needs exactly one row; mode_mask only hides
 * "switch to the mode you're already in". */
static const setting_node_t s_mode_menu[] = {
    { "Storage Mode", SETTING_ACTION, SETMASK_ALL,                       item_msc_cb,             NULL, 0 },
    { "Deck Mode",     SETTING_ACTION, SETMASK_ALL & ~SETMASK_DECK,     item_mode_to_deck_cb,     NULL, 0 },
    { "Monitor Mode",  SETTING_ACTION, SETMASK_ALL & ~SETMASK_MONITOR,  item_mode_to_monitor_cb,  NULL, 0 },
    { "Media Mode",    SETTING_ACTION, SETMASK_ALL & ~SETMASK_MEDIA,    item_mode_to_media_cb,    NULL, 0 },
    { "Touchpad Mode", SETTING_ACTION, SETMASK_ALL & ~SETMASK_TOUCHPAD, item_mode_to_touchpad_cb, NULL, 0 },
    { "Canvas Mode",   SETTING_ACTION, SETMASK_ALL & ~SETMASK_CANVAS,   item_mode_to_canvas_cb,   NULL, 0 },
};

static const setting_node_t s_root_menu[] = {
    { "System",        SETTING_SUBMENU, SETMASK_ALL,  NULL, s_system_menu,
      COUNT_OF(s_system_menu) },
#if ESDECK_ENABLE_BLE
    { "Bluetooth",     SETTING_SUBMENU, SETMASK_ALL,  NULL, s_bluetooth_menu, COUNT_OF(s_bluetooth_menu) },
#endif
    { "Custom",        SETTING_SUBMENU, SETMASK_ALL,  NULL, s_custom_menu,
      COUNT_OF(s_custom_menu) },
    { "Mode",          SETTING_SUBMENU, SETMASK_ALL,  NULL, s_mode_menu, COUNT_OF(s_mode_menu) },
};

/* -----------------------------------------------------------------------
 * Menu navigation / rendering
 * ----------------------------------------------------------------------- */
static void render_current_level(void);
static void choice_open(const struct setting_node_s *node);

/* usb_hid_conn_cb_t -- fires once TinyUSB's own ATTACHED/DETACHED event
 * lands (already hopped onto the LVGL task, see usb_hid.c), i.e. this is
 * the real confirmation a forced disconnect/connect actually took effect,
 * not just that the HID Status row was tapped. Only worth a redraw while
 * that row is actually the level on screen. */
static void hid_conn_cb(bool connected)
{
    (void)connected;
    if (s_stack_depth >= 0 && s_menu_stack[s_stack_depth] == s_system_menu)
        render_current_level();
}

#if ESDECK_ENABLE_BLE
/* Same idea as hid_conn_cb() above, for the Bluetooth detail page's
 * Connection row. */
static void ble_conn_cb(bool connected)
{
    (void)connected;
    if (s_stack_depth >= 0 && s_menu_stack[s_stack_depth] == s_bluetooth_menu)
        render_current_level();
}
#endif

static void generic_item_cb(lv_event_t *e)
{
    const setting_node_t *node = (const setting_node_t *)lv_event_get_user_data(e);

    if (node->type == SETTING_CHOICE) {
        choice_open(node);
        return;
    }
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

/* SETTING_TOGGLE rows -- only the lv_switch itself is the click target
 * (see render_current_level()), so there's no row-tap/switch-tap
 * double-toggle to reconcile. */
static void toggle_switch_cb(lv_event_t *e)
{
    const setting_node_t *node = (const setting_node_t *)lv_event_get_user_data(e);
    lv_obj_t *sw = lv_event_get_target(e);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if (node->set_state_cb) node->set_state_cb(on);
}

/* SETTING_VALUE "-" / "+" buttons. Row children (render_current_level()):
 * 0 = name label, 1 = minus, 2 = value label, 3 = plus. The button's own
 * user data says which direction it steps. */
static void value_step_btn_cb(lv_event_t *e)
{
    const setting_node_t *node = (const setting_node_t *)lv_event_get_user_data(e);
    lv_obj_t *btn = lv_event_get_target(e);
    int dir = (int)(intptr_t)lv_obj_get_user_data(btn);

    if (node->value_step_cb) node->value_step_cb(dir);

    lv_obj_t *val = lv_obj_get_child(lv_obj_get_parent(btn), 2);
    if (val && node->value_text_cb) lv_label_set_text(val, node->value_text_cb());
}

/* SETTING_CHOICE popup -- same look as the Boot Animation picker. Each
 * list item carries its index in its own user data; the popup root is
 * deleted on pick or on a tap outside the box. */
static const setting_node_t *s_choice_node = NULL;

static void choice_pick_cb(lv_event_t *e)
{
    lv_obj_t *btn  = lv_event_get_target(e);
    int       idx  = (int)(intptr_t)lv_obj_get_user_data(btn);
    lv_obj_t *root = lv_obj_get_parent(lv_obj_get_parent(btn));   /* root -> box -> item */

    if (s_choice_node && s_choice_node->choice_set_cb) s_choice_node->choice_set_cb(idx);
    s_choice_node = NULL;
    lv_obj_del(root);
    render_current_level();   /* refresh the row's value text */
}

static void choice_dismiss_cb(lv_event_t *e)
{
    s_choice_node = NULL;
    lv_obj_del(lv_event_get_target(e));
}

static void choice_open(const setting_node_t *node)
{
    const char *const *names = node->choice_names_cb ? node->choice_names_cb() : NULL;
    if (!names) return;
    int cur = node->choice_get_cb ? node->choice_get_cb() : -1;
    s_choice_node = node;

    lv_obj_t *root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(root, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_60, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_radius(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, choice_dismiss_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *box = lv_obj_create(root);
    lv_obj_set_size(box, 320, LV_SIZE_CONTENT);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1e1e1e), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 16, 0);
    lv_obj_set_layout(box, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_add_event_cb(box, lv_event_stop_processing, LV_EVENT_CLICKED, NULL);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, node->label);
    lv_obj_set_style_text_color(title, lv_color_hex(0x888888), 0);

    for (int i = 0; i < node->choice_count; i++) {
        lv_obj_t *item = lv_btn_create(box);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_style_bg_color(item, (i == cur) ? lv_color_hex(0x2a5a8a) : lv_color_hex(0x2a2a2a), 0);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x3a3a3a), LV_STATE_PRESSED);
        lv_obj_set_style_radius(item, 4, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_set_user_data(item, (void *)(intptr_t)i);
        lv_obj_add_event_cb(item, choice_pick_cb, LV_EVENT_CLICKED, NULL);

        lv_obj_t *lbl = lv_label_create(item);
        lv_label_set_text(lbl, names[i]);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 4, 0);
    }
}

static lv_obj_t *value_step_btn(lv_obj_t *row, const setting_node_t *node, const char *sym, int dir)
{
    lv_obj_t *b = lv_btn_create(row);
    lv_obj_set_size(b, 40, 36);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x0055cc), LV_STATE_PRESSED);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_set_user_data(b, (void *)(intptr_t)dir);
    lv_obj_add_event_cb(b, value_step_btn_cb, LV_EVENT_CLICKED, (void *)node);

    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
    return b;
}

/* Close Settings entirely and resume whatever mode is active -- the
 * counterpart to the deselect_current() call in ui_settings_select().
 * Called from the header's X button (s_close_btn), shown at every menu
 * level, so there is always a one-tap way out of Settings -- even from
 * deep inside a submenu, and for modes like Media that have no page list
 * to tap instead. */
static void close_settings(void)
{
    ui_settings_deselect();

    if (s_mode == UI_MODE_DECK)
        ui_deck_reselect_current();
    else if (s_mode == UI_MODE_MONITOR)
        ui_monitor_reselect_current();
    else if (s_mode == UI_MODE_TOUCHPAD)
        ui_touchpad_reselect_current();
    else if (s_mode == UI_MODE_CANVAS)
        ui_canvas_reselect_current();
    else
        ui_media_reselect_current();
}

static void close_btn_cb(lv_event_t *e)
{
    (void)e;
    close_settings();
}

static void back_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_stack_depth == 0) return;   /* hidden at the root anyway */
    s_stack_depth--;
    render_current_level();
}

/* -----------------------------------------------------------------------
 * List paging -- s_list is never scrolled (a drag redraws the whole list
 * every frame and the CPU can't keep up). Each level is split into pages
 * of as many whole rows as fit; only the current page's rows are built,
 * and the header's up/down buttons switch pages. 8 rows at 7 per page =
 * page 1 with 7, page 2 with 1.
 * ----------------------------------------------------------------------- */
#define SETTINGS_ROW_H     48
#define SETTINGS_ROW_GAP   8                                  /* s_list pad_row */
#define SETTINGS_ROW_STEP  (SETTINGS_ROW_H + SETTINGS_ROW_GAP)
/* s_list height: SCREEN_H - s_content padding (16*2) - header (40) -
 * s_content row gap (12). Used when the list hasn't been laid out yet. */
#define SETTINGS_LIST_H    (SCREEN_H - 32 - 40 - 12)

static int                   s_page_idx = 0;
static int                   s_page_cnt = 1;
static const setting_node_t *s_page_arr = NULL;   /* level the page index belongs to */

/* Row shown in this mode: mode_mask, then the node's own visible_cb. */
static bool node_shown(const setting_node_t *node, uint8_t mode_bit)
{
    if (!(node->mode_mask & mode_bit)) return false;
    return !node->visible_cb || node->visible_cb();
}

static int rows_per_page(void)
{
    lv_coord_t h = lv_obj_get_content_height(s_list);
    if (h <= 0) h = SETTINGS_LIST_H;
    int n = (h + SETTINGS_ROW_GAP) / SETTINGS_ROW_STEP;   /* last row needs no gap */
    return n > 0 ? n : 1;
}

static void page_btn_set_enabled(lv_obj_t *btn, bool en)
{
    if (en) lv_obj_clear_state(btn, LV_STATE_DISABLED);
    else    lv_obj_add_state(btn, LV_STATE_DISABLED);
    lv_obj_set_style_text_opa(btn, en ? LV_OPA_COVER : LV_OPA_30, 0);
}

static void update_page_btns(void)
{
    if (!s_pg_up_btn || !s_pg_dn_btn) return;
    if (s_page_cnt > 1) {
        lv_obj_clear_flag(s_pg_up_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_pg_dn_btn, LV_OBJ_FLAG_HIDDEN);
        page_btn_set_enabled(s_pg_up_btn, s_page_idx > 0);
        page_btn_set_enabled(s_pg_dn_btn, s_page_idx < s_page_cnt - 1);
    } else {
        lv_obj_add_flag(s_pg_up_btn, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pg_dn_btn, LV_OBJ_FLAG_HIDDEN);
    }
}

static void page_btn_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);   /* -1 up, +1 down */
    int idx = s_page_idx + dir;
    if (idx < 0 || idx >= s_page_cnt) return;
    s_page_idx = idx;
    render_current_level();
}

static lv_obj_t *make_header_btn(lv_obj_t *header, const char *sym)
{
    lv_obj_t *b = lv_btn_create(header);
    lv_obj_set_size(b, 36, 36);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_50, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(0x3a3a3a), LV_STATE_PRESSED);
    lv_obj_set_style_bg_grad_dir(b, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_outline_width(b, 0, 0);
    lv_obj_set_style_radius(b, 6, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_center(l);
    return b;
}

static void render_current_level(void)
{
    lv_obj_clean(s_list);

    uint8_t mode_bit = (uint8_t)(1u << s_mode);

    const setting_node_t *arr   = s_menu_stack[s_stack_depth];
    int                   count = s_menu_stack_count[s_stack_depth];

    /* Paging over the rows actually shown in this mode. Entering a
     * different level starts at its first page; re-rendering the same
     * level (toggle, choice pick, HID status refresh) keeps the page. */
    if (arr != s_page_arr) {
        s_page_arr = arr;
        s_page_idx = 0;
    }
    int visible = 0;
    for (int i = 0; i < count; i++)
        if (node_shown(&arr[i], mode_bit)) visible++;
    int per_page = rows_per_page();
    s_page_cnt = visible > 0 ? (visible + per_page - 1) / per_page : 1;
    if (s_page_idx >= s_page_cnt) s_page_idx = s_page_cnt - 1;
    int first = s_page_idx * per_page;
    int last  = first + per_page;   /* exclusive */

    int vis_idx = -1;
    for (int i = 0; i < count; i++) {
        const setting_node_t *node = &arr[i];
        if (!node_shown(node, mode_bit)) continue;
        vis_idx++;
        if (vis_idx < first || vis_idx >= last) continue;

        lv_obj_t *item = lv_btn_create(s_list);
        lv_obj_set_width(item, LV_PCT(100));
        lv_obj_set_height(item, SETTINGS_ROW_H);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x2a2a2a), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_50, 0);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x3a3a3a), LV_STATE_PRESSED);
        lv_obj_set_style_bg_grad_dir(item, LV_GRAD_DIR_NONE, 0);
        lv_obj_set_style_shadow_width(item, 0, 0);
        lv_obj_set_style_outline_width(item, 0, 0);
        lv_obj_set_style_radius(item, 6, 0);
        lv_obj_clear_flag(item, LV_OBJ_FLAG_PRESS_LOCK);

        lv_obj_t *lbl = lv_label_create(item);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 12, 0);

        if (node->action_cb == item_hid_status_cb) {
            bool connected = usb_hid_is_connected();
            lv_label_set_text_fmt(lbl, "HID Status: %s", connected ? "Connected" : "Disconnected");
            lv_obj_set_style_text_color(lbl, lv_color_hex(connected ? 0x33cc33 : 0xcc3333), 0);
        }
#if ESDECK_ENABLE_BLE
        else if (node->children == s_bluetooth_menu) {
            bool on = ble_manager_is_enabled();
            lv_label_set_text_fmt(lbl, "Bluetooth: %s", on ? "On" : "Off");
        } else if (node->action_cb == item_bt_conn_status_marker) {
            bool connected = ble_manager_is_connected();
            lv_label_set_text_fmt(lbl, "Connection: %s", connected ? "Connected" : "Not Connected");
            lv_obj_set_style_text_color(lbl, lv_color_hex(connected ? 0x33cc33 : 0x888888), 0);
        }
#endif
        else {
            lv_label_set_text(lbl, node->label);
        }

        if (node->type == SETTING_TOGGLE) {
            /* Only the switch is the click target -- the row itself isn't
             * wired to generic_item_cb, so there's no double-toggle to
             * reconcile between a row-tap and the switch's own tap.
             * CLICKABLE still needs clearing explicitly though: lv_btn
             * carries it by default regardless of whether anything is
             * actually wired to it, so without this the row's PRESSED-state
             * background (see the style set above) flashes on tap even
             * though nothing happens -- looks clickable when it isn't. */
            lv_obj_clear_flag(item, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t *sw = lv_switch_create(item);
            lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -8, 0);
            /* Plain add_state is safe here -- it used to visibly flash
             * off->on for a frame on reopen (LVGL drawing the fresh
             * switch's default state before this applies), fixed at the
             * theme level via CONFIG_LV_THEME_DEFAULT_TRANSITION_TIME=0
             * in sdkconfig rather than worked around per-widget here.
             * See https://github.com/lvgl/lvgl/issues/3157. */
            if (node->locked_cb && node->locked_cb()) {
                /* Forced on by another setting (e.g. Tap to Click while no
                 * top button is Left Click): show the effective state,
                 * don't let it be switched; the stored value comes back
                 * once the lock goes away. */
                lv_obj_add_state(sw, LV_STATE_CHECKED | LV_STATE_DISABLED);
                lv_label_set_text_fmt(lbl, "%s (always on)", node->label);
            } else if (node->get_state_cb && node->get_state_cb()) {
                lv_obj_add_state(sw, LV_STATE_CHECKED);
            }
            lv_obj_add_event_cb(sw, toggle_switch_cb, LV_EVENT_VALUE_CHANGED, (void *)node);
        } else if (node->type == SETTING_VALUE) {
            /* Same non-clickable row as TOGGLE; only - / + react.
             * Children order matters, see value_step_btn_cb(). */
            lv_obj_clear_flag(item, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t *minus = value_step_btn(item, node, LV_SYMBOL_MINUS, -1);
            lv_obj_t *val = lv_label_create(item);
            lv_obj_t *plus  = value_step_btn(item, node, LV_SYMBOL_PLUS, +1);

            lv_label_set_text(val, node->value_text_cb ? node->value_text_cb() : "");
            lv_obj_set_width(val, 64);
            lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(val, &lv_font_montserrat_16, 0);

            lv_obj_align(plus,  LV_ALIGN_RIGHT_MID, -8, 0);
            lv_obj_align_to(val, plus, LV_ALIGN_OUT_LEFT_MID, -4, 0);
            lv_obj_align_to(minus, val, LV_ALIGN_OUT_LEFT_MID, -4, 0);
        } else if (node->type == SETTING_LABEL) {
            /* Purely informational -- same CLICKABLE-clearing reasoning as
             * the TOGGLE row above (lv_btn is clickable by default whether
             * or not anything is wired to it), and no generic_item_cb/
             * chevron since there's nowhere for this row to go. */
            lv_obj_clear_flag(item, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_add_event_cb(item, generic_item_cb, LV_EVENT_CLICKED, (void *)node);

            if (node->type == SETTING_CHOICE) {
                const char *const *names = node->choice_names_cb ? node->choice_names_cb() : NULL;
                int cur = node->choice_get_cb ? node->choice_get_cb() : -1;
                lv_obj_t *val = lv_label_create(item);
                lv_label_set_text(val, (names && cur >= 0 && cur < node->choice_count) ? names[cur] : "");
                lv_obj_set_style_text_color(val, lv_color_hex(0x5aa0e0), 0);
                lv_obj_align(val, LV_ALIGN_RIGHT_MID, -12, 0);
            }
            if (node->type == SETTING_SUBMENU) {
                lv_obj_t *chevron = lv_label_create(item);
                lv_label_set_text(chevron, LV_SYMBOL_RIGHT);
                lv_obj_set_style_text_color(chevron, lv_color_hex(0x888888), 0);
                lv_obj_align(chevron, LV_ALIGN_RIGHT_MID, -12, 0);
            }
        }
    }

    /* X is always there; the back arrow only below the root. */
    if (s_stack_depth == 0) {
        lv_label_set_text(s_breadcrumb_lbl, "Settings");
        lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_label_set_text(s_breadcrumb_lbl, s_menu_stack_label[s_stack_depth]);
        lv_obj_clear_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_clear_flag(s_close_btn, LV_OBJ_FLAG_HIDDEN);
    update_page_btns();
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
    if (s_bg_applied) return;
    if (s_bg_ext) {
        if (!s_bg_dsc_ptr) return;
        goto attach;   /* caller's ready-made image, nothing to decode */
    }
    if (s_bg_image[0] == '\0') return;

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

attach:;
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
    /* Widgets exist only once attached (an external bg is set first and
     * attached on the next select). */
    if (s_bg_dsc_ptr && s_bg_applied) {
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
    s_bg_ext      = false;

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
static void gear_icon_clear(void)
{
    if (s_gear_has_icon) {
        /* Custom icon is gear_btn's child 1 (child 0 is the glyph label
         * created once in ui_settings_build()). */
        lv_obj_t *old_img = lv_obj_get_child(s_gear_btn, 1);
        if (old_img) lv_obj_del(old_img);
        s_gear_has_icon = false;
    }
    s_gear_ext = false;
    if (s_gear_glyph) lv_obj_clear_flag(s_gear_glyph, LV_OBJ_FLAG_HIDDEN);
}

void ui_settings_set_external_appearance(const lv_img_dsc_t *gear_icon, const lv_img_dsc_t *bg)
{
    /* Drop whatever file-based or external appearance is there. */
    ui_settings_apply_appearance(NULL);
    gear_icon_clear();
    settings_bg_release();

    if (gear_icon) {
        lv_img_cache_invalidate_src(gear_icon);
        if (s_gear_glyph) lv_obj_add_flag(s_gear_glyph, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *img = lv_img_create(s_gear_btn);
        lv_img_set_src(img, gear_icon);
        lv_obj_center(img);
        lv_obj_set_style_clip_corner(s_gear_btn, true, 0);
        s_gear_has_icon = true;
        s_gear_ext      = true;
    }
    if (bg) {
        lv_img_cache_invalidate_src(bg);
        s_bg_ext     = true;
        s_bg_dsc_ptr = (lv_img_dsc_t *)bg;
        s_bg_applied = false;
        if (!lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN)) settings_lazy_bg_set();   /* Settings is open: show it now */
    }

    /* Highlight style follows icon / no icon (see ui_settings_select()). */
    if (!lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_set_style_bg_color(s_gear_btn, lv_color_hex(s_gear_has_icon ? 0x2a2a2a : 0x0055cc), 0);
        lv_obj_set_style_outline_width(s_gear_btn, s_gear_has_icon ? 3 : 0, 0);
    }
}

void ui_settings_apply_appearance(const ui_settings_appearance_t *appearance)
{
    static const ui_settings_appearance_t empty = { 0 };
    if (!appearance) appearance = &empty;

    /* Leaving Canvas-style external images: back to plain first. */
    if (s_gear_ext) gear_icon_clear();
    if (s_bg_ext)   settings_bg_release();

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

    /* Header: [X][<]  breadcrumb (centered)  [^][v]
     * X always closes Settings (close_settings()), from any menu level;
     * < goes up one level and is hidden at the root (see
     * render_current_level()). */
    lv_obj_t *header = lv_obj_create(s_content);
    lv_obj_set_size(header, LV_PCT(100), 40);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_clear_flag(header, LV_OBJ_FLAG_SCROLLABLE);

    s_close_btn = make_header_btn(header, LV_SYMBOL_CLOSE);
    lv_obj_align(s_close_btn, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(s_close_btn, close_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_close_btn, LV_OBJ_FLAG_HIDDEN);

    s_back_btn = make_header_btn(header, LV_SYMBOL_LEFT);
    lv_obj_align(s_back_btn, LV_ALIGN_LEFT_MID, 44, 0);   /* same 44 px step as the page buttons */
    lv_obj_add_event_cb(s_back_btn, back_btn_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_back_btn, LV_OBJ_FLAG_HIDDEN);

    /* Centered on the page rather than balanced against the header
     * buttons' width -- simpler, and reads fine either way. */
    s_breadcrumb_lbl = lv_label_create(header);
    lv_label_set_text(s_breadcrumb_lbl, "Settings");
    lv_obj_set_style_text_color(s_breadcrumb_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(s_breadcrumb_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(s_breadcrumb_lbl, LV_ALIGN_CENTER, 0, 0);

    /* Page buttons, top right -- only shown when the level has more rows
     * than fit (see update_page_btns()). */
    s_pg_dn_btn = make_header_btn(header, LV_SYMBOL_DOWN);
    lv_obj_align(s_pg_dn_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(s_pg_dn_btn, page_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    lv_obj_add_flag(s_pg_dn_btn, LV_OBJ_FLAG_HIDDEN);

    s_pg_up_btn = make_header_btn(header, LV_SYMBOL_UP);
    lv_obj_align(s_pg_up_btn, LV_ALIGN_RIGHT_MID, -44, 0);
    lv_obj_add_event_cb(s_pg_up_btn, page_btn_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_add_flag(s_pg_up_btn, LV_OBJ_FLAG_HIDDEN);

    /* HID status lives as a System > HID Status row instead (see
     * s_system_menu[] / item_hid_status_cb) -- just register the redraw
     * hook here so that row's color/text stays live while visible. */
    usb_hid_set_conn_cb(hid_conn_cb);
#if ESDECK_ENABLE_BLE
    ble_manager_set_conn_cb(ble_conn_cb);
#endif

    /* Scrollable item list -- grows to fill remaining page height */
    s_list = lv_obj_create(s_content);
    lv_obj_set_width(s_list, LV_PCT(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_set_layout(s_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, SETTINGS_ROW_GAP, 0);
    /* No scrolling at all (too heavy to redraw) -- one page of rows at a
     * time, switched by the header buttons (see "List paging"). */
    lv_obj_clear_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);

    s_menu_stack[0]       = s_root_menu;
    s_menu_stack_count[0] = COUNT_OF(s_root_menu);
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
    else if (s_mode == UI_MODE_TOUCHPAD)
        ui_touchpad_deselect_current();
    else if (s_mode == UI_MODE_CANVAS)
        ui_canvas_deselect_current();
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
