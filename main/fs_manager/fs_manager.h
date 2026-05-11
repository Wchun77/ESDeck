#ifndef __FS_MANAGER_H
#define __FS_MANAGER_H

#include "stdint.h"
#include "stdbool.h"
#include "stddef.h"

/* --------------------------------------------------------------------------
 * Callers pass full paths, e.g. "/flash/data.bin" or "/sdcard/log.txt".
 * This layer does not route by prefix; it is purely POSIX file utilities.
 * Use fs_flash_status() / fs_sd_status() to guard SD-only paths.
 * -------------------------------------------------------------------------- */

typedef struct
{
    char     filepath[512];
    uint32_t size;
} fs_file_info_t;

/* Basic I/O */
bool     fs_exists(const char *path);
bool     fs_read(const char *path, char *buf, uint16_t size);
bool     fs_write(const char *path, char *buf, uint16_t size);
bool     fs_read_chunk(const char *path, uint8_t *buf, uint32_t offset, uint32_t size);
uint32_t fs_get_file_size(const char *path);

/* Directory utilities */
void fs_scan(const char *path);
bool fs_delete(const char *path);
int  fs_list_files(const char *path, const char *extension,
                   fs_file_info_t *out_list, int max_count);

/* Display helpers */
void fs_print_file_list(const char *title, const char *path,
                        fs_file_info_t *file_list, int count);
void fs_print_tree(const char *path, const char *title, int max_items);

#endif /* __FS_MANAGER_H */