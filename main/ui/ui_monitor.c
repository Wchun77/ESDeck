#include "ui_monitor.h"
#include "ui_clock_widget.h"
#include "ui_monitor_img.h"
#include "ui_monitor_config.h"
#include "ui.h"
#include "usb/usb_hid.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include <time.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>

#define TAG  "MON"

/* Content area dimensions */
#define CONTENT_X   SIDEBAR_W
#define CONTENT_W   (SCREEN_W - SIDEBAR_W)
#define CONTENT_H   SCREEN_H

/* -----------------------------------------------------------------------
 * Page indices
 * ----------------------------------------------------------------------- */

/* Maximum total pages: 1 clock + MON_PAGE_MAX data pages */
#define MON_TOTAL_PAGE_MAX  (1 + MON_PAGE_MAX)
#define MON_PAGE_IDX_CLOCK  0

/* -----------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */
static lv_obj_t  *s_sidebar_pages  = NULL;
static lv_obj_t  *s_sidebar_btns[MON_TOTAL_PAGE_MAX];
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
 * Queues — written from TinyUSB task, read from LVGL timer (safe).
 * ----------------------------------------------------------------------- */
typedef struct {
    uint8_t hour, min, sec;
    uint8_t month, day, wday;
} monitor_time_t;

static QueueHandle_t  s_data_queue    = NULL;
static QueueHandle_t  s_time_queue    = NULL;

static monitor_data_t s_data          = { 0 };
static monitor_time_t s_time          = { 0 };

/* Self-incrementing clock state.
 * s_time holds the last value received from PC.
 * s_display_time is what actually gets shown — incremented every timer tick
 * and only hard-corrected when the PC value diverges by more than 1 second. */
static monitor_time_t s_display_time  = { 0 };

static bool s_data_received  = false;
static int  s_data_timeout   = 0;
static bool s_time_received  = false;
static int  s_time_timeout   = 0;

/* Forward declarations — called from TinyUSB task via usb_hid */
static void ui_monitor_on_hid_data(uint8_t cpu_usage, uint8_t cpu_temp,
                                   uint8_t ram_usage, uint8_t gpu_usage,
                                   uint8_t gpu_temp, uint8_t gpu_vram);
static void ui_monitor_on_hid_time(uint8_t hour, uint8_t min, uint8_t sec,
                                   uint8_t month, uint8_t day, uint8_t wday);

/* -----------------------------------------------------------------------
 * Sidebar page switching
 * ----------------------------------------------------------------------- */
static void sidebar_btn_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_total_pages) return;
    if (idx == s_cur_page) return;

    lv_obj_set_style_bg_color(s_sidebar_btns[s_cur_page],
                              lv_color_hex(0x2a2a2a), 0);
    lv_obj_add_flag(s_pages[s_cur_page], LV_OBJ_FLAG_HIDDEN);

    s_cur_page = idx;

    lv_obj_set_style_bg_color(s_sidebar_btns[s_cur_page],
                              lv_color_hex(0x0055cc), 0);
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
 * Clock page
 *
 * Layout (content area 720x480):
 *   date label   -- centered, y=160
 *   HH:MM label  -- centered, y=200  (font_48)
 *   SS label     -- bottom-right, x=680 y=440 (font_20)
 * ----------------------------------------------------------------------- */
