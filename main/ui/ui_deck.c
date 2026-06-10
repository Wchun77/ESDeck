#include "ui_deck.h"
#include "ui_img_pool.h"
#include "ui.h"
#include "usb/usb_hid.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SIDEBAR_W  80
#define SCREEN_W   800
#define SCREEN_H   480

/* -----------------------------------------------------------------------
 * Deck state
 * ----------------------------------------------------------------------- */
static deck_cfg_t   s_cfg         = {0};
static lv_obj_t    *s_deck_root   = NULL;
static lv_obj_t   **s_pages       = NULL;
static lv_obj_t   **s_sidebar_btns = NULL;
static lv_obj_t    *s_sidebar_pages = NULL;
static int          s_page_count  = 0;
static int          s_cur_page    = 0;

/* -----------------------------------------------------------------------
 * LRU eviction accessors (called by ui_img_pool)
 * ----------------------------------------------------------------------- */
int ui_deck_page_count(void)
{
    return s_page_count;
}

const char *ui_deck_page_bg_image(int page_idx)
{
    if (page_idx < 0 || page_idx >= s_page_count) return NULL;
    return s_cfg.pages[page_idx].bg_image;
}

/* -----------------------------------------------------------------------
 * Background lazy-load
 * ----------------------------------------------------------------------- */
void ui_deck_lazy_bg_remove_widgets(int page_idx)
{
    if (!s_pages || !s_pages[page_idx]) return;
    if (!s_cfg.pages[page_idx].bg_image[0]) return;

    lv_obj_t *page = s_pages[page_idx];
    /* bg is child 0 (lv_img_class), mask is child 1 (opaque lv_obj).
     * btn_cont is transparent — stop when we reach it. */
    while (lv_obj_get_child_cnt(page) >= 1) {
        lv_obj_t *c0 = lv_obj_get_child(page, 0);
        if (lv_obj_check_type(c0, &lv_img_class)) {
            lv_obj_del(c0);
        } else if (lv_obj_get_style_bg_opa(c0, 0) != LV_OPA_TRANSP) {
            lv_obj_del(c0);
        } else {
            break;
        }
    }
}

