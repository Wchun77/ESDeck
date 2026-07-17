#pragma once

#include "lvgl.h"

/*
 * Monitor mode: clock page (fixed) + up to MON_PAGE_MAX data pages.
 *
 * Entry:
 *   1. ui_deck_destroy()         -- caller frees deck img pool
 *   2. ui_monitor_enter(sidebar) -- builds monitor sidebar + pages
 *
 * Exit:
 *   1. ui_monitor_exit()         -- tears down monitor widgets + timers
 *   2. ui_deck_build(...)        -- caller rebuilds deck
 */

/* -----------------------------------------------------------------------
 * Bar appearance defines
 * ----------------------------------------------------------------------- */

/* bar_max values per sensor type (degrees C or %) */
#define MON_BAR_MAX_USAGE   100
#define MON_BAR_MAX_TEMP    105

/* Five-step colour thresholds (% of bar_max) */
#define MON_BAR_THR_LOW     40
#define MON_BAR_THR_MID     60
#define MON_BAR_THR_HIGH    75
#define MON_BAR_THR_CRIT    90

/* Bar colours */
#define MON_BAR_COL_LOW     0x0055cc   /* blue   -- idle       */
#define MON_BAR_COL_MID     0x00aa44   /* green  -- normal     */
#define MON_BAR_COL_HIGH    0xddbb00   /* yellow -- elevated   */
#define MON_BAR_COL_WARN    0xff7700   /* orange -- high       */
#define MON_BAR_COL_CRIT    0xff2222   /* red    -- critical   */

/* Build monitor sidebar pages and content, start update timer.
 * sidebar is the shared static sidebar strip from ui_build_static(). */
void ui_monitor_enter(lv_obj_t *sidebar);

/* Tear down all monitor widgets and stop the update timer. */
void ui_monitor_exit(void);

/* Hide the currently shown page and clear its sidebar highlight without
 * selecting a new one. Called by ui_settings_select() when the gear
 * button takes over the content area. */
void ui_monitor_deselect_current(void);

/* Called by ui_monitor data receiver to push fresh system data.
 * Safe to call from any task — posts via lv_async_call internally. */
typedef struct {
    float    cpu_usage;   /* 0.0 - 100.0 %      */
    float    cpu_temp;    /* degrees C           */
    float    ram_usage;   /* 0.0 - 100.0 %      */
    float    gpu_usage;   /* 0.0 - 100.0 %      */
    float    gpu_temp;    /* degrees C           */
    float    gpu_vram;    /* 0.0 - 100.0 %      */
    float    cpu_freq;    /* GHz (decoded)       */
    float    net_up;      /* MB/s                */
    float    net_down;    /* MB/s                */
    float    disk_usage;  /* 0.0 - 100.0 %      */
    float    cpu_power;   /* W                   */
    float    gpu_power;   /* W (decoded x2)      */
    float    ssd_life;    /* 0.0 - 100.0 %      */
} monitor_data_t;

void ui_monitor_push_data(const monitor_data_t *data);