#pragma once

#include <stdlib.h>

/*
 * app_config.h
 *
 * Central place for project-wide identifiers and SD-card path layout.
 * Anything that used to be a hardcoded "/sdcard/..." string scattered
 * across ui_deck_config.h / ui_monitor_config.h / fs_sd.c now lives here
 * so there is exactly one place to change it.
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
#define SD_DIR_CONFIG_MEDIA     "config/media"
#define SD_DIR_ASSETS_ICONS     "assets/icons"
#define SD_DIR_ASSETS_BG        "assets/backgrounds"
#define SD_DIR_ASSETS_BOOT      "assets/boot"
#define SD_DIR_ASSETS_SIDE_ICON "assets/side_icons"
#define SD_DIR_UPDATE           "update"
/* Leading dot -- Unix-convention "hidden" (ls -a, most file managers on
 * macOS/Linux won't show it by default). Not a real hide on Windows:
 * FatFs's ff.h is linked into this build without chmod/attribute support
 * (no FF_USE_CHMOD Kconfig knob is exposed for this IDF version -- see
 * dump_manager.c), so there's no way to also set the FAT hidden-attribute
 * bit from the device side. Windows Explorer will still list this folder
 * normally. */
#define SD_DIR_DUMP             ".dump"

/* assets/fonts/bin/ holds pre-rasterized fonts (converted ahead of time via
 * lv_font_conv --format bin, loaded on-device with lv_font_load()) -- this
 * is the ONLY font mechanism this project uses. An earlier attempt used
 * FreeType to rasterize arbitrary user-supplied .ttf files on-device at
 * runtime instead; that was abandoned (see git history on this file)
 * because FreeType's rasterizer has stack requirements that can't be
 * bounded ahead of time -- crashes were reproduced even on a 6KB
 * dedicated task stack, and there's no way to guarantee safety against a
 * font file we don't control. Pre-rasterized .bin fonts are bitmap-blit
 * only (no hinting bytecode interpreter, no recursive composite-glyph
 * resolution), so that entire crash class doesn't apply, at the cost of
 * only covering whatever glyph set was baked in ahead of time.
 *
 * Each feature gets its own leaf under bin/ since a .bin font is
 * pre-converted for one specific pixel size/glyph-set/feature and isn't
 * reusable across features the way a .ttf would have been --
 * assets/fonts/bin/<feature>/, same idea as assets/boot/<name>/.
 *   bin/clock/  -- Monitor clock digits (oxanium_*.bin)
 *   bin/notify/ -- BLE/ANCS notification text: a curated common-Hanzi
 *                  subset only (not the full CJK range) -- see
 *                  doc/ESDeck_Monitor_字體轉換指南.md for how to build one. */
#define SD_DIR_ASSETS_FONTS            "assets/fonts"
#define SD_DIR_ASSETS_FONTS_BIN_CLOCK  SD_DIR_ASSETS_FONTS "/bin/clock"
#define SD_DIR_ASSETS_FONTS_BIN_NOTIFY SD_DIR_ASSETS_FONTS "/bin/notify"

#define SD_PATH_CONFIG_DECK     SD_MOUNT_POINT "/" SD_DIR_CONFIG_DECK
#define SD_PATH_CONFIG_MONITOR  SD_MOUNT_POINT "/" SD_DIR_CONFIG_MONITOR
#define SD_PATH_CONFIG_MEDIA    SD_MOUNT_POINT "/" SD_DIR_CONFIG_MEDIA

#define SD_PATH_ASSETS_ICONS    SD_MOUNT_POINT "/" SD_DIR_ASSETS_ICONS
#define SD_PATH_ASSETS_BG       SD_MOUNT_POINT "/" SD_DIR_ASSETS_BG
#define SD_PATH_ASSETS_BOOT     SD_MOUNT_POINT "/" SD_DIR_ASSETS_BOOT
#define SD_PATH_ASSETS_SIDE_ICON SD_MOUNT_POINT "/" SD_DIR_ASSETS_SIDE_ICON
#define SD_PATH_UPDATE          SD_MOUNT_POINT "/" SD_DIR_UPDATE
#define SD_PATH_DUMP            SD_MOUNT_POINT "/" SD_DIR_DUMP

#define SD_PATH_ASSETS_FONTS_BIN_CLOCK  SD_MOUNT_POINT "/" SD_DIR_ASSETS_FONTS_BIN_CLOCK
#define SD_PATH_ASSETS_FONTS_BIN_NOTIFY SD_MOUNT_POINT "/" SD_DIR_ASSETS_FONTS_BIN_NOTIFY

/* --------------------------------------------------------------------------
 * Custom boot animation
 *
 * Frames live directly under SD_PATH_ASSETS_BOOT, named
 * frame_0000.jpg, frame_0001.jpg, ... Each frame is stored as JPEG
 * (decoded on the fly via esp_jpeg / TJpgDec) instead of raw RGB565 --
 * raw frames at 800x480 are ~750KB each, which is far more than the SD
 * card's real-world sequential read throughput (~930KB/s measured) can
 * feed at any usable frame rate. JPEG shrinks each frame down to the
 * tens-of-KB range so the SD read is no longer the bottleneck.
 * -------------------------------------------------------------------------- */

