#include "ui_font_cjk.h"
#include "app_config.h"
#include "esp_log.h"
#include <stdbool.h>

static const char *TAG = "FONT_CJK";

/* Fixed filename for this first pass -- a Settings font picker (scan
 * SD_PATH_ASSETS_FONTS_BIN_NOTIFY, let the user choose, remember it in
 * NVS) is a later step, same shape as ui_settings.c's Boot Animation
 * picker. For now, drop a pre-converted common-Hanzi .bin font (see
 * doc/ESDeck_Monitor_字體轉換指南.md) on the SD card with exactly this
 * name to test.
 *
 * "S:" prefix is LVGL's registered file-driver letter for the SD card
 * (see lvgl_port.c) -- required by lv_font_load()/lv_font_free(), same
 * convention already used by ui_clock_widget.c's load_font(). */
#define CJK_FONT_FILE   "S:" SD_PATH_ASSETS_FONTS_BIN_NOTIFY "/notify.bin"

static lv_font_t *s_font  = NULL;
static bool       s_tried = false;

const lv_font_t *ui_font_cjk_get(void)
{
    if (s_tried) return s_font;
    s_tried = true;

    /* lv_font_load() already returns NULL cleanly if the file is missing
     * or malformed -- no separate existence check needed (and a plain
     * fopen() wouldn't understand the "S:" LVGL-virtual path anyway). */
    s_font = lv_font_load(CJK_FONT_FILE);
    if (!s_font) {
        ESP_LOGW(TAG, "no CJK font at %s -- notification text stays ASCII-only", CJK_FONT_FILE);
        return NULL;
    }

    ESP_LOGI(TAG, "CJK font loaded: %s", CJK_FONT_FILE);
    return s_font;
}
