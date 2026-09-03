#include "ui_monitor.h"
#include "ui_clock_widget.h"
#include "ui_img_pool.h"
#include "ui_monitor_config.h"
#include "ui_settings.h"
#include "ui.h"
#include "sys_clock.h"
#include "usb/usb_hid.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <stdio.h>

#define TAG  "MON"

/* Content area dimensions */
#define CONTENT_X   SIDEBAR_W
#define CONTENT_W   (SCREEN_W - SIDEBAR_W)
#define CONTENT_H   SCREEN_H

/* -----------------------------------------------------------------------
 * Page indices
 * ----------------------------------------------------------------------- */

/* Clock page is always index 0. */
#define MON_PAGE_IDX_CLOCK  0

/* -----------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */
static lv_obj_t  *s_sidebar_pages  = NULL;
static lv_obj_t  *s_sidebar_btns[MON_TOTAL_PAGE_MAX];
static bool       s_sidebar_has_icon[MON_TOTAL_PAGE_MAX];  /* true = button shows an image, selection uses outline instead of bg_color */
static lv_obj_t  *s_pages[MON_TOTAL_PAGE_MAX];
static int        s_total_pages    = 1;   /* clock always present */
static int        s_cur_page       = MON_PAGE_IDX_CLOCK;
static lv_timer_t *s_clock_timer   = NULL;

/* Clock page widget */
static ui_clock_widget_t s_clock_widget;

/* Current monitor config -- loaded on enter, valid until exit */
static monitor_cfg_t s_mon_cfg;

/* Data page cell widgets [page_idx][cell_idx] (page_idx=0 unused, clock is separate) */
static lv_obj_t  *s_cell_lbl[MON_TOTAL_PAGE_MAX][MON_PAGE_CELLS];
static lv_obj_t  *s_cell_bar[MON_TOTAL_PAGE_MAX][MON_PAGE_CELLS];

/* -----------------------------------------------------------------------
 * LRU eviction accessors, called by ui_img_pool's eviction match loop so
 * it can recognize a Monitor page and tear down its stale bg widget.
 * page_idx 0 is the fixed Clock page (s_mon_cfg.clock), 1..s_total_pages-1
 * are data pages (s_mon_cfg.pages[page_idx-1]) -- same indexing
 * monitor_lazy_bg_set()/ui_monitor_lazy_bg_remove_widget() use.
 * ----------------------------------------------------------------------- */
int ui_monitor_page_count(void)
{
    return s_total_pages;
}

const char *ui_monitor_page_bg_image(int page_idx)
{
    if (page_idx < 0 || page_idx >= s_total_pages) return NULL;
    return (page_idx == MON_PAGE_IDX_CLOCK)
           ? s_mon_cfg.clock.bg_image
           : s_mon_cfg.pages[page_idx - 1].bg_image;
}

/* -----------------------------------------------------------------------
 * Queue — written from TinyUSB task, read from LVGL timer (safe).
 * Time no longer has its own queue here -- sys_clock.c owns the running
 * clock now (ticks on its own, persists across mode switches), this file
 * just reads it via sys_clock_get() when painting the clock page.
 * ----------------------------------------------------------------------- */
static QueueHandle_t  s_data_queue    = NULL;

static monitor_data_t s_data          = { 0 };

static bool s_data_received  = false;
static int  s_data_timeout   = 0;

/* Forward declarations — called from TinyUSB task via usb_hid */
static void ui_monitor_on_hid_data(uint8_t cpu_usage, uint8_t cpu_temp,
                                   uint8_t ram_usage, uint8_t gpu_usage,
                                   uint8_t gpu_temp,  uint8_t gpu_vram,
                                   uint8_t cpu_freq,  uint8_t net_up,
                                   uint8_t net_down,  uint8_t disk_usage,
                                   uint8_t cpu_power, uint8_t gpu_power,
                                   uint8_t ssd_life);
static void monitor_lazy_bg_set(int page_idx);

/* Opens the per-metric history chart popup for the tapped cell -- see the
 * mon_hist_* section below (defined near monitor_timer_cb, which is what
 * feeds it). */
static void cell_click_cb(lv_event_t *e);

/* -----------------------------------------------------------------------
 * Sidebar page switching
 * ----------------------------------------------------------------------- */
static void sidebar_btn_cb(lv_event_t *e)
{
    /* Picking any page is how the user leaves the Settings page (no close
     * button there) -- always clear it first, even if idx == s_cur_page,
     * since s_cur_page still points at whatever page was active *before*
     * Settings was opened. */
    ui_settings_deselect();

    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_total_pages) return;
    /* No "idx == s_cur_page" shortcut here (unlike a plain page-to-page
     * tap) -- ui_settings_select() hides s_pages[s_cur_page] via
     * ui_monitor_deselect_current() without changing s_cur_page, so
     * re-picking the same page after Settings must still fall through
     * and re-show it, or it stays hidden with the stale switching-screen
     * cover showing through underneath. */

    lv_obj_set_style_bg_color(s_sidebar_btns[s_cur_page],
                              lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_outline_width(s_sidebar_btns[s_cur_page], 0, 0);
    lv_obj_add_flag(s_pages[s_cur_page], LV_OBJ_FLAG_HIDDEN);

    s_cur_page = idx;

    /* Icon buttons are covered edge-to-edge by the image, so a bg_color
     * swap would be invisible -- use an outline ring instead. Text buttons
     * keep the original full bg_color swap. */
    if (s_sidebar_has_icon[s_cur_page]) {
        lv_obj_set_style_outline_width(s_sidebar_btns[s_cur_page], 3, 0);
    } else {
        lv_obj_set_style_bg_color(s_sidebar_btns[s_cur_page],
                                  lv_color_hex(0x0055cc), 0);
    }
    monitor_lazy_bg_set(s_cur_page);
    lv_obj_clear_flag(s_pages[s_cur_page], LV_OBJ_FLAG_HIDDEN);
}

