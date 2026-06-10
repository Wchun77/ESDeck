#include "ui_config_dialog.h"
#include "ui_config.h"
#include "ui_deck.h"
#include "ui_img_pool.h"
#include "ui.h"
#include "lvgl.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

#define SCREEN_W        800
#define SCREEN_H        480
#define CFG_ROWS_PER_PAGE  4

/* -----------------------------------------------------------------------
 * Dialog state
 * ----------------------------------------------------------------------- */
static char  s_active_fname[UI_CONFIG_FNAME_LEN];

static lv_obj_t *s_dim          = NULL;
static lv_obj_t *s_dialog       = NULL;
static lv_obj_t *s_confirm_btn  = NULL;
static lv_obj_t *s_btn_prev     = NULL;
static lv_obj_t *s_btn_next     = NULL;
static lv_obj_t *s_page_lbl     = NULL;
static lv_obj_t *s_rows[CFG_ROWS_PER_PAGE];
static lv_obj_t *s_row_lbls[CFG_ROWS_PER_PAGE];

static json_scan_result_t s_scan  = { .names = NULL, .count = 0 };
static int  s_cfg_page   = 0;
static int  s_cfg_pages  = 0;
static int  s_sel_idx    = -1;
static lv_obj_t *s_sel_row = NULL;

/* New config loaded during the switch preload task. */
static deck_cfg_t s_new_cfg;

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
static void dialog_close(void)
{
    if (s_dim) {
        lv_obj_del(s_dim);
        s_dim = NULL;
    }
    if (s_dialog) {
        lv_obj_del(s_dialog);
        s_dialog      = NULL;
        s_confirm_btn = NULL;
        s_btn_prev    = NULL;
        s_btn_next    = NULL;
        s_page_lbl    = NULL;
        for (int i = 0; i < CFG_ROWS_PER_PAGE; i++) {
            s_rows[i]     = NULL;
            s_row_lbls[i] = NULL;
        }
    }
    s_sel_row   = NULL;
    s_sel_idx   = -1;
    s_cfg_page  = 0;
    s_cfg_pages = 0;
    ui_config_scan_free(&s_scan);
}

