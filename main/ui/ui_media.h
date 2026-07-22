#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"

/*
 * Media mode.
 *
 * Layout is its own "own sidebar sub-region + own content area" split,
 * with content specific to Media:
 *   - sidebar sub-region (80 x SCREEN_H-80): a single vertical level-meter
 *     bar, no page buttons.
 *   - content area (SCREEN_W-80 x SCREEN_H): a Now Playing player card
 *     (cover placeholder, title/artist, progress bar, transport buttons).
 *
 * Entirely HID-driven, no local fake/mock data -- see ui_media_push_progress()
 * / ui_media_push_level() below and HID_MEDIA_CMD_NOWPLAYING_PROGRESS /
 * HID_MEDIA_CMD_AUDIO_LEVEL in usb_hid.h, following the same subscribe-on-
 * enter/queue/timeout pattern as ui_monitor.c. Whenever no real data has
 * arrived yet or the PC stops sending (~3s of silence), the progress bar
 * and transport buttons go disabled (not draggable/clickable) and title/
 * artist/time show "None"/"-:--" rather than making anything up -- this is
 * meant to make "not actually connected to anything" visually obvious,
 * not to keep looking alive with placeholder motion.
 *
 * Transport buttons are one-way remote control: pressing one sends a
 * command to the PC (see usb_hid_media_play_pause() etc in usb_hid.h) and
 * relies on the resulting real progress packet to update the icon/track,
 * same as any other externally-driven state change.
 *
 * Title/artist always show "None" regardless of connection state -- there
 * is no title/artist HID protocol yet, that needs a PC-rendered image
 * pipeline (no CJK font on-device, see project notes §4.1) and is separate,
 * not-yet-designed work.
 *
 * Entry:
 *   1. caller tears down whatever mode was previously active
 *   2. ui_media_enter(sidebar)
 *
 * Exit:
 *   1. ui_media_exit()
 *   2. rebuild whichever mode is being switched to
 */

/* Build the sidebar bar + player card (starts in the disabled/"None"
 * state) and the HID-driven update timer.
 * sidebar is the shared static sidebar strip from ui_build_static(). */
void ui_media_enter(lv_obj_t *sidebar);

/* Tear down all media widgets and stop the update timer. */
void ui_media_exit(void);

/* Hide the player card (content area) without touching the sidebar bar,
 * which is left running underneath. Called by ui_settings_select() when
 * the gear button takes over the content area. */
void ui_media_deselect_current(void);

/* Counterpart to ui_media_deselect_current(): re-show the player card.
 * Called when the Settings page's root-level back button (shown as an X)
 * is used to close Settings and return to Media. */
void ui_media_reselect_current(void);

/* Push a real-time Now Playing progress update (position/duration in ms,
 * playing state) from HID. Internally queued (xQueueOverwrite) and
 * consumed by the media UI timer -- safe to call from the TinyUSB task
 * context, same pattern as ui_monitor_push_data(). No-op before
 * ui_media_enter() / after ui_media_exit(). */
void ui_media_push_progress(uint32_t position_ms, uint32_t duration_ms, bool playing);

/* Push a real-time system audio level (0-100) for the sidebar VU-meter bar,
 * from HID. Same queueing/timeout convention as ui_media_push_progress();
 * the bar drops to 0 whenever no real level has arrived yet or the PC
 * stops sending, rather than showing placeholder motion. */
void ui_media_push_level(uint8_t level);
