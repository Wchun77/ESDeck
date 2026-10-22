#pragma once

/* Show the floating keyboard overlay.
 * Clicking outside the panel dismisses it. */
void ui_keyboard_show(void);

#include <stdbool.h>

/* True while the keyboard overlay is up (Touchpad mode ignores pad input then). */
bool ui_keyboard_is_open(void);

/* Keyboard Zhuyin (Settings > Custom > Touchpad): show the standard (Dachen)
 * Bopomofo layout on the keys -- English small top-left, Zhuyin bottom-right.
 * While Shift or Fn is active the keys switch back to plain English
 * (upper case / symbols / F-keys, centered). Labels only: what's typed is
 * still decided by the PC's IME. Stored in NVS; takes effect the next
 * time the keyboard opens. */
bool ui_keyboard_get_zhuyin(void);
void ui_keyboard_set_zhuyin(bool on);
