#pragma once

#include "lvgl.h"
#include <stdbool.h>

/*
 * Monitor background image manager.
 *
 * Simple load-all / free-all model -- no LRU, no lazy load.
 * All page background images are loaded into PSRAM when entering Monitor
 * mode and freed when exiting.
 *
 * Image paths are configured via ui_monitor_img_set_path() before calling
 * ui_monitor_img_load_all(). Unset paths are silently skipped.
 *
 * Usage:
 *   ui_monitor_img_set_path(MON_PAGE_CLOCK,  "S:/sdcard/clock_bg.jpg");
 *   ui_monitor_img_set_path(MON_PAGE_SYSTEM, "S:/sdcard/sys_bg.jpg");
 *   ui_monitor_img_load_all();
 *   ...
 *   lv_img_dsc_t *dsc = ui_monitor_img_get(MON_PAGE_CLOCK);
 *   if (dsc) { lv_img_set_src(bg_obj, dsc); }
 *   ...
 *   ui_monitor_img_free_all();
 */

#define MON_IMG_PATH_LEN  128

/*
 * Set the background image path for a given page index.
 * Must be called before ui_monitor_img_load_all().
 * path: full LVGL FS path, e.g. "S:/sdcard/IMG_3238.jpg"
 *       Pass NULL or "" to clear.
 */
void ui_monitor_img_set_path(int page_idx, const char *path);

/*
 * Decode and load all configured background images into PSRAM.
 * Safe to call multiple times -- already-loaded images are skipped.
 */
void ui_monitor_img_load_all(void);

/*
 * Return the decoded image descriptor for page_idx, or NULL if not loaded.
 */
lv_img_dsc_t *ui_monitor_img_get(int page_idx);

/*
 * Free all loaded images and reset all paths.
 * Call when exiting Monitor mode.
 */
void ui_monitor_img_free_all(void);
