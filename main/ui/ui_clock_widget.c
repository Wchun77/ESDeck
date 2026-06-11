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
        snprintf(buf, sizeof(buf), "#111118 00:00#");
    } else {
        uint32_t col = w->colon_visible ? w->cfg_col_time : 0x0a0a14;
        snprintf(buf, sizeof(buf), "%02d#%06lx :#%02d",
                 w->cur_hour, (unsigned long)col, w->cur_min);
    }
    lv_label_set_text(w->time_label, buf);
}

static lv_font_t *load_font(const char *path, bool *fallback_flag,
                             lv_font_t *fallback)
{
    if (!path || path[0] == '\0') {
        *fallback_flag = true;
        return fallback;
    }
    lv_font_t *f = lv_font_load(path);
    if (!f) {
        ESP_LOGW(TAG, "font load failed: %s, using fallback", path);
        *fallback_flag = true;
        return fallback;
    }
    ESP_LOGI(TAG, "font loaded: %s", path);
    return f;
}

void ui_clock_widget_create(ui_clock_widget_t *w, lv_obj_t *parent,
                             const mon_clock_cfg_t *cfg)
{
    memset(w, 0, sizeof(*w));
    w->colon_visible = true;
    w->has_data      = false;
    w->cur_sec       = 0xFF;

    /* Cache config colours -- fall back to defaults if cfg is NULL */
    w->cfg_col_time  = cfg ? cfg->col_time  : MON_CFG_DEF_COL_TIME;
    w->cfg_col_colon = cfg ? cfg->col_colon : MON_CFG_DEF_COL_COLON;
    w->cfg_col_date  = cfg ? cfg->col_date  : MON_CFG_DEF_COL_DATE;
    w->cfg_col_day   = cfg ? cfg->col_day   : MON_CFG_DEF_COL_DAY;
    w->cfg_col_sec   = cfg ? cfg->col_sec   : MON_CFG_DEF_COL_SEC;
    w->cfg_sep_color = cfg ? cfg->sep_color : MON_CFG_DEF_SEP_COLOR;
    w->cfg_sep_width = cfg ? cfg->sep_width : MON_CFG_DEF_SEP_WIDTH;

    /* Build font paths from config filenames */
    char path_time[MON_CFG_FONT_LEN + 24];
    char path_sec[MON_CFG_FONT_LEN + 24];
    char path_date[MON_CFG_FONT_LEN + 24];

    if (cfg) {
        ui_monitor_config_font_path(cfg->font_time, path_time, sizeof(path_time));
        ui_monitor_config_font_path(cfg->font_sec,  path_sec,  sizeof(path_sec));
        ui_monitor_config_font_path(cfg->font_date, path_date, sizeof(path_date));
    } else {
        path_time[0] = path_sec[0] = path_date[0] = '\0';
    }

    w->font_time = load_font(path_time, &w->font_fallback,
                             (lv_font_t *)&lv_font_montserrat_48);
    w->font_sec  = load_font(path_sec,  &w->font_fallback,
                             (lv_font_t *)&lv_font_montserrat_20);
    w->font_date = load_font(path_date, &w->font_fallback,
                             (lv_font_t *)&lv_font_montserrat_16);

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
    lv_obj_set_style_text_color(w->date_label, lv_color_hex(w->cfg_col_date), 0);
    lv_obj_set_style_bg_opa(w->date_label, LV_OPA_TRANSP, 0);
    lv_obj_align(w->date_label, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_label_set_recolor(w->date_label, true);
    lv_label_set_text(w->date_label, "#111118 00/00#");

    w->day_label = lv_label_create(date_panel);
    lv_obj_set_style_text_font(w->day_label, w->font_date, 0);
    lv_obj_set_style_text_color(w->day_label, lv_color_hex(w->cfg_col_day), 0);
    lv_obj_set_style_bg_opa(w->day_label, LV_OPA_TRANSP, 0);
    lv_obj_align(w->day_label, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_label_set_recolor(w->day_label, true);
    lv_label_set_text(w->day_label, "#111118 XXX#");

    if (w->cfg_sep_width > 0) {
        static lv_point_t sep_pts[2] = {{ 0, 0 }, { CLK_SEP_LEN, 0 }};
        lv_obj_t *sep = lv_line_create(date_panel);
        lv_line_set_points(sep, sep_pts, 2);
        lv_obj_set_style_line_color(sep, lv_color_hex(w->cfg_sep_color), 0);
        lv_obj_set_style_line_width(sep, w->cfg_sep_width, 0);
        lv_obj_align(sep, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    }

    /* ---- Time panel ---- */
    int time_panel_y = (CH - TIME_PANEL_H) / 2;
    lv_obj_t *time_panel = make_panel(w->root,
                                       TIME_PANEL_X, time_panel_y,
                                       TIME_PANEL_W, TIME_PANEL_H);

    w->time_label = lv_label_create(time_panel);
    lv_obj_set_style_text_font(w->time_label, w->font_time, 0);
    lv_obj_set_style_text_color(w->time_label, lv_color_hex(w->cfg_col_time), 0);
    lv_obj_set_style_bg_opa(w->time_label, LV_OPA_TRANSP, 0);
    lv_label_set_recolor(w->time_label, true);
    lv_obj_align(w->time_label, LV_ALIGN_CENTER, 0, 0);
    rebuild_time_label(w);

    /* ---- Sec panel ---- */
    int sec_x = CW - SEC_PANEL_W - 4;
    int sec_y = CH - SEC_PANEL_H - 12;
    lv_obj_t *sec_panel = make_panel(w->root, sec_x, sec_y,
                                      SEC_PANEL_W, SEC_PANEL_H);

    w->sec_label = lv_label_create(sec_panel);
    lv_obj_set_style_text_font(w->sec_label, w->font_sec, 0);
    lv_obj_set_style_text_color(w->sec_label, lv_color_hex(w->cfg_col_sec), 0);
    lv_obj_set_style_bg_opa(w->sec_label, LV_OPA_TRANSP, 0);
    lv_label_set_recolor(w->sec_label, true);
    lv_label_set_text(w->sec_label, "#111118 00#");
    lv_obj_align(w->sec_label, LV_ALIGN_CENTER, 0, 0);
}

void ui_clock_widget_update(ui_clock_widget_t *w,
                             uint8_t hour, uint8_t min, uint8_t sec,
                             uint8_t month, uint8_t day, uint8_t wday)
{
    static const char *s_days[] = {
        "SUN", "MON", "TUE", "WED", "THU", "FRI", "SAT"
    };

    if (!w->root) return;

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
    w->has_data = false;
    w->cur_sec  = 0xFF;
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

    lv_obj_del(w->root);
    w->root = NULL;

    if (w->font_time && w->font_time != (lv_font_t *)&lv_font_montserrat_48)
        lv_font_free(w->font_time);
    if (w->font_sec  && w->font_sec  != (lv_font_t *)&lv_font_montserrat_20)
        lv_font_free(w->font_sec);
    if (w->font_date && w->font_date != (lv_font_t *)&lv_font_montserrat_16)
        lv_font_free(w->font_date);

    memset(w, 0, sizeof(*w));
}