#include "fs_sd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "ff.h"
#include "diskio_impl.h"
#include "diskio_sdmmc.h"
#include "string.h"
#include "dirent.h"
#include "sys/stat.h"

/*
 * Board: Waveshare ESP32-S3-Touch-LCD-7 V1.2
 *
 * SD card wired to SDMMC peripheral (4-bit mode):
 *   CLK  -> IO12
 *   CMD  -> IO11
 *   D0   -> IO13
 */

#define TAG  "[FS_SD]"

#define SD_PIN_CLK   12
#define SD_PIN_CMD   11
#define SD_PIN_D0    13

static bool          s_mounted = false;
static sdmmc_card_t *s_card    = NULL;

/* --------------------------------------------------------------------------
 * Mount / Unmount
 * -------------------------------------------------------------------------- */

bool fs_sd_init(void)
{
    if (s_mounted) return true;

    ESP_LOGI(TAG, "Probing SD card (SDMMC 4-bit) -> \"%s\"", FS_SD_MOUNT_POINT);

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk   = SD_PIN_CLK;
    slot.cmd   = SD_PIN_CMD;
    slot.d0    = SD_PIN_D0;
    slot.width = 1;   /* 1-bit mode */
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    slot.cd    = SDMMC_SLOT_NO_CD;
    slot.wp    = SDMMC_SLOT_NO_WP;

    const esp_vfs_fat_mount_config_t cfg = {
        .format_if_mount_failed = false,
        .max_files              = 4,
        .allocation_unit_size   = 16 * 1024,
    };

    esp_err_t err = esp_vfs_fat_sdmmc_mount(FS_SD_MOUNT_POINT, &host, &slot, &cfg, &s_card);
    if (err != ESP_OK) {
        if (err == ESP_FAIL) {
            ESP_LOGW(TAG, "SD mount failed (no card?) — skipping");
        } else {
            ESP_LOGW(TAG, "SD init error: %s — skipping", esp_err_to_name(err));
        }
        s_card    = NULL;
        s_mounted = false;
        return false;
    }

    s_mounted = true;
    ESP_LOGI(TAG, "SD card mounted");
    sdmmc_card_print_info(stdout, s_card);
    fs_sd_usage_printf();
    fs_sd_ensure_layout();
    fs_sd_scan();
    return true;
}

void fs_sd_deinit(void)
{
    fs_sd_unmount_fat();
    sdmmc_host_deinit();
    s_card = NULL;
}

void fs_sd_unmount_fat(void)
{
    if (!s_mounted) return;

    /* 只解除 VFS 註冊，不碰 SDMMC host.
     * 注意：pdrv 是 FatFs 註冊時分配的邏輯磁碟代號，
     * 跟 s_card->host.slot（SDMMC 硬體插槽編號）是兩回事，不能混用。 */
    BYTE pdrv = ff_diskio_get_pdrv_card(s_card);
    if (pdrv != 0xff) {
        ff_diskio_unregister(pdrv);
    } else {
        ESP_LOGW(TAG, "fs_sd_unmount_fat: pdrv not found for card");
    }
    esp_vfs_fat_unregister_path(FS_SD_MOUNT_POINT);

    s_mounted = false;
}

bool fs_sd_status(void)
{
    return s_mounted;
}

/* --------------------------------------------------------------------------
 * Format
 * -------------------------------------------------------------------------- */

bool fs_sd_format(void)
{
    if (!s_mounted || s_card == NULL) {
        ESP_LOGE(TAG, "Cannot format: SD not mounted");
        return false;
    }

    esp_vfs_fat_sdcard_unmount(FS_SD_MOUNT_POINT, s_card);
    s_mounted = false;

    uint32_t sectors_to_erase = (1024u * 1024u) / s_card->csd.sector_size;
    esp_err_t err = sdmmc_erase_sectors(s_card, 0, sectors_to_erase, SDMMC_ERASE_ARG);

    s_card = NULL;

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Erase failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "Format OK (re-run fs_sd_init to remount)");
    return true;
}

/* --------------------------------------------------------------------------
 * Usage
 * -------------------------------------------------------------------------- */

void fs_sd_usage_printf(void)
{
    FATFS  *fs;
    DWORD   free_clusters;

    FRESULT res = f_getfree("0:", &free_clusters, &fs);
    if (res != FR_OK) {
        ESP_LOGW(TAG, "f_getfree failed (%d)", res);
        return;
    }

    size_t total = (size_t)((fs->n_fatent - 2) * fs->csize) * fs->ssize;
    size_t free  = (size_t)(free_clusters      * fs->csize) * fs->ssize;

    ESP_LOGI(TAG, "%u kB total, %u kB free",
             (unsigned)(total / 1024), (unsigned)(free / 1024));
}

size_t fs_sd_total(void)
{
    FATFS  *fs;
    DWORD   free_clusters;
    FRESULT res = f_getfree("0:", &free_clusters, &fs);
    if (res != FR_OK) return 0;
    return (size_t)((fs->n_fatent - 2) * fs->csize) * fs->ssize / 1024;
}

