#pragma once

#include "lvgl.h"
#include "ui_monitor_config.h"   /* MON_TOTAL_PAGE_MAX */
#include <stdbool.h>

/*
 * Monitor background image manager.
 *
 * Lazy, per-page load model -- same shape as ui_deck.c's
 * ui_deck_lazy_bg_set(): each page's background is only decoded into
 * PSRAM the first time that page is actually shown, not for every page
 * up front on Monitor entry. A Monitor config can have many pages, each
 * with its own multi-hundred-KB background photo -- decoding all of them
 * on entry regardless of whether the user ever visits that page was
 * real, avoidable PSRAM pressure (see ui_monitor.c's monitor_lazy_bg_set()
 * for the call site).
 *
 * Lazy loading alone only defers WHEN a page's bg is decoded, not how
 * many stay resident -- visiting enough distinct pages in one session
 * would still exhaust PSRAM since nothing was ever freed until Monitor
 * was exited entirely. ui_monitor_img_load_one() also does LRU eviction
 * on PSRAM OOM (same pattern as ui_img_pool.c's pool for Deck): it frees
 * the least-recently-visited *other* slot's buffer and asks whoever owns
 * that slot to remove its now-stale bg image widget, then retries once.
 *
 * Slot MON_IMG_SETTINGS_SLOT is reserved (beyond the real page indices
 * 0..MON_TOTAL_PAGE_MAX-1) for ui_settings.c's own background image while
 * Monitor mode is active -- Settings shares this same lazy+LRU pool
 * instead of holding a separate always-resident buffer that doesn't
 * compete/cooperate with page images for the same PSRAM budget (see
 * ui_settings.c's settings_lazy_bg_set()).
 *
 * Image paths are configured via ui_monitor_img_set_path() up front (this
 * only records the path, it doesn't touch PSRAM); ui_monitor_img_load_one()
 * decodes a single slot on demand. Unset paths are silently skipped.
 *
 * Usage:
 *   ui_monitor_img_set_path(MON_PAGE_CLOCK,  "S:/sdcard/clock_bg.jpg");
 *   ui_monitor_img_set_path(MON_PAGE_SYSTEM, "S:/sdcard/sys_bg.jpg");
 *   ...
 *   ui_monitor_img_load_one(MON_PAGE_CLOCK);       // only when that page is shown
 *   lv_img_dsc_t *dsc = ui_monitor_img_get(MON_PAGE_CLOCK);
 *   if (dsc) { lv_img_set_src(bg_obj, dsc); }
 *   ...
 *   ui_monitor_img_free_all();
 */

#define MON_IMG_PATH_LEN       128
#define MON_IMG_SETTINGS_SLOT  MON_TOTAL_PAGE_MAX
#define MON_IMG_SLOT_COUNT     (MON_TOTAL_PAGE_MAX + 1)

/*
 * Set the background image path for a given slot (a real page index, or
 * MON_IMG_SETTINGS_SLOT for Settings' own bg).
 * path: full LVGL FS path, e.g. "S:/sdcard/IMG_3238.jpg"
 *       Pass NULL or "" to clear.
 */
void ui_monitor_img_set_path(int page_idx, const char *path);

/*
 * Decode and load the background image for a single slot into PSRAM, if
 * it has a path set and isn't already loaded. Safe to call every time a
 * page/Settings is selected -- already-loaded slots are a no-op (just
 * refreshes the LRU timestamp).
 * Returns true if page_idx now has a loaded image (either just now or
 * already before), false if there's no path set or decoding failed.
 */
bool ui_monitor_img_load_one(int page_idx);

/*
 * Return the decoded image descriptor for a slot, or NULL if not loaded.
 */
lv_img_dsc_t *ui_monitor_img_get(int page_idx);

/*
 * Free all loaded images (including the Settings slot, if in use) and
 * reset all paths. Call when exiting Monitor mode.
 */
void ui_monitor_img_free_all(void);
