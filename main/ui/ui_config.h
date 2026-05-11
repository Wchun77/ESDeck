#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define UI_CONFIG_MAX_PAGES    255
#define UI_CONFIG_MAX_BUTTONS  255

#define UI_CONFIG_LABEL_LEN    32
#define UI_CONFIG_ICON_LEN     32
#define UI_CONFIG_NAME_LEN     32

#define UI_CONFIG_ICON_PATH    "/sdcard"
#define UI_CONFIG_BG_PATH      "/sdcard"
#define UI_CONFIG_JSON_PATH    "/flash"
#define UI_CONFIG_JSON_PREFIX  "esp_"

#define UI_CONFIG_BG_LEN       64

typedef struct {
    char label[UI_CONFIG_LABEL_LEN];
    char icon[UI_CONFIG_ICON_LEN];
} btn_cfg_t;

typedef struct {
    char       name[UI_CONFIG_NAME_LEN];
    char       bg_image[UI_CONFIG_BG_LEN];  /* JPEG filename, empty = no background */
    btn_cfg_t *buttons;
    uint8_t    button_count;
} page_cfg_t;

typedef struct {
    page_cfg_t *pages;
    uint8_t     page_count;
} deck_cfg_t;

/*
 * Load config from /flash/esp_XXXX.json.
 * Scans for the first file matching the prefix and parses it.
 * Returns true on success. Call ui_config_free() when done.
 */
bool ui_config_load(deck_cfg_t *cfg);

/*
 * Free all heap memory allocated by ui_config_load().
 */
void ui_config_free(deck_cfg_t *cfg);

#define UI_CONFIG_FNAME_LEN    64

typedef struct {
    char  **names;   /* array of heap-allocated filename strings */
    int     count;
} json_scan_result_t;

/*
 * Scan UI_CONFIG_JSON_PATH for all *.json files.
 * Caller must free with ui_config_scan_free().
 */
json_scan_result_t ui_config_scan(void);

/*
 * Free all memory allocated by ui_config_scan().
 */
void ui_config_scan_free(json_scan_result_t *res);

#define CFG_NVS_NAMESPACE  "esdeck"
#define CFG_NVS_KEY        "cfg_file"

/*
 * Save selected config filename to NVS.
 */
bool ui_config_nvs_save(const char *filename);

/*
 * Load config filename from NVS.
 * Returns false if not set or on error.
 */
bool ui_config_nvs_load(char *out, size_t out_size);