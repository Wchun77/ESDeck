#pragma once

#include <stdbool.h>
#include "app_config.h"     /* CFG_FNAME_LEN, CFG_BG_LEN, cfg_scan_result_t -- shared across all three modes' config pickers */
#include "ui_settings.h"   /* ui_settings_appearance_t -- reused verbatim for the "settings" object below */

typedef struct {
    char bg_image[CFG_BG_LEN];         /* Media player card's own bg, under SD_PATH_ASSETS_BG; empty = flat color */
    ui_settings_appearance_t settings; /* Settings overlay's own bg_image + side_icon */
} ui_media_config_t;

/* Loads whichever config/media/<name>.json is currently selected (see
 * ui_media_config_nvs_load()/nvs_save() below) -- SD folder of named
 * *.json files + NVS stores the active filename. Top-level bg_image +
 * nested "settings" object:
 *
 *   {
 *       "bg_image": "sunset.jpg",
 *       "settings": {
 *           "bg_image": "panel_bg.jpg",
 *           "side_icon": "music.png"
 *       }
 *   }
 *
 * All three leaf fields optional/omittable independently. Always zeroes
 * *cfg first; returns false (with *cfg left all-empty) if no config is
 * selected yet in NVS, or the selected file is missing/invalid -- callers
 * should treat that as "no config" rather than surface it as an error. */
bool ui_media_config_load(ui_media_config_t *cfg);

/*
 * Save / load selected media config filename to/from NVS.
 */
bool ui_media_config_nvs_save(const char *filename);
bool ui_media_config_nvs_load(char *out, size_t out_size);

/*
 * Scan SD_PATH_CONFIG_MEDIA for all *.json files.
 * Returns count = -1 if the directory does not exist.
 * Caller must free with ui_media_config_scan_free().
 */

/* Alias of the shared cfg_scan_result_t / CFG_FNAME_LEN (see app_config.h)
 * -- kept as its own type name for API clarity within this module's own
 * functions. */
typedef cfg_scan_result_t media_scan_result_t;

media_scan_result_t ui_media_config_scan(void);
void                 ui_media_config_scan_free(media_scan_result_t *res);
