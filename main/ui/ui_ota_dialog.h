#pragma once

/*
 * Checks the SD card for a valid firmware update (see ota_manager.h).
 * If none is found, returns immediately (no-op).
 * If one is found, shows a confirm dialog on lv_scr_act() and BLOCKS the
 * calling task until the user answers:
 *   - "No"  -> dialog closes, function returns, normal boot continues.
 *   - "Yes" -> shows a progress screen and flashes the update.
 *              On success the device reboots (this function never returns).
 *              On failure, shows an error, then returns so boot can continue.
 *
 * Must be called from a plain task (e.g. app_main), NOT from inside an
 * LVGL event callback, since it blocks on a semaphore that an LVGL
 * button callback signals.
 *
 * Called before boot_anim_play(): turns the backlight on for the dialog
 * and off again before returning (screen left black).
 */
void ui_ota_check_and_prompt(void);

/*
 * Accept firmware updates pushed from ESDeckPC over HID (usb_hid.h,
 * HID_OTA_*): shows the same progress screen as the SD update, flashes,
 * and reboots on success; on failure shows an error and returns to the
 * UI. Call once at boot after the UI exists (my_ui_init()).
 */
void ui_ota_hid_init(void);

/*
 * True while the update progress screen is up (SD or PC update): mode and
 * config switches are refused meanwhile. The screen itself lives on
 * lv_layer_sys(), above everything else.
 */
#include <stdbool.h>
bool ui_ota_in_progress(void);