size_t fs_sd_free(void)
{
    FATFS  *fs;
    DWORD   free_clusters;
    FRESULT res = f_getfree("0:", &free_clusters, &fs);
    if (res != FR_OK) return 0;
    return (size_t)(free_clusters * fs->csize) * fs->ssize / 1024;
}

/* --------------------------------------------------------------------------
 * Sector-level access
 * -------------------------------------------------------------------------- */

uint32_t fs_sd_sector_count(void)
{
    if (s_card == NULL) return 0;
    return s_card->csd.capacity;
}

uint32_t fs_sd_sector_size(void)
{
    if (s_card == NULL) return 0;
    return (uint32_t)s_card->csd.sector_size;
}

esp_err_t fs_sd_read_sector(uint32_t lba, uint32_t offset, size_t size, void *dest)
{
    if (s_card == NULL)       return ESP_ERR_INVALID_STATE;
    if (offset + size > 512u) return ESP_ERR_INVALID_ARG;

    uint8_t tmp[512];
    esp_err_t err = sdmmc_read_sectors(s_card, tmp, lba, 1);
    if (err != ESP_OK) return err;

    memcpy(dest, tmp + offset, size);
    return ESP_OK;
}

esp_err_t fs_sd_write_sector(uint32_t lba, uint32_t offset, size_t size, const void *src)
{
    if (s_card == NULL)       return ESP_ERR_INVALID_STATE;
    if (offset + size > 512u) return ESP_ERR_INVALID_ARG;

    if (s_mounted) {
        ESP_LOGE(TAG, "Cannot write sector while FAT is mounted");
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t tmp[512];
    esp_err_t err = sdmmc_read_sectors(s_card, tmp, lba, 1);
    if (err != ESP_OK) return err;

    memcpy(tmp + offset, src, size);
    return sdmmc_write_sectors(s_card, tmp, lba, 1);
}

/* --------------------------------------------------------------------------
 * App folder layout
 * -------------------------------------------------------------------------- */

/* Sourced from app_config.h -- single place that owns the SD folder layout. */
static const char * const s_app_dirs[] = {
    SD_DIR_CONFIG_DECK,
    SD_DIR_CONFIG_MONITOR,
    SD_DIR_ASSETS_ICONS,
    SD_DIR_ASSETS_BG,
    SD_DIR_ASSETS_FONTS,
    SD_DIR_ASSETS_BOOT,
    SD_DIR_ASSETS_SIDE_ICON,
    SD_DIR_UPDATE,
};

/* Create dir and any missing parent dirs, relative to FS_SD_MOUNT_POINT.
 * Only creates missing path segments; existing files/folders are left
 * untouched (stat is checked before every mkdir). */
static void mkdir_p(const char *rel_path)
{
    char path[128];
    size_t prefix_len = strlen(FS_SD_MOUNT_POINT);
    snprintf(path, sizeof(path), "%s/%s", FS_SD_MOUNT_POINT, rel_path);

    struct stat st;

    /* mkdir each intermediate "/sdcard/a", "/sdcard/a/b", ... in turn */
    for (char *p = path + prefix_len + 1; *p != '\0'; p++) {
        if (*p != '/') continue;

        *p = '\0';
        if (stat(path, &st) != 0 && mkdir(path, 0775) != 0) {
            ESP_LOGW(TAG, "mkdir failed: %s", path);
            *p = '/';
            return;
        }
        *p = '/';
    }

    /* final segment */
    if (stat(path, &st) != 0 && mkdir(path, 0775) != 0) {
        ESP_LOGW(TAG, "mkdir failed: %s", path);
    }
}

void fs_sd_ensure_layout(void)
{
    if (!s_mounted) return;

    for (size_t i = 0; i < sizeof(s_app_dirs) / sizeof(s_app_dirs[0]); i++) {
        mkdir_p(s_app_dirs[i]);
    }
}

/* --------------------------------------------------------------------------
 * Directory scan
 * -------------------------------------------------------------------------- */

static void scan_dir(const char *path, uint8_t depth)
{
    static const char * const indent[4] = {
        "- ", "  - ", "    - ", "      - ",
    };

    if (depth >= 4) {
        ESP_LOGE(TAG, "fs_sd_scan: max depth reached");
        return;
    }

    DIR *dir = opendir(path);
    if (!dir) {
        ESP_LOGW(TAG, "Cannot open: %s", path);
        return;
    }

    struct dirent *de;
    char child[257];

    while ((de = readdir(dir)) != NULL) {
        ESP_LOGI(TAG, "%s%s", indent[depth], de->d_name);
        if (de->d_type == DT_DIR) {
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            scan_dir(child, depth + 1);
        }
    }

    closedir(dir);
}

void fs_sd_scan(void)
{
    ESP_LOGI(TAG, "Scan: %s", FS_SD_MOUNT_POINT);
    scan_dir(FS_SD_MOUNT_POINT, 0);
}

sdmmc_card_t *fs_sd_get_card(void)
{
    return s_card;
}
