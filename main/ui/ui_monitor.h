#pragma once

#include "lvgl.h"

/*
 * Monitor mode: two fixed pages (clock, system).
 *
 * Entry:
 *   1. ui_deck_destroy()         -- caller frees deck img pool
 *   2. ui_monitor_enter(sidebar) -- builds monitor sidebar + pages
 *
 * Exit:
 *   1. ui_monitor_exit()         -- tears down monitor widgets + timers
 *   2. ui_deck_build(...)        -- caller rebuilds deck
 */

/* Page indices */
#define MON_PAGE_CLOCK   0
#define MON_PAGE_SYSTEM  1
#define MON_PAGE_COUNT   2

/* Build monitor sidebar pages and content, start update timer.
 * sidebar is the shared static sidebar strip from ui_build_static(). */
void ui_monitor_enter(lv_obj_t *sidebar);

/* Tear down all monitor widgets and stop the update timer. */
void ui_monitor_exit(void);

/* Called by ui_monitor data receiver to push fresh system data.
 * Safe to call from any task — posts via lv_async_call internally. */
typedef struct {
    float    cpu_usage;   /* 0.0 - 100.0 */
    float    cpu_temp;    /* degrees C   */
    float    ram_usage;   /* 0.0 - 100.0 */
    float    gpu_usage;   /* 0.0 - 100.0 */
    float    gpu_temp;    /* degrees C   */
    float    gpu_vram;    /* 0.0 - 100.0 */
} monitor_data_t;

void ui_monitor_push_data(const monitor_data_t *data);