static void render_page(void)
{
    int base = s_cfg_page * CFG_ROWS_PER_PAGE;

    for (int i = 0; i < CFG_ROWS_PER_PAGE; i++) {
        int abs_idx = base + i;
        if (abs_idx < s_scan.count) {
            lv_label_set_text(s_row_lbls[i], s_scan.names[abs_idx]);
            lv_obj_clear_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
            uint32_t col = (abs_idx == s_sel_idx)
                           ? 0x0055cc : 0x2a2a2a;
            lv_obj_set_style_bg_color(s_rows[i], lv_color_hex(col), 0);
            if (abs_idx == s_sel_idx) s_sel_row = s_rows[i];
        } else {
            lv_obj_add_flag(s_rows[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_rows[i], lv_color_hex(0x2a2a2a), 0);
        }
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d / %d", s_cfg_page + 1, s_cfg_pages);
    lv_label_set_text(s_page_lbl, buf);

    if (s_cfg_page == 0)
        lv_obj_add_state(s_btn_prev, LV_STATE_DISABLED);
    else
        lv_obj_clear_state(s_btn_prev, LV_STATE_DISABLED);

    if (s_cfg_page >= s_cfg_pages - 1)
        lv_obj_add_state(s_btn_next, LV_STATE_DISABLED);
    else
        lv_obj_clear_state(s_btn_next, LV_STATE_DISABLED);
}

/* -----------------------------------------------------------------------
 * Config switch flow
 * ----------------------------------------------------------------------- */
static void on_switch_preload_done(void *arg)
{
    ui_deck_build(ui_get_sidebar(), &s_new_cfg);
    ESP_LOGI("CFG_DLG", "switch complete - PSRAM free: %d B",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

static void switch_preload_task(void *arg)
{
    ui_img_pool_load(&s_new_cfg);
    lv_async_call(on_switch_preload_done, NULL);
    vTaskDelete(NULL);
}

/* -----------------------------------------------------------------------
 * Callbacks
 * ----------------------------------------------------------------------- */
static void cancel_cb(lv_event_t *e)
{
    dialog_close();
}

static void confirm_cb(lv_event_t *e)
{
    if (s_sel_idx < 0 || s_sel_idx >= s_scan.count) return;

    const char *fname = s_scan.names[s_sel_idx];
    if (!ui_config_nvs_save(fname)) {
        ESP_LOGE("CFG_DLG", "Failed to save config to NVS");
        return;
    }
    ESP_LOGI("CFG_DLG", "switching config: %s", fname);

    dialog_close();
    ui_deck_destroy();

    bool cfg_ok = ui_config_load(&s_new_cfg);
    if (!cfg_ok || s_new_cfg.page_count == 0) {
        s_new_cfg.page_count = 1;
        s_new_cfg.pages      = calloc(1, sizeof(page_cfg_t));
        snprintf(s_new_cfg.pages[0].name, UI_CONFIG_NAME_LEN, "Main");
        s_new_cfg.pages[0].button_count = 0;
        s_new_cfg.pages[0].buttons      = NULL;
    }

    ui_show_switching_screen("Switching config...");
    xTaskCreate(switch_preload_task, "sw_preload", 8192, NULL, 3, NULL);
}

static void item_cb(lv_event_t *e)
{
    int row_pos = (int)(uintptr_t)lv_event_get_user_data(e);
    int abs_idx = s_cfg_page * CFG_ROWS_PER_PAGE + row_pos;
    if (abs_idx >= s_scan.count) return;

    lv_obj_t *row = lv_event_get_target(e);
    if (s_sel_row && s_sel_row != row)
        lv_obj_set_style_bg_color(s_sel_row, lv_color_hex(0x2a2a2a), 0);
    s_sel_row = row;
    s_sel_idx = abs_idx;
    lv_obj_set_style_bg_color(row, lv_color_hex(0x0055cc), 0);

    lv_obj_t *confirm_lbl = lv_obj_get_child(s_confirm_btn, 0);
    bool same = (s_active_fname[0] != '\0' &&
                 strcmp(s_scan.names[abs_idx], s_active_fname) == 0);
    if (same) {
        lv_obj_add_state(s_confirm_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s_confirm_btn, lv_color_hex(0x333333), 0);
        if (confirm_lbl)
            lv_obj_set_style_text_color(confirm_lbl, lv_color_hex(0x555555), 0);
    } else {
        lv_obj_clear_state(s_confirm_btn, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s_confirm_btn, lv_color_hex(0x3a3a3a), 0);
        if (confirm_lbl)
            lv_obj_set_style_text_color(confirm_lbl, lv_color_hex(0xcccccc), 0);
    }
}

static void prev_cb(lv_event_t *e)
{
    if (s_cfg_page <= 0) return;
    int pos = (s_sel_idx >= 0) ? (s_sel_idx % CFG_ROWS_PER_PAGE) : -1;
    s_cfg_page--;
    render_page();
    if (pos >= 0) {
        int base = s_cfg_page * CFG_ROWS_PER_PAGE;
        int tgt  = base + pos;
        int last = base + CFG_ROWS_PER_PAGE - 1;
        if (last >= s_scan.count) last = s_scan.count - 1;
        s_sel_idx = (tgt < s_scan.count) ? tgt : last;
        if (s_sel_row) lv_obj_set_style_bg_color(s_sel_row, lv_color_hex(0x2a2a2a), 0);
        s_sel_row = s_rows[s_sel_idx - base];
        lv_obj_set_style_bg_color(s_sel_row, lv_color_hex(0x0055cc), 0);
    }
}

static void next_cb(lv_event_t *e)
{
    if (s_cfg_page >= s_cfg_pages - 1) return;
    int pos = (s_sel_idx >= 0) ? (s_sel_idx % CFG_ROWS_PER_PAGE) : -1;
    s_cfg_page++;
    render_page();
    if (pos >= 0) {
        int base = s_cfg_page * CFG_ROWS_PER_PAGE;
        int tgt  = base + pos;
        int last = base + CFG_ROWS_PER_PAGE - 1;
        if (last >= s_scan.count) last = s_scan.count - 1;
        s_sel_idx = (tgt < s_scan.count) ? tgt : last;
        if (s_sel_row) lv_obj_set_style_bg_color(s_sel_row, lv_color_hex(0x2a2a2a), 0);
        s_sel_row = s_rows[s_sel_idx - base];
        lv_obj_set_style_bg_color(s_sel_row, lv_color_hex(0x0055cc), 0);
    }
}

/* -----------------------------------------------------------------------
 * Public
 * ----------------------------------------------------------------------- */
void ui_config_dialog_show(void)
{
    s_scan = ui_config_scan();
    if (s_scan.count == 0) {
        ui_config_scan_free(&s_scan);
        ESP_LOGW("CFG_DLG", "No JSON files found in %s", UI_CONFIG_JSON_PATH);
        return;
    }

    s_cfg_page  = 0;
    s_cfg_pages = (s_scan.count + CFG_ROWS_PER_PAGE - 1) / CFG_ROWS_PER_PAGE;

    lv_obj_t *scr = lv_scr_act();

    s_dim = lv_obj_create(scr);
    lv_obj_set_size(s_dim, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_dim, 0, 0);
    lv_obj_set_style_bg_color(s_dim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_dim, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_dim, 0, 0);
    lv_obj_set_style_radius(s_dim, 0, 0);
    lv_obj_clear_flag(s_dim, LV_OBJ_FLAG_SCROLLABLE);

    int dlg_w = (SCREEN_W * 80) / 100;
    int dlg_h = (SCREEN_H * 80) / 100;

    s_dialog = lv_obj_create(scr);
    lv_obj_set_size(s_dialog, dlg_w, dlg_h);
    lv_obj_center(s_dialog);
    lv_obj_set_style_bg_color(s_dialog, lv_color_hex(0x1e1e1e), 0);
    lv_obj_set_style_border_color(s_dialog, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(s_dialog, 1, 0);
    lv_obj_set_style_radius(s_dialog, 12, 0);
    lv_obj_set_style_pad_all(s_dialog, 0, 0);
    lv_obj_clear_flag(s_dialog, LV_OBJ_FLAG_SCROLLABLE);

    int title_h  = 48;
    int bottom_h = 64;
    int list_h   = dlg_h - title_h - 1 - bottom_h;

    /* Title bar */
    lv_obj_t *title_bar = lv_obj_create(s_dialog);
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

    /* Divider */
    lv_obj_t *div = lv_obj_create(s_dialog);
    lv_obj_set_size(div, dlg_w, 1);
    lv_obj_set_pos(div, 0, title_h);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_radius(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);

    /* List container */
    lv_obj_t *list_cont = lv_obj_create(s_dialog);
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
        lv_obj_set_style_bg_color(row, lv_color_hex(0x2a2a2a), 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(0x3a3a3a), LV_STATE_PRESSED);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_add_event_cb(row, item_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);
        lv_obj_t *lbl = lv_label_create(row);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xcccccc), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
        s_rows[i]     = row;
        s_row_lbls[i] = lbl;
    }

    /* Bottom bar */
    lv_obj_t *bottom = lv_obj_create(s_dialog);
    lv_obj_set_size(bottom, dlg_w, bottom_h);
    lv_obj_set_pos(bottom, 0, dlg_h - bottom_h);
    lv_obj_set_style_bg_color(bottom, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_width(bottom, 0, 0);
    lv_obj_set_style_radius(bottom, 0, 0);
    lv_obj_set_style_pad_hor(bottom, 16, 0);
    lv_obj_set_style_pad_ver(bottom, 0, 0);
    lv_obj_clear_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);

    s_btn_prev = lv_btn_create(bottom);
    lv_obj_set_size(s_btn_prev, 48, 40);
    lv_obj_align(s_btn_prev, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_btn_prev, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_color(s_btn_prev, lv_color_hex(0x4a4a4a), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_btn_prev, 0, 0);
    lv_obj_set_style_radius(s_btn_prev, 6, 0);
    lv_obj_add_event_cb(s_btn_prev, prev_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_prev = lv_label_create(s_btn_prev);
    lv_label_set_text(lbl_prev, LV_SYMBOL_UP);
    lv_obj_set_style_text_color(lbl_prev, lv_color_hex(0xcccccc), 0);
    lv_obj_center(lbl_prev);

    s_page_lbl = lv_label_create(bottom);
    lv_obj_set_style_text_color(s_page_lbl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(s_page_lbl, &lv_font_montserrat_14, 0);
    lv_obj_align(s_page_lbl, LV_ALIGN_LEFT_MID, 64, 0);

    s_btn_next = lv_btn_create(bottom);
    lv_obj_set_size(s_btn_next, 48, 40);
    lv_obj_align(s_btn_next, LV_ALIGN_LEFT_MID, 112, 0);
    lv_obj_set_style_bg_color(s_btn_next, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_color(s_btn_next, lv_color_hex(0x4a4a4a), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_btn_next, 0, 0);
    lv_obj_set_style_radius(s_btn_next, 6, 0);
    lv_obj_add_event_cb(s_btn_next, next_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_next = lv_label_create(s_btn_next);
    lv_label_set_text(lbl_next, LV_SYMBOL_DOWN);
    lv_obj_set_style_text_color(lbl_next, lv_color_hex(0xcccccc), 0);
    lv_obj_center(lbl_next);

    lv_obj_t *btn_cancel = lv_btn_create(bottom);
    lv_obj_set_size(btn_cancel, 120, 40);
    lv_obj_align(btn_cancel, LV_ALIGN_RIGHT_MID, -136, 0);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x4a4a4a), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn_cancel, 0, 0);
    lv_obj_set_style_radius(btn_cancel, 6, 0);
    lv_obj_add_event_cb(btn_cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_set_style_text_color(lbl_cancel, lv_color_hex(0xcccccc), 0);
    lv_obj_center(lbl_cancel);

    s_confirm_btn = lv_btn_create(bottom);
    lv_obj_set_size(s_confirm_btn, 120, 40);
    lv_obj_align(s_confirm_btn, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(s_confirm_btn, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(s_confirm_btn, lv_color_hex(0x4a4a4a), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(s_confirm_btn, 0, 0);
    lv_obj_set_style_radius(s_confirm_btn, 6, 0);
    lv_obj_add_state(s_confirm_btn, LV_STATE_DISABLED);
    lv_obj_add_event_cb(s_confirm_btn, confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_confirm = lv_label_create(s_confirm_btn);
    lv_label_set_text(lbl_confirm, "Confirm");
    lv_obj_set_style_text_color(lbl_confirm, lv_color_hex(0x555555), 0);
    lv_obj_center(lbl_confirm);

    /* Pre-select the currently active config. */
    s_active_fname[0] = '\0';
    ui_config_nvs_load(s_active_fname, sizeof(s_active_fname));
    for (int i = 0; i < s_scan.count; i++) {
        if (strcmp(s_scan.names[i], s_active_fname) == 0) {
            s_sel_idx  = i;
            s_cfg_page = i / CFG_ROWS_PER_PAGE;
            break;
        }
    }

    render_page();
}
