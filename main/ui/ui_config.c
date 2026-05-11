#include "ui_config.h"

#include "nvs_manager.h"

#include <string.h>
#include <dirent.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"

#define TAG  "[UI_CFG]"

/* --------------------------------------------------------------------------
 * Internal helpers
 * -------------------------------------------------------------------------- */

static bool find_json(char *out_path, size_t out_size)
{
    DIR *dir = opendir(UI_CONFIG_JSON_PATH);
    if (!dir) {
        ESP_LOGE(TAG, "Cannot open %s", UI_CONFIG_JSON_PATH);
        return false;
    }

    struct dirent *de;
    bool found = false;

    while ((de = readdir(dir)) != NULL) {
        if (de->d_type != DT_REG) continue;
        if (strncmp(de->d_name, UI_CONFIG_JSON_PREFIX,
                    strlen(UI_CONFIG_JSON_PREFIX)) != 0) continue;

        snprintf(out_path, out_size, "%s/%s", UI_CONFIG_JSON_PATH, de->d_name);
        ESP_LOGI(TAG, "Found config: %s", out_path);
        found = true;
        break;
    }

    closedir(dir);

    if (!found) {
        ESP_LOGW(TAG, "No %s*.json found in %s",
                 UI_CONFIG_JSON_PREFIX, UI_CONFIG_JSON_PATH);
    }

    return found;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open: %s", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);

    if (sz <= 0) {
        ESP_LOGE(TAG, "Empty file: %s", path);
        fclose(f);
        return NULL;
    }

    char *buf = heap_caps_malloc((size_t)sz + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        /* Fallback to internal RAM */
        buf = malloc((size_t)sz + 1);
    }
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

/* --------------------------------------------------------------------------
 * Public API
 * -------------------------------------------------------------------------- */
bool ui_config_nvs_save(const char *filename)
{
    return nvs_manager_set_str(CFG_NVS_NAMESPACE, CFG_NVS_KEY, filename);
}

bool ui_config_nvs_load(char *out, size_t out_size)
{
    return nvs_manager_get_str(CFG_NVS_NAMESPACE, CFG_NVS_KEY, out, out_size);
}

bool ui_config_load(deck_cfg_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    /* Step 1: find the JSON file */
    char json_path[UI_CONFIG_FNAME_LEN + 8];
    char nvs_fname[UI_CONFIG_FNAME_LEN];

    if (ui_config_nvs_load(nvs_fname, sizeof(nvs_fname))) {
        snprintf(json_path, sizeof(json_path), "%s/%s",
                 UI_CONFIG_JSON_PATH, nvs_fname);
        ESP_LOGI(TAG, "Using NVS config: %s", json_path);
    } else {
        ESP_LOGW(TAG, "No NVS config, skipping load");
        return false;
    }

    /* Step 2: read into buffer */
    char *buf = read_file(json_path);
    if (!buf) return false;

    /* Step 3: parse */
    cJSON *root = cJSON_Parse(buf);
    free(buf);

    if (!root) {
        ESP_LOGE(TAG, "JSON parse error: %s", cJSON_GetErrorPtr());
        return false;
    }

    cJSON *pages_arr = cJSON_GetObjectItem(root, "pages");
    if (!cJSON_IsArray(pages_arr)) {
        ESP_LOGE(TAG, "Missing or invalid 'pages' array");
        cJSON_Delete(root);
        return false;
    }

    int page_count = cJSON_GetArraySize(pages_arr);
    if (page_count <= 0) {
        ESP_LOGW(TAG, "Empty pages array");
        cJSON_Delete(root);
        return false;
    }

    cfg->pages = calloc((size_t)page_count, sizeof(page_cfg_t));
    if (!cfg->pages) {
        ESP_LOGE(TAG, "OOM allocating pages");
        cJSON_Delete(root);
        return false;
    }
    cfg->page_count = (uint8_t)page_count;

    for (int pi = 0; pi < page_count; pi++) {
        cJSON *page_obj = cJSON_GetArrayItem(pages_arr, pi);
        page_cfg_t *page = &cfg->pages[pi];

        cJSON *name = cJSON_GetObjectItem(page_obj, "name");
        if (cJSON_IsString(name) && name->valuestring) {
            snprintf(page->name, UI_CONFIG_NAME_LEN, "%s", name->valuestring);
        } else {
            snprintf(page->name, UI_CONFIG_NAME_LEN, "P%d", pi + 1);
        }

        cJSON *bg = cJSON_GetObjectItem(page_obj, "bg_image");
        if (cJSON_IsString(bg) && bg->valuestring) {
            snprintf(page->bg_image, UI_CONFIG_BG_LEN, "%s", bg->valuestring);
        }

        cJSON *btns_arr = cJSON_GetObjectItem(page_obj, "buttons");
        if (!cJSON_IsArray(btns_arr)) {
            ESP_LOGW(TAG, "Page %d has no buttons array", pi);
            continue;
        }

        int btn_count = cJSON_GetArraySize(btns_arr);
        if (btn_count <= 0) continue;

        page->buttons = calloc((size_t)btn_count, sizeof(btn_cfg_t));
        if (!page->buttons) {
            ESP_LOGE(TAG, "OOM allocating buttons for page %d", pi);
            ui_config_free(cfg);
            cJSON_Delete(root);
            return false;
        }
        page->button_count = (uint8_t)btn_count;

        for (int bi = 0; bi < btn_count; bi++) {
            cJSON *btn_obj = cJSON_GetArrayItem(btns_arr, bi);
            btn_cfg_t *btn = &page->buttons[bi];

            cJSON *label = cJSON_GetObjectItem(btn_obj, "label");
            if (cJSON_IsString(label) && label->valuestring) {
                snprintf(btn->label, UI_CONFIG_LABEL_LEN, "%s", label->valuestring);
            } else {
                snprintf(btn->label, UI_CONFIG_LABEL_LEN, "%d", bi + 1);
            }

            cJSON *icon = cJSON_GetObjectItem(btn_obj, "icon");
            if (cJSON_IsString(icon) && icon->valuestring) {
                snprintf(btn->icon, UI_CONFIG_ICON_LEN, "%s", icon->valuestring);
            }
        }
    }

    cJSON_Delete(root);

    ESP_LOGI(TAG, "Loaded %d page(s)", cfg->page_count);
    return true;
}

void ui_config_free(deck_cfg_t *cfg)
{
    if (!cfg) return;

    if (cfg->pages) {
        for (int i = 0; i < cfg->page_count; i++) {
            if (cfg->pages[i].buttons) {
                free(cfg->pages[i].buttons);
                cfg->pages[i].buttons = NULL;
            }
        }
        free(cfg->pages);
        cfg->pages = NULL;
    }

    cfg->page_count = 0;
}

json_scan_result_t ui_config_scan(void)
{
    json_scan_result_t res = { .names = NULL, .count = 0 };

    DIR *dir = opendir(UI_CONFIG_JSON_PATH);
    if (!dir) {
        ESP_LOGE(TAG, "Cannot open %s", UI_CONFIG_JSON_PATH);
        return res;
    }

    /* First pass: count matching files */
    int total = 0;
    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_type != DT_REG) continue;
        size_t len = strlen(de->d_name);
        if (len < 5) continue;  /* need at least "x.json" */
        if (strcmp(de->d_name + len - 5, ".json") != 0) continue;
        total++;
    }

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

    /* Second pass: copy filenames */
    rewinddir(dir);
    int idx = 0;
    while ((de = readdir(dir)) != NULL && idx < total) {
        if (de->d_type != DT_REG) continue;
        size_t len = strlen(de->d_name);
        if (len < 5) continue;
        if (strcmp(de->d_name + len - 5, ".json") != 0) continue;

        res.names[idx] = strndup(de->d_name, UI_CONFIG_FNAME_LEN - 1);
        if (!res.names[idx]) {
            ESP_LOGE(TAG, "OOM copying filename");
            /* free what we have so far, return partial result */
            res.count = idx;
            closedir(dir);
            return res;
        }
        idx++;
    }

    closedir(dir);
    res.count = idx;
    ESP_LOGI(TAG, "Scan found %d JSON file(s)", res.count);
    return res;
}

void ui_config_scan_free(json_scan_result_t *res)
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