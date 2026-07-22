#include "ui_img_pool.h"
#include "app_config.h"   /* CFG_BG_LEN, SD_PATH_ASSETS_BG -- generic, not any one mode's */
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define TAG  "IMG"

/* Eviction-match accessor callbacks, defined as weak-by-convention externs
 * (no header decl) so this file never has to #include any mode's own
 * header just to check whether an evicted entry belongs to it. In
 * production all TUs are always linked; each mode's own accessors return
 * count 0 / empty paths whenever that mode isn't the one currently active,
 * so at most one of the two page loops below ever actually matches. */
extern void        ui_deck_lazy_bg_remove_widgets(int page_idx);
extern int         ui_deck_page_count(void);
extern const char *ui_deck_page_bg_image(int page_idx);

extern void        ui_monitor_lazy_bg_remove_widget(int page_idx);
extern int         ui_monitor_page_count(void);
extern const char *ui_monitor_page_bg_image(int page_idx);

/* Settings' own bg image shares this same pool while Deck or Monitor mode
 * is active (see ui_settings.c's settings_lazy_bg_set()) -- it isn't one
 * of either mode's own configured page backgrounds, so the eviction match
 * loop below needs a separate check + removal callback for it. */
extern void ui_settings_bg_widget_remove(void);
extern void ui_settings_current_bg_path(char *out, size_t out_size);

/* -----------------------------------------------------------------------
 * Pool entry
 * ----------------------------------------------------------------------- */
typedef struct {
    char         key[CFG_BG_LEN + 16];
    lv_img_dsc_t dsc;
    bool         valid;
    bool         is_bg;
    uint32_t     last_used;
} psram_img_t;

static psram_img_t *s_pool     = NULL;
static int          s_pool_cap = 0;
static int          s_pool_n   = 0;

/* -----------------------------------------------------------------------
 * Pool operations
 * ----------------------------------------------------------------------- */
lv_img_dsc_t *ui_img_pool_find(const char *path)
{
    for (int i = 0; i < s_pool_n; i++) {
        if (s_pool[i].valid && strcmp(s_pool[i].key, path) == 0) {
            s_pool[i].last_used = xTaskGetTickCount();
            return &s_pool[i].dsc;
        }
    }
    return NULL;
}

