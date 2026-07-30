#include "ui_font_cjk.h"
#include "app_config.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>

static const char *TAG = "FONT_CJK";

/* Fixed filename for this first pass -- a Settings font picker (scan
 * SD_PATH_ASSETS_FONTS_BIN_NOTIFY, let the user choose, remember it in
 * NVS) is a later step, same shape as ui_settings.c's Boot Animation
 * picker. For now, drop a pre-converted common-Hanzi .bin font on the SD
 * card with exactly this name to test.
 *
 * "S:" prefix is LVGL's registered file-driver letter for the SD card
 * (see lvgl_port.c) -- required by lv_font_load()/lv_font_free(), same
 * convention already used by ui_clock_widget.c's load_font(). */
#define CJK_FONT_FILE   "S:" SD_PATH_ASSETS_FONTS_BIN_NOTIFY "/notify.bin"

typedef enum { CJK_NOT_STARTED, CJK_LOADING, CJK_READY, CJK_FAILED } cjk_state_t;

static lv_font_t   *s_font  = NULL;
static cjk_state_t   s_state = CJK_NOT_STARTED;
/* Guards only the two variables above -- never held across the actual
 * lv_font_load() call (see ui_font_cjk_get()), so this is safe as a
 * plain critical section even though it's used from multiple tasks. */
static portMUX_TYPE  s_lock = portMUX_INITIALIZER_UNLOCKED;

const lv_font_t *ui_font_cjk_get(void)
{
    portENTER_CRITICAL(&s_lock);
    s_state = CJK_LOADING;
    portEXIT_CRITICAL(&s_lock);

    /* lv_font_load() already returns NULL cleanly if the file is missing
     * or malformed -- no separate existence check needed (and a plain
     * fopen() wouldn't understand the "S:" LVGL-virtual path anyway).
     * Deliberately outside the lock -- this is the ~13s blocking part,
     * and s_lock must never be held across it. */
    lv_font_t *f = lv_font_load(CJK_FONT_FILE);

    portENTER_CRITICAL(&s_lock);
    s_font  = f;
    s_state = f ? CJK_READY : CJK_FAILED;
    portEXIT_CRITICAL(&s_lock);

    if (!f) {
        ESP_LOGW(TAG, "no CJK font at %s -- notification text stays ASCII-only", CJK_FONT_FILE);
    } else {
        ESP_LOGI(TAG, "CJK font loaded: %s", CJK_FONT_FILE);
    }
    return f;
}

ui_font_cjk_status_t ui_font_cjk_try_get(const lv_font_t **out_font)
{
    portENTER_CRITICAL(&s_lock);
    cjk_state_t state = s_state;
    lv_font_t  *font  = s_font;
    portEXIT_CRITICAL(&s_lock);

    if (state == CJK_READY) {
        if (out_font) *out_font = font;
        return UI_FONT_CJK_READY;
    }
    if (state == CJK_FAILED) {
        return UI_FONT_CJK_UNAVAILABLE;
    }
    return UI_FONT_CJK_LOADING;   /* CJK_NOT_STARTED or CJK_LOADING */
}
