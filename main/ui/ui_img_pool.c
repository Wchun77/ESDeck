#include "ui_img_pool.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

/* Callbacks into ui_deck for LRU eviction.
 * Defined as weak so the linker won't complain if ui_deck is not present
 * during unit testing, but in production both TUs are always linked. */
extern void        ui_deck_lazy_bg_remove_widgets(int page_idx);
extern int         ui_deck_page_count(void);
extern const char *ui_deck_page_bg_image(int page_idx);

/* -----------------------------------------------------------------------
 * Pool entry
 * ----------------------------------------------------------------------- */
typedef struct {
    char         key[UI_CONFIG_BG_LEN + 16];
    lv_img_dsc_t dsc;
    bool         valid;
    bool         is_bg;
    uint32_t     last_used;
} psram_img_t;

static psram_img_t *s_pool     = NULL;
static int          s_pool_cap = 0;
static int          s_pool_n   = 0;

static volatile bool s_preload_done    = false;
static volatile bool s_preload_started = false;
static TaskHandle_t  s_preload_caller  = NULL;

static deck_cfg_t s_preload_cfg;

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
            ESP_LOGW("IMG", "pool full, skipping %s", path);
            return NULL;
        }
        slot = s_pool_n;
    }

    lv_img_decoder_dsc_t dec;
    memset(&dec, 0, sizeof(dec));
    if (lv_img_decoder_open(&dec, path, lv_color_white(), 0) != LV_RES_OK) {
        ESP_LOGW("IMG", "open failed: %s", path);
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
            ESP_LOGI("IMG", "LRU evict: %s", s_pool[lru_idx].key);
            int n = ui_deck_page_count();
            for (int p = 0; p < n; p++) {
                const char *bg = ui_deck_page_bg_image(p);
                if (!bg || !bg[0]) continue;
                char chk[UI_CONFIG_BG_LEN + 12];
                snprintf(chk, sizeof(chk), "S:%s/%s", UI_CONFIG_BG_PATH, bg);
                if (strcmp(chk, s_pool[lru_idx].key) == 0) {
                    ui_deck_lazy_bg_remove_widgets(p);
                    break;
                }
            }
            heap_caps_free((void *)s_pool[lru_idx].dsc.data);
            s_pool[lru_idx].dsc.data = NULL;
            s_pool[lru_idx].valid    = false;
            buf = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
    }

    if (!buf) {
        ESP_LOGE("IMG", "PSRAM OOM %u KB: %s", (unsigned)(sz / 1024), path);
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
                ESP_LOGE("IMG", "read_line failed row %d: %s", y, path);
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

    ESP_LOGI("IMG", "cached %s [%ux%u %u KB]",
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
}

void ui_img_pool_load(const deck_cfg_t *cfg)
{
    int cap = 0;
    for (int p = 0; p < cfg->page_count; p++) {
        if (cfg->pages[p].bg_image[0]) cap++;
        cap += cfg->pages[p].button_count;
    }
    if (cap == 0) return;

    s_pool     = calloc((size_t)cap, sizeof(psram_img_t));
    s_pool_cap = cap;

    /* Decode bg images first so they occupy predictable slots. */
    for (int p = 0; p < cfg->page_count; p++) {
        if (!cfg->pages[p].bg_image[0]) continue;
        char path[UI_CONFIG_BG_LEN + 12];
        snprintf(path, sizeof(path), "S:%s/%s",
                 UI_CONFIG_BG_PATH, cfg->pages[p].bg_image);
        FILE *f = fopen(path + 2, "r");
        if (!f) continue;
        fclose(f);
        lv_img_dsc_t *dsc = ui_img_pool_decode(path);
        if (dsc) ui_img_pool_mark_bg(path);
    }

    /* Then decode icons. */
    for (int p = 0; p < cfg->page_count; p++) {
        for (int b = 0; b < cfg->pages[p].button_count; b++) {
            if (!cfg->pages[p].buttons[b].icon[0]) continue;
            char path[UI_CONFIG_ICON_LEN + 12];
            snprintf(path, sizeof(path), "S:%s/%s",
                     UI_CONFIG_ICON_PATH, cfg->pages[p].buttons[b].icon);
            FILE *f = fopen(path + 2, "r");
            if (!f) continue;
            fclose(f);
            ui_img_pool_decode(path);
        }
    }

    ESP_LOGI("IMG", "pool loaded - %d cached, PSRAM free: %d B",
             s_pool_n, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

/* -----------------------------------------------------------------------
 * Boot preload
 * ----------------------------------------------------------------------- */
static void preload_task_fn(void *arg)
{
    ui_img_pool_load(&s_preload_cfg);
    ESP_LOGI("IMG", "preload done - %d cached, PSRAM free: %d B",
             s_pool_n, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    s_preload_done = true;
    if (s_preload_caller) xTaskNotifyGive(s_preload_caller);
    vTaskDelete(NULL);
}

void ui_preload_start(void)
{
    if (s_preload_started) return;
    s_preload_started = true;

    bool cfg_ok = ui_config_load(&s_preload_cfg);
    if (!cfg_ok || s_preload_cfg.page_count == 0) {
        s_preload_cfg.page_count = 1;
        s_preload_cfg.pages      = calloc(1, sizeof(page_cfg_t));
        snprintf(s_preload_cfg.pages[0].name, UI_CONFIG_NAME_LEN, "Main");
    }

    s_preload_caller = xTaskGetCurrentTaskHandle();
    xTaskCreate(preload_task_fn, "img_preload", 8192, NULL, 3, NULL);
}

void ui_preload_wait(void)
{
    if (s_preload_started && !s_preload_done)
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
}

deck_cfg_t *ui_img_pool_take_preload_cfg(void)
{
    return &s_preload_cfg;
}