static void build_clock_page(lv_obj_t *parent)
{
    /* Background image from monitor img manager */
    lv_img_dsc_t *bg_dsc = ui_monitor_img_get(MON_PAGE_IDX_CLOCK);
    if (bg_dsc) {
        int      page_w   = CONTENT_W;
        int      page_h   = CONTENT_H;
        uint32_t zoom_x   = (uint32_t)page_w * 256 / bg_dsc->header.w;
        uint32_t zoom_y   = (uint32_t)page_h * 256 / bg_dsc->header.h;
        uint32_t zoom     = (zoom_x > zoom_y) ? zoom_x : zoom_y;
        int32_t  scaled_w = (int32_t)bg_dsc->header.w * (int32_t)zoom / 256;
        int32_t  offset_x = (page_w - scaled_w) / 2;

        lv_obj_t *bg = lv_img_create(parent);
        lv_img_set_src(bg, bg_dsc);
        lv_img_set_pivot(bg, 0, 0);
        lv_img_set_zoom(bg, (uint16_t)zoom);
        lv_obj_set_pos(bg, offset_x, 0);
    }

    /* Semi-transparent overlay -- same opacity as deck pages */
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

    if (!s_time_received) {
        ui_clock_widget_set_no_data(&s_clock_widget);
        return;
    }

    ui_clock_widget_update(&s_clock_widget,
                            s_display_time.hour, s_display_time.min, s_display_time.sec,
                            s_display_time.month, s_display_time.day, s_display_time.wday);
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
                            lv_obj_t **out_bar)
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
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0x666666), 0);
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

    /* Value */
    lv_obj_t *val_lbl = lv_label_create(cell);
    lv_obj_set_style_text_font(val_lbl, &lv_font_montserrat_36, 0);
    lv_obj_set_style_text_color(val_lbl, lv_color_hex(0xffffff), 0);
    lv_obj_align(val_lbl, LV_ALIGN_CENTER, 0, -10);
    lv_label_set_text(val_lbl, "-");

    /* Progress bar */
    lv_obj_t *bar = lv_bar_create(cell);
    lv_obj_set_size(bar, CELL_W - 32, 8);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x333333), 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(0x0055cc),
                              LV_PART_INDICATOR);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, 0, LV_ANIM_OFF);

    if (out_val_lbl) *out_val_lbl = val_lbl;
    if (out_bar)     *out_bar     = bar;

    return cell;
}

/* -----------------------------------------------------------------------
 * Cell metadata lookup
 * ----------------------------------------------------------------------- */
typedef struct {
    const char *label;   /* display name shown above the bar */
    const char *unit;    /* unit suffix for value string     */
    int         bar_max; /* bar upper bound                  */
    bool        is_temp; /* true = clamp bar to 100          */
} cell_meta_t;

static const cell_meta_t s_cell_meta[MON_CELL_COUNT] = {
    [MON_CELL_NONE]       = { "",           "",     100, false },
    [MON_CELL_CPU_USAGE]  = { "CPU Usage",  "%",    100, false },
    [MON_CELL_CPU_TEMP]   = { "CPU Temp",   " C",   100, true  },
    [MON_CELL_CPU_FREQ]   = { "CPU Freq",   " GHz", 100, false },
    [MON_CELL_RAM_USAGE]  = { "RAM Usage",  "%",    100, false },
    [MON_CELL_GPU_USAGE]  = { "GPU Usage",  "%",    100, false },
    [MON_CELL_GPU_TEMP]   = { "GPU Temp",   " C",   100, true  },
    [MON_CELL_GPU_VRAM]   = { "VRAM",       "%",    100, false },
    [MON_CELL_NET_UP]     = { "Net Up",     " MB/s",100, false },
    [MON_CELL_NET_DOWN]   = { "Net Down",   " MB/s",100, false },
    [MON_CELL_DISK_USAGE] = { "Disk",       "%",    100, false },
};

/* Retrieve the current float value for a given cell id from s_data. */
static float cell_value(mon_cell_id_t id)
{
    switch (id) {
        case MON_CELL_CPU_USAGE:  return s_data.cpu_usage;
        case MON_CELL_CPU_TEMP:   return s_data.cpu_temp;
        case MON_CELL_RAM_USAGE:  return s_data.ram_usage;
        case MON_CELL_GPU_USAGE:  return s_data.gpu_usage;
        case MON_CELL_GPU_TEMP:   return s_data.gpu_temp;
        case MON_CELL_GPU_VRAM:   return s_data.gpu_vram;
        default:                  return 0.0f;
    }
}

/* -----------------------------------------------------------------------
 * Dynamic data page builder
 * ----------------------------------------------------------------------- */
