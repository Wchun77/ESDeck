#ifndef __FS_SD_H
#define __FS_SD_H

#include "stdint.h"
#include "stdbool.h"
#include "stddef.h"
#include "esp_err.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "app_config.h"

#define FS_SD_MOUNT_POINT    SD_MOUNT_POINT

bool        fs_sd_init(void);
void        fs_sd_deinit(void);
void        fs_sd_unmount_fat(void);
bool        fs_sd_status(void);

bool        fs_sd_format(void);

void        fs_sd_scan(void);

/* Create any missing app folders under FS_SD_MOUNT_POINT (config/deck,
 * config/monitor, assets/icons, assets/backgrounds, assets/fonts/bin/clock,
 * assets/fonts/bin/notify, ...). Never touches or removes existing
 * files/folders. */
void        fs_sd_ensure_layout(void);

void        fs_sd_usage_printf(void);
size_t      fs_sd_total(void);
size_t      fs_sd_free(void);

uint32_t    fs_sd_sector_count(void);
uint32_t    fs_sd_sector_size(void);
esp_err_t   fs_sd_read_sector(uint32_t lba, uint32_t offset, size_t size, void *dest);
esp_err_t   fs_sd_write_sector(uint32_t lba, uint32_t offset, size_t size, const void *src);

sdmmc_card_t *fs_sd_get_card(void);

#endif /* __FS_SD_H */
