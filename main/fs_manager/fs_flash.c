#include "fs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_vfs_fat.h"
#include "esp_partition.h"
#include "ff.h"
#include "string.h"
#include "dirent.h"

#define TAG              "[FS_FLASH]"
#define PARTITION_LABEL  "storage"

static bool        s_mounted     = false;
static wl_handle_t s_wl_handle   = WL_INVALID_HANDLE;

/* --------------------------------------------------------------------------
 * Mount / Unmount
 * -------------------------------------------------------------------------- */

bool fs_flash_init(void)
{
    if (s_mounted)
    {
        return true;
    }

    ESP_LOGI(TAG, "Mounting FAT on flash partition \"%s\" -> \"%s\"",
             PARTITION_LABEL, FS_FLASH_MOUNT_POINT);

    const esp_vfs_fat_mount_config_t cfg =
    {
        .max_files             = 4,
        .format_if_mount_failed = true,
        .allocation_unit_size  = 4096,
    };

    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(FS_FLASH_MOUNT_POINT,
                                                      PARTITION_LABEL,
                                                      &cfg,
                                                      &s_wl_handle);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "Mount failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Mounted, wl_handle=%ld", s_wl_handle);
    s_mounted = true;

    fs_flash_usage_printf();
    fs_flash_scan();

    return true;
}

void fs_flash_deinit(void)
{
    if (!s_mounted)
    {
        return;
    }

    esp_task_wdt_config_t wdt =
    {
        .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
        .trigger_panic  = false,
        .timeout_ms     = 15000,
    };
    esp_task_wdt_reconfigure(&wdt);

    esp_vfs_fat_spiflash_unmount_rw_wl(FS_FLASH_MOUNT_POINT, s_wl_handle);

    wdt.timeout_ms = 5000;
    esp_task_wdt_reconfigure(&wdt);

    s_wl_handle = WL_INVALID_HANDLE;
    s_mounted   = false;

    ESP_LOGI(TAG, "Unmounted");
}

void fs_flash_unmount_for_usb(void)
{
    if (!s_mounted) return;
    esp_vfs_fat_spiflash_unmount_rw_wl(FS_FLASH_MOUNT_POINT, s_wl_handle);
    s_mounted   = false;
    s_wl_handle = WL_INVALID_HANDLE;
    ESP_LOGI(TAG, "Unmounted for USB");
}

bool fs_flash_status(void)
{
    return s_mounted;
}

/* --------------------------------------------------------------------------
 * Format
 * -------------------------------------------------------------------------- */

bool fs_flash_format(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                            ESP_PARTITION_SUBTYPE_ANY,
                                                            PARTITION_LABEL);
    if (part == NULL)
    {
        ESP_LOGE(TAG, "Partition \"%s\" not found", PARTITION_LABEL);
        return false;
    }

    if (esp_partition_erase_range(part, 0, part->size) != ESP_OK)
    {
        ESP_LOGE(TAG, "Erase failed");
        return false;
    }

    s_mounted = false;
    ESP_LOGI(TAG, "Format OK");
    return true;
}

/* --------------------------------------------------------------------------
 * Usage
 * -------------------------------------------------------------------------- */

void fs_flash_usage_printf(void)
{
    FATFS *fs;
    DWORD  free_clusters;
    FRESULT res = f_getfree("0:", &free_clusters, &fs);
    assert(res == FR_OK);

    size_t total = (size_t)((fs->n_fatent - 2) * fs->csize) * fs->ssize;
    size_t free  = (size_t)(free_clusters      * fs->csize) * fs->ssize;

    ESP_LOGI(TAG, "%u kB total, %u kB free",
             (unsigned)(total / 1024), (unsigned)(free / 1024));
}

size_t fs_flash_total(void)
{
    FATFS *fs;
    DWORD  free_clusters;
    FRESULT res = f_getfree("0:", &free_clusters, &fs);
    assert(res == FR_OK);
    return (size_t)((fs->n_fatent - 2) * fs->csize) * fs->ssize / 1024;
}

size_t fs_flash_free(void)
{
    FATFS *fs;
    DWORD  free_clusters;
    FRESULT res = f_getfree("0:", &free_clusters, &fs);
    assert(res == FR_OK);
    return (size_t)(free_clusters * fs->csize) * fs->ssize / 1024;
}

/* --------------------------------------------------------------------------
 * Sector-level access (raw WL)
 * -------------------------------------------------------------------------- */

uint32_t fs_flash_sector_count(void)
{
    size_t sz = wl_sector_size(s_wl_handle);
    if (sz == 0)
    {
        ESP_LOGW(TAG, "WL sector size is zero");
        return 0;
    }
    return (uint32_t)(wl_size(s_wl_handle) / sz);
}

uint32_t fs_flash_sector_size(void)
{
    return (uint32_t)wl_sector_size(s_wl_handle);
}

esp_err_t fs_flash_read_sector(uint32_t lba, uint32_t offset, size_t size, void *dest)
{
    size_t addr = (size_t)lba * fs_flash_sector_size() + offset;
    return wl_read(s_wl_handle, addr, dest, size);
}

esp_err_t fs_flash_write_sector(uint32_t lba, uint32_t offset, size_t size, const void *src)
{
    if (s_mounted)
    {
        ESP_LOGE(TAG, "Cannot write sector while FAT is mounted");
        return ESP_ERR_INVALID_STATE;
    }

    size_t       addr        = (size_t)lba * fs_flash_sector_size() + offset;
    size_t       sector_size = wl_sector_size(s_wl_handle);

    if (addr % sector_size != 0 || size % sector_size != 0)
    {
        return ESP_ERR_INVALID_ARG;
    }

    wl_erase_range(s_wl_handle, addr, size);
    return wl_write(s_wl_handle, addr, src, size);
}


/* --------------------------------------------------------------------------
 * Directory scan (called once after mount)
 * -------------------------------------------------------------------------- */

static void scan_dir(const char *path, uint8_t depth)
{
    static const char * const indent[4] =
    {
        "- ",
        "  - ",
        "    - ",
        "      - ",
    };

    if (depth >= 4)
    {
        ESP_LOGE(TAG, "fs_flash_scan: max depth reached");
        return;
    }

    DIR *dir = opendir(path);
    if (!dir)
    {
        ESP_LOGW(TAG, "Cannot open: %s", path);
        return;
    }

    struct dirent *de;
    char child[257];

    while ((de = readdir(dir)) != NULL)
    {
        ESP_LOGI(TAG, "%s%s", indent[depth], de->d_name);

        if (de->d_type == DT_DIR)
        {
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            scan_dir(child, depth + 1);
        }
    }

    closedir(dir);
}

void fs_flash_scan(void)
{
    ESP_LOGI(TAG, "Scan: %s", FS_FLASH_MOUNT_POINT);
    scan_dir(FS_FLASH_MOUNT_POINT, 0);
}

wl_handle_t fs_flash_get_wl_handle(void)
{
    return s_wl_handle;
}