static void build_data_page(lv_obj_t *parent, int page_idx)
{
    const mon_page_cfg_t *pg = &s_mon_cfg.pages[page_idx - 1]; /* page_idx 1-based for data */

    /* Background image */
    lv_img_dsc_t *bg_dsc = ui_monitor_img_get(page_idx);
    if (bg_dsc) {
        int      page_w   = CONTENT_W;
        int      page_h   = CONTENT_H;
        uint32_t zoom_x   = (uint32_t)page_w * 256 / bg_dsc->header.w;
        uint32_t zoom_y   = (uint32_t)page_h * 256 / bg_dsc->header.h;
        uint32_t zoom     = (zoom_x > zoom_y) ? zoom_x : zoom_y;
        int32_t  scaled_w = (int32_t)bg_dsc->header.w * (int32_t)zoom / 256;
        int32_t  offset_x = (page_w - scaled_w) / 2;

        lv_obj_t *bg = lv_img_create(parent);
        lv_img_set_src(bg, bg_dsc);
        lv_img_set_pivot(bg, 0, 0);
        lv_img_set_zoom(bg, (uint16_t)zoom);
        lv_obj_set_pos(bg, offset_x, 0);

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
        make_cell(parent, col, row, s_cell_meta[id].label,
                  &s_cell_lbl[page_idx][j],
                  &s_cell_bar[page_idx][j]);
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
            lv_obj_t *bar = s_cell_bar[pi][j];
            if (!lbl || !bar) continue;

            mon_cell_id_t id = pg->cells[j];

            if (!s_data_received) {
                lv_label_set_text(lbl, "-");
                lv_bar_set_value(bar, 0, LV_ANIM_OFF);
                continue;
            }

            float v = cell_value(id);
            const cell_meta_t *m = &s_cell_meta[id];

            if (m->is_temp)
                snprintf(buf, sizeof(buf), "%.0f%s", v, m->unit);
            else
                snprintf(buf, sizeof(buf), "%.1f%s", v, m->unit);

            lv_label_set_text(lbl, buf);
            lv_bar_set_value(bar, (int)fminf(v, (float)m->bar_max), LV_ANIM_ON);
        }
    }
}

/* -----------------------------------------------------------------------
 * Self-incrementing clock helpers
 * ----------------------------------------------------------------------- */

/* Advance display time by one second in-place. */
static void time_tick(monitor_time_t *t)
{
    t->sec++;
    if (t->sec < 60) return;
    t->sec = 0;
    t->min++;
    if (t->min < 60) return;
    t->min = 0;
    t->hour++;
    if (t->hour < 24) return;
    t->hour = 0;
}

/* Return the absolute second-of-day difference between two time structs.
 * Handles midnight wrap-around (max gap = 43200 s = 12 h). */
static int time_sec_diff(const monitor_time_t *a, const monitor_time_t *b)
{
    int sa = (int)a->hour * 3600 + (int)a->min * 60 + (int)a->sec;
    int sb = (int)b->hour * 3600 + (int)b->min * 60 + (int)b->sec;
    int d  = sa - sb;
    if (d >  43200) d -= 86400;
    if (d < -43200) d += 86400;
    return d < 0 ? -d : d;
}

/* -----------------------------------------------------------------------
 * Master 1-second timer — runs on LVGL task, safe to touch widgets
 * ----------------------------------------------------------------------- */
