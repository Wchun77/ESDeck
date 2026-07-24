#pragma once

/* -----------------------------------------------------------------------
 * Full-screen, tail -f style viewer over sys_log.c's ring buffer -- shows
 * the full boot-to-now backlog on open, then keeps appending new lines
 * live while it stays open (auto-follows the bottom unless the user has
 * scrolled up to read history, same convention as any terminal pager).
 *
 * Not reachable from any menu -- the only entry point is a 5-second
 * press-and-hold on the "ESDeck" label in the Info dialog (see
 * item_info_cb() in ui_settings.c). Deliberately hidden; this is a
 * debug/support tool, not a normal-use feature.
 *
 * Tap anywhere on screen to close.
 * ----------------------------------------------------------------------- */

void ui_log_view_show(void);
