#include "ota_manager.h"
#include "app_config.h"
#include "fs_manager/fs_sd.h"

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include <stdlib.h>
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

/* Read until buf is full or the source ends -- a stream source (HID) hands
 * out small pieces, but esp_ota_write() is happiest with whole 4 KB
 * blocks, and the project-name check needs the first ~112 bytes at once. */
static int read_full(ota_read_fn_t read_fn, void *ctx, uint8_t *buf, size_t want)
{
    size_t got = 0;
    while (got < want) {
        int n = read_fn(buf + got, want - got, ctx);
        if (n < 0) return n;
        if (n == 0) break;
        got += (size_t)n;
    }
    return (int)got;
}

/* esp_image_header_t (24) + first esp_image_segment_header_t (8), then
 * esp_app_desc_t: magic(4) secure_version(4) reserv1(8) version[32]
 * project_name[32] ... */
#define APP_DESC_OFFSET      32
#define APP_DESC_PROJ_OFFSET (APP_DESC_OFFSET + 4 + 4 + 8 + 32)
#define IMAGE_MAGIC          0xE9

static bool image_matches_project(const uint8_t *buf, size_t len)
{
    if (len < APP_DESC_PROJ_OFFSET + 32 || buf[0] != IMAGE_MAGIC) {
        ESP_LOGE(TAG, "Not an ESP app image");
        return false;
    }
    char proj[33];
    memcpy(proj, buf + APP_DESC_PROJ_OFFSET, 32);
    proj[32] = '\0';
    const char *mine = esp_app_get_description()->project_name;
    if (strncmp(proj, mine, 32) != 0) {
        ESP_LOGE(TAG, "Image is project \"%s\", running \"%s\" -- refused", proj, mine);
        return false;
    }
    return true;
}

bool ota_apply_stream(size_t size, ota_read_fn_t read_fn, void *read_ctx,
                      bool check_project,
                      ota_phase_cb_t on_erase_done,
                      ota_progress_cb_t progress_cb, void *user_data)
{
    if (read_fn == NULL || size == 0) {
        ESP_LOGE(TAG, "Invalid arguments");
        return false;
    }

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (target == NULL) {
        ESP_LOGE(TAG, "No OTA partition available");
        return false;
    }
    if (size > target->size) {
        ESP_LOGE(TAG, "Image (%u bytes) larger than partition (%lu bytes)",
                 (unsigned)size, (unsigned long)target->size);
        return false;
    }

    ESP_LOGI(TAG, "Flashing %u bytes -> partition \"%s\" @ 0x%06lx",
             (unsigned)size, target->label, (unsigned long)target->address);

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
        return false;
    }

    if (on_erase_done) on_erase_done(user_data);

    uint8_t *buf = malloc(OTA_READ_CHUNK);
    if (!buf) {
        ESP_LOGE(TAG, "malloc failed");
        esp_ota_abort(handle);
        return false;
    }

    size_t total = 0;
    bool   ok    = true;

    while (total < size) {
        size_t want = size - total;
        if (want > OTA_READ_CHUNK) want = OTA_READ_CHUNK;
        int n = read_full(read_fn, read_ctx, buf, want);
        if (n < 0) {
            ESP_LOGE(TAG, "read failed at %u / %u bytes", (unsigned)total, (unsigned)size);
            ok = false;
            break;
        }
        if (n == 0) {
            ESP_LOGE(TAG, "source ended early at %u / %u bytes", (unsigned)total, (unsigned)size);
            ok = false;
            break;
        }
        if (total == 0 && check_project && !image_matches_project(buf, (size_t)n)) {
            ok = false;
            break;
        }

        err = esp_ota_write(handle, buf, (size_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            ok = false;
            break;
        }

        total += (size_t)n;
        if (progress_cb) {
            uint8_t pct = (uint8_t)((total * 100) / size);
            if (pct > 100) pct = 100;
            progress_cb(pct, user_data);
        }
    }

    free(buf);

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
    ESP_LOGI(TAG, "OTA update flashed OK, ready to reboot");
    return true;
}

static int file_read(uint8_t *buf, size_t max, void *ctx)
{
    FILE *f = (FILE *)ctx;
    size_t n = fread(buf, 1, max, f);
    if (n == 0 && ferror(f)) return -1;
    return (int)n;
}

bool ota_apply_update(const char *path, size_t size,
                      ota_phase_cb_t on_erase_done,
                      ota_progress_cb_t progress_cb, void *user_data)
{
    if (path == NULL || size == 0) {
        ESP_LOGE(TAG, "Invalid arguments");
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Cannot open %s", path);
        return false;
    }
    ESP_LOGI(TAG, "SD update: %s", path);

    /* SD keeps its old rule (file name must contain FW_PROJECT_NAME, see
     * ota_check_update()) -- no project-name check on the image itself. */
    bool ok = ota_apply_stream(size, file_read, f, false, on_erase_done, progress_cb, user_data);
    fclose(f);

    if (ok) remove(path);   /* done -- don't offer the same update again on next boot */
    return ok;
}