#define BOOT_ANIM_CUSTOM_FPS    12

/* --------------------------------------------------------------------------
 * Feature toggles
 *
 * ESDECK_ENABLE_BLE -- compile-time on/off switch for the whole BLE/ANCS
 * feature (main/ble/ble_manager.c, the Settings "Bluetooth" toggle row).
 * Set to 0 to build without it.
 *
 * This only guards call sites (ble_manager_init() never gets called, the
 * Settings row never gets added) -- it does NOT strip ble_manager.c or
 * the "bt" component out of the build (main/CMakeLists.txt still globs
 * and links ble/ (glob) unconditionally), matching how FS_SD_SCAN_LOG_ENABLE
 * below only silences logging rather than removing the underlying code.
 * Flash/link footprint is unchanged either way; only the runtime
 * behavior (NimBLE host never starts, never advertises) and the visible
 * Settings entry are affected. If a real build-exclusion (dropping the
 * bt component and ble/ (glob) entirely) is ever needed, that's a separate,
 * bigger change in main/CMakeLists.txt.
 * -------------------------------------------------------------------------- */

#define ESDECK_ENABLE_BLE  1

/* --------------------------------------------------------------------------
 * Boot-time log toggles
 *
 * These only silence logging -- the underlying features (SD scan,
 * boot animation) keep running exactly as before either way. Comment
 * out to disable, uncomment (or re-add the #define) to re-enable.
 * Both left disabled here for the current feature branch -- with a lot
 * of files on the SD card and many boot-anim frames, fs_sd_scan()'s
 * recursive directory print and the boot animation's per-frame timing
 * logs were drowning out everything else in the boot log. Flip back on
 * before merging if those logs should ship enabled.
 * -------------------------------------------------------------------------- */

// #define FS_SD_SCAN_LOG_ENABLE
// #define BOOT_ANIM_FRAME_LOG_ENABLE

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

/* --------------------------------------------------------------------------
 * NVS keys -- "which config file is currently selected" per mode.
 *
 * Lives here (not in any one mode's config module) because it's read/
 * written by all of them: ui_deck_config.c, ui_monitor_config.c,
 * ui_media_config.c, and boot_anim.c all share this same namespace. It
 * used to live in ui_config.h (Deck's own config module) purely because
 * Deck was written first -- Monitor/Media/boot_anim including "Deck's"
 * header just to get their own NVS key was exactly the kind of
 * misleading-name problem this file exists to avoid.
 * -------------------------------------------------------------------------- */
#define CFG_NVS_NAMESPACE     "esdeck"
#define CFG_NVS_KEY_DECK      "deck_cfg"
#define CFG_NVS_KEY_MONITOR   "mon_cfg"
#define CFG_NVS_KEY_MEDIA     "media_cfg"
#define CFG_NVS_KEY_BOOT_ANIM "boot_anim"

/* --------------------------------------------------------------------------
 * Config file list/select -- shared shape across all three modes' config
 * pickers (ui_deck_config.c/deck_scan_result_t, ui_monitor_config.c/
 * mon_scan_result_t, ui_media_config.c/media_scan_result_t are all just
 * aliases of cfg_scan_result_t below) and the one dialog that lists and
 * switches between them (ui_config_dialog.c -- despite the filename, it
 * isn't Deck-only: it shows Deck's, Monitor's, and Media's config lists
 * from the same dialog UI, picked via its own cfg_dialog_mode_t). Lives
 * here for the same reason CFG_NVS_* above does: read/used by all three
 * modes' config code plus the shared dialog, not owned by any single one.
 *
 * CFG_BG_LEN is the same story, one level down: every mode's own config
 * struct has a bg_image[CFG_BG_LEN] field (filename only, resolved against
 * SD_PATH_ASSETS_BG), and the shared image pool's own cache-entry struct
 * (ui_img_pool.c's psram_img_t) sizes its path-key buffer off it too, so
 * it belongs here rather than under any one mode's name.
 * -------------------------------------------------------------------------- */
#define CFG_FNAME_LEN  64
#define CFG_BG_LEN     64

typedef struct {
    char **names;
    int    count;
} cfg_scan_result_t;

/* Free all memory allocated by any mode's *_config_scan(). Each mode keeps
 * its own scan_free() wrapper (ui_deck_config_scan_free(),
 * ui_monitor_config_scan_free(), ui_media_config_scan_free()) for API
 * symmetry with its own _scan() -- all three just delegate to this one
 * implementation. ui_config_dialog.c, which isn't tied to any single
 * mode, calls this directly instead of borrowing one mode's wrapper. */
static inline void cfg_scan_result_free(cfg_scan_result_t *res)
{
    if (!res) return;
    for (int i = 0; i < res->count; i++) {
        free(res->names[i]);
        res->names[i] = NULL;
    }
    free(res->names);
    res->names = NULL;
    res->count = 0;
}
