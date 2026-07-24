#pragma once

#include <stdbool.h>
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Global self-ticking wall clock -- lives here (not under ui/ or usb/)
 * because it's neither UI-mode-owned nor USB-protocol-owned: it's a
 * boot-to-shutdown singleton that outlives every mode switch, fed by
 * usb_hid.c's CMD_TIME callback (registered once in my_ui_init(), not
 * per-mode -- see ui.c) and read by whichever mode wants to display it
 * (currently only ui_monitor.c's clock page).
 *
 * Value is derived on demand (epoch-seconds-at-last-sync + hardware-timer-
 * measured monotonic elapsed time since then, see sys_clock.c), not
 * accumulated by counting periodic callback firings -- an lv_timer only
 * fires "at least every N ms", not "exactly every N ms", and mode
 * switches block the LVGL task for a while rebuilding pages, so a naive
 * per-callback tick would silently lose whatever real time the task was
 * blocked for. This way the clock stays correct across day/month/year
 * boundaries (via mktime()/localtime_r(), not carried over from whatever
 * the last packet said) regardless of how choppy the LVGL task's own
 * scheduling gets. A fresh PC packet only hard-corrects the clock if it
 * has drifted from this by more than 1 second (small jitter from the
 * PC-side timer is otherwise ignored so the display doesn't flicker).
 *
 * sys_clock_is_valid() is false from boot until the very first CMD_TIME
 * packet ever arrives, and stays true for the rest of the session after
 * that -- it does not revert to false if packets stop arriving later
 * (e.g. the PC is connected but not in Monitor mode, so isn't streaming
 * corrections). Once the clock has a starting point it's trusted to
 * free-run on its own.
 * ----------------------------------------------------------------------- */

typedef struct {
    uint8_t hour, min, sec;
    uint8_t month, day, wday;   /* wday: 0=Sunday, matches struct tm */
} sys_time_t;

/* Call once at boot (my_ui_init(), under the LVGL lock) -- starts the
 * background timer that drains incoming CMD_TIME packets, independent of
 * which UI mode is active. */
void sys_clock_init(void);

/* Feed a CMD_TIME packet in from usb_hid.c. Called from the TinyUSB task
 * context -- queue-based hand-off into the LVGL tick, same convention as
 * ui_monitor.c's own HID data callbacks (build a plain struct, no libc/
 * LVGL calls here). year is the full year (e.g. 2026); needed internally
 * for correct mktime() rollover even though nothing currently displays it. */
void sys_clock_push_hid_time(uint16_t year, uint8_t month, uint8_t day,
                              uint8_t hour, uint8_t min, uint8_t sec);

/* True once the first CMD_TIME packet has ever been received this
 * session (see rollover note above -- never reverts to false). */
bool sys_clock_is_valid(void);

/* Current self-ticking display time. Only meaningful once
 * sys_clock_is_valid() is true. */
sys_time_t sys_clock_get(void);