/* Hide the current page and clear its sidebar highlight without selecting
 * a new one -- called by ui_settings_select() when the gear button takes
 * over the content area. s_cur_page is left untouched so sidebar_btn_cb()
 * can restore it correctly if the same page is picked again afterward. */
void ui_monitor_deselect_current(void)
{
    if (s_total_pages <= 0 || !s_pages[s_cur_page]) return;

    lv_obj_set_style_bg_color(s_sidebar_btns[s_cur_page],
                              lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_outline_width(s_sidebar_btns[s_cur_page], 0, 0);
    lv_obj_add_flag(s_pages[s_cur_page], LV_OBJ_FLAG_HIDDEN);
}

void ui_monitor_reselect_current(void)
{
    if (s_total_pages <= 0 || !s_pages[s_cur_page]) return;

    if (s_sidebar_has_icon[s_cur_page]) {
        lv_obj_set_style_outline_width(s_sidebar_btns[s_cur_page], 3, 0);
    } else {
        lv_obj_set_style_bg_color(s_sidebar_btns[s_cur_page],
                                  lv_color_hex(0x0055cc), 0);
    }
    lv_obj_clear_flag(s_pages[s_cur_page], LV_OBJ_FLAG_HIDDEN);
}

/* -----------------------------------------------------------------------
 * Shared page container
 * ----------------------------------------------------------------------- */
static lv_obj_t *make_page(lv_obj_t *parent)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, CONTENT_W, CONTENT_H);
    lv_obj_set_pos(page, CONTENT_X, 0);
    lv_obj_set_style_bg_color(page, lv_color_hex(0x222222), 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(page, LV_SCROLLBAR_MODE_OFF);
    return page;
}

/* -----------------------------------------------------------------------
 * Lazy background attach -- decode + insert page_idx's bg image the
 * first time that page is actually shown, instead of every page eagerly
 * on Monitor entry. Calls the shared pool directly (ui_img_pool_find()/
 * decode()/mark_bg()) so PSRAM budget and LRU eviction are shared with
 * whatever else is using the pool -- insert at child index 0 so it sits
 * behind whatever mask/content already exists from build_clock_page()/
 * build_data_page(), and skip re-inserting if it's already there.
 * ----------------------------------------------------------------------- */
static void monitor_lazy_bg_set(int page_idx)
{
    if (page_idx < 0 || page_idx >= s_total_pages) return;

    lv_obj_t *page = s_pages[page_idx];
    if (!page) return;

    const char *bg_image = ui_monitor_page_bg_image(page_idx);
    if (!bg_image || !bg_image[0]) return;

    char bg_path[CFG_BG_LEN + 16];
    ui_monitor_config_bg_path(bg_image, bg_path, sizeof(bg_path));

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
            ESP_LOGW(TAG, "lazy bg decode failed: %s", bg_path);
            return;
        }
        ui_img_pool_mark_bg(bg_path);
    }

    uint32_t zoom_x   = (uint32_t)CONTENT_W * 256 / cached->header.w;
    uint32_t zoom_y   = (uint32_t)CONTENT_H * 256 / cached->header.h;
    uint32_t zoom     = (zoom_x > zoom_y) ? zoom_x : zoom_y;
    int32_t  scaled_w = (int32_t)cached->header.w * (int32_t)zoom / 256;
    int32_t  offset_x = (CONTENT_W - scaled_w) / 2;

    lv_obj_t *bg = lv_img_create(page);
    lv_obj_move_to_index(bg, 0);
    lv_img_set_src(bg, cached);
    lv_img_set_pivot(bg, 0, 0);
    lv_img_set_zoom(bg, (uint16_t)zoom);
    lv_obj_set_pos(bg, offset_x, 0);

    ESP_LOGI(TAG, "lazy bg set page %d - PSRAM free: %d B",
             page_idx, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* Called by ui_img_pool.c (extern, no header decl) when that
 * page's bg buffer gets LRU-evicted to make room for a different page.
 * Removes just the bg image widget (child 0, if present) so the page
 * stops pointing at freed PSRAM; the mask stays untouched since it's
 * tied to "does this page have a configured bg" (see build_clock_page()/
 * build_data_page()), not to whether the bg is currently resident.
 * monitor_lazy_bg_set() will decode + re-attach it the next time this
 * page is actually selected again. */
void ui_monitor_lazy_bg_remove_widget(int page_idx)
{
    if (page_idx < 0 || page_idx >= s_total_pages) return;

    lv_obj_t *page = s_pages[page_idx];
    if (!page) return;

    if (lv_obj_get_child_cnt(page) >= 1 &&
        lv_obj_check_type(lv_obj_get_child(page, 0), &lv_img_class)) {
        lv_obj_del(lv_obj_get_child(page, 0));
    }
}

/* -----------------------------------------------------------------------
 * Clock page
 *
 * Layout (content area 720x480):
 *   date label   -- centered, y=160
 *   HH:MM label  -- centered, y=200  (font_48)
 *   SS label     -- bottom-right, x=680 y=440 (font_20)
 * ----------------------------------------------------------------------- */
static void build_clock_page(lv_obj_t *parent)
{
    /* Background image is NOT attached here -- decoding it is slow SD I/O
     * and this runs for every page at Monitor-entry time. It's added
     * later, lazily, by monitor_lazy_bg_set() the first time this page is
     * actually shown (see call sites in ui_monitor_enter()/
     * sidebar_btn_cb()). The mask below stays unconditional either way --
     * it's cheap (a plain color overlay, no decode) and
     * monitor_lazy_bg_set() inserts the bg behind it via
     * lv_obj_move_to_index() once it's ready. */

    /* Semi-transparent overlay */
    lv_obj_t *mask = lv_obj_create(parent);
    lv_obj_set_size(mask, CONTENT_W, CONTENT_H);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_50, 0);
    lv_obj_set_style_border_width(mask, 0, 0);
    lv_obj_set_style_radius(mask, 0, 0);
    lv_obj_clear_flag(mask, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    ui_clock_widget_create(&s_clock_widget, parent, &s_mon_cfg.clock);
    ui_clock_widget_set_no_data(&s_clock_widget);
}

static void update_clock(void)
{
    if (!s_clock_widget.root) return;

    if (!sys_clock_is_valid()) {
        ui_clock_widget_set_no_data(&s_clock_widget);
        return;
    }

    sys_time_t t = sys_clock_get();
    ui_clock_widget_update(&s_clock_widget,
                            t.hour, t.min, t.sec, t.month, t.day, t.wday);
}

/* -----------------------------------------------------------------------
 * System page
 *
 * 2x2 grid, each cell:
 *   title (small, top)
 *   value (large, center)
 *   bar   (bottom, 8px tall)
 *
 * Cell size: 320x200, gap: 20
 * Grid origin: (20, 40) relative to content area
 * ----------------------------------------------------------------------- */
#define CELL_W      320
#define CELL_H      200

/* Each quadrant of the 720x480 content area is 360x240.
 * Center the cell inside its quadrant. */
#define QUAD_W      (CONTENT_W / 2)
#define QUAD_H      (CONTENT_H / 2)
#define CELL_OFF_X  ((QUAD_W - CELL_W) / 2)
#define CELL_OFF_Y  ((QUAD_H - CELL_H) / 2)

static lv_obj_t *make_cell(lv_obj_t *parent, int col, int row,
                            const char *title,
                            lv_obj_t **out_val_lbl,
                            lv_obj_t **out_bar,
                            int bar_max)
{
    int x = col * QUAD_W + CELL_OFF_X;
    int y = row * QUAD_H + CELL_OFF_Y;

    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_pos(cell, x, y);
    lv_obj_set_size(cell, CELL_W, CELL_H);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x1e1e1e), 0);
    lv_obj_set_style_bg_opa(cell, LV_OPA_50, 0);
    lv_obj_set_style_border_color(cell, lv_color_hex(0x333333), 0);
    lv_obj_set_style_border_width(cell, 1, 0);
    lv_obj_set_style_radius(cell, 10, 0);
    lv_obj_set_style_pad_all(cell, 16, 0);
    lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

    /* Title */
    lv_obj_t *title_lbl = lv_label_create(cell);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0xaaaaaa), 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Value label -- same position regardless of bar presence */
    lv_obj_t *val_lbl = lv_label_create(cell);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(val_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_align(val_lbl, LV_ALIGN_CENTER, 0, -10);
    lv_label_set_text(val_lbl, "-");

    if (bar_max > 0) {
        lv_obj_t *bar = lv_bar_create(cell);
        lv_obj_set_size(bar, CELL_W - 32, 8);
        lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(0x333333), 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(MON_BAR_COL_LOW), LV_PART_INDICATOR);
        lv_obj_set_style_radius(bar, 4, 0);
        lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
        lv_bar_set_range(bar, 0, bar_max);
        lv_bar_set_value(bar, 0, LV_ANIM_OFF);
        if (out_bar) *out_bar = bar;
    } else {
        if (out_bar) *out_bar = NULL;
    }

    if (out_val_lbl) *out_val_lbl = val_lbl;

    return cell;
}

