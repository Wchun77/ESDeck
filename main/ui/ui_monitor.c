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

/* -----------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */
static lv_obj_t  *s_sidebar_pages  = NULL;
static lv_obj_t  *s_sidebar_btns[MON_PAGE_COUNT];
static lv_obj_t  *s_pages[MON_PAGE_COUNT];
static int        s_cur_page       = MON_PAGE_CLOCK;
static lv_timer_t *s_clock_timer   = NULL;

/* Clock page widget */
static ui_clock_widget_t s_clock_widget;

/* Current monitor config -- loaded on enter, valid until exit */
static monitor_cfg_t s_mon_cfg;

/* System page widgets */
static lv_obj_t  *s_sys_cpu_bar   = NULL;
static lv_obj_t  *s_sys_cpu_lbl   = NULL;
static lv_obj_t  *s_sys_ram_bar   = NULL;
static lv_obj_t  *s_sys_ram_lbl   = NULL;
static lv_obj_t  *s_sys_temp_bar  = NULL;
static lv_obj_t  *s_sys_temp_lbl  = NULL;
static lv_obj_t  *s_sys_gpu_bar   = NULL;
static lv_obj_t  *s_sys_gpu_lbl   = NULL;

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

static bool s_data_received  = false;
static int  s_data_timeout   = 0;
static bool s_time_received  = false;
static int  s_time_timeout   = 0;

/* Forward declarations — called from TinyUSB task via usb_hid */
static void ui_monitor_on_hid_data(uint8_t cpu_usage, uint8_t cpu_temp,
                                   uint8_t ram_usage, uint8_t gpu_usage);
static void ui_monitor_on_hid_time(uint8_t hour, uint8_t min, uint8_t sec,
                                   uint8_t month, uint8_t day, uint8_t wday);

/* -----------------------------------------------------------------------
 * Sidebar page switching
 * ----------------------------------------------------------------------- */
static void sidebar_btn_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
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
    lv_img_dsc_t *bg_dsc = ui_monitor_img_get(MON_PAGE_CLOCK);
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
                            s_time.hour, s_time.min, s_time.sec,
                            s_time.month, s_time.day, s_time.wday);
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
#define CELL_W   320
#define CELL_H   200
#define CELL_GAP  20

