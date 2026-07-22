#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "app_config.h"
#include "ui_settings.h"

/* UI_MONITOR_PATH (config/monitor JSON directory) and UI_MONITOR_FONT_PATH
 * (the clock-specific pre-rasterized .bin font folder) are genuinely
 * Monitor's own. There is deliberately no UI_MONITOR_BG_PATH/ICON_PATH or
 * a Monitor-specific bg length here (there used to be, as plain aliases/
 * copies of app_config.h's SD_PATH_ASSETS_BG/SIDE_ICON and CFG_BG_LEN with
 * no added meaning) -- those asset folders and that length aren't
 * Monitor's, every mode's images live there and are the same length
 * limit, so code building a path into them uses SD_PATH_ASSETS_* and
 * CFG_BG_LEN directly. */
#define UI_MONITOR_PATH        SD_PATH_CONFIG_MONITOR
#define UI_MONITOR_FONT_PATH   SD_PATH_ASSETS_FONTS_BIN_CLOCK

#define MON_CFG_FONT_LEN    64
#define MON_CFG_ICON_LEN    32

/* Default values -- used when JSON key is absent or file not found */
#define MON_CFG_DEF_COL_TIME    0xf0f2ff
#define MON_CFG_DEF_COL_COLON   0x1e2e66
#define MON_CFG_DEF_COL_DATE    0xf0f2ff
#define MON_CFG_DEF_COL_DAY     0xf0f2ff
#define MON_CFG_DEF_COL_SEC     0xf0f2ff
#define MON_CFG_DEF_SEP_COLOR   0x3a4a77
#define MON_CFG_DEF_SEP_WIDTH   1

typedef struct {
    char     bg_image[CFG_BG_LEN];        /* filename only, under SD_PATH_ASSETS_BG, e.g. "IMG_3238.jpg" */
    char     side_icon[MON_CFG_ICON_LEN]; /* filename only, under SD_PATH_ASSETS_SIDE_ICON; empty = show "Clock" text on sidebar button */
    char     font_time[MON_CFG_FONT_LEN]; /* filename only, e.g. "oxanium_270.bin" */
    char     font_sec[MON_CFG_FONT_LEN];
    char     font_date[MON_CFG_FONT_LEN];
    uint32_t col_time;
    uint32_t col_colon;
    uint32_t col_date;
    uint32_t col_day;
    uint32_t col_sec;
    uint8_t  opa_time;    /* 0-255, default 255 */
    uint8_t  opa_colon;
    uint8_t  opa_date;
    uint8_t  opa_day;
    uint8_t  opa_sec;
    int      colon_gap;   /* px gap between digits and colon, default 30 */
    uint32_t sep_color;
    int      sep_width;
} mon_clock_cfg_t;

/* Maximum number of data pages (excluding the fixed clock page). */
#define MON_PAGE_MAX        3

/* Total pages including the fixed clock page. */
#define MON_TOTAL_PAGE_MAX  (1 + MON_PAGE_MAX)

/* Maximum cells per data page. */
#define MON_PAGE_CELLS      4

/* Maximum length of a page name. */
#define MON_PAGE_NAME_LEN   32

/* Cell data source identifiers.
 * MON_CELL_NONE means the slot is empty (null in JSON). */
typedef enum {
    MON_CELL_NONE = 0,
    MON_CELL_CPU_USAGE,
    MON_CELL_CPU_TEMP,
    MON_CELL_CPU_FREQ,
    MON_CELL_RAM_USAGE,
    MON_CELL_GPU_USAGE,
    MON_CELL_GPU_TEMP,
    MON_CELL_GPU_VRAM,
    MON_CELL_NET_UP,
    MON_CELL_NET_DOWN,
    MON_CELL_DISK_USAGE,
    MON_CELL_CPU_POWER,
    MON_CELL_GPU_POWER,
    MON_CELL_SSD_LIFE,
    MON_CELL_COUNT,
} mon_cell_id_t;

typedef struct {
    char         name[MON_PAGE_NAME_LEN];
    char         bg_image[CFG_BG_LEN];         /* filename only, under SD_PATH_ASSETS_BG */
    char         side_icon[MON_CFG_ICON_LEN];  /* filename only, under SD_PATH_ASSETS_SIDE_ICON; empty = show name text on sidebar button */
    mon_cell_id_t cells[MON_PAGE_CELLS];  /* MON_CELL_NONE = empty slot */
} mon_page_cfg_t;

typedef struct {
    mon_clock_cfg_t           clock;
    mon_page_cfg_t            pages[MON_PAGE_MAX];
    int                       page_count;   /* 0 = no data pages */
    ui_settings_appearance_t  settings;     /* "settings" object in the JSON, sibling to "clock" / "pages" */
} monitor_cfg_t;

/*
 * Load monitor config from NVS-specified file under UI_MONITOR_PATH.
 * Falls back to defaults for any missing field.
 * Always succeeds -- returns default config even if no JSON exists.
 */
void ui_monitor_config_load(monitor_cfg_t *cfg);

/*
 * Free any heap memory allocated by ui_monitor_config_load.
 * Currently a no-op (all fields are static arrays), provided for symmetry.
 */
void ui_monitor_config_free(monitor_cfg_t *cfg);

/*
 * Save / load selected monitor config filename to/from NVS.
 */
bool ui_monitor_config_nvs_save(const char *filename);
bool ui_monitor_config_nvs_load(char *out, size_t out_size);

/*
 * Scan UI_MONITOR_PATH for all *.json files.
 * Returns count = -1 if the directory does not exist.
 * Caller must free with ui_monitor_config_scan_free().
 */

/* Alias of the shared cfg_scan_result_t / CFG_FNAME_LEN (see app_config.h)
 * -- kept as its own type name for API clarity within this module's own
 * functions. */
typedef cfg_scan_result_t mon_scan_result_t;

mon_scan_result_t  ui_monitor_config_scan(void);
void               ui_monitor_config_scan_free(mon_scan_result_t *res);

/*
 * Build full LVGL FS path for a monitor background image.
 * out must be at least CFG_BG_LEN + 16 bytes.
 */
void ui_monitor_config_bg_path(const char *filename, char *out, size_t out_size);

/*
 * Build full LVGL FS path for a monitor font file.
 * out must be at least MON_CFG_FONT_LEN + 24 bytes.
 */
void ui_monitor_config_font_path(const char *filename, char *out, size_t out_size);

/*
 * Build full LVGL FS path for a monitor sidebar page icon.
 * out must be at least MON_CFG_ICON_LEN + 24 bytes.
 */
void ui_monitor_config_icon_path(const char *filename, char *out, size_t out_size);