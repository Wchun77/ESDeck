#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "app_config.h"
#include "ui_settings.h"

/* Deck mode's own config file format/loader -- named ui_deck_config (not
 * just ui_config) because this only ever handled Deck: it predates
 * Monitor and Media, which each got their own properly mode-prefixed
 * config module (ui_monitor_config.c/mon_scan_result_t,
 * ui_media_config.c/media_scan_result_t) when they were added later.
 * "ui_config" never got renamed to match until now, which made it look
 * like shared/generic infrastructure when it never was -- see
 * CFG_NVS_NAMESPACE/CFG_NVS_KEY_* and CFG_FNAME_LEN/cfg_scan_result_t in
 * app_config.h for the pieces of this file that genuinely were shared
 * (used by Monitor/Media too, or by the mode-agnostic ui_config_dialog.c)
 * and have since moved there.
 *
 * Note there is deliberately no UI_DECK_CONFIG_ICON_PATH/BG_PATH/
 * SIDE_ICON_PATH here, and page_cfg_t.bg_image below is sized with
 * app_config.h's CFG_BG_LEN, not a Deck-specific length (there used to be
 * both, as plain aliases/copies with no added meaning) -- those asset
 * folders and that length aren't Deck's, every mode's images live there
 * and are the same length limit, so code building a path into them
 * (Deck's own included) uses SD_PATH_ASSETS_* and CFG_BG_LEN directly
 * instead of reaching into this header for a same-value rename. UI_DECK_CONFIG_PATH
 * below is different: the config/deck JSON directory genuinely is
 * Deck-only. */

#define UI_DECK_CONFIG_MAX_PAGES    255
#define UI_DECK_CONFIG_MAX_BUTTONS  255

#define UI_DECK_CONFIG_LABEL_LEN    32
#define UI_DECK_CONFIG_ICON_LEN     32
#define UI_DECK_CONFIG_NAME_LEN     32

#define UI_DECK_CONFIG_PATH           SD_PATH_CONFIG_DECK
#define UI_DECK_CONFIG_JSON_PREFIX    "esp_"

#define UI_DECK_CONFIG_SIDE_ICON_LEN 32

typedef struct {
    char label[UI_DECK_CONFIG_LABEL_LEN];
    char icon[UI_DECK_CONFIG_ICON_LEN];
} btn_cfg_t;

typedef struct {
    char       name[UI_DECK_CONFIG_NAME_LEN];
    char       bg_image[CFG_BG_LEN];  /* filename only, under SD_PATH_ASSETS_BG -- see app_config.h's CFG_BG_LEN doc comment */
    char       side_icon[UI_DECK_CONFIG_SIDE_ICON_LEN];  /* filename only, under SD_PATH_ASSETS_SIDE_ICON; empty = show name text on sidebar button */
    btn_cfg_t *buttons;
    uint8_t    button_count;
} page_cfg_t;

typedef struct {
    page_cfg_t               *pages;
    uint8_t                    page_count;
    ui_settings_appearance_t   settings;  /* "settings" object in the JSON, sibling to "pages" */
} deck_cfg_t;

/*
 * Load deck config from NVS-specified file under UI_DECK_CONFIG_PATH.
 * Returns true on success. Call ui_deck_config_free() when done.
 */
bool ui_deck_config_load(deck_cfg_t *cfg);

/*
 * Free all heap memory allocated by ui_deck_config_load().
 */
void ui_deck_config_free(deck_cfg_t *cfg);

/* Alias of the shared cfg_scan_result_t / CFG_FNAME_LEN (see app_config.h)
 * -- kept as its own type name for API clarity within this module's own
 * functions, same as Monitor's mon_scan_result_t / Media's
 * media_scan_result_t. */
typedef cfg_scan_result_t deck_scan_result_t;

/*
 * Scan UI_DECK_CONFIG_PATH for all *.json files.
 * Returns count = -1 if the directory does not exist.
 * Caller must free with ui_deck_config_scan_free().
 */
deck_scan_result_t ui_deck_config_scan(void);

/*
 * Free all memory allocated by ui_deck_config_scan().
 */
void ui_deck_config_scan_free(deck_scan_result_t *res);

/*
 * Save / load selected deck config filename to NVS.
 */
bool ui_deck_config_nvs_save(const char *filename);
bool ui_deck_config_nvs_load(char *out, size_t out_size);
