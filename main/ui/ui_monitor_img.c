#include "ui_monitor_img.h"
#include "ui_monitor.h"
#include "ui_monitor_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stddef.h>

#define TAG  "MON_IMG"

/* Callback into ui_monitor.c to tear down a page's bg image widget when
 * its buffer gets LRU-evicted below -- same reasoning as ui_img_pool.c's
 * extern into ui_deck.c: without this, the page would keep pointing an
 * lv_img_dsc_t at a freed buffer the next time it's shown. */
extern void ui_monitor_lazy_bg_remove_widget(int page_idx);

/* Same idea, for MON_IMG_SETTINGS_SLOT specifically -- ui_settings.c owns
 * that slot's widget (bg+mask on its own panel, not one of s_pages[]), so
 * eviction/teardown needs a different callback for it than a real page. */
extern void ui_settings_bg_widget_remove(void);

typedef struct {
    char          path[MON_IMG_PATH_LEN];
    lv_img_dsc_t  dsc;
    bool          loaded;
    uint32_t      last_used;
} mon_img_entry_t;

static mon_img_entry_t s_imgs[MON_IMG_SLOT_COUNT];

/* -----------------------------------------------------------------------
 * Decode one JPEG/PNG from LVGL FS into a PSRAM buffer.
 * Returns true on success; fills out *dsc.
 * ----------------------------------------------------------------------- */
static bool decode_to_psram(const char *path, lv_img_dsc_t *dsc)
{
    lv_img_decoder_dsc_t dec;
    memset(&dec, 0, sizeof(dec));

    if (lv_img_decoder_open(&dec, path, lv_color_white(), 0) != LV_RES_OK) {
        ESP_LOGW(TAG, "open failed: %s", path);
        return false;
    }

    uint32_t    w  = dec.header.w;
    uint32_t    h  = dec.header.h;
    lv_img_cf_t cf = dec.header.cf;
    uint8_t     px = lv_img_cf_get_px_size(cf) / 8;

    /* JPEG reports px_size == 0 -- fix up to TRUE_COLOR */
    if (px == 0) {
        cf = LV_IMG_CF_TRUE_COLOR;
        px = sizeof(lv_color_t);
    }

    size_t   sz  = (size_t)w * h * px;
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        ESP_LOGE(TAG, "PSRAM OOM %lu KB: %s", (unsigned long)(sz / 1024), path);
        lv_img_decoder_close(&dec);
        return false;
    }

    bool ok = true;
    if (dec.img_data) {
        memcpy(buf, dec.img_data, sz);
    } else {
        size_t stride = (size_t)w * px;
        for (lv_coord_t y = 0; y < (lv_coord_t)h && ok; y++) {
            if (lv_img_decoder_read_line(&dec, 0, y, (lv_coord_t)w,
                                          buf + (size_t)y * stride) != LV_RES_OK) {
                ESP_LOGE(TAG, "read_line failed row %d: %s", y, path);
                ok = false;
            }
        }
    }

    lv_img_decoder_close(&dec);

    if (!ok) {
        heap_caps_free(buf);
        return false;
    }

    dsc->header.cf          = cf;
    dsc->header.always_zero = 0;
    dsc->header.reserved    = 0;
    dsc->header.w           = w;
    dsc->header.h           = h;
    dsc->data_size          = sz;
    dsc->data               = buf;

    ESP_LOGI(TAG, "loaded %lux%lu (%lu KB): %s", (unsigned long)w, (unsigned long)h,
             (unsigned long)(sz / 1024), path);
    return true;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void ui_monitor_img_set_path(int page_idx, const char *path)
{
    if (page_idx < 0 || page_idx >= MON_IMG_SLOT_COUNT) return;

    if (!path || path[0] == '\0') {
        s_imgs[page_idx].path[0] = '\0';
    } else {
        snprintf(s_imgs[page_idx].path, MON_IMG_PATH_LEN, "%s", path);
    }
}

