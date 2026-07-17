#pragma once

#include "lvgl.h"

/* Screen dimensions — shared across all UI modules. */
#define SCREEN_W   800
#define SCREEN_H   480
#define SIDEBAR_W   80

/* Entry point — called once after ui_preload_wait(). */
void my_ui_init(void);

/* Show a full-screen "Switching config..." cover while image decode runs.
 * Deletes any previous cover first, so at most one ever exists. */
void ui_show_switching_screen(const char *msg);

/* Delete the switching-screen cover, if one is currently up. Call once the
 * destination UI (deck/monitor) has finished building on top of it, so it
 * doesn't linger as a leaked full-screen object. */
void ui_hide_switching_screen(void);

/* Accessors for shared static widgets.
 * Modules that need to parent into the sidebar or bring the context panel
 * to the foreground use these instead of storing their own copies. */
lv_obj_t *ui_get_sidebar(void);
lv_obj_t *ui_get_context_panel(void);
