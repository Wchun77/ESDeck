#include "ui_media_config.h"
#include "cfg_common.h"
#include "app_config.h"
#include "nvs_manager.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cJSON.h"

#define TAG  "[MEDIA_CFG]"

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
bool ui_media_config_load(ui_media_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    char nvs_fname[CFG_FNAME_LEN];
    if (!ui_media_config_nvs_load(nvs_fname, sizeof(nvs_fname))) {
        ESP_LOGW(TAG, "no NVS config, treating as no config");
        return false;
    }

    char path[CFG_FNAME_LEN + 24];
    snprintf(path, sizeof(path), "%s/%s", SD_PATH_CONFIG_MEDIA, nvs_fname);

    char *buf = cfg_read_file(path);
    if (!buf) return false;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "parse failed: %s", path);
        return false;
    }

    cfg_str_field(root, "bg_image", cfg->bg_image, sizeof(cfg->bg_image));

    cJSON *settings_obj = cJSON_GetObjectItem(root, "settings");
    if (cJSON_IsObject(settings_obj)) {
        cfg_str_field(settings_obj, "bg_image",  cfg->settings.bg_image,  sizeof(cfg->settings.bg_image));
        cfg_str_field(settings_obj, "side_icon", cfg->settings.side_icon, sizeof(cfg->settings.side_icon));
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "loaded: %s bg_image=\"%s\" settings.bg_image=\"%s\" settings.side_icon=\"%s\"",
             path, cfg->bg_image, cfg->settings.bg_image, cfg->settings.side_icon);
    return true;
}

bool ui_media_config_nvs_save(const char *filename)
{
    return nvs_manager_set_str(CFG_NVS_NAMESPACE, CFG_NVS_KEY_MEDIA, filename);
}

bool ui_media_config_nvs_load(char *out, size_t out_size)
{
    return nvs_manager_get_str(CFG_NVS_NAMESPACE, CFG_NVS_KEY_MEDIA, out, out_size);
}

media_scan_result_t ui_media_config_scan(void)
{
    return cfg_scan_json_dir(SD_PATH_CONFIG_MEDIA);
}

void ui_media_config_scan_free(media_scan_result_t *res)
{
    /* media_scan_result_t is just an alias of the shared cfg_scan_result_t
     * (see app_config.h) -- kept as its own wrapper for API symmetry with
     * ui_media_config_scan(). */
    cfg_scan_result_free(res);
}
