#pragma once

#include "lvgl.h"
#include <stdbool.h>

/*
 * PSRAM image pre-decode pool -- generic path-keyed cache shared by every
 * mode that needs one: Deck's own page/icon images (see ui_deck.c's
 * ui_deck_lazy_bg_set() / ui_deck_preload_icons()), Monitor's per-page
 * backgrounds (see ui_monitor.c's monitor_lazy_bg_set()), and Settings'
 * own bg/icon while either mode is active (see ui_settings.c's
 * settings_lazy_bg_set()). Deliberately knows nothing about any one
 * mode's config format or struct layout: callers reserve capacity, hand
 * it full LVGL-FS paths, and the pool decodes/caches/evicts by path
 * alone. The eviction match loop (in ui_img_pool.c) is the only place
 * that reaches out to specific modes, via extern accessor callbacks
 * (ui_deck_page_count()/ui_deck_page_bg_image(), ui_monitor_page_count()/
 * ui_monitor_page_bg_image(), ui_settings_current_bg_path()) rather than
 * by including any mode's header.
 *
 * Images decoded from SD card into PSRAM are cached by file path.
 * Background images are eligible for LRU eviction when PSRAM is full --
 * on OOM, ui_img_pool_decode() frees the least-recently-used bg entry and
 * asks whichever consumer owns it to tear down its now-stale widget,
 * then retries once. Icon images are never evicted once marked as such
 * (the default -- see ui_img_pool_mark_bg()).
 */

/* Allocate pool sized for exactly cap slots without decoding anything.
 * Caller is responsible for its own capacity accounting (icons + bg
 * slots + any shared slots like Settings' own bg) and for calling
 * ui_img_pool_decode() itself afterward for anything it wants eagerly
 * resident -- see ui_deck.c's ui_deck_preload_icons() for the one mode
 * that does eager decoding, and ui_monitor.c's ui_monitor_enter() for a
 * reserve-only, fully-lazy caller. */
void ui_img_pool_reserve(int cap);

/* Free all PSRAM pixel buffers and reset pool state. Called on both Deck
 * exit (ui_deck_destroy()) and Monitor exit (ui_monitor_exit()). */
void ui_img_pool_free(void);

/* Return cached descriptor for path, or NULL if not in pool.
 * Updates last_used timestamp on hit. */
lv_img_dsc_t *ui_img_pool_find(const char *path);

/* Decode image into PSRAM and cache it.
 * Returns cached descriptor, or NULL on failure.
 * May trigger LRU eviction of a background image if PSRAM is full. */
lv_img_dsc_t *ui_img_pool_decode(const char *path);

/* Mark a pool entry as a background image, making it eligible for LRU
 * eviction. Must be called after ui_img_pool_decode() for bg images. */
void ui_img_pool_mark_bg(const char *path);
