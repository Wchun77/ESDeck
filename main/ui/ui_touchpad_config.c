#include "ui_touchpad_config.h"
#include "app_config.h"
#include "nvs_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#define TAG  "[TPAD_CFG]"

/* -----------------------------------------------------------------------
 * Internal helpers -- copy of ui_media_config.c's read_file()/str_field(),
 * same "each mode owns its own small copy" convention as the other modes.
 * ----------------------------------------------------------------------- */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;   /* not logged as an error -- "no config yet" is the common case */

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz <= 0) {
        fclose(f);
        return NULL;
    }

    char *buf = heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) buf = malloc((size_t)sz + 1);
    if (!buf) {
        ESP_LOGE(TAG, "OOM reading %s (%ld bytes)", path, sz);
        fclose(f);
        return NULL;
    }

    fread(buf, 1, (size_t)sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

static void str_field(const cJSON *obj, const char *key, char *out, size_t out_size)
{
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && item->valuestring)
        snprintf(out, out_size, "%s", item->valuestring);
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
bool ui_touchpad_config_load(ui_touchpad_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    char nvs_fname[CFG_FNAME_LEN];
    if (!ui_touchpad_config_nvs_load(nvs_fname, sizeof(nvs_fname))) {
        ESP_LOGW(TAG, "no NVS config, treating as no config");
        return false;
    }

    char path[CFG_FNAME_LEN + 24];
    snprintf(path, sizeof(path), "%s/%s", SD_PATH_CONFIG_TOUCHPAD, nvs_fname);

    char *buf = read_file(path);
    if (!buf) return false;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "parse failed: %s", path);
        return false;
    }

    str_field(root, "bg_image", cfg->bg_image, sizeof(cfg->bg_image));

    cJSON *settings_obj = cJSON_GetObjectItem(root, "settings");
    if (cJSON_IsObject(settings_obj)) {
        str_field(settings_obj, "bg_image",  cfg->settings.bg_image,  sizeof(cfg->settings.bg_image));
        str_field(settings_obj, "side_icon", cfg->settings.side_icon, sizeof(cfg->settings.side_icon));
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "loaded: %s bg_image=\"%s\" settings.bg_image=\"%s\" settings.side_icon=\"%s\"",
             path, cfg->bg_image, cfg->settings.bg_image, cfg->settings.side_icon);
    return true;
}

bool ui_touchpad_config_nvs_save(const char *filename)
{
    return nvs_manager_set_str(CFG_NVS_NAMESPACE, CFG_NVS_KEY_TOUCHPAD, filename);
}

bool ui_touchpad_config_nvs_load(char *out, size_t out_size)
{
    return nvs_manager_get_str(CFG_NVS_NAMESPACE, CFG_NVS_KEY_TOUCHPAD, out, out_size);
}

touchpad_scan_result_t ui_touchpad_config_scan(void)
{
    touchpad_scan_result_t res = { .names = NULL, .count = 0 };

    DIR *dir = opendir(SD_PATH_CONFIG_TOUCHPAD);
    if (!dir) {
        ESP_LOGE(TAG, "touchpad config directory not found: %s", SD_PATH_CONFIG_TOUCHPAD);
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
        ESP_LOGE(TAG, "OOM allocating scan result");
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
        res.names[idx] = strndup(de->d_name, CFG_FNAME_LEN - 1);
        if (!res.names[idx]) {
            ESP_LOGE(TAG, "OOM copying filename");
            res.count = idx;
            closedir(dir);
            return res;
        }
        idx++;
    }

    closedir(dir);
    res.count = idx;
    ESP_LOGI(TAG, "scan found %d JSON file(s) in %s", res.count, SD_PATH_CONFIG_TOUCHPAD);
    return res;
}

void ui_touchpad_config_scan_free(touchpad_scan_result_t *res)
{
    /* touchpad_scan_result_t is just an alias of the shared cfg_scan_result_t
     * (see app_config.h) -- kept as its own wrapper for API symmetry with
     * ui_touchpad_config_scan(). */
    cfg_scan_result_free(res);
}
