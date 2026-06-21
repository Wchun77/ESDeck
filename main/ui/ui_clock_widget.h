#pragma once

#include "lvgl.h"
#include "ui_monitor_config.h"
#include <stdint.h>
#include <stdbool.h>

#define CLK_SEP_LEN  160   /* separator line length px */

typedef struct {
    lv_font_t  *font_time;
    lv_font_t  *font_sec;
    lv_font_t  *font_date;

    lv_obj_t   *date_label;
    lv_obj_t   *day_label;
    lv_obj_t   *h_tens_label;    /* hour tens digit  */
    lv_obj_t   *h_units_label;   /* hour units digit */
    lv_obj_t   *colon_label;
    lv_obj_t   *m_tens_label;    /* minute tens digit  */
    lv_obj_t   *m_units_label;   /* minute units digit */
    lv_obj_t   *sec_label;

    bool        colon_visible;
    uint8_t     cur_hour;
    uint8_t     cur_min;
    uint8_t     cur_sec;   /* 0xFF = not yet received */
    bool        has_data;

    /* Cached config values */
    uint32_t    cfg_col_time;
    uint32_t    cfg_col_colon;
    uint32_t    cfg_col_date;
    uint32_t    cfg_col_day;
    uint32_t    cfg_col_sec;
    uint32_t    cfg_sep_color;
    int         cfg_sep_width;
    uint8_t     cfg_opa_time;
    uint8_t     cfg_opa_colon;
    uint8_t     cfg_opa_date;
    uint8_t     cfg_opa_day;
    uint8_t     cfg_opa_sec;

    lv_obj_t   *root;
    bool        font_fallback;
} ui_clock_widget_t;

void ui_clock_widget_create(ui_clock_widget_t *w, lv_obj_t *parent,
                             const mon_clock_cfg_t *cfg);

void ui_clock_widget_update(ui_clock_widget_t *w,
                             uint8_t hour, uint8_t min, uint8_t sec,
                             uint8_t month, uint8_t day, uint8_t wday);

void ui_clock_widget_set_no_data(ui_clock_widget_t *w);

void ui_clock_widget_destroy(ui_clock_widget_t *w);