static lv_obj_t *make_cell(lv_obj_t *parent, int col, int row,
                            const char *title,
                            lv_obj_t **out_val_lbl,
                            lv_obj_t **out_bar)
{
    int x = CELL_GAP + col * (CELL_W + CELL_GAP);
    int y = CELL_GAP + row * (CELL_H + CELL_GAP);

    lv_obj_t *cell = lv_obj_create(parent);
    lv_obj_set_pos(cell, x, y);
    lv_obj_set_size(cell, CELL_W, CELL_H);
    lv_obj_set_style_bg_color(cell, lv_color_hex(0x1e1e1e), 0);
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

static void build_system_page(lv_obj_t *parent)
{
    make_cell(parent, 0, 0, "CPU Usage",
              &s_sys_cpu_lbl, &s_sys_cpu_bar);
    make_cell(parent, 1, 0, "CPU Temp",
              &s_sys_temp_lbl, &s_sys_temp_bar);
    make_cell(parent, 0, 1, "RAM Usage",
              &s_sys_ram_lbl, &s_sys_ram_bar);
    make_cell(parent, 1, 1, "GPU Usage",
              &s_sys_gpu_lbl, &s_sys_gpu_bar);
}

static void update_system(void)
{
    if (!s_sys_cpu_lbl) return;

    char buf[16];

    if (!s_data_received) {
        lv_label_set_text(s_sys_cpu_lbl,  "-");
        lv_label_set_text(s_sys_temp_lbl, "-");
        lv_label_set_text(s_sys_ram_lbl,  "-");
        lv_label_set_text(s_sys_gpu_lbl,  "-");
        lv_bar_set_value(s_sys_cpu_bar,  0, LV_ANIM_OFF);
        lv_bar_set_value(s_sys_temp_bar, 0, LV_ANIM_OFF);
        lv_bar_set_value(s_sys_ram_bar,  0, LV_ANIM_OFF);
        lv_bar_set_value(s_sys_gpu_bar,  0, LV_ANIM_OFF);
        return;
    }

    snprintf(buf, sizeof(buf), "%.1f%%", s_data.cpu_usage);
    lv_label_set_text(s_sys_cpu_lbl, buf);
    lv_bar_set_value(s_sys_cpu_bar, (int)s_data.cpu_usage, LV_ANIM_ON);

    snprintf(buf, sizeof(buf), "%.0f C", s_data.cpu_temp);
    lv_label_set_text(s_sys_temp_lbl, buf);
    lv_bar_set_value(s_sys_temp_bar,
                     (int)fminf(s_data.cpu_temp, 100.0f), LV_ANIM_ON);

    snprintf(buf, sizeof(buf), "%.1f%%", s_data.ram_usage);
    lv_label_set_text(s_sys_ram_lbl, buf);
    lv_bar_set_value(s_sys_ram_bar, (int)s_data.ram_usage, LV_ANIM_ON);

    snprintf(buf, sizeof(buf), "%.1f%%", s_data.gpu_usage);
    lv_label_set_text(s_sys_gpu_lbl, buf);
    lv_bar_set_value(s_sys_gpu_bar, (int)s_data.gpu_usage, LV_ANIM_ON);
}

/* -----------------------------------------------------------------------
 * Master 1-second timer — runs on LVGL task, safe to touch widgets
 * ----------------------------------------------------------------------- */
static void monitor_timer_cb(lv_timer_t *t)
{
    /* Drain time queue */
    monitor_time_t time_d;
    bool got_time = false;
    while (s_time_queue && xQueueReceive(s_time_queue, &time_d, 0) == pdTRUE)
        got_time = true;

    if (got_time) {
        s_time          = time_d;
        s_time_received = true;
        s_time_timeout  = 0;
    } else if (s_time_received) {
        s_time_timeout++;
        if (s_time_timeout >= 3) {
            s_time_received = false;
            s_time_timeout  = 0;
        }
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
    update_system();
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

    static const char *page_names[MON_PAGE_COUNT] = { "Clock", "Sys" };

    for (int i = 0; i < MON_PAGE_COUNT; i++) {
        lv_obj_t *btn = lv_btn_create(s_sidebar_pages);
        lv_obj_set_size(btn, 64, 56);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x2a2a2a), 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_add_event_cb(btn, sidebar_btn_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
        s_sidebar_btns[i] = btn;

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, page_names[i]);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_12, 0);
        lv_obj_set_style_text_color(lbl, lv_color_hex(0xcccccc), 0);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_obj_set_width(lbl, 60);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lbl);
    }

    /* Highlight first page */
    lv_obj_set_style_bg_color(s_sidebar_btns[MON_PAGE_CLOCK],
                              lv_color_hex(0x0055cc), 0);

    /* Load config then background images */
    ui_monitor_config_load(&s_mon_cfg);

    char bg_path[MON_CFG_BG_LEN + 16];
    ui_monitor_config_bg_path(s_mon_cfg.clock.bg_image, bg_path, sizeof(bg_path));
    ui_monitor_img_set_path(MON_PAGE_CLOCK, bg_path);

    ui_monitor_config_bg_path(s_mon_cfg.system.bg_image, bg_path, sizeof(bg_path));
    ui_monitor_img_set_path(MON_PAGE_SYSTEM, bg_path);

    ui_monitor_img_load_all();

    /* Content pages */
    s_pages[MON_PAGE_CLOCK]  = make_page(scr);
    s_pages[MON_PAGE_SYSTEM] = make_page(scr);
    lv_obj_add_flag(s_pages[MON_PAGE_SYSTEM], LV_OBJ_FLAG_HIDDEN);

    build_clock_page(s_pages[MON_PAGE_CLOCK]);
    build_system_page(s_pages[MON_PAGE_SYSTEM]);

    /* Bring sidebar and context panel above the new content pages */
    lv_obj_t *ctx = ui_get_context_panel();
    lv_obj_move_foreground(sidebar);
    if (ctx) lv_obj_move_foreground(ctx);

    /* Start 1-second update timer */
    s_clock_timer = lv_timer_create(monitor_timer_cb, 1000, NULL);
    lv_timer_ready(s_clock_timer);

    s_cur_page = MON_PAGE_CLOCK;

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
     * w->root is a child of s_pages[MON_PAGE_CLOCK] -- must be freed
     * first, otherwise lv_obj_del on the page fires events into freed memory. */
    ui_clock_widget_destroy(&s_clock_widget);

    if (s_sidebar_pages) {
        lv_obj_del(s_sidebar_pages);
        s_sidebar_pages = NULL;
    }

    for (int i = 0; i < MON_PAGE_COUNT; i++) {
        if (s_pages[i]) {
            lv_obj_del(s_pages[i]);
            s_pages[i] = NULL;
        }
        s_sidebar_btns[i] = NULL;
    }

    /* Free background image buffers after all LVGL objects are deleted.
     * Must come after lv_obj_del so no lv_img is still referencing the buffer. */
    ui_monitor_img_free_all();

    /* Clear system widget pointers */
    s_sys_cpu_bar    = NULL;
    s_sys_cpu_lbl    = NULL;
    s_sys_ram_bar    = NULL;
    s_sys_ram_lbl    = NULL;
    s_sys_temp_bar   = NULL;
    s_sys_temp_lbl   = NULL;
    s_sys_gpu_bar    = NULL;
    s_sys_gpu_lbl    = NULL;

    s_cur_page      = MON_PAGE_CLOCK;
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
                                   uint8_t ram_usage, uint8_t gpu_usage)
{
    monitor_data_t d = {
        .cpu_usage = (float)cpu_usage,
        .cpu_temp  = (float)cpu_temp,
        .ram_usage = (float)ram_usage,
        .gpu_usage = (float)gpu_usage,
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