void ui_deck_lazy_bg_set(int page_idx)
{
    const page_cfg_t *page_cfg = &s_cfg.pages[page_idx];
    lv_obj_t         *page     = s_pages[page_idx];

    if (!page_cfg->bg_image[0]) return;

    int page_w = SCREEN_W - SIDEBAR_W;
    int page_h = SCREEN_H;

    char bg_path[UI_CONFIG_BG_LEN + 12];
    snprintf(bg_path, sizeof(bg_path), "S:%s/%s",
             UI_CONFIG_BG_PATH, page_cfg->bg_image);

    /* If bg widget already exists, just refresh last_used and return. */
    if (lv_obj_get_child_cnt(page) >= 1 &&
        lv_obj_check_type(lv_obj_get_child(page, 0), &lv_img_class)) {
        ui_img_pool_find(bg_path);
        return;
    }

    lv_img_dsc_t *cached = ui_img_pool_find(bg_path);
    if (!cached) {
        cached = ui_img_pool_decode(bg_path);
        if (!cached) {
            ESP_LOGW("DECK", "lazy bg decode failed: %s", bg_path);
            return;
        }
        ui_img_pool_mark_bg(bg_path);
    }

    uint32_t zoom_x   = (uint32_t)page_w * 256 / cached->header.w;
    uint32_t zoom_y   = (uint32_t)page_h * 256 / cached->header.h;
    uint32_t zoom     = (zoom_x > zoom_y) ? zoom_x : zoom_y;
    int32_t  scaled_w = (int32_t)cached->header.w * (int32_t)zoom / 256;
    int32_t  scaled_h = (int32_t)cached->header.h * (int32_t)zoom / 256;
    int32_t  off_x    = ((int32_t)page_w - scaled_w) / 2;
    int32_t  off_y    = ((int32_t)page_h - scaled_h) / 2;

    lv_obj_t *bg = lv_img_create(page);
    lv_obj_move_to_index(bg, 0);
    lv_img_set_src(bg, cached);
    lv_img_set_pivot(bg, 0, 0);
    lv_obj_set_pos(bg, off_x, off_y);
    lv_obj_set_size(bg, page_w, page_h);
    lv_img_set_zoom(bg, (uint16_t)zoom);
    lv_obj_add_flag(bg, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(bg, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *mask = lv_obj_create(page);
    lv_obj_move_to_index(mask, 1);
    lv_obj_set_size(mask, page_w, page_h);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_50, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_set_style_radius(mask, 0, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    ESP_LOGI("DECK", "lazy bg set page %d - PSRAM free: %d B",
             page_idx, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* -----------------------------------------------------------------------
 * Button / sidebar callbacks
 * ----------------------------------------------------------------------- */
static void btn_event_cb(lv_event_t *e)
{
    uintptr_t packed = (uintptr_t)lv_event_get_user_data(e);
    uint8_t   page   = (uint8_t)((packed >> 8) & 0xFF);
    uint8_t   btn    = (uint8_t)(packed & 0xFF);
    ESP_LOGI("BTN", "page=0x%02X btn=0x%02X", page + 1, btn + 1);
    usb_hid_send(page + 1, btn + 1);
}

static void sidebar_btn_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    lv_obj_set_style_bg_color(s_sidebar_btns[s_cur_page], lv_color_hex(0x2a2a2a), 0);
    lv_obj_add_flag(s_pages[s_cur_page], LV_OBJ_FLAG_HIDDEN);
    s_cur_page = idx;
    lv_obj_set_style_bg_color(s_sidebar_btns[s_cur_page], lv_color_hex(0x0055cc), 0);
    ui_deck_lazy_bg_set(s_cur_page);
    lv_obj_clear_flag(s_pages[s_cur_page], LV_OBJ_FLAG_HIDDEN);
}

/* -----------------------------------------------------------------------
 * Page and button creation
 * ----------------------------------------------------------------------- */
static lv_obj_t *create_page(lv_obj_t *parent, const page_cfg_t *page_cfg,
                              lv_obj_t **out_btn_cont)
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
    lv_obj_set_flex_align(btn_cont, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(btn_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(btn_cont, LV_SCROLLBAR_MODE_OFF);

    if (out_btn_cont) *out_btn_cont = btn_cont;
    return page;
}

static void create_buttons(lv_obj_t *btn_cont, int page_idx,
                            const page_cfg_t *page_cfg)
{
    for (int i = 0; i < page_cfg->button_count; i++) {
        const btn_cfg_t *bcfg = &page_cfg->buttons[i];

        lv_obj_t *btn = lv_btn_create(btn_cont);
        lv_obj_set_size(btn, 160, 150);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2d2d2d), 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_50, 0);
        lv_obj_set_style_radius(btn, 10, 0);
        lv_obj_set_style_pad_all(btn, 4, 0);
        lv_obj_set_style_pad_row(btn, 8, 0);
        lv_obj_set_layout(btn, LV_LAYOUT_FLEX);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        bool has_icon = false;

        if (bcfg->icon[0] != '\0') {
            char icon_path[UI_CONFIG_ICON_LEN + 12];
            snprintf(icon_path, sizeof(icon_path), "S:%s/%s",
                     UI_CONFIG_ICON_PATH, bcfg->icon);

            lv_img_dsc_t *cached = ui_img_pool_find(icon_path);
            bool usable = (cached != NULL);
            if (!usable) {
                FILE *f = fopen(icon_path + 2, "r");
                if (f) { fclose(f); usable = true; }
            }

            if (usable) {
                const void *src = cached ? (const void *)cached
                                         : (const void *)icon_path;

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
 * Public lifecycle
 * ----------------------------------------------------------------------- */
void ui_deck_build(lv_obj_t *sidebar, deck_cfg_t *cfg)
{
    s_cfg        = *cfg;
    s_page_count = s_cfg.page_count;
    s_cur_page   = 0;

    s_deck_root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_deck_root, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_deck_root, 0, 0);
    lv_obj_set_style_bg_opa(s_deck_root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_deck_root, 0, 0);
    lv_obj_set_style_pad_all(s_deck_root, 0, 0);
    lv_obj_set_style_radius(s_deck_root, 0, 0);
    lv_obj_clear_flag(s_deck_root,
                      LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scrollbar_mode(s_deck_root, LV_SCROLLBAR_MODE_OFF);

    s_pages        = calloc((size_t)s_page_count, sizeof(lv_obj_t *));
    s_sidebar_btns = calloc((size_t)s_page_count, sizeof(lv_obj_t *));

    s_sidebar_pages = lv_obj_create(sidebar);
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
        lv_obj_add_event_cb(btn, sidebar_btn_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
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

    lv_obj_set_style_bg_color(s_sidebar_btns[0], lv_color_hex(0x0055cc), 0);

    /* s_deck_root was just added on top — bring sidebar and context panel
     * back to the foreground so they render above the deck. */
    lv_obj_t *ctx = ui_get_context_panel();
    if (sidebar) lv_obj_move_foreground(sidebar);
    if (ctx)     lv_obj_move_foreground(ctx);

    ui_deck_lazy_bg_set(s_cur_page);

    ESP_LOGI("DECK", "built %d page(s) - PSRAM free: %d B",
             s_page_count, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void ui_deck_destroy(void)
{
    s_pages        = NULL;
    s_sidebar_btns = NULL;
    s_cur_page     = 0;
    s_page_count   = 0;

    if (s_sidebar_pages) {
        lv_obj_del(s_sidebar_pages);
        s_sidebar_pages = NULL;
    }
    if (s_deck_root) {
        lv_obj_del(s_deck_root);
        s_deck_root = NULL;
    }

    ui_img_pool_free();
    ui_config_free(&s_cfg);

    ESP_LOGI("DECK", "destroyed - PSRAM free: %d B",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
