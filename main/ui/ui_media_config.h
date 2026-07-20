#pragma once

#include <stdbool.h>
#include "ui_settings.h"   /* ui_settings_appearance_t -- reused verbatim for the "settings" object below */

/* Same field length as Deck/Monitor's UI_CONFIG_BG_LEN -- filename only,
 * not a full path (resolved against the shared assets/backgrounds folder
 * both other modes use). */
#define UI_MEDIA_CFG_BG_LEN    64

typedef struct {
    char bg_image[UI_MEDIA_CFG_BG_LEN];   /* Media player card's own bg, empty = flat color */
    ui_settings_appearance_t settings;    /* Settings overlay's own bg_image + side_icon -- same struct Deck/Monitor's "settings" object already uses */
} ui_media_config_t;

/* Loads config/media/settings.json -- a single fixed file, no picker UI
 * or multi-config selection yet (Deck/Monitor both support multiple named
 * configs with the active one persisted in NVS; Media will grow the same
 * shape later -- this is deliberately scoped down to unblock background
 * image + mask testing without building that plumbing yet). The JSON
 * schema is written to stay identical either way, and deliberately mirrors
 * Deck/Monitor's own config files (top-level bg_image + nested "settings"
 * object) rather than inventing a different shape:
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
 * *cfg first; returns false (with *cfg left all-empty) if the file is
 * missing or invalid -- callers should treat that as "no config" rather
 * than surface it as an error. */
bool ui_media_config_load(ui_media_config_t *cfg);
