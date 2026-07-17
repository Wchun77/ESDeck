#pragma once

#include "lvgl.h"

typedef enum {
    UI_MODE_DECK    = 0,
    UI_MODE_MONITOR = 1,
} ui_mode_t;

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