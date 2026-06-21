#include "ui_monitor_config.h"
#include "ui_config.h"
#include "nvs_manager.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include <string.h>
#include <stdlib.h>
#include <dirent.h>

#define TAG  "MON_CFG"

/* -----------------------------------------------------------------------
 * Helpers
 * ----------------------------------------------------------------------- */
static void set_defaults(monitor_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    cfg->clock.col_time  = MON_CFG_DEF_COL_TIME;
    cfg->clock.col_colon = MON_CFG_DEF_COL_COLON;
    cfg->clock.col_date  = MON_CFG_DEF_COL_DATE;
    cfg->clock.col_day   = MON_CFG_DEF_COL_DAY;
    cfg->clock.col_sec   = MON_CFG_DEF_COL_SEC;
    cfg->clock.sep_color = MON_CFG_DEF_SEP_COLOR;
    cfg->clock.sep_width = MON_CFG_DEF_SEP_WIDTH;
    cfg->clock.opa_time  = 255;
    cfg->clock.opa_colon = 255;
    cfg->clock.opa_date  = 255;
    cfg->clock.opa_day   = 255;
    cfg->clock.opa_sec   = 255;
}

static uint32_t parse_hex_color(const char *s, uint32_t fallback)
{
    if (!s || s[0] == '\0') return fallback;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s) return fallback;
    return (uint32_t)v;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz <= 0) { fclose(f); return NULL; }

    char *buf = heap_caps_malloc((size_t)sz + 1,
                                  MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }

    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static void str_field(cJSON *obj, const char *key,
                       char *out, size_t out_size)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && item->valuestring)
        snprintf(out, out_size, "%s", item->valuestring);
}

static uint32_t color_field(cJSON *obj, const char *key, uint32_t fallback)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item))
        return parse_hex_color(item->valuestring, fallback);
    return fallback;
}

static int int_field(cJSON *obj, const char *key, int fallback)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsNumber(item)) return (int)item->valuedouble;
    return fallback;
}

/* -----------------------------------------------------------------------
 * Cell ID lookup table
 * ----------------------------------------------------------------------- */
static const struct { const char *name; mon_cell_id_t id; } s_cell_map[] = {
    { "cpu_usage",  MON_CELL_CPU_USAGE  },
    { "cpu_temp",   MON_CELL_CPU_TEMP   },
    { "cpu_freq",   MON_CELL_CPU_FREQ   },
    { "ram_usage",  MON_CELL_RAM_USAGE  },
    { "gpu_usage",  MON_CELL_GPU_USAGE  },
    { "gpu_temp",   MON_CELL_GPU_TEMP   },
    { "gpu_vram",   MON_CELL_GPU_VRAM   },
    { "net_up",     MON_CELL_NET_UP     },
    { "net_down",   MON_CELL_NET_DOWN   },
    { "disk_usage", MON_CELL_DISK_USAGE },
    { "cpu_power",  MON_CELL_CPU_POWER  },
    { "gpu_power",  MON_CELL_GPU_POWER  },
    { "ssd_life",   MON_CELL_SSD_LIFE   },
};

