#pragma once

#include "lvgl.h"

typedef enum {
    UI_MODE_DECK    = 0,
    UI_MODE_MONITOR = 1,
} ui_mode_t;

/* Build the context panel (gear menu) as a child of scr.
 * Must be called once during static UI setup.
 * Returns the panel object so ui.c can store it. */
lv_obj_t *ui_settings_build(lv_obj_t *scr);

/* Toggle the context panel visibility.
 * Called by the gear button click handler. */
void ui_settings_toggle(void);

/* Return the current UI mode. */
ui_mode_t ui_settings_get_mode(void);

/* Exit and re-enter monitor mode to apply a new config.
 * Shows switching screen and re-enters asynchronously. */
void ui_settings_monitor_reload(void);