/* -----------------------------------------------------------------------
 * Cell metadata lookup
 * ----------------------------------------------------------------------- */
typedef struct {
    const char *label;    /* display name shown above the bar */
    const char *unit;     /* unit suffix for value string     */
    int         bar_max;  /* bar upper bound; 0 = no bar      */
    bool        is_temp;  /* true = use 0 decimal for value   */
    bool        invert;   /* true = high value is good (e.g. SSD life) */
} cell_meta_t;

static const cell_meta_t s_cell_meta[MON_CELL_COUNT] = {
    [MON_CELL_NONE]       = { "",           "",         0,                 false, false },
    [MON_CELL_CPU_USAGE]  = { "CPU Usage",  "%",        MON_BAR_MAX_USAGE, false, false },
    [MON_CELL_CPU_TEMP]   = { "CPU Temp",   " C",       MON_BAR_MAX_TEMP,  true,  false },
    [MON_CELL_CPU_FREQ]   = { "CPU Freq",   " GHz",     0,                 false, false },
    [MON_CELL_RAM_USAGE]  = { "RAM Usage",  "%",        MON_BAR_MAX_USAGE, false, false },
    [MON_CELL_GPU_USAGE]  = { "GPU Usage",  "%",        MON_BAR_MAX_USAGE, false, false },
    [MON_CELL_GPU_TEMP]   = { "GPU Temp",   " C",       MON_BAR_MAX_TEMP,  true,  false },
    [MON_CELL_GPU_VRAM]   = { "VRAM",       "%",        MON_BAR_MAX_USAGE, false, false },
    [MON_CELL_NET_UP]     = { "Net Up",     " MB/s",    0,                 false, false },
    [MON_CELL_NET_DOWN]   = { "Net Down",   " MB/s",    0,                 false, false },
    [MON_CELL_DISK_USAGE] = { "Disk",       "%",        MON_BAR_MAX_USAGE, false, false },
    [MON_CELL_CPU_POWER]  = { "CPU Power",  " W",       0,                 false, false },
    [MON_CELL_GPU_POWER]  = { "GPU Power",  " % TDP",   MON_BAR_MAX_USAGE, false, false },
    [MON_CELL_SSD_LIFE]   = { "SSD Life",   "%",        MON_BAR_MAX_USAGE, false, true  },
};

/* Retrieve the current float value for a given cell id from s_data. */
static float cell_value(mon_cell_id_t id)
{
    switch (id) {
        case MON_CELL_CPU_USAGE:  return s_data.cpu_usage;
        case MON_CELL_CPU_TEMP:   return s_data.cpu_temp;
        case MON_CELL_CPU_FREQ:   return s_data.cpu_freq;
        case MON_CELL_RAM_USAGE:  return s_data.ram_usage;
        case MON_CELL_GPU_USAGE:  return s_data.gpu_usage;
        case MON_CELL_GPU_TEMP:   return s_data.gpu_temp;
        case MON_CELL_GPU_VRAM:   return s_data.gpu_vram;
        case MON_CELL_NET_UP:     return s_data.net_up;
        case MON_CELL_NET_DOWN:   return s_data.net_down;
        case MON_CELL_DISK_USAGE: return s_data.disk_usage;
        case MON_CELL_CPU_POWER:  return s_data.cpu_power;
        case MON_CELL_GPU_POWER:  return s_data.gpu_power;
        case MON_CELL_SSD_LIFE:   return s_data.ssd_life;
        default:                  return 0.0f;
    }
}

