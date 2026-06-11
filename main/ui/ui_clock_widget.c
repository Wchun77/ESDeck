#include "ui_clock_widget.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>

#define TAG  "CLK_WIDGET"

#define CW  720
#define CH  480

#define DATE_PANEL_X     8
#define DATE_PANEL_Y    16
#define DATE_PANEL_W   200
#define DATE_PANEL_H    88

#define TIME_PANEL_X     0
#define TIME_PANEL_W   720
#define TIME_PANEL_H   280

#define SEC_PANEL_W    124
#define SEC_PANEL_H     60
#define SEC_PANEL_X    (CW - SEC_PANEL_W - 4)
#define SEC_PANEL_Y    (CH - SEC_PANEL_H - 12)

/* Near-invisible colour for no-data placeholders */
#define NO_DATA_COL  0x111118

static lv_obj_t *make_panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, w, h);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_style_bg_opa(p, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return p;
}

static void rebuild_time_label(ui_clock_widget_t *w)
{
    if (!w->time_label) return;

    char buf[32];
    if (!w->has_data) {
        snprintf(buf, sizeof(buf), "#%06lx 00:00#", (unsigned long)NO_DATA_COL);
    } else {
        uint32_t col = w->colon_visible ? CLK_COL_TIME : 0x0a0a14;
        snprintf(buf, sizeof(buf), "%02d#%06lx :#%02d",
                 w->cur_hour, (unsigned long)col, w->cur_min);
    }
    lv_label_set_text(w->time_label, buf);
}

static lv_font_t *load_font(const char *path, bool *fallback_flag)
{
    lv_font_t *f = lv_font_load(path);
    if (!f) {
        ESP_LOGW(TAG, "font load failed: %s, using fallback", path);
        *fallback_flag = true;
        return (lv_font_t *)&lv_font_montserrat_48;
    }
    ESP_LOGI(TAG, "font loaded: %s", path);
    return f;
}

