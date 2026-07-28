#pragma once

#include "lvgl.h"

/* -----------------------------------------------------------------------
 * Generic top-level notification banner.
 *
 * A single reusable overlay that slides down from the top edge of the
 * screen over whatever is currently showing (Deck/Monitor/Media, even the
 * Settings page or the "switching config" cover), holds for a few seconds,
 * then slides back up on its own. A swipe-up gesture on the banner
 * dismisses it early.
 *
 * Events are queued -- only one shows at a time. Pushing an event whose
 * merge_key matches the one currently showing (or still waiting in the
 * queue) updates that entry's label/count and restarts its hold timer
 * instead of enqueuing a separate banner. merge_key == NULL (or "") means
 * "never merge, always its own entry" -- used for one-off system events
 * like BLE/HID connect/disconnect. A real merge_key (e.g. a future ANCS
 * notification's app identifier) is what lets "LINE x1" turn into
 * "LINE x2" instead of showing two banners back to back.
 *
 * count is the caller's current total for that merge_key (not a delta) --
 * the caller owns the real count (e.g. an ANCS Added/Removed-tracked
 * per-app active set later on); this module only displays whatever it's
 * given.
 * ----------------------------------------------------------------------- */

/* Call once from my_ui_init(), after ui_build_static() so the overlay is
 * created last and naturally sits above the sidebar/context panel. */
void ui_toast_init(void);

/* Enqueue a banner. label and merge_key are copied (no need to keep either
 * pointer alive past the call). count < 1 is treated as 1; count == 1
 * hides the "xN" suffix, count > 1 shows it (e.g. "LINE  x3").
 *
 * font selects which font renders this banner's label -- pass NULL for
 * the default (montserrat_20, ASCII-only, used by our own fixed labels
 * like "BLE Connected"). Real ANCS notification text needs a CJK-capable
 * font instead (see ui_font_cjk_get()) since montserrat_20 has no Hanzi
 * glyphs and would just show missing-glyph placeholders. Each queued
 * banner remembers its own font, so an ASCII system toast and a CJK
 * notification toast can be queued back to back without one clobbering
 * the other's font.
 *
 * Must be called from LVGL context -- same rule as every other ui_* call
 * in this codebase. If the caller is a FreeRTOS task outside the LVGL
 * thread (e.g. the ANCS/BLE host task), wrap the call in lv_async_call()
 * like ui_settings.c's enter_monitor_task() does for its own callbacks. */
void ui_toast_push(const char *label, int count, const char *merge_key,
                    const lv_font_t *font);
