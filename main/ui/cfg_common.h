#pragma once
#include <stddef.h>
#include "cJSON.h"
#include "app_config.h"   /* cfg_scan_result_t, CFG_FNAME_LEN */

/* --------------------------------------------------------------------------
 * Helpers shared by every mode's config loader (ui_deck_config.c,
 * ui_monitor_config.c, ui_media_config.c, ui_touchpad_config.c).
 * -------------------------------------------------------------------------- */

/* Whole file as a NUL-terminated string (PSRAM first, internal RAM as a
 * fallback). NULL if it can't be opened, is empty, or out of memory -- only
 * the OOM case is logged ("no config yet" is the common case); the caller
 * logs anything else it cares about. Free with free(). */
char *cfg_read_file(const char *path);

/* Copy obj[key] into out if it's a string; out is left untouched otherwise
 * (so it keeps whatever default the caller put there). */
void cfg_str_field(const cJSON *obj, const char *key, char *out, size_t out_size);

/* List the *.json files in dir (names only, each up to CFG_FNAME_LEN - 1
 * chars). count = -1 if dir doesn't exist. Free with cfg_scan_result_free(). */
cfg_scan_result_t cfg_scan_json_dir(const char *dir);
