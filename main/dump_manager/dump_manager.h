#pragma once

/* -----------------------------------------------------------------------
 * Crash coredump -> SD card export.
 *
 * ESP-IDF's espcoredump component (see sdkconfig: ESP_COREDUMP_ENABLE_TO_
 * FLASH) writes a coredump straight to the "coredump" flash partition from
 * the panic handler itself when the device crashes -- that part needs no
 * app code at all, it happens before anything of ours could still be
 * running. This module is the other half: once at boot, check whether a
 * valid coredump is sitting in that partition and, if so, copy it out to
 * the SD card (SD_PATH_DUMP, see app_config.h) and erase the flash copy.
 *
 * Split into two calls instead of one because esp_core_dump_image_check()
 * itself was observed to crash (Guru Meditation StoreProhibited) when
 * called later in app_main(), after waveshare_esp32_s3_rgb_lcd_init() has
 * armed the LVGL tick esp_timer and other interrupt sources -- but not
 * when the equivalent check ESP-IDF itself runs automatically pre-
 * app_main() (CONFIG_ESP_COREDUMP_CHECK_BOOT, now disabled here to avoid
 * calling esp_core_dump_image_check() a second, redundant time). Splitting
 * lets the one risky flash-checksum read happen as early as possible --
 * before any timer/interrupt of ours exists -- while the SD-card copy
 * (which needs fs_sd_init() first) still happens later.
 *
 * dump_manager_check()  -- call FIRST, as the very first thing in
 *                           app_main() (before heap_caps_malloc_extmem_
 *                           enable()/waveshare_esp32_s3_rgb_lcd_init()).
 *                           No SD access, just flags whether a valid dump
 *                           exists in flash. Cheap when there's nothing to
 *                           do (a few bytes of flash header read).
 * dump_manager_export() -- call SECOND, after fs_sd_init() and before
 *                           anything performance-sensitive like boot_anim_
 *                           play(). No-op if dump_manager_check() found
 *                           nothing.
 * ----------------------------------------------------------------------- */
void dump_manager_check(void);
void dump_manager_export(void);