bool ui_monitor_img_load_one(int page_idx)
{
    if (page_idx < 0 || page_idx >= MON_IMG_SLOT_COUNT) return false;

    if (s_imgs[page_idx].loaded) {
        /* Touch last_used even on a cache hit -- otherwise a slot that's
         * visited often but only ever decoded once would look "cold" to
         * the LRU scan below and get evicted ahead of slots nobody has
         * looked at in a while. */
        s_imgs[page_idx].last_used = xTaskGetTickCount();
        return true;
    }
    if (s_imgs[page_idx].path[0] == '\0') return false;

    if (!decode_to_psram(s_imgs[page_idx].path, &s_imgs[page_idx].dsc)) {
        /* Likely PSRAM OOM -- unlike Deck's shared pool, Monitor never
         * freed anything until the whole mode was exited, so visiting
         * enough distinct pages (or Settings, which now shares this same
         * pool via MON_IMG_SETTINGS_SLOT) in one session could exhaust
         * PSRAM even with lazy loading (each slot decoded once and kept
         * forever). Evict the least-recently-used *other* loaded slot
         * and retry once. */
        int      lru_idx  = -1;
        uint32_t lru_tick = UINT32_MAX;
        for (int i = 0; i < MON_IMG_SLOT_COUNT; i++) {
            if (i != page_idx && s_imgs[i].loaded && s_imgs[i].last_used < lru_tick) {
                lru_tick = s_imgs[i].last_used;
                lru_idx  = i;
            }
        }
        if (lru_idx < 0) return false;   /* nothing left to evict */

        ESP_LOGI(TAG, "LRU evict slot %d: %s", lru_idx, s_imgs[lru_idx].path);
        if (lru_idx == MON_IMG_SETTINGS_SLOT) {
            ui_settings_bg_widget_remove();
        } else {
            ui_monitor_lazy_bg_remove_widget(lru_idx);
        }
        heap_caps_free((void *)s_imgs[lru_idx].dsc.data);
        s_imgs[lru_idx].dsc.data = NULL;
        s_imgs[lru_idx].loaded   = false;

        if (!decode_to_psram(s_imgs[page_idx].path, &s_imgs[page_idx].dsc)) {
            return false;   /* still OOM after evicting one -- give up */
        }
    }

    s_imgs[page_idx].loaded    = true;
    s_imgs[page_idx].last_used = xTaskGetTickCount();
    return true;
}

lv_img_dsc_t *ui_monitor_img_get(int page_idx)
{
    if (page_idx < 0 || page_idx >= MON_IMG_SLOT_COUNT) return NULL;
    if (!s_imgs[page_idx].loaded)                       return NULL;
    return &s_imgs[page_idx].dsc;
}

void ui_monitor_img_free_all(void)
{
    for (int i = 0; i < MON_IMG_SLOT_COUNT; i++) {
        if (s_imgs[i].loaded && s_imgs[i].dsc.data) {
            heap_caps_free((void *)s_imgs[i].dsc.data);
            s_imgs[i].dsc.data = NULL;
        }
        memset(&s_imgs[i], 0, sizeof(s_imgs[i]));
    }

    /* Settings may have been borrowing MON_IMG_SETTINGS_SLOT above --
     * that buffer is now gone, so its widget/reference must go too, or
     * it's left pointing at freed PSRAM the next time Settings opens
     * (this can't rely on ui_settings_apply_appearance()'s path-diff
     * check alone: the *next* config's bg_image filename could
     * coincidentally match the old one and skip that path). */
    ui_settings_bg_widget_remove();

    /* s_imgs[] is a static array -- &s_imgs[i].dsc is the SAME lv_img_dsc_t*
     * every time Monitor is (re)entered, only its contents change (new
     * decode, possibly different w/h/format for a different config). LVGL's
     * image cache keys decoded entries by that source pointer, not its
     * contents, so without this it can keep serving a stale cached bitmap
     * against the new header/buffer -- exactly the "freed buffer address"
     * problem ui_settings.c's back_to_deck_task() already invalidates for
     * on the Deck side; Monitor re-entry/config-reload needed the same
     * treatment. */
    lv_img_cache_invalidate_src(NULL);
}