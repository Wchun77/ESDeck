#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Global log ring buffer -- lives here (not under ui/) for the same reason
 * sys_clock.c does: a boot-to-shutdown singleton, not owned by any UI
 * mode, that something in ui/ (ui_log_view.c) later reads from.
 *
 * Hooks esp_log_set_vprintf() to duplicate every ESP_LOG* line into a 64KB
 * PSRAM ring buffer, in addition to -- never instead of -- whatever the
 * previously-installed vprintf already did (UART0 / USB-Serial-JTAG
 * console output keeps working exactly as before; this is purely an
 * additional tap, not a replacement). Capture starts the moment
 * sys_log_init() runs, independent of whether anything is ever open to
 * view it -- call it as the very first thing in app_main(), before
 * anything else has a chance to log, so the buffer already holds the full
 * boot log by the time a viewer opens.
 *
 * Readers use a cursor (sys_log_cursor_t) to track their own position
 * independently -- there can be multiple readers, each just tracking how
 * far it's read. If a reader falls behind and the writer wraps past
 * unread data, the next sys_log_read() resyncs the cursor to the new
 * oldest byte and reports it via dropped_out, rather than returning
 * garbage.
 * ----------------------------------------------------------------------- */

/* Call once, before anything else may log. */
void sys_log_init(void);

/* Ring buffer capacity in bytes -- exposed so a reader (ui_log_view.c)
 * can size a one-shot scratch buffer for draining the full backlog
 * without guessing. */
#define SYS_LOG_RING_SIZE  (64 * 1024)

typedef struct {
    uint32_t pos;   /* absolute byte offset into the log stream, monotonic */
} sys_log_cursor_t;

/* Seed a cursor at the oldest byte still available in the buffer -- use
 * once when opening a viewer to start from the full backlog. */
sys_log_cursor_t sys_log_cursor_start(void);

/* Copy whatever has been written since *cursor into out (NUL-terminated,
 * at most out_cap - 1 bytes), advancing *cursor past what was copied.
 * Returns bytes copied (0 if nothing new since last call).
 *
 * out_cap should be a modest chunk size (a few KB), not the whole 64KB --
 * the copy happens inside a short critical section (interrupts disabled)
 * sized to out_cap, so a huge single call would hold interrupts off for
 * too long. To drain a large backlog, call this repeatedly in a loop
 * until it returns 0; each call only holds the lock for its own chunk.
 *
 * dropped_out (may be NULL) is set true for one call if the reader had
 * fallen behind far enough that the writer already overwrote data it
 * hadn't read yet -- the cursor is resynced to the new oldest byte in
 * that case, so the caller can show a "older lines dropped" marker. */
size_t sys_log_read(sys_log_cursor_t *cursor, char *out, size_t out_cap,
                     bool *dropped_out);