lv_img_dsc_t *ui_img_pool_decode(const char *path)
{
    lv_img_dsc_t *hit = ui_img_pool_find(path);
    if (hit) return hit;

    /* Prefer a previously evicted (invalid) slot before appending. */
    int slot = -1;
    for (int i = 0; i < s_pool_n; i++) {
        if (!s_pool[i].valid) { slot = i; break; }
    }
    if (slot < 0) {
        if (s_pool_n >= s_pool_cap) {
            ESP_LOGW(TAG, "pool full, skipping %s", path);
            return NULL;
        }
        slot = s_pool_n;
    }

    lv_img_decoder_dsc_t dec;
    memset(&dec, 0, sizeof(dec));
    if (lv_img_decoder_open(&dec, path, lv_color_white(), 0) != LV_RES_OK) {
        ESP_LOGW(TAG, "open failed: %s", path);
        return NULL;
    }

    uint32_t    w  = dec.header.w;
    uint32_t    h  = dec.header.h;
    lv_img_cf_t cf = dec.header.cf;
    uint8_t     px = lv_img_cf_get_px_size(cf) / 8;

    /* JPEG streams via read_line and reports px_size == 0.
     * Fix up to TRUE_COLOR so stride/size calculations are correct. */
    if (px == 0) {
        cf = LV_IMG_CF_TRUE_COLOR;
        px = sizeof(lv_color_t);
    }

    size_t sz  = (size_t)w * h * px;
    uint8_t *buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!buf) {
        /* LRU eviction: free the least-recently-used background image. */
        int      lru_idx  = -1;
        uint32_t lru_tick = UINT32_MAX;
        for (int i = 0; i < s_pool_n; i++) {
            if (s_pool[i].valid && s_pool[i].is_bg &&
                s_pool[i].last_used < lru_tick) {
                lru_tick = s_pool[i].last_used;
                lru_idx  = i;
            }
        }
        if (lru_idx >= 0) {
            ESP_LOGI(TAG, "LRU evict: %s", s_pool[lru_idx].key);
            bool matched = false;

            /* Is it one of Deck's own page backgrounds? */
            int deck_n = ui_deck_page_count();
            for (int p = 0; p < deck_n && !matched; p++) {
                const char *bg = ui_deck_page_bg_image(p);
                if (!bg || !bg[0]) continue;
                char chk[sizeof("S:") + sizeof(SD_PATH_ASSETS_BG) + 1 + CFG_BG_LEN];
                snprintf(chk, sizeof(chk), "S:%s/%s", SD_PATH_ASSETS_BG, bg);
                if (strcmp(chk, s_pool[lru_idx].key) == 0) {
                    ui_deck_lazy_bg_remove_widgets(p);
                    matched = true;
                }
            }

            /* Not a Deck page -- is it one of Monitor's own page
             * backgrounds instead? (Deck and Monitor are never both
             * active at once, so at most one of these two loops ever
             * actually iterates over anything -- the inactive mode's
             * accessor just returns count 0 / empty paths.) */
            if (!matched) {
                int mon_n = ui_monitor_page_count();
                for (int p = 0; p < mon_n && !matched; p++) {
                    const char *bg = ui_monitor_page_bg_image(p);
                    if (!bg || !bg[0]) continue;
                    char chk[sizeof("S:") + sizeof(SD_PATH_ASSETS_BG) + 1 + CFG_BG_LEN];
                    snprintf(chk, sizeof(chk), "S:%s/%s", SD_PATH_ASSETS_BG, bg);
                    if (strcmp(chk, s_pool[lru_idx].key) == 0) {
                        ui_monitor_lazy_bg_remove_widget(p);
                        matched = true;
                    }
                }
            }

            if (!matched) {
                /* Not one of Deck's or Monitor's own pages -- check
                 * whether it's Settings' bg instead (it shares this pool
                 * too, whichever mode is active). */
                char settings_path[sizeof("S:") + sizeof(SD_PATH_ASSETS_BG) + 1 + CFG_BG_LEN];
                ui_settings_current_bg_path(settings_path, sizeof(settings_path));
                if (settings_path[0] && strcmp(settings_path, s_pool[lru_idx].key) == 0) {
                    ui_settings_bg_widget_remove();
                }
            }
            heap_caps_free((void *)s_pool[lru_idx].dsc.data);
            s_pool[lru_idx].dsc.data = NULL;
            s_pool[lru_idx].valid    = false;
            buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
    }

    if (!buf) {
        ESP_LOGE(TAG, "PSRAM OOM %u KB: %s", (unsigned)(sz / 1024), path);
        lv_img_decoder_close(&dec);
        return NULL;
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
        return NULL;
    }

    if (slot == s_pool_n) s_pool_n++;
    psram_img_t *e = &s_pool[slot];
    snprintf(e->key, sizeof(e->key), "%s", path);
    e->dsc.header.cf          = cf;
    e->dsc.header.always_zero = 0;
    e->dsc.header.reserved    = 0;
    e->dsc.header.w           = w;
    e->dsc.header.h           = h;
    e->dsc.data_size          = sz;
    e->dsc.data               = buf;
    e->valid                  = true;
    e->is_bg                  = false;
    e->last_used              = xTaskGetTickCount();

    ESP_LOGI(TAG, "cached %s [%ux%u %u KB]",
             path, (unsigned)w, (unsigned)h, (unsigned)(sz / 1024));
    return &e->dsc;
}

void ui_img_pool_mark_bg(const char *path)
{
    for (int i = 0; i < s_pool_n; i++) {
        if (s_pool[i].valid && strcmp(s_pool[i].key, path) == 0) {
            s_pool[i].is_bg = true;
            return;
        }
    }
}

void ui_img_pool_free(void)
{
    for (int i = 0; i < s_pool_n; i++) {
        if (s_pool[i].valid && s_pool[i].dsc.data) {
            heap_caps_free((void *)s_pool[i].dsc.data);
            s_pool[i].dsc.data = NULL;
        }
    }
    free(s_pool);
    s_pool     = NULL;
    s_pool_cap = 0;
    s_pool_n   = 0;

    /* Settings may have been borrowing a slot in this pool -- that
     * buffer is now gone, so its widget/reference must go too, or it's
     * left pointing at freed PSRAM the next time Settings opens (same
     * reasoning applies whether the pool being freed was backing Deck or
     * Monitor). */
    ui_settings_bg_widget_remove();
}

void ui_img_pool_reserve(int cap)
{
    if (cap <= 0) return;
    s_pool     = calloc((size_t)cap, sizeof(psram_img_t));
    s_pool_cap = cap;
    s_pool_n   = 0;

    ESP_LOGI(TAG, "pool reserved - %d slot(s), PSRAM free: %d B",
             cap, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