/* -----------------------------------------------------------------------
 * Dynamic data page builder
 * ----------------------------------------------------------------------- */
static void build_data_page(lv_obj_t *parent, int page_idx)
{
    const mon_page_cfg_t *pg = &s_mon_cfg.pages[page_idx - 1]; /* page_idx 1-based for data */

    /* Background image is NOT decoded/attached here -- same reasoning as
     * build_clock_page(): it's slow SD I/O and this runs for every data
     * page at Monitor-entry time regardless of whether the user ever
     * visits it. monitor_lazy_bg_set() adds it later, only for the page
     * actually being shown.
     *
     * The mask, however, is keyed off whether a bg is *configured*
     * (pg->bg_image non-empty), not off whether it decoded successfully
     * -- that mirrors the original behavior (mask only when there's a
     * photo to dim) without depending on decode timing, which now
     * happens well after this function returns. */
    if (pg->bg_image[0] != '\0') {
        lv_obj_t *mask = lv_obj_create(parent);
        lv_obj_set_size(mask, CONTENT_W, CONTENT_H);
        lv_obj_set_pos(mask, 0, 0);
        lv_obj_set_style_bg_color(mask, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(mask, LV_OPA_50, 0);
        lv_obj_set_style_border_width(mask, 0, 0);
        lv_obj_set_style_radius(mask, 0, 0);
        lv_obj_clear_flag(mask, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    /* Build cells from JSON config -- 2x2 grid, col-major order:
     * cell[0]=top-left  cell[1]=top-right
     * cell[2]=bot-left  cell[3]=bot-right */
    for (int j = 0; j < MON_PAGE_CELLS; j++) {
        mon_cell_id_t id = pg->cells[j];
        if (id == MON_CELL_NONE) continue;

        int col = j % 2;
        int row = j / 2;
        lv_obj_t *cell = make_cell(parent, col, row, s_cell_meta[id].label,
                  &s_cell_lbl[page_idx][j],
                  &s_cell_bar[page_idx][j],
                  s_cell_meta[id].bar_max);

        /* bar range is already set via make_cell's bar_max param */

        /* Tap any cell -- bar or not -- to see its recorded history.
         * lv_obj_create() objects aren't clickable by default (unlike
         * lv_btn_create(), see ui_settings.c's toggle-row bug), so this
         * needs to be added explicitly. */
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, cell_click_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)id);
    }
}

/* -----------------------------------------------------------------------
 * Dynamic data page updater
 * ----------------------------------------------------------------------- */
static void update_data_pages(void)
{
    char buf[16];

    for (int pi = 1; pi < s_total_pages; pi++) {
        const mon_page_cfg_t *pg = &s_mon_cfg.pages[pi - 1];

        for (int j = 0; j < MON_PAGE_CELLS; j++) {
            lv_obj_t *lbl = s_cell_lbl[pi][j];
            if (!lbl) continue;

            lv_obj_t *bar = s_cell_bar[pi][j];  /* may be NULL if show_bar=false */
            mon_cell_id_t id = pg->cells[j];

            if (!s_data_received) {
                lv_label_set_text(lbl, "-");
                if (bar) lv_bar_set_value(bar, 0, LV_ANIM_OFF);
                continue;
            }

            float v = cell_value(id);
            const cell_meta_t *m = &s_cell_meta[id];

            if (m->is_temp)
                snprintf(buf, sizeof(buf), "%.0f%s", v, m->unit);
            else
                snprintf(buf, sizeof(buf), "%.1f%s", v, m->unit);

            lv_label_set_text(lbl, buf);
            if (bar) {
                int bar_val = (int)fminf(v, (float)m->bar_max);
                lv_bar_set_value(bar, bar_val, LV_ANIM_ON);

                /* Five-step colour based on % of bar_max.
                 * Inverted sensors (e.g. SSD life): high = good, low = bad. */
                float pct = v / (float)m->bar_max * 100.0f;
                if (m->invert) pct = 100.0f - pct;
                uint32_t col;
                if      (pct >= MON_BAR_THR_CRIT) col = MON_BAR_COL_CRIT;
                else if (pct >= MON_BAR_THR_HIGH) col = MON_BAR_COL_WARN;
                else if (pct >= MON_BAR_THR_MID)  col = MON_BAR_COL_HIGH;
                else if (pct >= MON_BAR_THR_LOW)  col = MON_BAR_COL_MID;
                else                              col = MON_BAR_COL_LOW;
                lv_obj_set_style_bg_color(bar, lv_color_hex(col), LV_PART_INDICATOR);
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * Per-metric history -- tap any cell (bar or not) to see a chart of its
 * last MON_HIST_CAPACITY samples (5 min @ 1 sample/sec, matching
 * monitor_timer_cb's rate). Session-scoped only: reset on every
 * ui_monitor_enter(), never persisted -- plain SRAM, no NVS/SD involved.
 *
 * One fixed-size ring buffer per metric, 1 byte/sample, completely
 * decoupled from the popup's lv_chart widget -- recording keeps running
 * whether or not the chart is currently open, while the chart object
 * itself only exists for as long as the popup is on screen. FIFO: once
 * full, each new sample overwrites the oldest.
 *
 * All of this only ever runs from monitor_timer_cb() on the LVGL task
 * (fed from s_data, itself already copied off the TinyUSB-task queue by
 * the time this runs) -- never touched from ui_monitor_on_hid_data()
 * directly, since that runs on the TinyUSB task and this code calls
 * lv_chart_* functions when a popup is open.
 * ----------------------------------------------------------------------- */
#define MON_HIST_CAPACITY  300   /* 5 min @ 1 Hz -- was 10 min/600, halved to
                                  * ease internal DRAM pressure (~4.5KB back
                                  * across 13 metrics); the extra 5 min of
                                  * window wasn't showing anything useful
                                  * anyway. */

static uint8_t  s_hist_buf[MON_CELL_COUNT][MON_HIST_CAPACITY];
static uint16_t s_hist_head[MON_CELL_COUNT];   /* next write index (circular) */
static uint16_t s_hist_count[MON_CELL_COUNT];  /* valid samples, saturates at capacity */
static uint8_t  s_hist_max[MON_CELL_COUNT];    /* max over the currently valid window */

/* Scratch buffer for chronological readout -- module-static rather than a
 * local array so it doesn't sit on whatever task's stack calls into this
 * (see dump_manager.c's comment on the same lesson: this exact class of
 * "big local buffer, small task stack" bug already bit this project once). */
static uint8_t s_hist_scratch[MON_HIST_CAPACITY];

/* Popup state -- NULL/MON_CELL_NONE when no chart is open. */
static lv_obj_t          *s_hist_dim      = NULL;
static lv_obj_t          *s_hist_chart    = NULL;
static lv_chart_series_t *s_hist_series   = NULL;
static lv_obj_t          *s_hist_peak_lbl = NULL;
static mon_cell_id_t      s_hist_open_id  = MON_CELL_NONE;

/* Called once from ui_monitor_enter() -- "every time you enter Monitor,
 * history starts over" per design discussion. */
static void mon_hist_reset(void)
{
    memset(s_hist_head,  0, sizeof(s_hist_head));
    memset(s_hist_count, 0, sizeof(s_hist_count));
    memset(s_hist_max,   0, sizeof(s_hist_max));
    /* s_hist_buf contents don't need clearing -- count[] gates how much of
     * it is ever read. */
}

/* Copies the valid window for one metric into s_hist_scratch in
 * chronological order (oldest first) -- s_hist_count[id] bytes are valid
 * on return. */
static void mon_hist_copy_chrono(mon_cell_id_t id)
{
    uint16_t n = s_hist_count[id];
    if (n < MON_HIST_CAPACITY) {
        /* Hasn't wrapped yet -- already in order starting at index 0. */
        memcpy(s_hist_scratch, s_hist_buf[id], n);
    } else {
        /* Full -- oldest sample sits at head (next slot to be overwritten),
         * the rest wraps around from there. */
        uint16_t start = s_hist_head[id];
        memcpy(s_hist_scratch, &s_hist_buf[id][start], MON_HIST_CAPACITY - start);
        memcpy(s_hist_scratch + (MON_HIST_CAPACITY - start), s_hist_buf[id], start);
    }
}

/* Re-populates the currently-open chart from scratch (whole window, not
 * just the newest point) -- simplest way to stay correct whether the
 * window is still growing (session < 5 min old) or already steady-state
 * FIFO, without juggling two different LVGL chart update paths. Trivial
 * cost at this scale (<=MON_HIST_CAPACITY points, once/sec, only while a
 * popup happens to be open). No-op if no chart is open. */
static void refresh_open_chart(mon_cell_id_t id)
{
    if (s_hist_open_id != id || !s_hist_chart || !s_hist_series) return;

    uint16_t n = s_hist_count[id];
    mon_hist_copy_chrono(id);

    lv_chart_set_point_count(s_hist_chart, n > 0 ? n : 1);
    for (uint16_t i = 0; i < n; i++) {
        lv_chart_set_value_by_id(s_hist_chart, s_hist_series, i, s_hist_scratch[i]);
    }

    uint8_t max = s_hist_max[id];
    const cell_meta_t *m = &s_cell_meta[id];

    /* Metrics that already have a bar (CPU/RAM/GPU usage, temps, VRAM,
     * Disk, GPU Power%, SSD Life) have a meaningful fixed ceiling -- the
     * same one the bar widget itself uses. Reusing it for the chart's Y
     * axis keeps the two visually consistent, and avoids the chart
     * rescaling to fill the height whenever the metric has just been
     * sitting low, which would make ordinary idle fluctuation look
     * artificially dramatic. Metrics without a bar (CPU Freq, Net Up/
     * Down, CPU Power) have no natural ceiling, so those stay dynamic --
     * scaled to the highest value actually seen in the current window. */
    uint8_t range_max = (m->bar_max > 0) ? (uint8_t)m->bar_max : (max > 0 ? max : 1);
    lv_chart_set_range(s_hist_chart, LV_CHART_AXIS_PRIMARY_Y, 0, range_max);
    lv_chart_refresh(s_hist_chart);

    if (s_hist_peak_lbl) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Peak: %u%s", max, m->unit);
        lv_label_set_text(s_hist_peak_lbl, buf);
    }
}

/* Records one sample for one metric -- called once per metric per
 * received tick from monitor_timer_cb(), regardless of which page/cell is
 * currently visible on screen. */
static void mon_hist_append(mon_cell_id_t id, uint8_t v)
{
    uint16_t head = s_hist_head[id];
    s_hist_buf[id][head] = v;
    s_hist_head[id] = (uint16_t)((head + 1) % MON_HIST_CAPACITY);
    if (s_hist_count[id] < MON_HIST_CAPACITY) s_hist_count[id]++;

    /* Full rescan for the window max -- buffer is tiny (MON_HIST_CAPACITY
     * bytes) and this runs once/sec, so this is cheap, and it's simpler and more
     * correct than trying to maintain a running max incrementally (which
     * would need extra bookkeeping for when the max-holding sample ages
     * out of the FIFO window). */
    uint8_t max = 0;
    uint16_t n = s_hist_count[id];
    for (uint16_t i = 0; i < n; i++) {
        if (s_hist_buf[id][i] > max) max = s_hist_buf[id][i];
    }
    s_hist_max[id] = max;

    refresh_open_chart(id);
}

/* Closes the history popup if one is open -- also called defensively from
 * ui_monitor_exit() in case a mode switch somehow lands while it's open
 * (shouldn't normally be reachable: the popup's own full-screen dim
 * overlay sits above the context panel and swallows taps, but this is
 * cheap insurance against a control path this file doesn't know about). */
static void close_history_chart(void)
{
    if (s_hist_dim) {
        lv_obj_del(s_hist_dim);
        s_hist_dim = NULL;
    }
    s_hist_chart    = NULL;
    s_hist_series   = NULL;
    s_hist_peak_lbl = NULL;
    s_hist_open_id  = MON_CELL_NONE;
}

static void hist_dismiss_cb(lv_event_t *e)
{
    (void)e;
    close_history_chart();
}

/* Must match the major_cnt passed to lv_chart_set_axis_tick() for the
 * X axis below -- hist_chart_draw_cb() needs to know it too (see comment
 * there for why) and there's no public getter for it on an existing
 * chart, so this is the one shared source of truth for both call sites. */
#define MON_HIST_X_MAJOR_CNT  5

/* X-axis tick labels -- LVGL's default would print the raw point index,
 * which is what this was first written to assume dsc->value was. That's
 * wrong: for a LINE-type chart (ours), lv_chart.c's draw_x_ticks() sets
 * `tick_value = i / minor_cnt`, i.e. dsc->value is just the major tick's
 * ordinal (0..MON_HIST_X_MAJOR_CNT-1, left to right) -- it is NOT a point
 * index, and is NOT related to point_count at all. (The point-index
 * mapping only happens for LV_CHART_TYPE_SCATTER.) Treating it as a point
 * index near the end of a several-hundred-point buffer was the bug behind
 * "all 5 labels show the same minute, and they all flip together" -- every
 * ordinal 0..4 landed within a few seconds of point_count-1, which all
 * round down to the same "/60" minute value.
 *
 * Fixed here by mapping the ordinal back onto the real point range
 * ourselves before doing the "how long ago" math. */
static void hist_chart_draw_cb(lv_event_t *e)
{
    lv_obj_t *chart = lv_event_get_target(e);
    lv_obj_draw_part_dsc_t *dsc = lv_event_get_draw_part_dsc(e);
    if (!lv_obj_draw_part_check_type(dsc, &lv_chart_class, LV_CHART_DRAW_PART_TICK_LABEL)) return;
    if (dsc->id != LV_CHART_AXIS_PRIMARY_X || !dsc->text) return;

    int32_t point_cnt = (int32_t)lv_chart_get_point_count(chart);
    int32_t idx = point_cnt > 1
                  ? (point_cnt - 1) * dsc->value / (MON_HIST_X_MAJOR_CNT - 1)
                  : 0;
    int32_t seconds_ago = point_cnt - 1 - idx;

    if (seconds_ago <= 0) {
        lv_snprintf(dsc->text, dsc->text_length, "now");
    } else {
        lv_snprintf(dsc->text, dsc->text_length, "-%dm", (int)(seconds_ago / 60));
    }
}

/* Tap handler wired onto every data-page cell in build_data_page(). Opens
 * a Select-Config-sized (80% x 80% screen) popup -- see ui_config_dialog.c
 * -- with an lv_chart (gridlines + Y value ticks + X "time ago" ticks, per
 * design discussion) plotting this metric's current history window. */
static void cell_click_cb(lv_event_t *e)
{
    mon_cell_id_t id = (mon_cell_id_t)(uintptr_t)lv_event_get_user_data(e);
    if (id <= MON_CELL_NONE || id >= MON_CELL_COUNT) return;

    close_history_chart();   /* in case one was somehow already open */

    const cell_meta_t *m = &s_cell_meta[id];
    lv_obj_t *scr = lv_scr_act();

    s_hist_dim = lv_obj_create(scr);
    lv_obj_set_size(s_hist_dim, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_hist_dim, 0, 0);
    lv_obj_set_style_bg_color(s_hist_dim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_hist_dim, LV_OPA_60, 0);
    lv_obj_set_style_border_width(s_hist_dim, 0, 0);
    lv_obj_set_style_radius(s_hist_dim, 0, 0);
    lv_obj_clear_flag(s_hist_dim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_hist_dim, hist_dismiss_cb, LV_EVENT_CLICKED, NULL);

    int dlg_w = (SCREEN_W * 80) / 100;
    int dlg_h = (SCREEN_H * 80) / 100;

    lv_obj_t *box = lv_obj_create(s_hist_dim);
    lv_obj_set_size(box, dlg_w, dlg_h);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x1e1e1e), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 20, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, m->label);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    s_hist_peak_lbl = lv_label_create(box);
    lv_obj_set_style_text_color(s_hist_peak_lbl, lv_color_hex(0xaaaaaa), 0);
    lv_obj_set_style_text_font(s_hist_peak_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_hist_peak_lbl, LV_ALIGN_TOP_RIGHT, 0, 0);

    /* Explicit pos/size (not align) -- axis ticks draw labels outside the
     * chart's own rectangle (its "extended draw size"), so it needs real
     * margin on the left (Y value labels) and bottom (X time labels), on
     * top of the box's own pad_all(20) and the title/peak row above it. */
    lv_obj_t *chart = lv_chart_create(box);
    lv_obj_set_pos(chart, 46, 40);
    lv_obj_set_size(chart, dlg_w - 96, dlg_h - 110);
    lv_chart_set_type(chart, LV_CHART_TYPE_LINE);
    lv_chart_set_div_line_count(chart, 4, 4);   /* light grid, aligned to the ticks below */
    lv_obj_set_style_line_color(chart, lv_color_hex(0x333333), LV_PART_MAIN);
    lv_obj_set_style_size(chart, 0, LV_PART_INDICATOR);   /* line only, no point dots */
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(chart, 0, 0);

    /* Y: value ticks (LVGL's default numeric label is already meaningful
     * here -- these are raw %, W, C, GHz etc, no custom formatting
     * needed). X: "time ago" ticks via hist_chart_draw_cb() above. */
    lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_Y, 6, 3, 5, 2, true, 40);
    lv_chart_set_axis_tick(chart, LV_CHART_AXIS_PRIMARY_X, 6, 3, MON_HIST_X_MAJOR_CNT, 2, true, 20);
    lv_obj_add_event_cb(chart, hist_chart_draw_cb, LV_EVENT_DRAW_PART_BEGIN, NULL);

    lv_chart_series_t *ser = lv_chart_add_series(chart, lv_color_hex(0x00aaff),
                                                  LV_CHART_AXIS_PRIMARY_Y);

    s_hist_chart   = chart;
    s_hist_series  = ser;
    s_hist_open_id = id;

    refresh_open_chart(id);   /* initial populate + range + peak label */
}

/* -----------------------------------------------------------------------
 * Master 1-second timer — runs on LVGL task, safe to touch widgets.
 * The clock itself ticks independently in sys_clock.c (survives mode
 * switches); this timer just repaints from it each second while the
 * clock page happens to be visible, same as it repaints the data pages.
 * ----------------------------------------------------------------------- */
static void monitor_timer_cb(lv_timer_t *t)
{
    /* Drain data queue */
    monitor_data_t data_d;
    bool got_data = false;
    while (s_data_queue && xQueueReceive(s_data_queue, &data_d, 0) == pdTRUE)
        got_data = true;

    if (got_data) {
        s_data          = data_d;
        s_data_received = true;
        s_data_timeout  = 0;

        /* History recording -- one sample per metric per received tick,
         * regardless of which page/cell is currently visible. Values are
         * clamped to a byte, same range the wire protocol already uses
         * for everything except cpu_freq (sent as GHz*10 raw, decoded to
         * a float GHz here) -- that one loses sub-GHz resolution in the
         * recorded history, an accepted trade-off for keeping this at a
         * flat 1 byte/sample instead of hooking the raw pre-decode bytes
         * (which arrive on the TinyUSB task -- see mon_hist_append()'s own
         * comment on why this must stay LVGL-task-only). */
        for (int id = MON_CELL_CPU_USAGE; id < MON_CELL_COUNT; id++) {
            float v = cell_value((mon_cell_id_t)id);
            if (v < 0)   v = 0;
            if (v > 255) v = 255;
            mon_hist_append((mon_cell_id_t)id, (uint8_t)v);
        }
    } else if (s_data_received) {
        s_data_timeout++;
        if (s_data_timeout >= 3) {
            s_data_received = false;
            s_data_timeout  = 0;
        }
    }

    update_clock();
    update_data_pages();
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void ui_monitor_enter(lv_obj_t *sidebar)
{
    /* Create data queue before starting timer or registering HID callbacks.
     * No time queue here anymore -- sys_clock.c's is registered once at
     * boot and keeps running regardless of mode. */
    s_data_queue = xQueueCreate(1, sizeof(monitor_data_t));

    /* Fresh history every time Monitor is entered -- see mon_hist_reset(). */
    mon_hist_reset();

    lv_obj_t *scr = lv_scr_act();

    /* Sidebar pages for monitor */
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

    /* Load config first so page_count is known */
    ui_monitor_config_load(&s_mon_cfg);
    s_total_pages = 1 + s_mon_cfg.page_count;   /* clock + data pages */

    /* Settings' own bg/icon follows whichever Monitor config is active --
     * same "settings" object convention as this config's own "clock". */
    ui_settings_apply_appearance(&s_mon_cfg.settings);

    /* Clamp to array bounds just in case */
    if (s_total_pages > MON_TOTAL_PAGE_MAX)
        s_total_pages = MON_TOTAL_PAGE_MAX;

    memset(s_cell_lbl, 0, sizeof(s_cell_lbl));
    memset(s_cell_bar, 0, sizeof(s_cell_bar));

    /* Sidebar buttons -- clock first, then data pages in JSON order */
    for (int i = 0; i < s_total_pages; i++) {
        const char *name = (i == MON_PAGE_IDX_CLOCK)
                           ? "Clock"
                           : s_mon_cfg.pages[i - 1].name;
        const char *side_icon = (i == MON_PAGE_IDX_CLOCK)
                                ? s_mon_cfg.clock.side_icon
                                : s_mon_cfg.pages[i - 1].side_icon;

        lv_obj_t *btn = lv_btn_create(s_sidebar_pages);
        lv_obj_set_size(btn, 64, 56);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a2a2a), 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_clip_corner(btn, true, 0);
        lv_obj_set_style_outline_color(btn, lv_color_hex(0x0055cc), 0);
        lv_obj_set_style_outline_width(btn, 0, 0);
        lv_obj_add_event_cb(btn, sidebar_btn_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_PRESS_LOCK);
        s_sidebar_btns[i] = btn;

        /* Custom icon replaces the text label entirely when present and the
         * file actually exists on SD -- see side_icon field comment in
         * ui_monitor_config.h. Falls back to the page name text otherwise. */
        bool has_icon = false;
        if (side_icon[0] != '\0') {
            char icon_path[MON_CFG_ICON_LEN + 24];
            ui_monitor_config_icon_path(side_icon, icon_path, sizeof(icon_path));

            FILE *f = fopen(icon_path + 2, "r");
            if (f) {
                fclose(f);
                lv_obj_t *img = lv_img_create(btn);
                lv_img_set_src(img, icon_path);
                lv_obj_center(img);
                has_icon = true;
                ESP_LOGI(TAG, "sidebar page %d '%s' icon: %s", i, name, icon_path);
            } else {
                ESP_LOGW(TAG, "sidebar page %d '%s' side_icon set but not found: %s (falling back to text)",
                         i, name, icon_path);
            }
        }

        if (!has_icon) {
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, name);
            lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
            lv_obj_set_style_text_color(lbl, lv_color_hex(0xcccccc), 0);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
            lv_obj_set_width(lbl, 60);
            lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_center(lbl);
        }

        s_sidebar_has_icon[i] = has_icon;
    }

    /* Highlight clock button */
    if (s_sidebar_has_icon[MON_PAGE_IDX_CLOCK]) {
        lv_obj_set_style_outline_width(s_sidebar_btns[MON_PAGE_IDX_CLOCK], 3, 0);
    } else {
        lv_obj_set_style_bg_color(s_sidebar_btns[MON_PAGE_IDX_CLOCK],
                                  lv_color_hex(0x0055cc), 0);
    }

    /* Reserve shared pool capacity: one slot per page (incl. Clock) that
     * has a configured bg image, +1 for Settings' own bg image, which
     * shares this same pool while Monitor mode is active (see
     * ui_settings.c's settings_lazy_bg_set()). No eager decode here --
     * Monitor has no icons or backgrounds to preload up front, everything
     * decodes lazily on demand via monitor_lazy_bg_set() below. */
    int bg_count = s_mon_cfg.clock.bg_image[0] ? 1 : 0;
    for (int i = 0; i < s_mon_cfg.page_count; i++) {
        if (s_mon_cfg.pages[i].bg_image[0]) bg_count++;
    }
    ui_img_pool_reserve(bg_count + 1);

    /* Content pages -- bg images are NOT decoded here, only the pool
     * capacity above was reserved. monitor_lazy_bg_set() decodes+attaches
     * one page's bg at a time, only for pages actually shown (see below
     * for the initial page, and sidebar_btn_cb() for every switch after). */
    for (int i = 0; i < s_total_pages; i++) {
        s_pages[i] = make_page(scr);
        if (i != MON_PAGE_IDX_CLOCK)
            lv_obj_add_flag(s_pages[i], LV_OBJ_FLAG_HIDDEN);
    }

    build_clock_page(s_pages[MON_PAGE_IDX_CLOCK]);
    for (int i = 1; i < s_total_pages; i++)
        build_data_page(s_pages[i], i);

    /* Bring sidebar and context panel above the new content pages */
    lv_obj_t *ctx = ui_get_context_panel();
    lv_obj_move_foreground(sidebar);
    if (ctx) lv_obj_move_foreground(ctx);

    /* Start 1-second update timer */
    s_clock_timer = lv_timer_create(monitor_timer_cb, 1000, NULL);
    lv_timer_ready(s_clock_timer);

    s_cur_page = MON_PAGE_IDX_CLOCK;
    monitor_lazy_bg_set(s_cur_page);   /* only the page that's actually visible on entry */

    /* Register HID callback and notify PC to start sending data.
     * Time is not registered/subscribed here -- sys_clock.c's CMD_TIME
     * callback is permanent (see my_ui_init()), independent of mode. */
    usb_hid_set_monitor_cb(ui_monitor_on_hid_data);
    usb_hid_monitor_subscribe();

    ESP_LOGI(TAG, "entered monitor mode");
}

void ui_monitor_exit(void)
{
    /* Defensive -- see close_history_chart()'s comment on why this
     * shouldn't normally be reachable with a popup still open. */
    close_history_chart();

    /* Notify PC to stop sending data and unregister callback first.
     * Time callback stays registered -- it's global, not Monitor's. */
    usb_hid_monitor_unsubscribe();
    usb_hid_set_monitor_cb(NULL);

    /* Delete queue before stopping timer */
    if (s_data_queue) { vQueueDelete(s_data_queue); s_data_queue = NULL; }

    if (s_clock_timer) {
        lv_timer_del(s_clock_timer);
        s_clock_timer = NULL;
    }

    /* Destroy clock widget before deleting its parent page.
     * w->root is a child of s_pages[MON_PAGE_IDX_CLOCK] -- must be freed
     * first, otherwise lv_obj_del on the page fires events into freed memory. */
    ui_clock_widget_destroy(&s_clock_widget);

    if (s_sidebar_pages) {
        lv_obj_del(s_sidebar_pages);
        s_sidebar_pages = NULL;
    }

    for (int i = 0; i < s_total_pages; i++) {
        if (s_pages[i]) {
            lv_obj_del(s_pages[i]);
            s_pages[i] = NULL;
        }
        s_sidebar_btns[i] = NULL;
    }

    /* Free background image buffers after all LVGL objects are deleted.
     * Must come after lv_obj_del so no lv_img is still referencing the
     * buffer. Also releases Settings' borrowed slot if it had one (see
     * ui_img_pool_free()). */
    ui_img_pool_free();

    memset(s_cell_lbl, 0, sizeof(s_cell_lbl));
    memset(s_cell_bar, 0, sizeof(s_cell_bar));
    s_total_pages = 1;

    s_cur_page      = MON_PAGE_IDX_CLOCK;
    s_data_received = false;
    s_data_timeout  = 0;

    ESP_LOGI(TAG, "exited monitor mode");
}

void ui_monitor_push_data(const monitor_data_t *data)
{
    if (!s_data_queue) return;
    xQueueOverwrite(s_data_queue, data);
}

/* -----------------------------------------------------------------------
 * HID callbacks — called from TinyUSB task context.
 * Only queue operations allowed here; no LVGL calls.
 * ----------------------------------------------------------------------- */
static void ui_monitor_on_hid_data(uint8_t cpu_usage, uint8_t cpu_temp,
                                   uint8_t ram_usage, uint8_t gpu_usage,
                                   uint8_t gpu_temp,  uint8_t gpu_vram,
                                   uint8_t cpu_freq,  uint8_t net_up,
                                   uint8_t net_down,  uint8_t disk_usage,
                                   uint8_t cpu_power, uint8_t gpu_power,
                                   uint8_t ssd_life)
{
    monitor_data_t d = {
        .cpu_usage  = (float)cpu_usage,
        .cpu_temp   = (float)cpu_temp,
        .ram_usage  = (float)ram_usage,
        .gpu_usage  = (float)gpu_usage,
        .gpu_temp   = (float)gpu_temp,
        .gpu_vram   = (float)gpu_vram,
        .cpu_freq   = (float)cpu_freq * 100.0f / 1000.0f,   /* MHz/100 -> GHz */
        .net_up     = (float)net_up,
        .net_down   = (float)net_down,
        .disk_usage = (float)disk_usage,
        .cpu_power  = (float)cpu_power,
        .gpu_power  = (float)gpu_power,                             /* % of TDP */
        .ssd_life   = (float)ssd_life,
    };
    ui_monitor_push_data(&d);
}

