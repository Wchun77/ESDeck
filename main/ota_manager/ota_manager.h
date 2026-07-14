#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * ota_manager
 *
 * Local (SD-card) firmware update.
 *
 * A tester drags a new firmware .bin into SD_PATH_UPDATE via USB MSC.
 * On next boot, ota_check_update() finds it (filename must contain
 * FW_PROJECT_NAME and end in ".bin" -- see app_config.h), the app asks
 * the user for confirmation, and if accepted, ota_apply_update() flashes
 * it to the inactive OTA partition and reboots into it.
 */

typedef struct {
    bool    found;
    char    path[320];      /* full path under SD_PATH_UPDATE            */
    char    filename[256];  /* filename only, for display                */
    size_t  size;           /* file size in bytes                        */
} ota_scan_result_t;

/*
 * Scan SD_PATH_UPDATE for the first file whose name contains
 * FW_PROJECT_NAME and ends in FW_UPDATE_EXTENSION (".bin").
 * Safe to call even if the SD card / folder isn't there (returns found=false).
 */
ota_scan_result_t ota_check_update(void);

/* 0-100, called from whatever task runs ota_apply_update(). Not called
 * from the LVGL thread -- the UI layer must marshal back via lv_async_call
 * (or similar) if it touches LVGL objects from this callback. */
typedef void (*ota_progress_cb_t)(uint8_t percent, void *user_data);

/* Called once, right after the target partition has been erased and just
 * before the first byte is written. Erasing the whole partition up front
 * (one long blocking call) is far faster overall than erasing sector-by-
 * sector as we go, but it means the caller should show a plain "preparing"
 * message (no animation) until this fires, then switch to a progress bar --
 * that way the one unavoidable multi-second pause happens on a static
 * screen instead of stuttering a live animation. */
typedef void (*ota_phase_cb_t)(void *user_data);

/*
 * Flash `path` (as found by ota_check_update) to the inactive OTA slot.
 * On success: the file is deleted from the SD card, the new slot is set
 * as the next boot partition, and this function returns true (caller is
 * expected to esp_restart()).
 * On failure: the file is left untouched so the update can be retried,
 * and this function returns false.
 */
bool ota_apply_update(const char *path, size_t size,
                      ota_phase_cb_t on_erase_done,
                      ota_progress_cb_t progress_cb, void *user_data);
