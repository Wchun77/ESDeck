#include "ui_monitor_img.h"
#include "ui_monitor.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <stddef.h>

#define TAG  "MON_IMG"

typedef struct {
    char          path[MON_IMG_PATH_LEN];
    lv_img_dsc_t  dsc;
    bool          loaded;
} mon_img_entry_t;

static mon_img_entry_t s_imgs[MON_PAGE_COUNT];

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
    if (page_idx < 0 || page_idx >= MON_PAGE_COUNT) return;

    if (!path || path[0] == '\0') {
        s_imgs[page_idx].path[0] = '\0';
    } else {
        snprintf(s_imgs[page_idx].path, MON_IMG_PATH_LEN, "%s", path);
    }
}

void ui_monitor_img_load_all(void)
{
    for (int i = 0; i < MON_PAGE_COUNT; i++) {
        if (s_imgs[i].path[0] == '\0') continue;
        if (s_imgs[i].loaded)          continue;

        if (decode_to_psram(s_imgs[i].path, &s_imgs[i].dsc)) {
            s_imgs[i].loaded = true;
        }
    }
}

lv_img_dsc_t *ui_monitor_img_get(int page_idx)
{
    if (page_idx < 0 || page_idx >= MON_PAGE_COUNT) return NULL;
    if (!s_imgs[page_idx].loaded)                   return NULL;
    return &s_imgs[page_idx].dsc;
}

void ui_monitor_img_free_all(void)
{
    for (int i = 0; i < MON_PAGE_COUNT; i++) {
        if (s_imgs[i].loaded && s_imgs[i].dsc.data) {
            heap_caps_free((void *)s_imgs[i].dsc.data);
            s_imgs[i].dsc.data = NULL;
        }
        memset(&s_imgs[i], 0, sizeof(s_imgs[i]));
    }
}