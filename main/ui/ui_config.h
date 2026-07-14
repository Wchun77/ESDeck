#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "app_config.h"

#define UI_CONFIG_MAX_PAGES    255
#define UI_CONFIG_MAX_BUTTONS  255

#define UI_CONFIG_LABEL_LEN    32
#define UI_CONFIG_ICON_LEN     32
#define UI_CONFIG_NAME_LEN     32

#define UI_CONFIG_ICON_PATH    SD_PATH_ASSETS_ICONS
#define UI_CONFIG_BG_PATH      SD_PATH_ASSETS_BG
#define UI_CONFIG_DECK_PATH    SD_PATH_CONFIG_DECK
#define UI_CONFIG_JSON_PREFIX  "esp_"

#define UI_CONFIG_BG_LEN       64

typedef struct {
    char label[UI_CONFIG_LABEL_LEN];
    char icon[UI_CONFIG_ICON_LEN];
} btn_cfg_t;

typedef struct {
    char       name[UI_CONFIG_NAME_LEN];
    char       bg_image[UI_CONFIG_BG_LEN];
    btn_cfg_t *buttons;
    uint8_t    button_count;
} page_cfg_t;

typedef struct {
    page_cfg_t *pages;
    uint8_t     page_count;
} deck_cfg_t;

/*
 * Load deck config from NVS-specified file under UI_CONFIG_DECK_PATH.
 * Returns true on success. Call ui_config_free() when done.
 */
bool ui_config_load(deck_cfg_t *cfg);

/*
 * Free all heap memory allocated by ui_config_load().
 */
void ui_config_free(deck_cfg_t *cfg);

#define UI_CONFIG_FNAME_LEN    64

typedef struct {
    char  **names;
    int     count;
} json_scan_result_t;

/*
 * Scan UI_CONFIG_DECK_PATH for all *.json files.
 * Returns count = -1 if the directory does not exist.
 * Caller must free with ui_config_scan_free().
 */
json_scan_result_t ui_config_scan(void);

/*
 * Free all memory allocated by ui_config_scan().
 */
void ui_config_scan_free(json_scan_result_t *res);

#define CFG_NVS_NAMESPACE   "esdeck"
#define CFG_NVS_KEY_DECK    "deck_cfg"
#define CFG_NVS_KEY_MONITOR "mon_cfg"

/*
 * Save / load selected deck config filename to NVS.
 */
bool ui_config_nvs_save(const char *filename);
bool ui_config_nvs_load(char *out, size_t out_size);