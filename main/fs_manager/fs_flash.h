#ifndef __FS_FLASH_H
#define __FS_FLASH_H

#include "stdint.h"
#include "stdbool.h"
#include "stddef.h"

#include "wear_levelling.h"

#include "esp_err.h"

#define FS_FLASH_MOUNT_POINT    "/flash"

bool        fs_flash_init(void);
void        fs_flash_deinit(void);
void        fs_flash_unmount_for_usb(void);
bool        fs_flash_status(void);

bool        fs_flash_format(void);

void        fs_flash_scan(void);

void        fs_flash_usage_printf(void);
size_t      fs_flash_total(void);
size_t      fs_flash_free(void);

uint32_t    fs_flash_sector_count(void);
uint32_t    fs_flash_sector_size(void);
esp_err_t   fs_flash_read_sector(uint32_t lba, uint32_t offset, size_t size, void *dest);
esp_err_t   fs_flash_write_sector(uint32_t lba, uint32_t offset, size_t size, const void *src);

wl_handle_t fs_flash_get_wl_handle(void);

#endif /* __FS_FLASH_H */