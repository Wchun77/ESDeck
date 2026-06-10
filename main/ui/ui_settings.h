#pragma once

#include "lvgl.h"

/* Build the context panel (gear menu) as a child of scr.
 * Must be called once during static UI setup.
 * Returns the panel object so ui.c can store it. */
lv_obj_t *ui_settings_build(lv_obj_t *scr);

/* Toggle the context panel visibility.
 * Called by the gear button click handler. */
void ui_settings_toggle(void);
