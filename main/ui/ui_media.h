#pragma once

#include "lvgl.h"

/*
 * Media mode -- UI PROTOTYPE ONLY.
 *
 * Layout mirrors Deck/Monitor's "own sidebar sub-region + own content
 * area" split, but with different content:
 *   - sidebar sub-region (80 x SCREEN_H-80): a single vertical level-meter
 *     bar, no page buttons.
 *   - content area (SCREEN_W-80 x SCREEN_H): a mock Now Playing player
 *     card (cover placeholder, title/artist, progress bar, transport
 *     buttons).
 *
 * Everything is driven by a local fake-data timer (see ui_media.c) --
 * there is no PC/HID wiring yet. That comes later once this layout is
 * validated; see ui_deck.h / ui_monitor.h for the pattern this will
 * eventually follow (HID subscribe/unsubscribe on enter/exit).
 *
 * Entry:
 *   1. destroy whatever mode was active (ui_deck_destroy() / ui_monitor_exit())
 *   2. ui_media_enter(sidebar)
 *
 * Exit:
 *   1. ui_media_exit()
 *   2. rebuild whichever mode is being switched to
 */

/* Build the sidebar bar + player card, start the fake-data timer.
 * sidebar is the shared static sidebar strip from ui_build_static(). */
void ui_media_enter(lv_obj_t *sidebar);

/* Tear down all media widgets and stop the fake-data timer. */
void ui_media_exit(void);

/* Hide the player card (content area) without touching the sidebar bar,
 * which is left running underneath -- same idea as
 * ui_deck_deselect_current() / ui_monitor_deselect_current(). Called by
 * ui_settings_select() when the gear button takes over the content area. */
void ui_media_deselect_current(void);

/* Counterpart to ui_media_deselect_current(): re-show the player card.
 * Called when the Settings page's root-level back button (shown as an X)
 * is used to close Settings and return to Media. */
void ui_media_reselect_current(void);
