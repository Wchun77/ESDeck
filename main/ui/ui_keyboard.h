#pragma once

/* Show the floating keyboard overlay.
 * Clicking outside the panel dismisses it. */
void ui_keyboard_show(void);

#include <stdbool.h>

/* True while the keyboard overlay is up (Touchpad mode ignores pad input then). */
bool ui_keyboard_is_open(void);
