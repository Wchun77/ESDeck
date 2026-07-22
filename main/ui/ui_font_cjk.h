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
 * ----------------------------------------------------------------------- */
const lv_font_t *ui_font_cjk_get(void);
