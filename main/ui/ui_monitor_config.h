#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define UI_MONITOR_PATH        "/sdcard/config/monitor"
#define UI_MONITOR_BG_PATH     "/sdcard/assets/backgrounds"
#define UI_MONITOR_FONT_PATH   "/sdcard/assets/fonts"

#define MON_CFG_FNAME_LEN   64
#define MON_CFG_BG_LEN      64
#define MON_CFG_FONT_LEN    64

/* Default values -- used when JSON key is absent or file not found */
#define MON_CFG_DEF_COL_TIME    0xf0f2ff
#define MON_CFG_DEF_COL_COLON   0x1e2e66
#define MON_CFG_DEF_COL_DATE    0xf0f2ff
#define MON_CFG_DEF_COL_DAY     0xf0f2ff
#define MON_CFG_DEF_COL_SEC     0xf0f2ff
#define MON_CFG_DEF_SEP_COLOR   0x3a4a77
#define MON_CFG_DEF_SEP_WIDTH   1

typedef struct {
    char     bg_image[MON_CFG_BG_LEN];    /* filename only, e.g. "IMG_3238.jpg" */
    char     font_time[MON_CFG_FONT_LEN]; /* filename only, e.g. "oxanium_270.bin" */
    char     font_sec[MON_CFG_FONT_LEN];
    char     font_date[MON_CFG_FONT_LEN];
    uint32_t col_time;
    uint32_t col_colon;
    uint32_t col_date;
    uint32_t col_day;
    uint32_t col_sec;
    uint32_t sep_color;
    int      sep_width;
} mon_clock_cfg_t;

typedef struct {
    char bg_image[MON_CFG_BG_LEN];
} mon_system_cfg_t;

typedef struct {
    mon_clock_cfg_t  clock;
    mon_system_cfg_t system;
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
typedef struct {
    char **names;
    int    count;
} mon_scan_result_t;

mon_scan_result_t  ui_monitor_config_scan(void);
void               ui_monitor_config_scan_free(mon_scan_result_t *res);

/*
 * Build full LVGL FS path for a monitor background image.
 * out must be at least MON_CFG_BG_LEN + 16 bytes.
 */
void ui_monitor_config_bg_path(const char *filename, char *out, size_t out_size);

/*
 * Build full LVGL FS path for a monitor font file.
 * out must be at least MON_CFG_FONT_LEN + 24 bytes.
 */
void ui_monitor_config_font_path(const char *filename, char *out, size_t out_size);