static void monitor_timer_cb(lv_timer_t *t)
{
    /* Drain time queue — keep only the latest entry */
    monitor_time_t time_d;
    bool got_time = false;
    while (s_time_queue && xQueueReceive(s_time_queue, &time_d, 0) == pdTRUE)
        got_time = true;

    if (got_time) {
        s_time          = time_d;
        s_time_received = true;
        s_time_timeout  = 0;

        if (!s_time_received) {
            /* First sync: initialise display time directly */
            s_display_time = time_d;
        } else {
            /* Already running: only correct if drift exceeds 1 second.
             * Small jitter from WinForms timer is ignored so the display
             * does not flicker on every tick. */
            if (time_sec_diff(&time_d, &s_display_time) > 1) {
                s_display_time = time_d;
            }
        }
    } else if (s_time_received) {
        s_time_timeout++;
        if (s_time_timeout >= 3) {
            s_time_received = false;
            s_time_timeout  = 0;
        }
    }

    /* Advance display clock by one second regardless of whether the PC sent
     * a packet this tick — keeps the display smooth even if a packet is late. */
    if (s_time_received) {
        time_tick(&s_display_time);
        /* Carry over date/wday fields from last received packet */
        s_display_time.month = s_time.month;
        s_display_time.day   = s_time.day;
        s_display_time.wday  = s_time.wday;
    }

    /* Drain data queue */
    monitor_data_t data_d;
    bool got_data = false;
    while (s_data_queue && xQueueReceive(s_data_queue, &data_d, 0) == pdTRUE)
        got_data = true;

    if (got_data) {
        s_data          = data_d;
        s_data_received = true;
        s_data_timeout  = 0;
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
    /* Create data queues before starting timer or registering HID callbacks */
    s_data_queue = xQueueCreate(1, sizeof(monitor_data_t));
    s_time_queue = xQueueCreate(1, sizeof(monitor_time_t));

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

        lv_obj_t *btn = lv_btn_create(s_sidebar_pages);
        lv_obj_set_size(btn, 64, 56);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a2a2a), 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_add_event_cb(btn, sidebar_btn_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        s_sidebar_btns[i] = btn;

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, name);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xcccccc), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(lbl, 60);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lbl);
    }

    /* Highlight clock button */
    lv_obj_set_style_bg_color(s_sidebar_btns[MON_PAGE_IDX_CLOCK],
                              lv_color_hex(0x0055cc), 0);

    /* Load background images */
    char bg_path[MON_CFG_BG_LEN + 16];
    ui_monitor_config_bg_path(s_mon_cfg.clock.bg_image, bg_path, sizeof(bg_path));
    ui_monitor_img_set_path(MON_PAGE_IDX_CLOCK, bg_path);

    for (int i = 0; i < s_mon_cfg.page_count; i++) {
        ui_monitor_config_bg_path(s_mon_cfg.pages[i].bg_image, bg_path, sizeof(bg_path));
        ui_monitor_img_set_path(i + 1, bg_path);
    }

    ui_monitor_img_load_all();

    /* Content pages */
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

    /* Register HID callbacks and notify PC to start sending data */
    usb_hid_set_monitor_cb(ui_monitor_on_hid_data);
    usb_hid_set_time_cb(ui_monitor_on_hid_time);
    usb_hid_monitor_subscribe();

    ESP_LOGI(TAG, "entered monitor mode");
}

void ui_monitor_exit(void)
{
    /* Notify PC to stop sending data and unregister callbacks first */
    usb_hid_monitor_unsubscribe();
    usb_hid_set_monitor_cb(NULL);
    usb_hid_set_time_cb(NULL);

    /* Delete queues before stopping timer */
    if (s_data_queue) { vQueueDelete(s_data_queue); s_data_queue = NULL; }
    if (s_time_queue) { vQueueDelete(s_time_queue); s_time_queue = NULL; }

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
     * Must come after lv_obj_del so no lv_img is still referencing the buffer. */
    ui_monitor_img_free_all();

    memset(s_cell_lbl, 0, sizeof(s_cell_lbl));
    memset(s_cell_bar, 0, sizeof(s_cell_bar));
    s_total_pages = 1;

    s_cur_page      = MON_PAGE_IDX_CLOCK;
    s_data_received = false;
    s_data_timeout  = 0;
    s_time_received = false;
    s_time_timeout  = 0;

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
                                   uint8_t gpu_temp, uint8_t gpu_vram)
{
    monitor_data_t d = {
        .cpu_usage = (float)cpu_usage,
        .cpu_temp  = (float)cpu_temp,
        .ram_usage = (float)ram_usage,
        .gpu_usage = (float)gpu_usage,
        .gpu_temp  = (float)gpu_temp,
        .gpu_vram  = (float)gpu_vram,
    };
    ui_monitor_push_data(&d);
}

static void ui_monitor_on_hid_time(uint8_t hour, uint8_t min, uint8_t sec,
                                   uint8_t month, uint8_t day, uint8_t wday)
{
    if (!s_time_queue) return;
    monitor_time_t t = {
        .hour  = hour,
        .min   = min,
        .sec   = sec,
        .month = month,
        .day   = day,
        .wday  = wday,
    };
    xQueueOverwrite(s_time_queue, &t);
}