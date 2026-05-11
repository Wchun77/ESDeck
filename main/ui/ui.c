#include "lvgl.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui.h"
#include "ui_config.h"
#include "usb/usb_manager.h"
#include "usb/usb_hid.h"

#define SIDEBAR_W   80
#define SCREEN_W    800
#define SCREEN_H    480

#define ARRAY_SIZE(arr)  ((int)(sizeof(arr) / sizeof((arr)[0])))

/* Forward declarations */
static void ui_destroy_deck(void);
static void ui_deck_build_widgets(void);
static void ui_show_switching_screen(void);
static void img_pool_load(const deck_cfg_t *cfg);

/* Global config — populated by ui_preload_start() or config switch flow */
static deck_cfg_t s_cfg;

/* -----------------------------------------------------------------------
 * MSC mode screen
 * ----------------------------------------------------------------------- */
static void show_msc_screen(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x111111), 0);

    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, 400, 200);
    lv_obj_center(cont);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 12, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cont, 20, 0);
    lv_obj_set_style_pad_row(cont, 12, 0);

    lv_obj_t *icon = lv_label_create(cont);
    lv_label_set_text(icon, LV_SYMBOL_USB);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xffffff), 0);

    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_text(title, "USB Mass Storage Mode");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    lv_obj_t *sub = lv_label_create(cont);
    lv_label_set_text(sub, "Flash + SD card connected\nRestart to return to HID mode");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
}

static void msc_screen_timer_cb(lv_timer_t *t)
{
    show_msc_screen();
    lv_timer_del(t);
}

static void msc_start_timer_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    usb_manager_request_msc();
    lv_timer_create(msc_screen_timer_cb, 800, NULL);
}

/* -----------------------------------------------------------------------
 * Confirm dialog (MSC)
 * ----------------------------------------------------------------------- */
static lv_obj_t *s_dialog_overlay = NULL;

