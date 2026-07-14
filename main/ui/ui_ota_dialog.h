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
 */
void ui_ota_check_and_prompt(void);
