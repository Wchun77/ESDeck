#pragma once

#include "lvgl.h"

/* -----------------------------------------------------------------------
 * CJK notification font (pre-rasterized .bin, loaded from SD via LVGL's
 * own font loader).
 *
 * Lazily loads a curated common-Hanzi .bin font (converted ahead of time
 * via lv_font_conv --format bin -- see
 * doc/ESDeck_Monitor_字體轉換指南.md) from SD_PATH_ASSETS_FONTS_BIN_NOTIFY
 * on first call, then reuses the same lv_font_t* afterward.
 *
 * Deliberately NOT FreeType/TTF: an earlier version of this file
 * rasterized an arbitrary user-supplied .ttf on-device at runtime, which
 * turned out to have unbounded stack requirements with no way to
 * guarantee safety against a font we don't control (crashes reproduced
 * even on a 6KB dedicated task stack). Pre-rasterized .bin fonts are
 * bitmap-blit only -- no hinting bytecode interpreter, no recursive
 * composite-glyph resolution -- so that entire crash class doesn't
 * apply, and the glyph set is fixed/curated by us ahead of time (common
 * Hanzi only), not arbitrary user input.
 *
 * Returns NULL if the file isn't present on the SD card, or if loading
 * fails for any reason. Callers must treat NULL as "no CJK rendering
 * available right now" and fall back to ASCII-only content -- our own
 * hardcoded app-name labels or the raw ANCS bundle identifier, never a
 * phone-supplied localized display name (see design notes: that's the
 * one thing that can't be assumed ASCII-safe). A character outside the
 * curated common set falls back to LVGL's own configured missing-glyph
 * placeholder -- predictable and ours, never a phone-rendered tofu box.
 *
 * ui_font_cjk_get() is BLOCKING (the ~13s SD-card load happens inline)
 * and must only ever be called from the dedicated background preload
 * task (see cjk_font_preload_task() in ui.c) -- never from the LVGL task
 * or any other caller that can't afford to stall for that long. Every
 * other caller must use ui_font_cjk_try_get() instead, which never
 * blocks and never triggers the load itself; it only reports whatever
 * the preload task has gotten to so far. Calling ui_font_cjk_get() from
 * two different tasks concurrently, or from anywhere other than the
 * preload task, is undefined behavior -- there used to be exactly this
 * bug: a lazy "s_tried" flag got set *before* the load finished, so a
 * second caller racing the preload task could observe "already tried"
 * and read back a still-NULL s_font mid-load, instead of either the
 * genuinely finished font or a value it could tell was still pending.
 * ----------------------------------------------------------------------- */
const lv_font_t *ui_font_cjk_get(void);

typedef enum {
    UI_FONT_CJK_LOADING,      /* preload task hasn't finished yet -- retry later */
    UI_FONT_CJK_READY,        /* *out_font is valid */
    UI_FONT_CJK_UNAVAILABLE,  /* no file on SD / load failed -- no point retrying */
} ui_font_cjk_status_t;

/* Non-blocking, safe to call from any task at any time. *out_font is only
 * written when the return value is UI_FONT_CJK_READY (left untouched
 * otherwise, so callers can pre-initialize it to NULL and use it either
 * way without checking the return value twice). */
ui_font_cjk_status_t ui_font_cjk_try_get(const lv_font_t **out_font);
