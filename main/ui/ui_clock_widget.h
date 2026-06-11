#pragma once

#include "lvgl.h"
#include <stdint.h>
#include <stdbool.h>

#define CLK_FONT_PATH_TIME  "S:/sdcard/fonts/oxanium_270.bin"
#define CLK_FONT_PATH_SEC   "S:/sdcard/fonts/oxanium_48.bin"
#define CLK_FONT_PATH_DATE  "S:/sdcard/fonts/oxanium_36.bin"

#define CLK_COL_TIME   0xf0f2ff
#define CLK_COL_COLON  0x1e2e66
#define CLK_COL_DATE   0x5577dd
#define CLK_COL_DAY    0x4466bb
#define CLK_COL_SEC    0x3d57aa

typedef struct {
    lv_font_t  *font_time;
    lv_font_t  *font_sec;
    lv_font_t  *font_date;

    lv_obj_t   *date_label;
    lv_obj_t   *day_label;
    lv_obj_t   *time_label;
    lv_obj_t   *sec_label;

    bool        colon_visible;
    uint8_t     cur_hour;
    uint8_t     cur_min;
    uint8_t     cur_sec;   /* 0xFF = not yet received */
    bool        has_data;

    lv_obj_t   *root;
    bool        font_fallback;
} ui_clock_widget_t;

void ui_clock_widget_create(ui_clock_widget_t *w, lv_obj_t *parent);

void ui_clock_widget_update(ui_clock_widget_t *w,
                             uint8_t hour, uint8_t min, uint8_t sec,
                             uint8_t month, uint8_t day, uint8_t wday);

void ui_clock_widget_set_no_data(ui_clock_widget_t *w);

void ui_clock_widget_destroy(ui_clock_widget_t *w);