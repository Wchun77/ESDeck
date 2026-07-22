#pragma once

#include "lvgl.h"
#include "ui_deck_config.h"

/*
 * Deck UI: config-dependent sidebar page buttons and content pages.
 *
 * Lifecycle:
 *   ui_deck_build()   - allocate and render all deck widgets from cfg.
 *   ui_deck_destroy() - tear down widgets, free image pool and config.
 *
 * LRU eviction callbacks (called by ui_img_pool during PSRAM pressure):
 *   ui_deck_lazy_bg_remove_widgets()
 *   ui_deck_page_count()
 *   ui_deck_page_bg_image()
 *
 * Icon preload (Deck's own eager-decode feature -- see ui_deck_preload_icons()
 * doc comment below for why this lives here and not in ui_img_pool.c, which
 * is otherwise a generic pool with no mode-specific knowledge):
 *   ui_deck_preload_icons()
 *   ui_deck_preload_start() / ui_deck_preload_wait() / ui_deck_preload_take_cfg()
 */

/* Build all deck widgets (sidebar page buttons + content pages).
 * s_sidebar must already exist (created by ui_build_static()).
 * Consumes cfg by value; caller should not free it afterward. */
void ui_deck_build(lv_obj_t *sidebar, deck_cfg_t *cfg);

/* Tear down all deck widgets, free image pool, free config. */
void ui_deck_destroy(void);

/* Hide the currently shown page and clear its sidebar highlight without
 * selecting a new one. Called by ui_settings_select() when the gear
 * button takes over the content area. */
void ui_deck_deselect_current(void);

/* Counterpart to ui_deck_deselect_current(): re-show the current page and
 * restore its sidebar highlight, without changing s_cur_page. Called when
 * the Settings page's root-level back button (shown as an X) is used to
 * close Settings and return to whatever page was active before. */
void ui_deck_reselect_current(void);

/* Apply the lazy background image for the given page index.
 * No-op if the bg widget already exists or no bg_image is set. */
void ui_deck_lazy_bg_set(int page_idx);

/* Remove bg and mask widgets for page_idx (called during LRU eviction). */
void ui_deck_lazy_bg_remove_widgets(int page_idx);

/* Accessors used by ui_img_pool during LRU eviction. */
int         ui_deck_page_count(void);
const char *ui_deck_page_bg_image(int page_idx);

/* Reserve ui_img_pool capacity for cfg (icons + bg slots + Settings' own
 * bg slot) and eagerly decode every button/side icon it references.
 * Deck-specific: only Deck preloads icons up front -- Monitor has no
 * eager preload step. Used both by the boot preload sequence below and
 * by a plain Deck config switch (see ui_config_dialog.c). */
void ui_deck_preload_icons(const deck_cfg_t *cfg);

/* Boot-time preload: load Deck's own NVS-selected config and kick off
 * ui_deck_preload_icons() on a background task, so the pool is already
 * warm by the time ui_deck_build() runs. Call ui_deck_preload_start()
 * once, early in app_main(); ui_deck_preload_wait() blocks until the
 * background task finishes; ui_deck_preload_take_cfg() then hands over
 * the config it loaded, once, for ui_deck_build() to consume (see ui.c's
 * my_ui_init()). */
void ui_deck_preload_start(void);
void ui_deck_preload_wait(void);
deck_cfg_t *ui_deck_preload_take_cfg(void);

/* Config switch flow:
 *   1. Call ui_deck_destroy().
 *   2. Load new cfg with ui_deck_config_load().
 *   3. Call ui_deck_preload_icons() in a background task.
 *   4. Call ui_deck_build() via lv_async_call when done. */
