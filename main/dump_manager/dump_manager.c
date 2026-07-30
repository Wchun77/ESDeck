#include "dump_manager.h"
#include "app_config.h"
#include "esp_core_dump.h"
#include "esp_partition.h"
#include "esp_log.h"
#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static const char *TAG = "DUMP_MGR";

/* Deliberately small: this buffer lives on app_main()'s own task stack
 * (CONFIG_ESP_MAIN_TASK_STACK_SIZE -- was only 3584 bytes, bumped to 8192
 * in sdkconfig, see below). A naive 4096-byte chunk buffer here overflowed
 * the old 3584-byte stack by itself the moment a real dump needed
 * exporting, corrupting unrelated memory and crashing somewhere else
 * entirely (this is exactly what happened: a 4KB local buffer here, on a
 * 3584-byte stack, produced a StoreProhibited/assert crash inside
 * vTaskGenericNotifyGiveFromISR -- a totally unrelated subsystem --
 * because the overflow smashed nearby task/heap bookkeeping rather than
 * crashing at the overflow site itself). Kept small even with the bigger
 * stack now in place, since there's no real throughput need here. */
#define DUMP_COPY_CHUNK   256
#define DUMP_NAME_LEN     48

/* Result of dump_manager_check(), consumed by dump_manager_export() --
 * see dump_manager.h for why this is split into two calls. */
static bool   s_dump_valid = false;
static size_t s_dump_size  = 0;

/* Next unused "coredump_NNNN.elf" index under SD_PATH_DUMP -- scans
 * existing files rather than persisting a counter in NVS. Deliberately
 * NOT using nvs_manager here: this only ever runs after an actual crash,
 * so it's not worth adding another routine write path to NVS for (see
 * app_config.h's SD_DIR_DUMP comment on why NVS writes are avoided where
 * they aren't already needed elsewhere in this project). */
static int next_dump_index(void)
{
    int max_seen = -1;

    DIR *dir = opendir(SD_PATH_DUMP);
    if (!dir) return 0;   /* folder doesn't exist yet -- start at 0 */

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        int idx;
        if (sscanf(de->d_name, "coredump_%d.elf", &idx) == 1 && idx > max_seen) {
            max_seen = idx;
        }
    }
    closedir(dir);

    return max_seen + 1;
}

void dump_manager_check(void)
{
    if (esp_core_dump_image_check() != ESP_OK) {
        return;   /* normal boot -- no crash dump waiting */
    }

    size_t addr = 0, size = 0;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK || size == 0) {
        ESP_LOGW(TAG, "coredump flagged valid but image_get failed -- leaving it in flash");
        return;
    }

    s_dump_valid = true;
    s_dump_size  = size;
}

void dump_manager_export(void)
{
    if (!s_dump_valid) {
        return;
    }

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (!part) {
        ESP_LOGW(TAG, "coredump partition not found");
        return;
    }

    size_t size = s_dump_size;
    if (size > part->size) {
        ESP_LOGW(TAG, "coredump reported size (%u) exceeds partition (%u), clamping",
                 (unsigned)size, (unsigned)part->size);
        size = part->size;
    }

    /* Created lazily (only when there's actually something to export)
     * rather than always via fs_sd_ensure_layout() at boot, so a device
     * that never crashes never gets an empty .dump folder cluttering the
     * card. */
    mkdir(SD_PATH_DUMP, 0775);

    char path[DUMP_NAME_LEN + sizeof(SD_PATH_DUMP) + 1];
    snprintf(path, sizeof(path), "%s/coredump_%04d.elf", SD_PATH_DUMP, next_dump_index());

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "failed to open %s for writing -- leaving flash copy intact", path);
        return;
    }

    uint8_t buf[DUMP_COPY_CHUNK];
    size_t remaining = size;
    size_t offset    = 0;
    bool   ok        = true;

    while (remaining > 0) {
        size_t chunk = (remaining < sizeof(buf)) ? remaining : sizeof(buf);
        if (esp_partition_read(part, offset, buf, chunk) != ESP_OK ||
            fwrite(buf, 1, chunk, f) != chunk) {
            ok = false;
            break;
        }
        offset    += chunk;
        remaining -= chunk;
    }
    fclose(f);

    if (!ok) {
        ESP_LOGW(TAG, "coredump copy to SD failed -- leaving flash copy intact");
        remove(path);
        return;
    }

    ESP_LOGI(TAG, "coredump copied to %s (%u bytes)", path, (unsigned)size);

    /* esp_core_dump_image_erase() (not a raw esp_partition_erase_range())
     * so the espcoredump component's own header/checksum state gets
     * invalidated correctly, not just the underlying flash bytes -- matters
     * because ESP_COREDUMP_FLASH_NO_OVERWRITE is on, which otherwise
     * refuses to write a future crash's dump over whatever's here. */
    esp_err_t erase_err = esp_core_dump_image_erase();
    if (erase_err != ESP_OK) {
        ESP_LOGW(TAG, "failed to erase flash coredump after copy: %d", erase_err);
    }

    s_dump_valid = false;
}