static mon_cell_id_t parse_cell_id(const char *s)
{
    if (!s) return MON_CELL_NONE;
    for (size_t i = 0; i < sizeof(s_cell_map) / sizeof(s_cell_map[0]); i++) {
        if (strcmp(s, s_cell_map[i].name) == 0)
            return s_cell_map[i].id;
    }
    return MON_CELL_NONE;
}
void ui_monitor_config_load(monitor_cfg_t *cfg)
{
    set_defaults(cfg);

    char nvs_fname[MON_CFG_FNAME_LEN];
    if (!ui_monitor_config_nvs_load(nvs_fname, sizeof(nvs_fname))) {
        ESP_LOGW(TAG, "no NVS config, using defaults");
        return;
    }

    char path[MON_CFG_FNAME_LEN + 24];
    snprintf(path, sizeof(path), "%s/%s", UI_MONITOR_PATH, nvs_fname);

    char *buf = read_file(path);
    if (!buf) {
        ESP_LOGW(TAG, "cannot read %s, using defaults", path);
        return;
    }

    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        ESP_LOGE(TAG, "JSON parse error, using defaults");
        return;
    }

    /* Clock section */
    cJSON *clk = cJSON_GetObjectItem(root, "clock");
    if (cJSON_IsObject(clk)) {
        str_field(clk, "bg_image",  cfg->clock.bg_image,  MON_CFG_BG_LEN);
        str_field(clk, "font_time", cfg->clock.font_time, MON_CFG_FONT_LEN);
        str_field(clk, "font_sec",  cfg->clock.font_sec,  MON_CFG_FONT_LEN);
        str_field(clk, "font_date", cfg->clock.font_date, MON_CFG_FONT_LEN);

        cfg->clock.col_time  = color_field(clk, "col_time",  MON_CFG_DEF_COL_TIME);
        cfg->clock.col_colon = color_field(clk, "col_colon", MON_CFG_DEF_COL_COLON);
        cfg->clock.col_date  = color_field(clk, "col_date",  MON_CFG_DEF_COL_DATE);
        cfg->clock.col_day   = color_field(clk, "col_day",   MON_CFG_DEF_COL_DAY);
        cfg->clock.col_sec   = color_field(clk, "col_sec",   MON_CFG_DEF_COL_SEC);
        cfg->clock.sep_color = color_field(clk, "sep_color", MON_CFG_DEF_SEP_COLOR);
        cfg->clock.sep_width = int_field  (clk, "sep_width", MON_CFG_DEF_SEP_WIDTH);
        cfg->clock.opa_time  = (uint8_t)int_field(clk, "opa_time",  255);
        cfg->clock.opa_colon = (uint8_t)int_field(clk, "opa_colon", 255);
        cfg->clock.opa_date  = (uint8_t)int_field(clk, "opa_date",  255);
        cfg->clock.opa_day   = (uint8_t)int_field(clk, "opa_day",   255);
        cfg->clock.opa_sec   = (uint8_t)int_field(clk, "opa_sec",   255);
    }

    /* Pages array */
    cJSON *pages = cJSON_GetObjectItem(root, "pages");
    if (cJSON_IsArray(pages)) {
        int n = cJSON_GetArraySize(pages);
        if (n > MON_PAGE_MAX) n = MON_PAGE_MAX;
        cfg->page_count = n;

        for (int i = 0; i < n; i++) {
            cJSON *pg = cJSON_GetArrayItem(pages, i);
            if (!cJSON_IsObject(pg)) continue;

            mon_page_cfg_t *p = &cfg->pages[i];
            str_field(pg, "name",     p->name,     MON_PAGE_NAME_LEN);
            str_field(pg, "bg_image", p->bg_image,  MON_CFG_BG_LEN);

            /* Default page name if absent */
            if (p->name[0] == '\0')
                snprintf(p->name, MON_PAGE_NAME_LEN, "Page %d", i + 1);

            cJSON *cells = cJSON_GetObjectItem(pg, "cells");
            if (cJSON_IsArray(cells)) {
                int nc = cJSON_GetArraySize(cells);
                if (nc > MON_PAGE_CELLS) nc = MON_PAGE_CELLS;
                for (int j = 0; j < nc; j++) {
                    cJSON *cell = cJSON_GetArrayItem(cells, j);
                    if (cJSON_IsString(cell))
                        p->cells[j] = parse_cell_id(cell->valuestring);
                    /* null in JSON -> MON_CELL_NONE (already zero from memset) */
                }
            }
        }
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "loaded: %s", path);
}

void ui_monitor_config_free(monitor_cfg_t *cfg)
{
    (void)cfg;
}

bool ui_monitor_config_nvs_save(const char *filename)
{
    return nvs_manager_set_str(CFG_NVS_NAMESPACE, CFG_NVS_KEY_MONITOR, filename);
}

bool ui_monitor_config_nvs_load(char *out, size_t out_size)
{
    return nvs_manager_get_str(CFG_NVS_NAMESPACE, CFG_NVS_KEY_MONITOR, out, out_size);
}

mon_scan_result_t ui_monitor_config_scan(void)
{
    mon_scan_result_t res = { .names = NULL, .count = 0 };

    DIR *dir = opendir(UI_MONITOR_PATH);
    if (!dir) {
        ESP_LOGE(TAG, "monitor config directory not found: %s", UI_MONITOR_PATH);
        res.count = -1;
        return res;
    }

    int total = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_type != DT_REG) continue;
        size_t len = strlen(de->d_name);
        if (len < 5) continue;
        if (strcmp(de->d_name + len - 5, ".json") != 0) continue;
        total++;
    }

    if (total == 0) { closedir(dir); return res; }

    res.names = calloc((size_t)total, sizeof(char *));
    if (!res.names) {
        closedir(dir);
        return res;
    }

    rewinddir(dir);
    int idx = 0;
    while ((de = readdir(dir)) != NULL && idx < total) {
        if (de->d_type != DT_REG) continue;
        size_t len = strlen(de->d_name);
        if (len < 5) continue;
        if (strcmp(de->d_name + len - 5, ".json") != 0) continue;
        res.names[idx] = strndup(de->d_name, MON_CFG_FNAME_LEN - 1);
        if (!res.names[idx]) {
            res.count = idx;
            closedir(dir);
            return res;
        }
        idx++;
    }

    closedir(dir);
    res.count = idx;
    ESP_LOGI(TAG, "scan found %d JSON file(s) in %s", res.count, UI_MONITOR_PATH);
    return res;
}

void ui_monitor_config_scan_free(mon_scan_result_t *res)
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

void ui_monitor_config_bg_path(const char *filename, char *out, size_t out_size)
{
    if (!filename || filename[0] == '\0') {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_size, "S:%s/%s", UI_MONITOR_BG_PATH, filename);
}

void ui_monitor_config_font_path(const char *filename, char *out, size_t out_size)
{
    if (!filename || filename[0] == '\0') {
        out[0] = '\0';
        return;
    }
    snprintf(out, out_size, "S:%s/%s", UI_MONITOR_FONT_PATH, filename);
}