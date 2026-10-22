#include "cfg_common.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_heap_caps.h"

#define TAG  "[CFG]"

char *cfg_read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

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

    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

void cfg_str_field(const cJSON *obj, const char *key, char *out, size_t out_size)
{
    const cJSON *item = cJSON_GetObjectItem(obj, key);
    if (cJSON_IsString(item) && item->valuestring)
        snprintf(out, out_size, "%s", item->valuestring);
}

static bool is_json_file(const struct dirent *de)
{
    if (de->d_type != DT_REG) return false;
    size_t len = strlen(de->d_name);
    return len >= 5 && strcmp(de->d_name + len - 5, ".json") == 0;
}

cfg_scan_result_t cfg_scan_json_dir(const char *dir_path)
{
    cfg_scan_result_t res = { .names = NULL, .count = 0 };

    DIR *dir = opendir(dir_path);
    if (!dir) {
        ESP_LOGE(TAG, "config directory not found: %s", dir_path);
        res.count = -1;
        return res;
    }

    /* First pass: count, second pass: copy names. */
    int total = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL)
        if (is_json_file(de)) total++;

    if (total == 0) {
        closedir(dir);
        return res;
    }

    res.names = calloc((size_t)total, sizeof(char *));
    if (!res.names) {
        ESP_LOGE(TAG, "OOM allocating scan result");
        closedir(dir);
        return res;
    }

    rewinddir(dir);
    int idx = 0;
    while ((de = readdir(dir)) != NULL && idx < total) {
        if (!is_json_file(de)) continue;
        res.names[idx] = strndup(de->d_name, CFG_FNAME_LEN - 1);
        if (!res.names[idx]) {
            ESP_LOGE(TAG, "OOM copying filename");
            break;
        }
        idx++;
    }

    closedir(dir);
    res.count = idx;
    ESP_LOGI(TAG, "scan found %d JSON file(s) in %s", res.count, dir_path);
    return res;
}
