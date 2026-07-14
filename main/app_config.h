#pragma once

/*
 * app_config.h
 *
 * Central place for project-wide identifiers and SD-card path layout.
 * Anything that used to be a hardcoded "/sdcard/..." string scattered
 * across ui_config.h / ui_monitor_config.h / fs_sd.c now lives here so
 * there is exactly one place to change it.
 *
 * Must match the CMake project() name in the top-level CMakeLists.txt --
 * used to validate OTA update filenames (see ota_manager).
 */
#define FW_PROJECT_NAME     "ESDeck"

/* --------------------------------------------------------------------------
 * SD card mount point
 * -------------------------------------------------------------------------- */

#define SD_MOUNT_POINT      "/sdcard"

/* --------------------------------------------------------------------------
 * SD card folder layout
 *
 * SD_DIR_*  -- path relative to SD_MOUNT_POINT (no leading slash),
 *              used by fs_sd_ensure_layout() to auto-create folders.
 * SD_PATH_* -- full absolute path, used by the rest of the app.
 * -------------------------------------------------------------------------- */

#define SD_DIR_CONFIG_DECK      "config/deck"
#define SD_DIR_CONFIG_MONITOR   "config/monitor"
#define SD_DIR_ASSETS_ICONS     "assets/icons"
#define SD_DIR_ASSETS_BG        "assets/backgrounds"
#define SD_DIR_ASSETS_FONTS     "assets/fonts"
#define SD_DIR_UPDATE           "update"

#define SD_PATH_CONFIG_DECK     SD_MOUNT_POINT "/" SD_DIR_CONFIG_DECK
#define SD_PATH_CONFIG_MONITOR  SD_MOUNT_POINT "/" SD_DIR_CONFIG_MONITOR
#define SD_PATH_ASSETS_ICONS    SD_MOUNT_POINT "/" SD_DIR_ASSETS_ICONS
#define SD_PATH_ASSETS_BG       SD_MOUNT_POINT "/" SD_DIR_ASSETS_BG
#define SD_PATH_ASSETS_FONTS    SD_MOUNT_POINT "/" SD_DIR_ASSETS_FONTS
#define SD_PATH_UPDATE          SD_MOUNT_POINT "/" SD_DIR_UPDATE

/* --------------------------------------------------------------------------
 * OTA update file naming
 *
 * A file under SD_PATH_UPDATE is only treated as a valid update image if
 * its name contains FW_PROJECT_NAME and ends in ".bin", e.g.
 *   ESDeck_v1_0_2.bin
 * This keeps testers from accidentally flashing a .bin meant for a
 * different board (Blue_Bridge, etc.).
 * -------------------------------------------------------------------------- */

#define FW_UPDATE_EXTENSION  ".bin"
