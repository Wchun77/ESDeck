#include "ota_manager.h"
#include "app_config.h"
#include "fs_manager/fs_sd.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "dirent.h"
#include "sys/stat.h"
#include "string.h"
#include "stdio.h"

#define TAG              "[OTA]"
#define OTA_READ_CHUNK   4096

ota_scan_result_t ota_check_update(void)
{
    ota_scan_result_t res = { .found = false };

    if (!fs_sd_status()) return res;

    DIR *dir = opendir(SD_PATH_UPDATE);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open %s (folder missing?)", SD_PATH_UPDATE);
        return res;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;   /* skip . / .. / hidden */

        /* must contain FW_PROJECT_NAME and end in FW_UPDATE_EXTENSION */
        if (strstr(de->d_name, FW_PROJECT_NAME) == NULL) continue;

        size_t len = strlen(de->d_name);
        size_t ext_len = strlen(FW_UPDATE_EXTENSION);
        if (len <= ext_len) continue;
        if (strcmp(de->d_name + (len - ext_len), FW_UPDATE_EXTENSION) != 0) continue;

        snprintf(res.path, sizeof(res.path), "%s/%s", SD_PATH_UPDATE, de->d_name);
        snprintf(res.filename, sizeof(res.filename), "%s", de->d_name);

        struct stat st;
        if (stat(res.path, &st) == 0) {
            res.size = (size_t)st.st_size;
        }

        res.found = true;
        break;
    }

    closedir(dir);
    return res;
}

bool ota_apply_update(const char *path, size_t size,
                      ota_phase_cb_t on_erase_done,
                      ota_progress_cb_t progress_cb, void *user_data)
{
    if (path == NULL || size == 0) {
        ESP_LOGE(TAG, "Invalid arguments");
        return false;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        ESP_LOGE(TAG, "No OTA partition available");
        return false;
    }

    ESP_LOGI(TAG, "Flashing %s (%u bytes) -> partition \"%s\" @ 0x%06lx",
             path, (unsigned)size, target->label, (unsigned long)target->address);

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return false;
    }

    /* OTA_SIZE_UNKNOWN erases the whole partition here, in one blocking call.
     * We tried the alternative (OTA_WITH_SEQUENTIAL_WRITES, which erases
     * sector-by-sector inside esp_ota_write instead) and it was worse: every
     * ~4KB write then also has to erase, so the stutter spreads across the
     * *entire* transfer instead of being a single upfront pause, and without
     * the large-block-erase optimization it's also slower overall. Better to
     * eat one bulk erase now (caller shows a static "preparing" screen for
     * this, via on_erase_done/progress_cb sequencing) and get fully-smooth
     * writes afterwards. */
    ESP_LOGI(TAG, "Erasing partition (this can take a few seconds)...");
    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        fclose(f);
        return false;
    }

    if (on_erase_done) on_erase_done(user_data);

    uint8_t *buf = malloc(OTA_READ_CHUNK);
    if (!buf) {
        ESP_LOGE(TAG, "malloc failed");
        esp_ota_abort(handle);
        fclose(f);
        return false;
    }

    size_t total_read = 0;
    bool   ok = true;

    while (1) {
        size_t n = fread(buf, 1, OTA_READ_CHUNK, f);
        if (n == 0) break;

        err = esp_ota_write(handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            ok = false;
            break;
        }

        total_read += n;
        if (progress_cb) {
            uint8_t pct = (size > 0) ? (uint8_t)((total_read * 100) / size) : 0;
            if (pct > 100) pct = 100;
            progress_cb(pct, user_data);
        }
    }

    free(buf);
    fclose(f);

    if (!ok) {
        esp_ota_abort(handle);
        return false;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s (image invalid/corrupt?)", esp_err_to_name(err));
        return false;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return false;
    }

    if (progress_cb) progress_cb(100, user_data);

    remove(path);   /* done -- don't offer the same update again on next boot */
    ESP_LOGI(TAG, "OTA update flashed OK, ready to reboot");
    return true;
}
