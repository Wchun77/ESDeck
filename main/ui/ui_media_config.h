#pragma once

#include <stdbool.h>
#include "ui_settings.h"   /* ui_settings_appearance_t -- reused verbatim for the "settings" object below */

/* Same field length as Deck/Monitor's UI_CONFIG_BG_LEN -- filename only,
 * not a full path (resolved against the shared assets/backgrounds folder
 * both other modes use). */
#define UI_MEDIA_CFG_BG_LEN    64

/* Same shape as Deck/Monitor's UI_CONFIG_FNAME_LEN / MON_CFG_FNAME_LEN --
 * max length of a *.json filename under SD_PATH_CONFIG_MEDIA. */
#define UI_MEDIA_CFG_FNAME_LEN 64

typedef struct {
    char bg_image[UI_MEDIA_CFG_BG_LEN];   /* Media player card's own bg, empty = flat color */
    ui_settings_appearance_t settings;    /* Settings overlay's own bg_image + side_icon -- same struct Deck/Monitor's "settings" object already uses */
} ui_media_config_t;

/* Loads whichever config/media/<name>.json is currently selected (see
 * ui_media_config_nvs_load()/nvs_save() below) -- same multi-config shape
 * Deck/Monitor already use (SD folder of named *.json files + NVS stores
 * the active filename). The JSON schema is unchanged from the original
 * single-file version and deliberately mirrors Deck/Monitor's own config
 * files (top-level bg_image + nested "settings" object):
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
typedef struct {
    char **names;
    int    count;
} media_scan_result_t;

media_scan_result_t ui_media_config_scan(void);
void                 ui_media_config_scan_free(media_scan_result_t *res);