static void dialog_confirm_cb(lv_event_t *e)
{
    if (s_dialog_overlay) {
        lv_obj_del(s_dialog_overlay);
        s_dialog_overlay = NULL;
    }
    lv_obj_t *black = lv_obj_create(lv_scr_act());
    lv_obj_set_size(black, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(black, 0, 0);
    lv_obj_set_style_bg_color(black, lv_color_black(), 0);
    lv_obj_set_style_border_width(black, 0, 0);
    lv_obj_set_style_radius(black, 0, 0);
    lv_timer_create(msc_start_timer_cb, 150, NULL);
}

static void dialog_cancel_cb(lv_event_t *e)
{
    if (s_dialog_overlay) {
        lv_obj_del(s_dialog_overlay);
        s_dialog_overlay = NULL;
    }
}

static void show_confirm_dialog(void)
{
    s_dialog_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_dialog_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_dialog_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_dialog_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_dialog_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_dialog_overlay, 0, 0);
    lv_obj_set_style_radius(s_dialog_overlay, 0, 0);
    lv_obj_clear_flag(s_dialog_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(s_dialog_overlay);
    lv_obj_set_size(box, 460, 220);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 24, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "Switch to MSC Mode");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_pos(title, 0, 0);

    lv_obj_t *div = lv_obj_create(box);
    lv_obj_set_size(div, 412, 1);
    lv_obj_set_pos(div, 0, 28);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_radius(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *body = lv_label_create(box);
    lv_label_set_text(body, "Flash and SD card will be available to PC.\nDevice must restart to return to HID mode.");
    lv_obj_set_style_text_color(body, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(body, 0, 44);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, 412);

    lv_obj_t *btn_row = lv_obj_create(box);
    lv_obj_set_size(btn_row, 412, 48);
    lv_obj_set_pos(btn_row, 0, 128);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_layout(btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 12, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_cancel = lv_btn_create(btn_row);
    lv_obj_set_size(btn_cancel, 120, 40);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x4a4a4a), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn_cancel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(btn_cancel, 1, 0);
    lv_obj_set_style_radius(btn_cancel, 6, 0);
    lv_obj_add_event_cb(btn_cancel, dialog_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_set_style_text_color(lbl_cancel, lv_color_hex(0xcccccc), 0);
    lv_obj_center(lbl_cancel);

    lv_obj_t *btn_confirm = lv_btn_create(btn_row);
    lv_obj_set_size(btn_confirm, 120, 40);
    lv_obj_set_style_bg_color(btn_confirm, lv_color_hex(0x0055cc), 0);
    lv_obj_set_style_bg_color(btn_confirm, lv_color_hex(0x0044aa), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn_confirm, 0, 0);
    lv_obj_set_style_radius(btn_confirm, 6, 0);
    lv_obj_add_event_cb(btn_confirm, dialog_confirm_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_confirm = lv_label_create(btn_confirm);
    lv_label_set_text(lbl_confirm, "Switch");
    lv_obj_set_style_text_color(lbl_confirm, lv_color_hex(0xffffff), 0);
    lv_obj_center(lbl_confirm);
}

/* -----------------------------------------------------------------------
 * Select Config dialog
 * ----------------------------------------------------------------------- */
#define CFG_ROWS_PER_PAGE  4

static lv_obj_t        *s_config_dim      = NULL;
static lv_obj_t        *s_config_dialog   = NULL;
static lv_obj_t        *s_config_confirm  = NULL;
static lv_obj_t        *s_config_rows[CFG_ROWS_PER_PAGE];
static lv_obj_t        *s_config_row_lbls[CFG_ROWS_PER_PAGE];
static lv_obj_t        *s_config_btn_prev = NULL;
static lv_obj_t        *s_config_btn_next = NULL;
static lv_obj_t        *s_config_page_lbl = NULL;

static json_scan_result_t s_scan_res     = { .names = NULL, .count = 0 };
static int                s_cfg_page     = 0;
static int                s_cfg_pages    = 0;
static int                s_selected_idx = -1;
static lv_obj_t          *s_selected_row = NULL;

static void config_dialog_close(void)
{
    if (s_config_dim) {
        lv_obj_del(s_config_dim);
        s_config_dim = NULL;
    }
    if (s_config_dialog) {
        lv_obj_del(s_config_dialog);
        s_config_dialog   = NULL;
        s_config_confirm  = NULL;
        s_config_btn_prev = NULL;
        s_config_btn_next = NULL;
        s_config_page_lbl = NULL;
        for (int i = 0; i < CFG_ROWS_PER_PAGE; i++) {
            s_config_rows[i]     = NULL;
            s_config_row_lbls[i] = NULL;
        }
    }
    s_selected_row = NULL;
    s_selected_idx = -1;
    s_cfg_page     = 0;
    s_cfg_pages    = 0;
    ui_config_scan_free(&s_scan_res);
}

static void config_render_page(void)
{
    int base = s_cfg_page * CFG_ROWS_PER_PAGE;

    for (int i = 0; i < CFG_ROWS_PER_PAGE; i++) {
        int abs_idx = base + i;
        if (abs_idx < s_scan_res.count) {
            lv_label_set_text(s_config_row_lbls[i], s_scan_res.names[abs_idx]);
            lv_obj_clear_flag(s_config_rows[i], LV_OBJ_FLAG_HIDDEN);
            if (abs_idx == s_selected_idx) {
                lv_obj_add_state(s_config_rows[i], LV_STATE_CHECKED);
                s_selected_row = s_config_rows[i];
            } else {
                lv_obj_clear_state(s_config_rows[i], LV_STATE_CHECKED);
            }
        } else {
            lv_obj_add_flag(s_config_rows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_state(s_config_rows[i], LV_STATE_CHECKED);
        }
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d / %d", s_cfg_page + 1, s_cfg_pages);
    lv_label_set_text(s_config_page_lbl, buf);

    if (s_cfg_page == 0) {
        lv_obj_add_state(s_config_btn_prev, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(s_config_btn_prev, LV_STATE_DISABLED);
    }

    if (s_cfg_page >= s_cfg_pages - 1) {
        lv_obj_add_state(s_config_btn_next, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(s_config_btn_next, LV_STATE_DISABLED);
    }
}

static void config_cancel_cb(lv_event_t *e)
{
    config_dialog_close();
}

/* Called from the switch preload task via lv_async_call — executes safely
 * inside the LVGL task after image decoding is complete. */
static void on_switch_preload_done(void *arg)
{
    ui_deck_build_widgets();
    ESP_LOGI("UI", "Config switch complete - PSRAM free: %d B",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* Background task: decodes images for the new config, then schedules
 * ui_deck_build_widgets() back on the LVGL task via lv_async_call. */
static void switch_preload_task(void *arg)
{
    img_pool_load(&s_cfg);
    lv_async_call(on_switch_preload_done, NULL);
    vTaskDelete(NULL);
}

static void config_confirm_cb(lv_event_t *e)
{
    if (s_selected_idx < 0 || s_selected_idx >= s_scan_res.count) return;

    const char *fname = s_scan_res.names[s_selected_idx];
    if (!ui_config_nvs_save(fname)) {
        ESP_LOGE("CFG", "Failed to save config to NVS");
        return;
    }

    ESP_LOGI("CFG", "Switching config: %s", fname);

    /* Close dialog before touching the deck so no dialog widget points
     * into the about-to-be-destroyed deck objects. */
    config_dialog_close();

    /* Tear down old deck and free its PSRAM image buffers. */
    ui_destroy_deck();

    /* Load new config metadata (fast — JSON parse only, no image I/O). */
    bool cfg_ok = ui_config_load(&s_cfg);
    if (!cfg_ok || s_cfg.page_count == 0) {
        s_cfg.page_count = 1;
        s_cfg.pages      = calloc(1, sizeof(page_cfg_t));
        snprintf(s_cfg.pages[0].name, UI_CONFIG_NAME_LEN, "Main");
        s_cfg.pages[0].button_count = 0;
        s_cfg.pages[0].buttons      = NULL;
    }

    /* Show switching screen so the user sees feedback during image decode. */
    ui_show_switching_screen();

    /* Decode images in a background task so LVGL stays responsive.
     * on_switch_preload_done() will be called on the LVGL task when done. */
    xTaskCreate(switch_preload_task, "sw_preload", 8192, NULL, 3, NULL);
}

static void config_item_cb(lv_event_t *e)
{
    int row_pos = (int)(uintptr_t)lv_event_get_user_data(e);
    int abs_idx = s_cfg_page * CFG_ROWS_PER_PAGE + row_pos;

    if (abs_idx >= s_scan_res.count) return;

    lv_obj_t *row = lv_event_get_target(e);

    if (s_selected_row && s_selected_row != row) {
        lv_obj_clear_state(s_selected_row, LV_STATE_CHECKED);
    }

    s_selected_row = row;
    s_selected_idx = abs_idx;
    lv_obj_add_state(row, LV_STATE_CHECKED);

    lv_obj_clear_state(s_config_confirm, LV_STATE_DISABLED);
    lv_obj_set_style_bg_color(s_config_confirm, lv_color_hex(0x3a3a3a), 0);
    lv_obj_t *confirm_lbl = lv_obj_get_child(s_config_confirm, 0);
    if (confirm_lbl) {
        lv_obj_set_style_text_color(confirm_lbl, lv_color_hex(0xcccccc), 0);
    }
}

static void config_prev_cb(lv_event_t *e)
{
    if (s_cfg_page <= 0) return;

    int pos_in_page = (s_selected_idx >= 0) ? (s_selected_idx % CFG_ROWS_PER_PAGE) : -1;
    s_cfg_page--;
    config_render_page();

    if (pos_in_page >= 0) {
        int base       = s_cfg_page * CFG_ROWS_PER_PAGE;
        int target_abs = base + pos_in_page;
        int last_abs   = base + CFG_ROWS_PER_PAGE - 1;
        if (last_abs >= s_scan_res.count) last_abs = s_scan_res.count - 1;

        s_selected_idx = (target_abs < s_scan_res.count) ? target_abs : last_abs;

        int new_pos = s_selected_idx - base;
        if (s_selected_row) lv_obj_clear_state(s_selected_row, LV_STATE_CHECKED);
        s_selected_row = s_config_rows[new_pos];
        lv_obj_add_state(s_selected_row, LV_STATE_CHECKED);
    }
}

static void config_next_cb(lv_event_t *e)
{
    if (s_cfg_page >= s_cfg_pages - 1) return;

    int pos_in_page = (s_selected_idx >= 0) ? (s_selected_idx % CFG_ROWS_PER_PAGE) : -1;
    s_cfg_page++;
    config_render_page();

    if (pos_in_page >= 0) {
        int base       = s_cfg_page * CFG_ROWS_PER_PAGE;
        int target_abs = base + pos_in_page;
        int last_abs   = base + CFG_ROWS_PER_PAGE - 1;
        if (last_abs >= s_scan_res.count) last_abs = s_scan_res.count - 1;

        s_selected_idx = (target_abs < s_scan_res.count) ? target_abs : last_abs;

        int new_pos = s_selected_idx - base;
        if (s_selected_row) lv_obj_clear_state(s_selected_row, LV_STATE_CHECKED);
        s_selected_row = s_config_rows[new_pos];
        lv_obj_add_state(s_selected_row, LV_STATE_CHECKED);
    }
}

static void show_select_config_dialog(void)
{
    s_scan_res = ui_config_scan();

    if (s_scan_res.count == 0) {
        ui_config_scan_free(&s_scan_res);
        ESP_LOGW("CFG", "No JSON files found in %s", UI_CONFIG_JSON_PATH);
        return;
    }

    s_cfg_page  = 0;
    s_cfg_pages = (s_scan_res.count + CFG_ROWS_PER_PAGE - 1) / CFG_ROWS_PER_PAGE;

    lv_obj_t *scr = lv_scr_act();

    s_config_dim = lv_obj_create(scr);
    lv_obj_set_size(s_config_dim, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_config_dim, 0, 0);
    lv_obj_set_style_bg_color(s_config_dim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_config_dim, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_config_dim, 0, 0);
    lv_obj_set_style_radius(s_config_dim, 0, 0);
    lv_obj_clear_flag(s_config_dim, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    int dlg_w = (SCREEN_W * 80) / 100;
    int dlg_h = (SCREEN_H * 80) / 100;

    s_config_dialog = lv_obj_create(scr);
    lv_obj_set_size(s_config_dialog, dlg_w, dlg_h);
    lv_obj_center(s_config_dialog);
    lv_obj_set_style_bg_color(s_config_dialog, lv_color_hex(0x1e1e1e), 0);
    lv_obj_set_style_border_color(s_config_dialog, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(s_config_dialog, 1, 0);
    lv_obj_set_style_radius(s_config_dialog, 12, 0);
    lv_obj_set_style_pad_all(s_config_dialog, 0, 0);
    lv_obj_clear_flag(s_config_dialog, LV_OBJ_FLAG_SCROLLABLE);

    int title_h  = 48;
    int bottom_h = 64;
    int list_h   = dlg_h - title_h - 1 - bottom_h;

    lv_obj_t *title_bar = lv_obj_create(s_config_dialog);
    lv_obj_set_size(title_bar, dlg_w, title_h);
    lv_obj_set_pos(title_bar, 0, 0);
    lv_obj_set_style_bg_color(title_bar, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_width(title_bar, 0, 0);
    lv_obj_set_style_radius(title_bar, 0, 0);
    lv_obj_set_style_pad_hor(title_bar, 16, 0);
    lv_obj_clear_flag(title_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_lbl = lv_label_create(title_bar);
    lv_label_set_text(title_lbl, "Select Config");
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *div = lv_obj_create(s_config_dialog);
    lv_obj_set_size(div, dlg_w, 1);
    lv_obj_set_pos(div, 0, title_h);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_radius(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *list_cont = lv_obj_create(s_config_dialog);
    lv_obj_set_size(list_cont, dlg_w, list_h);
    lv_obj_set_pos(list_cont, 0, title_h + 1);
    lv_obj_set_style_bg_opa(list_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list_cont, 0, 0);
    lv_obj_set_style_pad_all(list_cont, 12, 0);
    lv_obj_set_style_pad_row(list_cont, 8, 0);
    lv_obj_set_layout(list_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(list_cont, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < CFG_ROWS_PER_PAGE; i++) {
        lv_obj_t *row = lv_btn_create(list_cont);
        lv_obj_set_width(row, LV_PCT(100));
        lv_obj_set_height(row, 48);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CHECKABLE);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x2a2a2a), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x3a3a3a), LV_STATE_PRESSED);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x3a3a3a), LV_STATE_CHECKED);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_add_event_cb(row, config_item_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xcccccc), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);

        s_config_rows[i]     = row;
        s_config_row_lbls[i] = lbl;
    }

    lv_obj_t *bottom_bar = lv_obj_create(s_config_dialog);
    lv_obj_set_size(bottom_bar, dlg_w, bottom_h);
    lv_obj_set_pos(bottom_bar, 0, dlg_h - bottom_h);
    lv_obj_set_style_bg_color(bottom_bar, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_width(bottom_bar, 0, 0);
    lv_obj_set_style_radius(bottom_bar, 0, 0);
    lv_obj_set_style_pad_hor(bottom_bar, 16, 0);
    lv_obj_set_style_pad_ver(bottom_bar, 0, 0);
    lv_obj_clear_flag(bottom_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_config_btn_prev = lv_btn_create(bottom_bar);
    lv_obj_set_size(s_config_btn_prev, 48, 40);
    lv_obj_align(s_config_btn_prev, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_config_btn_prev, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_color(s_config_btn_prev, lv_color_hex(0x4a4a4a), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_config_btn_prev, 0, 0);
    lv_obj_set_style_radius(s_config_btn_prev, 6, 0);
    lv_obj_add_event_cb(s_config_btn_prev, config_prev_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_prev = lv_label_create(s_config_btn_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_UP);
    lv_obj_set_style_text_color(lbl_prev, lv_color_hex(0xcccccc), 0);
    lv_obj_center(lbl_prev);

    s_config_page_lbl = lv_label_create(bottom_bar);
    lv_obj_set_style_text_color(s_config_page_lbl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_config_page_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_config_page_lbl, LV_ALIGN_LEFT_MID, 64, 0);

    s_config_btn_next = lv_btn_create(bottom_bar);
    lv_obj_set_size(s_config_btn_next, 48, 40);
    lv_obj_align(s_config_btn_next, LV_ALIGN_LEFT_MID, 112, 0);
    lv_obj_set_style_bg_color(s_config_btn_next, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_color(s_config_btn_next, lv_color_hex(0x4a4a4a), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_config_btn_next, 0, 0);
    lv_obj_set_style_radius(s_config_btn_next, 6, 0);
    lv_obj_add_event_cb(s_config_btn_next, config_next_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_next = lv_label_create(s_config_btn_next);
    lv_label_set_text(lbl_next, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_color(lbl_next, lv_color_hex(0xcccccc), 0);
    lv_obj_center(lbl_next);

    lv_obj_t *btn_cancel = lv_btn_create(bottom_bar);
    lv_obj_set_size(btn_cancel, 120, 40);
    lv_obj_align(btn_cancel, LV_ALIGN_RIGHT_MID, -136, 0);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x4a4a4a), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn_cancel, 0, 0);
    lv_obj_set_style_radius(btn_cancel, 6, 0);
    lv_obj_add_event_cb(btn_cancel, config_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_set_style_text_color(lbl_cancel, lv_color_hex(0xcccccc), 0);
    lv_obj_center(lbl_cancel);

    s_config_confirm = lv_btn_create(bottom_bar);
    lv_obj_set_size(s_config_confirm, 120, 40);
    lv_obj_align(s_config_confirm, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_config_confirm, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(s_config_confirm, lv_color_hex(0x4a4a4a), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_config_confirm, 0, 0);
    lv_obj_set_style_radius(s_config_confirm, 6, 0);
    lv_obj_add_state(s_config_confirm, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_config_confirm, config_confirm_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl_confirm = lv_label_create(s_config_confirm);
    lv_label_set_text(lbl_confirm, "Confirm");
    lv_obj_set_style_text_color(lbl_confirm, lv_color_hex(0x555555), 0);
    lv_obj_center(lbl_confirm);

    /* Pre-select the currently active config if found in scan result */
    char nvs_fname[UI_CONFIG_FNAME_LEN];
    if (ui_config_nvs_load(nvs_fname, sizeof(nvs_fname))) {
        for (int i = 0; i < s_scan_res.count; i++) {
            if (strcmp(s_scan_res.names[i], nvs_fname) == 0) {
                s_selected_idx = i;
                s_cfg_page     = i / CFG_ROWS_PER_PAGE;
                break;
            }
        }
    }

    config_render_page();
}

/* -----------------------------------------------------------------------
 * Keyboard mode
 *
 * Grid constants:
 *   KEY_W / KEY_H = 54
 *   GAP           = 4
 *   STEP          = 58  (KEY_W + GAP)
 *   PAD           = 20  (outer padding, same on all four sides)
 *
 * Column X offsets (relative to panel, include PAD):
 *   col 0  = PAD + 0*STEP  = 20
 *   col 1  = PAD + 1*STEP  = 78
 *   col 2  = PAD + 2*STEP  = 136
 *   ...
 *   col 10 = PAD + 10*STEP = 600
 *   col 11 = PAD + 11*STEP = 658   <- Bksp / Enter right edge / ArrowUp / ArrowRight
 *
 * Row Y offsets:
 *   row 0  = PAD + 0*STEP  = 20
 *   row 1  = PAD + 1*STEP  = 78
 *   row 2  = PAD + 2*STEP  = 136
 *   row 3  = PAD + 3*STEP  = 194
 *
 * Panel size:
 *   W = PAD + 11*STEP + KEY_W + PAD = 20 + 638 + 54 + 20 = 732
 *   H = PAD + 3*STEP  + KEY_H + PAD = 20 + 174 + 54 + 20 = 268
 *
 * Special key widths:
 *   Shift  = 2*KEY_W + GAP = 112   (col 0, spans col 0-1)
 *   Enter  = 2*KEY_W + GAP = 112   (col 10, spans col 10-11)
 *   Bksp   = KEY_W         = 54    (col 11)
 *   Space  = 7*KEY_W+6*GAP = 402   (col 0, spans col 0-6)
 *   ABC    = KEY_W         = 54    (col 7)
 *   !@#    = KEY_W         = 54    (col 8)
 *
 * Key encoding sent via usb_hid_send(0x00, key_byte):
 *   bit[7] = Shift modifier
 *   bit[6:0] = USB HID keycode
 * ----------------------------------------------------------------------- */
#define KB_PAD      20
#define KB_KEY_W    54
#define KB_KEY_H    54
#define KB_GAP      4
#define KB_STEP     58   /* KB_KEY_W + KB_GAP */
#define KB_COL(n)   (KB_PAD + (n) * KB_STEP)
#define KB_ROW(n)   (KB_PAD + (n) * KB_STEP)
#define KB_PANEL_W  (KB_PAD + 11 * KB_STEP + KB_KEY_W + KB_PAD)  /* 732 */
#define KB_PANEL_H  (KB_PAD +  3 * KB_STEP + KB_KEY_H + KB_PAD)  /* 268 */
#define KB_RIGHT_W   160
#define KB_LEFT_W    (SCREEN_W - KB_RIGHT_W)   /* 640 */
#define KB_ROWS      4
#define KB_GAP       4

static lv_obj_t *s_context_panel = NULL;
static lv_obj_t *s_overlay       = NULL;
static lv_obj_t *s_sidebar       = NULL;  /* static sidebar strip, kept for z-order restore */
static lv_obj_t *s_sidebar_pages = NULL;  /* config-dependent page buttons inside s_sidebar */

/* -----------------------------------------------------------------------
 * PSRAM image pre-decode pool
 * ----------------------------------------------------------------------- */
typedef struct {
    char         key[UI_CONFIG_BG_LEN + 16];
    lv_img_dsc_t dsc;
    bool         valid;
} psram_img_t;

static psram_img_t *s_img_pool     = NULL;
static int          s_img_pool_cap = 0;
static int          s_img_pool_n   = 0;

static volatile bool  s_preload_done    = false;
static volatile bool  s_preload_started = false;
static TaskHandle_t   s_preload_caller  = NULL;

static lv_img_dsc_t *img_pool_find(const char *path)
{
    for (int i = 0; i < s_img_pool_n; i++)
        if (s_img_pool[i].valid && strcmp(s_img_pool[i].key, path) == 0)
            return &s_img_pool[i].dsc;
    return NULL;
}

/* Decode image from SD card into a PSRAM buffer. Returns cached descriptor
 * or NULL on failure. Deduplicates: same path decoded only once. */
static lv_img_dsc_t *img_pool_decode(const char *path)
{
    lv_img_dsc_t *hit = img_pool_find(path);
    if (hit) return hit;

    if (s_img_pool_n >= s_img_pool_cap) {
        ESP_LOGW("IMG", "pool full, skipping %s", path);
        return NULL;
    }

    lv_img_decoder_dsc_t dec;
    memset(&dec, 0, sizeof(dec));
    if (lv_img_decoder_open(&dec, path, lv_color_white(), 0) != LV_RES_OK) {
        ESP_LOGW("IMG", "open failed: %s", path);
        return NULL;
    }

    uint32_t    w  = dec.header.w;
    uint32_t    h  = dec.header.h;
    lv_img_cf_t cf = dec.header.cf;
    uint8_t     px = lv_img_cf_get_px_size(cf) / 8;

    /* JPEG uses LV_IMG_CF_RAW (streamed via read_line), px_size == 0.
     * read_line always outputs TRUE_COLOR pixels, so fix up cf and px. */
    if (px == 0) {
        cf = LV_IMG_CF_TRUE_COLOR;
        px = sizeof(lv_color_t);
    }

    size_t sz = (size_t)w * h * px;

    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE("IMG", "PSRAM OOM %u KB: %s", (unsigned)(sz / 1024), path);
        lv_img_decoder_close(&dec);
        return NULL;
    }

    bool ok = true;
    if (dec.img_data) {
        memcpy(buf, dec.img_data, sz);
    } else {
        size_t stride = (size_t)w * px;
        for (lv_coord_t y = 0; y < (lv_coord_t)h && ok; y++) {
            if (lv_img_decoder_read_line(&dec, 0, y, (lv_coord_t)w,
                                         buf + (size_t)y * stride) != LV_RES_OK) {
                ESP_LOGE("IMG", "read_line failed row %d: %s", y, path);
                ok = false;
            }
        }
    }

    lv_img_decoder_close(&dec);

    if (!ok) {
        heap_caps_free(buf);
        return NULL;
    }

    psram_img_t *e        = &s_img_pool[s_img_pool_n++];
    snprintf(e->key, sizeof(e->key), "%s", path);
    e->dsc.header.cf          = cf;
    e->dsc.header.always_zero = 0;
    e->dsc.header.reserved    = 0;
    e->dsc.header.w           = w;
    e->dsc.header.h           = h;
    e->dsc.data_size          = sz;
    e->dsc.data               = buf;
    e->valid                  = true;

    ESP_LOGI("IMG", "cached %s [%ux%u %u KB]", path, (unsigned)w, (unsigned)h, (unsigned)(sz / 1024));
    return &e->dsc;
}


/* Free all PSRAM pixel buffers and reset pool state. */
static void img_pool_free(void)
{
    for (int i = 0; i < s_img_pool_n; i++) {
        if (s_img_pool[i].valid && s_img_pool[i].dsc.data) {
            heap_caps_free((void *)s_img_pool[i].dsc.data);
            s_img_pool[i].dsc.data = NULL;
        }
    }
    free(s_img_pool);
    s_img_pool     = NULL;
    s_img_pool_cap = 0;
    s_img_pool_n   = 0;
}

/* Allocate pool metadata and decode all images referenced by cfg.
 * Called both from the boot preload task and from the config switch flow. */
static void img_pool_load(const deck_cfg_t *cfg)
{
    int cap = 0;
    for (int p = 0; p < cfg->page_count; p++) {
        if (cfg->pages[p].bg_image[0]) cap++;
        cap += cfg->pages[p].button_count;
    }
    if (cap == 0) return;

    s_img_pool     = calloc((size_t)cap, sizeof(psram_img_t));
    s_img_pool_cap = cap;

    for (int p = 0; p < cfg->page_count; p++) {
        if (!cfg->pages[p].bg_image[0]) continue;
        char path[UI_CONFIG_BG_LEN + 12];
        snprintf(path, sizeof(path), "S:%s/%s",
                 UI_CONFIG_BG_PATH, cfg->pages[p].bg_image);
        FILE *f = fopen(path + 2, "r");
        if (!f) { ESP_LOGW("IMG", "bg not found: %s", path); continue; }
        fclose(f);
        img_pool_decode(path);
    }

    for (int p = 0; p < cfg->page_count; p++) {
        for (int b = 0; b < cfg->pages[p].button_count; b++) {
            if (!cfg->pages[p].buttons[b].icon[0]) continue;
            char path[UI_CONFIG_ICON_LEN + 12];
            snprintf(path, sizeof(path), "S:%s/%s",
                     UI_CONFIG_ICON_PATH, cfg->pages[p].buttons[b].icon);
            FILE *f = fopen(path + 2, "r");
            if (!f) continue;
            fclose(f);
            img_pool_decode(path);
        }
    }

    ESP_LOGI("IMG", "pool loaded - %d cached, PSRAM free: %d B",
             s_img_pool_n, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* Background preload task: acquires LVGL lock once per image so the boot
 * animation can run in between. Self-deletes when finished. */
static void preload_task_fn(void *arg)
{
    img_pool_load(&s_cfg);

    ESP_LOGI("IMG", "preload done - %d cached, PSRAM free: %d B",
             s_img_pool_n, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    s_preload_done = true;
    if (s_preload_caller) xTaskNotifyGive(s_preload_caller);
    vTaskDelete(NULL);
}

/* Call before boot_anim_play(). Loads config and starts background preload. */
void ui_preload_start(void)
{
    if (s_preload_started) return;
    s_preload_started = true;

    bool cfg_ok = ui_config_load(&s_cfg);
    if (!cfg_ok || s_cfg.page_count == 0) {
        s_cfg.page_count = 1;
        s_cfg.pages      = calloc(1, sizeof(page_cfg_t));
        snprintf(s_cfg.pages[0].name, UI_CONFIG_NAME_LEN, "Main");
    }

    s_preload_caller = xTaskGetCurrentTaskHandle();
    xTaskCreate(preload_task_fn, "img_preload", 8192, NULL, 3, NULL);
}

/* Call after boot_anim_play() and before my_ui_init(). Blocks until done. */
void ui_preload_wait(void)
{
    if (s_preload_started && !s_preload_done)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

/* -----------------------------------------------------------------------
 * Deck state
 * ----------------------------------------------------------------------- */

/*
 * s_deck_root: single parent for all config-dependent widgets.
 * Deleting it tears down sidebar pages + content pages in one call.
 */
static lv_obj_t  *s_deck_root    = NULL;
static lv_obj_t **s_pages        = NULL;
static lv_obj_t **s_sidebar_btns = NULL;
static int        s_cur_page     = 0;
static int        s_page_count   = 0;

static lv_obj_t *s_keyboard_screen = NULL;
static lv_obj_t *s_kb_abc_cont     = NULL;
static lv_obj_t *s_kb_sym_cont     = NULL;
static lv_obj_t *s_kb_tab_abc      = NULL;
static lv_obj_t *s_kb_tab_sym      = NULL;
static lv_obj_t *s_kb_shift_btn    = NULL;
static bool      s_kb_shift_active = false;
static lv_obj_t *s_kb_letter_lbls[26];  /* a-z label objects */
static lv_obj_t *s_kb_sym_tab_abc = NULL;
static lv_obj_t *s_kb_sym_tab_sym = NULL;
static lv_obj_t *s_kb_sym_shift_btn    = NULL;
static bool      s_kb_sym_shift_active = false;
static lv_obj_t *s_kb_sym_digit_lbls[10];   /* Row 0: 1-0 labels */
static lv_obj_t *s_kb_sym_punct_lbls[10];   /* Row 1: `[]\;',./ labels */

static lv_obj_t *kb_btn(lv_obj_t *parent, int x, int y, int w, int h,
                          const char *text, uint8_t keycode, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x585858), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(uintptr_t)keycode);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xeeeeee), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);

    return btn;
}

/* Normal single key */
#define KB_KEY(parent, col, row, text, kc, cb) \
    kb_btn(parent, KB_COL(col), KB_ROW(row), KB_KEY_W, KB_KEY_H, text, kc, cb)

static void kb_key_cb(lv_event_t *e)
{
    uint8_t keycode  = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    uint8_t key_byte = keycode;

    if (s_kb_shift_active) {
        key_byte |= 0x80;
        s_kb_shift_active = false;
        if (s_kb_shift_btn) {
            lv_obj_clear_state(s_kb_shift_btn, LV_STATE_CHECKED);
        }
        static const char *lower[] = {
            "q","w","e","r","t","y","u","i","o","p",
            "a","s","d","f","g","h","j","k","l",
            "z","x","c","v","b","n","m"
        };
        for (int i = 0; i < 26; i++) {
            if (s_kb_letter_lbls[i]) {
                lv_label_set_text(s_kb_letter_lbls[i], lower[i]);
            }
        }
    } else if (s_kb_sym_shift_active) {
        key_byte |= 0x80;
        s_kb_sym_shift_active = false;
        if (s_kb_sym_shift_btn) {
            lv_obj_clear_state(s_kb_sym_shift_btn, LV_STATE_CHECKED);
        }
        static const char *digits_normal[] = {
            "1","2","3","4","5","6","7","8","9","0"
        };
        static const char *punct_normal[] = {
            "`","[","]","\\",";","'",",",".","/",
        };
        for (int i = 0; i < 10; i++) {
            if (s_kb_sym_digit_lbls[i]) {
                lv_label_set_text(s_kb_sym_digit_lbls[i], digits_normal[i]);
            }
        }
        for (int i = 0; i < 9; i++) {
            if (s_kb_sym_punct_lbls[i]) {
                lv_label_set_text(s_kb_sym_punct_lbls[i], punct_normal[i]);
            }
        }
    }

    usb_hid_send(0x00, key_byte);
    ESP_LOGI("KB", "key=0x%02X shift=%d", keycode, (key_byte >> 7) & 1);
}

static void kb_fixed_key_cb(lv_event_t *e)
{
    uint8_t keycode = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    usb_hid_send(0x00, keycode);
    ESP_LOGI("KB", "fixed=0x%02X", keycode);
}

static void kb_shift_cb(lv_event_t *e)
{
    static const char *lower[] = {
        "q","w","e","r","t","y","u","i","o","p",
        "a","s","d","f","g","h","j","k","l",
        "z","x","c","v","b","n","m"
    };
    static const char *upper[] = {
        "Q","W","E","R","T","Y","U","I","O","P",
        "A","S","D","F","G","H","J","K","L",
        "Z","X","C","V","B","N","M"
    };
 
    s_kb_shift_active = !s_kb_shift_active;
 
    if (s_kb_shift_btn) {
        if (s_kb_shift_active) {
            lv_obj_add_state(s_kb_shift_btn, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_kb_shift_btn, LV_STATE_CHECKED);
        }
    }
 
    const char **labels = s_kb_shift_active ? upper : lower;
    for (int i = 0; i < 26; i++) {
        if (s_kb_letter_lbls[i]) {
            lv_label_set_text(s_kb_letter_lbls[i], labels[i]);
        }
    }
}

static void keyboard_exit_cb(lv_event_t *e)
{
    if (s_keyboard_screen) {
        lv_obj_del(s_keyboard_screen);
        s_keyboard_screen = NULL;
        s_kb_abc_cont     = NULL;
        s_kb_sym_cont     = NULL;
        s_kb_tab_abc      = NULL;
        s_kb_tab_sym      = NULL;
        s_kb_shift_btn    = NULL;
        s_kb_shift_active = false;
        s_kb_sym_tab_abc  = NULL;
        s_kb_sym_tab_sym  = NULL;
    }
}

/* -----------------------------------------------------------------------
 * kb_sym_shift_cb
 * Toggles symbol page shift — updates Row 0 and Row 1 labels only
 * ----------------------------------------------------------------------- */
static void kb_sym_shift_cb(lv_event_t *e)
{
    static const char *digits_normal[] = { "1","2","3","4","5","6","7","8","9","0" };
    static const char *digits_shift[]  = { "!","@","#","$","%","^","&","*","(",")" };
    static const char *punct_normal[]  = { "`","[","]","\\",";","'",",",".","/","" };
    static const char *punct_shift[]   = { "~","{","}","|",":","\""," <",">","?","" };
 
    s_kb_sym_shift_active = !s_kb_sym_shift_active;
 
    if (s_kb_sym_shift_btn) {
        if (s_kb_sym_shift_active) {
            lv_obj_add_state(s_kb_sym_shift_btn, LV_STATE_CHECKED);
        } else {
            lv_obj_clear_state(s_kb_sym_shift_btn, LV_STATE_CHECKED);
        }
    }
 
    const char **dlabels = s_kb_sym_shift_active ? digits_shift : digits_normal;
    const char **plabels = s_kb_sym_shift_active ? punct_shift  : punct_normal;
 
    for (int i = 0; i < 10; i++) {
        if (s_kb_sym_digit_lbls[i]) {
            lv_label_set_text(s_kb_sym_digit_lbls[i], dlabels[i]);
        }
    }
    for (int i = 0; i < 9; i++) {
        if (s_kb_sym_punct_lbls[i]) {
            lv_label_set_text(s_kb_sym_punct_lbls[i], plabels[i]);
        }
    }
}
 
/* -----------------------------------------------------------------------
 * Helper: reset symbol page shift
 * ----------------------------------------------------------------------- */
static void kb_sym_shift_reset(void)
{
    static const char *digits_normal[] = { "1","2","3","4","5","6","7","8","9","0" };
    static const char *punct_normal[]  = { "`","[","]","\\",";","'",",",".","/","" };
 
    if (!s_kb_sym_shift_active) return;
 
    s_kb_sym_shift_active = false;
    if (s_kb_sym_shift_btn) {
        lv_obj_clear_state(s_kb_sym_shift_btn, LV_STATE_CHECKED);
    }
    for (int i = 0; i < 10; i++) {
        if (s_kb_sym_digit_lbls[i]) {
            lv_label_set_text(s_kb_sym_digit_lbls[i], digits_normal[i]);
        }
    }
    for (int i = 0; i < 9; i++) {
        if (s_kb_sym_punct_lbls[i]) {
            lv_label_set_text(s_kb_sym_punct_lbls[i], punct_normal[i]);
        }
    }
}
 
/* -----------------------------------------------------------------------
 * Helper: reset ABC page shift
 * ----------------------------------------------------------------------- */
static void kb_abc_shift_reset(void)
{
    static const char *lower[] = {
        "q","w","e","r","t","y","u","i","o","p",
        "a","s","d","f","g","h","j","k","l",
        "z","x","c","v","b","n","m"
    };
 
    if (!s_kb_shift_active) return;
 
    s_kb_shift_active = false;
    if (s_kb_shift_btn) {
        lv_obj_clear_state(s_kb_shift_btn, LV_STATE_CHECKED);
    }
    for (int i = 0; i < 26; i++) {
        if (s_kb_letter_lbls[i]) {
            lv_label_set_text(s_kb_letter_lbls[i], lower[i]);
        }
    }
}

/* -----------------------------------------------------------------------
 * kb_tab_abc_cb  — switch to ABC page, reset sym shift
 * ----------------------------------------------------------------------- */
static void kb_tab_abc_cb(lv_event_t *e)
{
    kb_sym_shift_reset();
 
    lv_obj_clear_flag(s_kb_abc_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_kb_sym_cont, LV_OBJ_FLAG_HIDDEN);
 
    lv_obj_add_state(s_kb_tab_abc, LV_STATE_CHECKED);
    lv_obj_clear_state(s_kb_tab_sym, LV_STATE_CHECKED);
    if (s_kb_sym_tab_abc) lv_obj_add_state(s_kb_sym_tab_abc, LV_STATE_CHECKED);
    if (s_kb_sym_tab_sym) lv_obj_clear_state(s_kb_sym_tab_sym, LV_STATE_CHECKED);
}

/* -----------------------------------------------------------------------
 * kb_tab_sym_cb  — switch to symbol page, reset ABC shift
 * ----------------------------------------------------------------------- */
static void kb_tab_sym_cb(lv_event_t *e)
{
    kb_abc_shift_reset();
 
    lv_obj_add_flag(s_kb_abc_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_kb_sym_cont, LV_OBJ_FLAG_HIDDEN);
 
    lv_obj_clear_state(s_kb_tab_abc, LV_STATE_CHECKED);
    lv_obj_add_state(s_kb_tab_sym, LV_STATE_CHECKED);
    if (s_kb_sym_tab_abc) lv_obj_clear_state(s_kb_sym_tab_abc, LV_STATE_CHECKED);
    if (s_kb_sym_tab_sym) lv_obj_add_state(s_kb_sym_tab_sym, LV_STATE_CHECKED);
}

static void kb_build_abc(lv_obj_t *cont)
{
    /* Letter keycodes in layout order (q-p, a-l, z-m) */
    static const uint8_t letter_kc[] = {
        0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, 0x12, 0x13,  /* q-p */
        0x04, 0x16, 0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F,         /* a-l */
        0x1D, 0x1B, 0x06, 0x19, 0x05, 0x11, 0x10                       /* z-m */
    };
    static const char *lower[] = {
        "q","w","e","r","t","y","u","i","o","p",
        "a","s","d","f","g","h","j","k","l",
        "z","x","c","v","b","n","m"
    };
 
    /* Row 0 col positions for q-p */
    static const int row0_col[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
    /* Row 1 col positions for a-l */
    static const int row1_col[] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    /* Row 2 col positions for z-m */
    static const int row2_col[] = { 2, 3, 4, 5, 6, 7, 8 };
 
    /* ----------------------------------------------------------------
     * Row 0: Esc q-p Bksp
     * ---------------------------------------------------------------- */
    KB_KEY(cont, 0, 0, "Esc", 0x29, kb_key_cb);
 
    for (int i = 0; i < 10; i++) {
        lv_obj_t *btn = kb_btn(cont,
                               KB_COL(row0_col[i]), KB_ROW(0),
                               KB_KEY_W, KB_KEY_H,
                               lower[i], letter_kc[i], kb_key_cb);
        s_kb_letter_lbls[i] = lv_obj_get_child(btn, 0);
    }
 
    KB_KEY(cont, 11, 0, LV_SYMBOL_BACKSPACE, 0x2A, kb_fixed_key_cb);
 
    /* ----------------------------------------------------------------
     * Row 1: Tab a-l Enter
     * ---------------------------------------------------------------- */
    KB_KEY(cont, 0, 1, "Tab", 0x2B, kb_key_cb);
 
    for (int i = 0; i < 9; i++) {
        lv_obj_t *btn = kb_btn(cont,
                               KB_COL(row1_col[i]), KB_ROW(1),
                               KB_KEY_W, KB_KEY_H,
                               lower[10 + i], letter_kc[10 + i], kb_key_cb);
        s_kb_letter_lbls[10 + i] = lv_obj_get_child(btn, 0);
    }
 
    kb_btn(cont, KB_COL(10), KB_ROW(1),
           KB_KEY_W * 2 + KB_GAP, KB_KEY_H,
           LV_SYMBOL_NEW_LINE, 0x28, kb_fixed_key_cb);
 
    /* ----------------------------------------------------------------
     * Row 2: Shift z-m ArrowUp
     * ---------------------------------------------------------------- */
    s_kb_shift_btn = kb_btn(cont, KB_COL(0), KB_ROW(2),
                             KB_KEY_W * 2 + KB_GAP, KB_KEY_H,
                             LV_SYMBOL_UP " Shift", 0x00, kb_shift_cb);
    lv_obj_add_flag(s_kb_shift_btn, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(s_kb_shift_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_color(s_kb_shift_btn, lv_color_hex(0x777777), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_kb_shift_btn, lv_color_hex(0x0055cc), LV_STATE_CHECKED);
 
    for (int i = 0; i < 7; i++) {
        lv_obj_t *btn = kb_btn(cont,
                               KB_COL(row2_col[i]), KB_ROW(2),
                               KB_KEY_W, KB_KEY_H,
                               lower[19 + i], letter_kc[19 + i], kb_key_cb);
        s_kb_letter_lbls[19 + i] = lv_obj_get_child(btn, 0);
    }
 
    KB_KEY(cont, 10, 2, LV_SYMBOL_UP, 0x52, kb_fixed_key_cb);
 
    /* ----------------------------------------------------------------
     * Row 3: Space ABC !@# ArrowLeft ArrowDown ArrowRight
     * ---------------------------------------------------------------- */
    kb_btn(cont, KB_COL(0), KB_ROW(3),
           KB_KEY_W * 7 + KB_GAP * 6, KB_KEY_H,
           "Space", 0x2C, kb_fixed_key_cb);
 
    s_kb_tab_abc = kb_btn(cont, KB_COL(7), KB_ROW(3),
                           KB_KEY_W, KB_KEY_H, "ABC", 0x00, kb_tab_abc_cb);
    lv_obj_add_flag(s_kb_tab_abc, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_state(s_kb_tab_abc, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_kb_tab_abc, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_color(s_kb_tab_abc, lv_color_hex(0x0055cc), LV_STATE_CHECKED);
 
    s_kb_tab_sym = kb_btn(cont, KB_COL(8), KB_ROW(3),
                           KB_KEY_W, KB_KEY_H, "!@#", 0x00, kb_tab_sym_cb);
    lv_obj_add_flag(s_kb_tab_sym, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(s_kb_tab_sym, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_color(s_kb_tab_sym, lv_color_hex(0x0055cc), LV_STATE_CHECKED);
 
    KB_KEY(cont,  9, 3, LV_SYMBOL_LEFT,  0x50, kb_fixed_key_cb);
    KB_KEY(cont, 10, 3, LV_SYMBOL_DOWN,  0x51, kb_fixed_key_cb);
    KB_KEY(cont, 11, 3, LV_SYMBOL_RIGHT, 0x4F, kb_fixed_key_cb);
}

/* -----------------------------------------------------------------------
 * kb_build_sym
 * Row 0: Esc + 1-0 (shift: !@#$%^&*() ) + Bksp
 * Row 1: Tab + `[]\;',./ (shift: ~{}|:"<>?) + Enter
 * Row 2: Shift(2u wide) + ArrowUp(col10)
 * Row 3: Space + ABC + !@# + arrows
 * ----------------------------------------------------------------------- */
static void kb_build_sym(lv_obj_t *cont)
{
    /* ----------------------------------------------------------------
     * Row 0: Esc(col0)  1-0(col1~10)  Bksp(col11)
     * ---------------------------------------------------------------- */
    KB_KEY(cont, 0, 0, "Esc", 0x29, kb_key_cb);
 
    static const uint8_t digit_kc[] = {
        0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27
    };
    static const char *digits_normal[] = {
        "1","2","3","4","5","6","7","8","9","0"
    };
    for (int i = 0; i < 10; i++) {
        lv_obj_t *btn = kb_btn(cont,
                               KB_COL(i + 1), KB_ROW(0),
                               KB_KEY_W, KB_KEY_H,
                               digits_normal[i], digit_kc[i], kb_key_cb);
        s_kb_sym_digit_lbls[i] = lv_obj_get_child(btn, 0);
    }
 
    KB_KEY(cont, 11, 0, LV_SYMBOL_BACKSPACE, 0x2A, kb_fixed_key_cb);
 
    /* ----------------------------------------------------------------
     * Row 1: Tab(col0)  `[]\;',./(col1~9)  Enter(col10~11)
     * ---------------------------------------------------------------- */
    KB_KEY(cont, 0, 1, "Tab", 0x2B, kb_key_cb);
 
    static const uint8_t punct_kc[] = {
        0x35, 0x2F, 0x30, 0x31, 0x33, 0x34, 0x36, 0x37, 0x38
    };
    static const char *punct_normal[] = {
        "`","[","]","\\",";","'",",",".","/",
    };
    for (int i = 0; i < 9; i++) {
        lv_obj_t *btn = kb_btn(cont,
                               KB_COL(i + 1), KB_ROW(1),
                               KB_KEY_W, KB_KEY_H,
                               punct_normal[i], punct_kc[i], kb_key_cb);
        s_kb_sym_punct_lbls[i] = lv_obj_get_child(btn, 0);
    }
 
    kb_btn(cont, KB_COL(10), KB_ROW(1),
           KB_KEY_W * 2 + KB_GAP, KB_KEY_H,
           LV_SYMBOL_NEW_LINE, 0x28, kb_fixed_key_cb);
 
    /* ----------------------------------------------------------------
     * Row 2: Shift(col0~1)  [col2~9 empty]  ArrowUp(col10)
     * ---------------------------------------------------------------- */
    s_kb_sym_shift_btn = kb_btn(cont, KB_COL(0), KB_ROW(2),
                                 KB_KEY_W * 2 + KB_GAP, KB_KEY_H,
                                 LV_SYMBOL_UP " Shift", 0x00, kb_sym_shift_cb);
    lv_obj_add_flag(s_kb_sym_shift_btn, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(s_kb_sym_shift_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_color(s_kb_sym_shift_btn, lv_color_hex(0x777777), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_kb_sym_shift_btn, lv_color_hex(0x0055cc), LV_STATE_CHECKED);
 
    KB_KEY(cont, 10, 2, LV_SYMBOL_UP, 0x52, kb_fixed_key_cb);
 
    /* ----------------------------------------------------------------
     * Row 3: Space(col0~6) ABC(col7) !@#(col8) arrows(col9~11)
     * ---------------------------------------------------------------- */
    kb_btn(cont, KB_COL(0), KB_ROW(3),
           KB_KEY_W * 7 + KB_GAP * 6, KB_KEY_H,
           "Space", 0x2C, kb_fixed_key_cb);
 
    s_kb_sym_tab_abc = kb_btn(cont, KB_COL(7), KB_ROW(3),
                               KB_KEY_W, KB_KEY_H, "ABC", 0x00, kb_tab_abc_cb);
    lv_obj_add_flag(s_kb_sym_tab_abc, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(s_kb_sym_tab_abc, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_color(s_kb_sym_tab_abc, lv_color_hex(0x0055cc), LV_STATE_CHECKED);
 
    s_kb_sym_tab_sym = kb_btn(cont, KB_COL(8), KB_ROW(3),
                               KB_KEY_W, KB_KEY_H, "!@#", 0x00, kb_tab_sym_cb);
    lv_obj_add_flag(s_kb_sym_tab_sym, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_state(s_kb_sym_tab_sym, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_kb_sym_tab_sym, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_color(s_kb_sym_tab_sym, lv_color_hex(0x0055cc), LV_STATE_CHECKED);
 
    KB_KEY(cont,  9, 3, LV_SYMBOL_LEFT,  0x50, kb_fixed_key_cb);
    KB_KEY(cont, 10, 3, LV_SYMBOL_DOWN,  0x51, kb_fixed_key_cb);
    KB_KEY(cont, 11, 3, LV_SYMBOL_RIGHT, 0x4F, kb_fixed_key_cb);
}

static void show_keyboard(void)
{
    lv_obj_t *scr = lv_scr_act();

    /* Full-screen overlay — dim background, click outside panel to exit */
    s_keyboard_screen = lv_obj_create(scr);
    lv_obj_set_size(s_keyboard_screen, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_keyboard_screen, 0, 0);
    lv_obj_set_style_bg_color(s_keyboard_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_keyboard_screen, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_keyboard_screen, 0, 0);
    lv_obj_set_style_radius(s_keyboard_screen, 0, 0);
    lv_obj_clear_flag(s_keyboard_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_keyboard_screen, keyboard_exit_cb, LV_EVENT_CLICKED, NULL);

    /* Centered panel */
    lv_obj_t *panel = lv_obj_create(s_keyboard_screen);
    lv_obj_set_size(panel, KB_PANEL_W, KB_PANEL_H);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1e1e1e), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(panel, lv_event_stop_processing, LV_EVENT_CLICKED, NULL);

    /* ABC page — visible by default */
    s_kb_abc_cont = lv_obj_create(panel);
    lv_obj_set_size(s_kb_abc_cont, KB_PANEL_W, KB_PANEL_H);
    lv_obj_set_pos(s_kb_abc_cont, 0, 0);
    lv_obj_set_style_bg_opa(s_kb_abc_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_kb_abc_cont, 0, 0);
    lv_obj_set_style_pad_all(s_kb_abc_cont, 0, 0);
    lv_obj_clear_flag(s_kb_abc_cont, LV_OBJ_FLAG_SCROLLABLE);
    kb_build_abc(s_kb_abc_cont);

    /* Symbol page — hidden by default */
    s_kb_sym_cont = lv_obj_create(panel);
    lv_obj_set_size(s_kb_sym_cont, KB_PANEL_W, KB_PANEL_H);
    lv_obj_set_pos(s_kb_sym_cont, 0, 0);
    lv_obj_set_style_bg_opa(s_kb_sym_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_kb_sym_cont, 0, 0);
    lv_obj_set_style_pad_all(s_kb_sym_cont, 0, 0);
    lv_obj_clear_flag(s_kb_sym_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_kb_sym_cont, LV_OBJ_FLAG_HIDDEN);
    kb_build_sym(s_kb_sym_cont);
}

/* -----------------------------------------------------------------------
 * Context menu
 * ----------------------------------------------------------------------- */
static void keyboard_mode_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_context_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
    show_keyboard();
}

static void select_config_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_context_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
    show_select_config_dialog();
}

static void context_item_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_context_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
    show_confirm_dialog();
}

static void overlay_cb(lv_event_t *e)
{
    lv_obj_add_flag(s_context_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_del(s_overlay);
    s_overlay = NULL;
}

static void settings_btn_cb(lv_event_t *e)
{
    if (lv_obj_has_flag(s_context_panel, LV_OBJ_FLAG_HIDDEN)) {
        s_overlay = lv_obj_create(lv_scr_act());
        lv_obj_set_size(s_overlay, SCREEN_W, SCREEN_H);
        lv_obj_set_pos(s_overlay, 0, 0);
        lv_obj_set_style_bg_opa(s_overlay, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_overlay, 0, 0);
        lv_obj_add_event_cb(s_overlay, overlay_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_move_foreground(s_context_panel);
        lv_obj_clear_flag(s_context_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_context_panel, LV_OBJ_FLAG_HIDDEN);
        if (s_overlay) {
            lv_obj_del(s_overlay);
            s_overlay = NULL;
        }
    }
}

/* -----------------------------------------------------------------------
 * Button / sidebar callbacks
 * ----------------------------------------------------------------------- */
static void btn_event_cb(lv_event_t *e)
{
    /*
     * user_data encodes page and button as a single pointer-sized value:
     *   high byte = page index (0-based)
     *   low byte  = button index (0-based)
     * HID report adds 1 to each so 0x00 stays reserved for release.
     */
    uintptr_t packed = (uintptr_t)lv_event_get_user_data(e);
    uint8_t page = (uint8_t)((packed >> 8) & 0xFF);
    uint8_t btn  = (uint8_t)(packed & 0xFF);

    ESP_LOGI("BTN", "page=0x%02X btn=0x%02X", page + 1, btn + 1);
    usb_hid_send(page + 1, btn + 1);
}

static void sidebar_btn_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_set_style_bg_color(s_sidebar_btns[s_cur_page], lv_color_hex(0x2a2a2a), 0);
    lv_obj_add_flag(s_pages[s_cur_page], LV_OBJ_FLAG_HIDDEN);
    s_cur_page = idx;
    lv_obj_set_style_bg_color(s_sidebar_btns[s_cur_page], lv_color_hex(0x3a3a3a), 0);
    lv_obj_clear_flag(s_pages[s_cur_page], LV_OBJ_FLAG_HIDDEN);
}

/* -----------------------------------------------------------------------
 * Page / button creation
 * ----------------------------------------------------------------------- */
/*
 * create_page: returns the page root object.
 * The page itself uses no layout (manual positioning) so that background
 * and mask layers can be pinned at (0,0) independently of the button area.
 * A btn_container child carries the flex layout for buttons.
 * Returns the btn_container so create_buttons() can parent into it.
 */
static lv_obj_t *create_page(lv_obj_t *parent, const page_cfg_t *page_cfg, lv_obj_t **out_btn_cont)
{
    int page_w = SCREEN_W - SIDEBAR_W;
    int page_h = SCREEN_H;

    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, page_w, page_h);
    lv_obj_set_pos(page, SIDEBAR_W, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);

    /* Background image layer (optional) */
    if (page_cfg->bg_image[0] != '\0') {
        char bg_path[UI_CONFIG_BG_LEN + 12];
        snprintf(bg_path, sizeof(bg_path), "S:%s/%s",
                 UI_CONFIG_BG_PATH, page_cfg->bg_image);

        lv_img_dsc_t *cached = img_pool_find(bg_path);
        const void   *src    = cached ? (const void *)cached : (const void *)bg_path;
        bool          usable = (cached != NULL);
        if (!usable) {
            FILE *f = fopen(bg_path + 2, "r");
            if (f) { fclose(f); usable = true; }
        }

        if (usable) {
            lv_obj_t *bg = lv_img_create(page);
            lv_img_set_src(bg, src);
            lv_obj_set_pos(bg, 0, 0);
            lv_obj_set_size(bg, page_w, page_h);
            lv_img_set_zoom(bg, 256);
            lv_obj_add_flag(bg, LV_OBJ_FLAG_EVENT_BUBBLE);
            lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);
        } else {
            ESP_LOGW("UI", "bg_image not found: %s", bg_path);
        }

        /* Semi-transparent black mask over background */
        lv_obj_t *mask = lv_obj_create(page);
        lv_obj_set_size(mask, page_w, page_h);
        lv_obj_set_pos(mask, 0, 0);
        lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(mask, LV_OPA_50, 0);
        lv_obj_set_style_border_width(mask, 0, 0);
        lv_obj_set_style_radius(mask, 0, 0);
        lv_obj_add_flag(mask, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_clear_flag(mask, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Button container — carries the flex layout */
    lv_obj_t *btn_cont = lv_obj_create(page);
    lv_obj_set_size(btn_cont, page_w, page_h);
    lv_obj_set_pos(btn_cont, 0, 0);
    lv_obj_set_style_bg_opa(btn_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_cont, 0, 0);
    lv_obj_set_style_radius(btn_cont, 0, 0);
    lv_obj_set_style_pad_all(btn_cont, 8, 0);
    lv_obj_set_style_pad_row(btn_cont, 10, 0);
    lv_obj_set_style_pad_column(btn_cont, 10, 0);
    lv_obj_set_layout(btn_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn_cont, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(btn_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(btn_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(btn_cont, LV_SCROLLBAR_MODE_OFF);

    if (out_btn_cont) *out_btn_cont = btn_cont;
    return page;
}

static void create_buttons(lv_obj_t *page, int page_idx, const page_cfg_t *page_cfg)
{
    for (int i = 0; i < page_cfg->button_count; i++) {
        const btn_cfg_t *bcfg = &page_cfg->buttons[i];

        lv_obj_t *btn = lv_btn_create(page);
        lv_obj_set_size(btn, 160, 150);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2d2d2d), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_50, 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_pad_all(btn, 4, 0);
        lv_obj_set_style_pad_row(btn, 8, 0);
        lv_obj_set_layout(btn, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        bool has_icon = false;

        if (bcfg->icon[0] != '\0') {
            char icon_path[UI_CONFIG_ICON_LEN + 12];
            snprintf(icon_path, sizeof(icon_path), "S:%s/%s",
                UI_CONFIG_ICON_PATH, bcfg->icon);

            lv_img_dsc_t *cached = img_pool_find(icon_path);
            const void   *src    = cached ? (const void *)cached : (const void *)icon_path;
            bool          usable = (cached != NULL);
            if (!usable) {
                FILE *f = fopen(icon_path + 2, "r");
                if (f) { fclose(f); usable = true; }
            }

            if (usable) {
                lv_obj_t *img_cont = lv_obj_create(btn);
                lv_obj_set_size(img_cont, 100, 100);
                lv_obj_set_style_bg_opa(img_cont, LV_OPA_TRANSP, 0);
                lv_obj_set_style_border_width(img_cont, 0, 0);
                lv_obj_set_style_pad_all(img_cont, 0, 0);
                lv_obj_clear_flag(img_cont, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_flag(img_cont, LV_OBJ_FLAG_EVENT_BUBBLE);
                lv_obj_clear_flag(img_cont, LV_OBJ_FLAG_CLICKABLE);

                lv_obj_t *img = lv_img_create(img_cont);
                lv_img_set_src(img, src);
                lv_obj_center(img);
                lv_obj_add_flag(img, LV_OBJ_FLAG_EVENT_BUBBLE);
                lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);

                has_icon = true;
            }
        }

        lv_obj_t *label = lv_label_create(btn);
        lv_label_set_text(label, bcfg->label);
        lv_obj_set_style_text_color(label, lv_color_hex(0xcccccc), 0);

        if (!has_icon) {
            lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
            lv_obj_center(label);
        }

        uintptr_t packed = ((uintptr_t)(uint8_t)page_idx << 8) | (uint8_t)i;
        lv_obj_add_event_cb(btn, btn_event_cb, LV_EVENT_CLICKED, (void *)packed);
    }
}

/* -----------------------------------------------------------------------
 * Deck UI lifecycle
 * ----------------------------------------------------------------------- */

/*
 * ui_destroy_deck: tears down all config-dependent widgets, frees the
 * img_pool PSRAM buffers, and releases config data.
 * Safe to call even if deck was never built.
 */
static void ui_destroy_deck(void)
{
    /* Null out pointers before deleting to prevent stale access from
     * in-flight event callbacks during the delete traversal. */
    s_pages        = NULL;
    s_sidebar_btns = NULL;
    s_cur_page     = 0;
    s_page_count   = 0;

    /* Delete config-dependent sidebar page buttons. */
    if (s_sidebar_pages) {
        lv_obj_del(s_sidebar_pages);
        s_sidebar_pages = NULL;
    }

    if (s_deck_root) {
        lv_obj_del(s_deck_root);
        s_deck_root = NULL;
    }

    /* Release PSRAM pixel buffers from the previous config. */
    img_pool_free();

    ui_config_free(&s_cfg);

    ESP_LOGI("UI", "Deck destroyed - PSRAM free: %d B",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void ui_deck_build_widgets(void)
{
    s_page_count = s_cfg.page_count;

    s_deck_root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_deck_root, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_deck_root, 0, 0);
    lv_obj_set_style_bg_opa(s_deck_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_deck_root, 0, 0);
    lv_obj_set_style_pad_all(s_deck_root, 0, 0);
    lv_obj_set_style_radius(s_deck_root, 0, 0);
    lv_obj_clear_flag(s_deck_root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(s_deck_root, LV_SCROLLBAR_MODE_OFF);

    s_pages        = calloc((size_t)s_page_count, sizeof(lv_obj_t *));
    s_sidebar_btns = calloc((size_t)s_page_count, sizeof(lv_obj_t *));

    /* sidebar_pages is parented under s_sidebar (the static strip) so it
     * renders inside the sidebar area without z-order conflicts. */
    s_sidebar_pages = lv_obj_create(s_sidebar);
    lv_obj_set_size(s_sidebar_pages, SIDEBAR_W, SCREEN_H - 80);
    lv_obj_set_pos(s_sidebar_pages, 0, 0);
    lv_obj_set_style_bg_opa(s_sidebar_pages, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_sidebar_pages, 0, 0);
    lv_obj_set_style_pad_all(s_sidebar_pages, 8, 0);
    lv_obj_set_style_pad_row(s_sidebar_pages, 6, 0);
    lv_obj_set_layout(s_sidebar_pages, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_sidebar_pages, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollbar_mode(s_sidebar_pages, LV_SCROLLBAR_MODE_OFF);

    for (int i = 0; i < s_page_count; i++) {
        lv_obj_t *btn = lv_btn_create(s_sidebar_pages);
        lv_obj_set_size(btn, 64, 56);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a2a2a), 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_add_event_cb(btn, sidebar_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        s_sidebar_btns[i] = btn;

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, s_cfg.pages[i].name);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xcccccc), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(lbl, 60);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lbl);
    }

    for (int i = 0; i < s_page_count; i++) {
        lv_obj_t *btn_cont = NULL;
        s_pages[i] = create_page(s_deck_root, &s_cfg.pages[i], &btn_cont);
        create_buttons(btn_cont, i, &s_cfg.pages[i]);
        if (i != 0) lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_obj_set_style_bg_color(s_sidebar_btns[0], lv_color_hex(0x3a3a3a), 0);

    /* s_deck_root is freshly added to scr and sits on top of everything.
     * Pull s_sidebar and s_context_panel back to the foreground. */
    if (s_sidebar) {
        lv_obj_move_foreground(s_sidebar);
    }
    if (s_context_panel) {
        lv_obj_move_foreground(s_context_panel);
    }
}

/* -----------------------------------------------------------------------
 * Switching screen
 * ----------------------------------------------------------------------- */

/* Covers the entire screen with a dark overlay and a status message.
 * Displayed while the background image decode task is running.
 * The new s_deck_root built by ui_deck_build_widgets() will render on top
 * of this overlay, so it does not need to be explicitly removed. */
static void ui_show_switching_screen(void)
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
 * Static UI (built once at startup, never destroyed)
 * ----------------------------------------------------------------------- */
static void ui_build_static(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x111111), 0);

    s_sidebar = lv_obj_create(scr);
    lv_obj_set_size(s_sidebar, SIDEBAR_W, SCREEN_H);
    lv_obj_set_pos(s_sidebar, 0, 0);
    lv_obj_set_style_bg_color(s_sidebar, lv_color_hex(0x111111), 0);
    lv_obj_set_style_border_width(s_sidebar, 0, 0);
    lv_obj_set_style_radius(s_sidebar, 0, 0);
    lv_obj_set_style_pad_all(s_sidebar, 0, 0);
    lv_obj_clear_flag(s_sidebar, LV_OBJ_FLAG_SCROLLABLE);

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

    s_context_panel = lv_obj_create(scr);
    lv_obj_set_size(s_context_panel, 220, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_context_panel, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_width(s_context_panel, 1, 0);
    lv_obj_set_style_border_color(s_context_panel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(s_context_panel, 8, 0);
    lv_obj_set_style_pad_all(s_context_panel, 8, 0);
    lv_obj_set_layout(s_context_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_context_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_context_panel, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *item = lv_btn_create(s_context_panel);
    lv_obj_set_width(item, LV_PCT(100));
    lv_obj_set_style_bg_color(item, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_bg_color(item, lv_color_hex(0x3a3a3a), LV_STATE_PRESSED);
    lv_obj_set_style_radius(item, 4, 0);
    lv_obj_add_event_cb(item, context_item_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *item_label = lv_label_create(item);
    lv_label_set_text(item_label, "Switch to MSC mode");
    lv_obj_align(item_label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *item2 = lv_btn_create(s_context_panel);
    lv_obj_set_width(item2, LV_PCT(100));
    lv_obj_set_style_bg_color(item2, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_bg_color(item2, lv_color_hex(0x3a3a3a), LV_STATE_PRESSED);
    lv_obj_set_style_radius(item2, 4, 0);
    lv_obj_add_event_cb(item2, select_config_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *item2_label = lv_label_create(item2);
    lv_label_set_text(item2_label, "Select Config");
    lv_obj_align(item2_label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_t *item3 = lv_btn_create(s_context_panel);
    lv_obj_set_width(item3, LV_PCT(100));
    lv_obj_set_style_bg_color(item3, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_bg_color(item3, lv_color_hex(0x3a3a3a), LV_STATE_PRESSED);
    lv_obj_set_style_radius(item3, 4, 0);
    lv_obj_add_event_cb(item3, keyboard_mode_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *item3_label = lv_label_create(item3);
    lv_label_set_text(item3_label, "Keyboard Mode");
    lv_obj_align(item3_label, LV_ALIGN_LEFT_MID, 0, 0);

    lv_obj_update_layout(s_context_panel);
    lv_obj_set_pos(s_context_panel,
                   SIDEBAR_W + 8,
                   SCREEN_H - lv_obj_get_height(s_context_panel) - 8);
}

/* -----------------------------------------------------------------------
 * Public entry points
 * ----------------------------------------------------------------------- */
void my_ui_init(void)
{
    /* s_cfg and s_img_pool were populated by ui_preload_start/wait.
     * Just build the static shell and then the deck widgets. */
    ui_build_static();
    ui_deck_build_widgets();

    ESP_LOGI("MEM", "After UI init - PSRAM free: %d bytes", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    ESP_LOGI("MEM", "After UI init - Internal free: %d bytes", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
}