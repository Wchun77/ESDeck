#pragma once

#include "lvgl.h"
#include "ui_config.h"

/*
 * PSRAM image pre-decode pool.
 *
 * Images decoded from SD card into PSRAM are cached by file path.
 * Background images are eligible for LRU eviction when PSRAM is full.
 * Icon images are never evicted.
 */

/* Allocate pool and decode all icon images referenced by cfg. */
void ui_img_pool_load(const deck_cfg_t *cfg);

/* Free all PSRAM pixel buffers and reset pool state. */
void ui_img_pool_free(void);

/* Return cached descriptor for path, or NULL if not in pool.
 * Updates last_used timestamp on hit. */
lv_img_dsc_t *ui_img_pool_find(const char *path);

/* Decode image into PSRAM and cache it.
 * Returns cached descriptor, or NULL on failure.
 * May trigger LRU eviction of a background image if PSRAM is full. */
lv_img_dsc_t *ui_img_pool_decode(const char *path);

/* Mark a pool entry as a background image, making it eligible for LRU eviction.
 * Must be called after ui_img_pool_decode() for bg images. */
void ui_img_pool_mark_bg(const char *path);

/* Boot preload sequence.
 * ui_preload_start(): load config from NVS, start background decode task.
 * ui_preload_wait():  block until decode task finishes. */
void ui_preload_start(void);
void ui_preload_wait(void);

/* Return pointer to the deck config loaded during boot preload.
 * Called once by ui_deck after ui_preload_wait(). */
deck_cfg_t *ui_img_pool_take_preload_cfg(void);
