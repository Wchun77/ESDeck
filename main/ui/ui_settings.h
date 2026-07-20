#pragma once

#include "lvgl.h"

typedef enum {
    UI_MODE_DECK    = 0,
    UI_MODE_MONITOR = 1,
    UI_MODE_MEDIA   = 2,
} ui_mode_t;

/* Settings page appearance -- bg_image + side_icon, same filename-only
 * convention as a normal Deck/Monitor page (ui_config.h / ui_monitor_config.h).
 * Lives embedded in deck_cfg_t / monitor_cfg_t (one "settings" object per
 * config file, sibling to "pages" / "clock") instead of its own standalone
 * file, so Settings' look follows whichever Deck or Monitor config is
 * currently active -- same idea as Monitor's own fixed Clock page. */
#define UI_SETTINGS_BG_LEN        64
#define UI_SETTINGS_SIDE_ICON_LEN 32

typedef struct {
    char bg_image[UI_SETTINGS_BG_LEN];
    char side_icon[UI_SETTINGS_SIDE_ICON_LEN];
} ui_settings_appearance_t;

/* Build the Settings page as a child of scr, styled like a normal
 * deck/monitor page (same size/position as the content area). gear_btn is
 * the sidebar gear button -- stored so it can be highlighted/unhighlighted
 * the same way a sidebar page button is.
 * Must be called once during static UI setup, after the gear button
 * already exists. Returns the panel object so ui.c can store it. */
lv_obj_t *ui_settings_build(lv_obj_t *scr, lv_obj_t *gear_btn);

/* Select the Settings page: hides whatever Deck/Monitor page is currently
 * showing, resets the menu to its root level, and highlights the gear
 * button. Called by the gear button click handler. */
void ui_settings_select(void);

/* Deselect the Settings page: hides it and unhighlights the gear button,
 * without touching Deck/Monitor page state. No-op if not currently shown.
 * Called by Deck/Monitor's sidebar_btn_cb when the user picks a normal
 * page -- picking a page is the only way to leave Settings, there is no
 * close button on the page itself. */
void ui_settings_deselect(void);

/* Return the current UI mode. */
ui_mode_t ui_settings_get_mode(void);

/* Exit and re-enter monitor mode to apply a new config.
 * Shows switching screen and re-enters asynchronously. */
void ui_settings_monitor_reload(void);

/* Update the Settings page's background image and gear sidebar icon to
 * match whichever Deck or Monitor config is currently active. Called by
 * ui_deck_build() and ui_monitor_enter() right after loading their own
 * config. Passing all-empty fields (or NULL) falls back to the plain
 * background + default LV_SYMBOL_SETTINGS glyph. Safe to call even while
 * Settings isn't currently selected -- the background is still applied
 * lazily on next ui_settings_select(), only the "which file" bookkeeping
 * updates immediately. */
void ui_settings_apply_appearance(const ui_settings_appearance_t *appearance);