void ui_clock_widget_create(ui_clock_widget_t *w, lv_obj_t *parent)
{
    memset(w, 0, sizeof(*w));
    w->colon_visible = true;
    w->has_data      = false;
    w->cur_sec       = 0xFF;   /* invalid sentinel */

    w->font_time = load_font(CLK_FONT_PATH_TIME, &w->font_fallback);
    w->font_sec  = load_font(CLK_FONT_PATH_SEC,  &w->font_fallback);
    w->font_date = load_font(CLK_FONT_PATH_DATE,  &w->font_fallback);

    /* Root container */
    w->root = lv_obj_create(parent);
    lv_obj_set_size(w->root, CW, CH);
    lv_obj_set_pos(w->root, 0, 0);
    lv_obj_set_style_bg_opa(w->root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(w->root, 0, 0);
    lv_obj_set_style_pad_all(w->root, 0, 0);
    lv_obj_set_style_radius(w->root, 0, 0);
    lv_obj_clear_flag(w->root, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* ---- Date panel ---- */
    lv_obj_t *date_panel = make_panel(w->root,
                                       DATE_PANEL_X, DATE_PANEL_Y,
                                       DATE_PANEL_W, DATE_PANEL_H);

    w->date_label = lv_label_create(date_panel);
    lv_obj_set_style_text_font(w->date_label, w->font_date, 0);
    lv_obj_set_style_text_color(w->date_label, lv_color_hex(CLK_COL_DATE), 0);
    lv_obj_set_style_bg_opa(w->date_label, LV_OPA_TRANSP, 0);
    lv_obj_align(w->date_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_recolor(w->date_label, true);
    lv_label_set_text(w->date_label, "#111118 00/00#");

    w->day_label = lv_label_create(date_panel);
    lv_obj_set_style_text_font(w->day_label, w->font_date, 0);
    lv_obj_set_style_text_color(w->day_label, lv_color_hex(CLK_COL_DAY), 0);
    lv_obj_set_style_bg_opa(w->day_label, LV_OPA_TRANSP, 0);
    lv_obj_align(w->day_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_recolor(w->day_label, true);
    lv_label_set_text(w->day_label, "#111118 XXX#");

    static lv_point_t sep_pts[2] = {{ 0, 0 }, { 160, 0 }};
    lv_obj_t *sep = lv_line_create(date_panel);
    lv_line_set_points(sep, sep_pts, 2);
    lv_obj_set_style_line_color(sep, lv_color_hex(0x1a2a55), 0);
    lv_obj_set_style_line_width(sep, 1, 0);
    lv_obj_align(sep, LV_ALIGN_BOTTOM_LEFT, 0, 0);

    /* ---- Time panel ---- */
    int time_panel_y = (CH - TIME_PANEL_H) / 2;
    lv_obj_t *time_panel = make_panel(w->root,
                                       TIME_PANEL_X, time_panel_y,
                                       TIME_PANEL_W, TIME_PANEL_H);

    w->time_label = lv_label_create(time_panel);
    lv_obj_set_style_text_font(w->time_label, w->font_time, 0);
    lv_obj_set_style_text_color(w->time_label, lv_color_hex(CLK_COL_TIME), 0);
    lv_obj_set_style_bg_opa(w->time_label, LV_OPA_TRANSP, 0);
    lv_label_set_recolor(w->time_label, true);
    lv_obj_align(w->time_label, LV_ALIGN_CENTER, 0, 0);
    rebuild_time_label(w);

    /* ---- Sec panel: absolute position, bottom-right ---- */
    lv_obj_t *sec_panel = make_panel(w->root,
                                      SEC_PANEL_X, SEC_PANEL_Y,
                                      SEC_PANEL_W, SEC_PANEL_H);

    w->sec_label = lv_label_create(sec_panel);
    lv_obj_set_style_text_font(w->sec_label, w->font_sec, 0);
    lv_obj_set_style_text_color(w->sec_label, lv_color_hex(CLK_COL_SEC), 0);
    lv_obj_set_style_bg_opa(w->sec_label, LV_OPA_TRANSP, 0);
    lv_label_set_recolor(w->sec_label, true);
    lv_label_set_text(w->sec_label, "#111118 00#");
    lv_obj_align(w->sec_label, LV_ALIGN_CENTER, 0, 0);

    /* No colon timer -- blink is driven by PC time updates */
}

void ui_clock_widget_update(ui_clock_widget_t *w,
                             uint8_t hour, uint8_t min, uint8_t sec,
                             uint8_t month, uint8_t day, uint8_t wday)
{
    static const char *s_days[] = {
        "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
    };

    if (!w->root) return;

    /* Toggle colon when second changes */
    if (sec != w->cur_sec) {
        w->colon_visible = !w->colon_visible;
        w->cur_sec = sec;
    }

    w->has_data = true;
    w->cur_hour = hour;
    w->cur_min  = min;
    rebuild_time_label(w);

    char buf[16];
    snprintf(buf, sizeof(buf), "%02d", sec);
    lv_label_set_text(w->sec_label, buf);

    snprintf(buf, sizeof(buf), "%02d/%02d", month, day);
    lv_label_set_text(w->date_label, buf);

    if (wday < 7)
        lv_label_set_text(w->day_label, s_days[wday]);
}

void ui_clock_widget_set_no_data(ui_clock_widget_t *w)
{
    if (!w->root) return;
    w->has_data  = false;
    w->cur_sec   = 0xFF;
    rebuild_time_label(w);
    lv_label_set_text(w->sec_label,  "#111118 00#");
    lv_label_set_text(w->date_label, "#111118 00/00#");
    lv_label_set_text(w->day_label,  "#111118 XXX#");
}

void ui_clock_widget_destroy(ui_clock_widget_t *w)
{
    if (!w->root) return;

    w->time_label = NULL;
    w->sec_label  = NULL;
    w->date_label = NULL;
    w->day_label  = NULL;

    /* No colon_timer to delete */

    lv_obj_del(w->root);
    w->root = NULL;

    if (w->font_time && w->font_time != (lv_font_t *)&lv_font_montserrat_48)
        lv_font_free(w->font_time);
    if (w->font_sec  && w->font_sec  != (lv_font_t *)&lv_font_montserrat_48)
        lv_font_free(w->font_sec);
    if (w->font_date && w->font_date != (lv_font_t *)&lv_font_montserrat_48)
        lv_font_free(w->font_date);

    memset(w, 0, sizeof(*w));
}