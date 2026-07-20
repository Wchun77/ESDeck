#include "ui_media_config.h"
#include "app_config.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#define TAG  "[MEDIA_CFG]"

/* Single fixed filename -- see ui_media_config.h's doc comment for why. */
#define MEDIA_CONFIG_PATH  SD_PATH_CONFIG_MEDIA "/settings.json"

/* -----------------------------------------------------------------------
 * Internal helpers -- same read_file()/str_field() shape as
 * ui_config.c/ui_monitor_config.c use, kept as its own small copy rather
 * than shared (matches how those two don't share one either).
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
bool ui_media_config_load(ui_media_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    char *buf = read_file(MEDIA_CONFIG_PATH);
    if (!buf) return false;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "parse failed: %s", MEDIA_CONFIG_PATH);
        return false;
    }

    str_field(root, "bg_image", cfg->bg_image, sizeof(cfg->bg_image));

    cJSON *settings_obj = cJSON_GetObjectItem(root, "settings");
    if (cJSON_IsObject(settings_obj)) {
        str_field(settings_obj, "bg_image",  cfg->settings.bg_image,  sizeof(cfg->settings.bg_image));
        str_field(settings_obj, "side_icon", cfg->settings.side_icon, sizeof(cfg->settings.side_icon));
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "loaded: bg_image=\"%s\" settings.bg_image=\"%s\" settings.side_icon=\"%s\"",
             cfg->bg_image, cfg->settings.bg_image, cfg->settings.side_icon);
    return true;
}
