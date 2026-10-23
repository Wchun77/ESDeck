#include "ui_canvas.h"
#include "ui.h"
#include "ui_settings.h"
#include "usb/usb_hid.h"
#include "lvgl_port.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "CANVAS";

/* -----------------------------------------------------------------------
 * Layout / limits
 * ----------------------------------------------------------------------- */
#define CANVAS_X          SIDEBAR_W
#define CANVAS_W          (SCREEN_W - SIDEBAR_W)   /* 720 */
#define CANVAS_H          SCREEN_H                 /* 480 */
#define CANVAS_FB_BYTES   ((size_t)CANVAS_W * CANVAS_H * 2)

#define ICON_W            64
#define ICON_H            64

#define REC_MAX_LEN       (512 * 1024)   /* one record's encoded data */
#define ITEM_Q_LEN        16
#define EV_Q_LEN          32
#define RX_PUSH_WAIT_MS   500            /* TinyUSB task waits this long for the worker (backpressure) */

#define HEARTBEAT_MS      6000           /* no Feature report from the PC for this long = ESDeckPC gone */
#define COVER_MAX_MS      6000           /* "Loading..." never stays longer than this */
#define TOUCH_MOVE_MS     30             /* touch move reports at most this often */

#define COL_BG            0x000000
#define COL_TEXT_DIM      0x666666
#define COL_BTN           0x2a2a2a
#define COL_SEL           0x0055cc

/* -----------------------------------------------------------------------
 * Work items: TinyUSB task (rx_cb) -> worker task
 * ----------------------------------------------------------------------- */
typedef enum { ITEM_BEGIN, ITEM_REC, ITEM_END } item_kind_t;

typedef struct {
    item_kind_t kind;
    uint8_t     flags, page, count;    /* BEGIN */
    uint8_t     type, enc;             /* REC */
    uint16_t    x, y, w, h;            /* REC (x = slot for icons) */
    uint32_t    len, got;              /* REC */
    uint8_t    *data;                  /* REC, PSRAM, owned by the item */
} canvas_item_t;

/* -----------------------------------------------------------------------
 * State
 *
 * Locking: LVGL objects -- LVGL task, or the worker holding the LVGL lock.
 * Frame / icon buffers -- s_buf_mtx. Whoever needs both takes the LVGL lock
 * FIRST (exit runs on the LVGL task holding it, then takes s_buf_mtx; the
 * worker never waits for the LVGL lock while holding s_buf_mtx).
 * ----------------------------------------------------------------------- */
static volatile bool     s_active      = false;   /* between enter and exit */
static QueueHandle_t     s_item_q      = NULL;
static QueueHandle_t     s_ev_q        = NULL;    /* 8-byte IN reports waiting to go out */
static SemaphoreHandle_t s_buf_mtx     = NULL;
static TaskHandle_t      s_worker      = NULL;

/* frame buffers */
static uint8_t     *s_fb[2]       = { NULL, NULL };
static lv_img_dsc_t s_fb_dsc[2];
static int          s_front       = 0;

/* sidebar icons from the PC */
static uint8_t     *s_icon_buf[CANVAS_MAX_PAGES];
static lv_img_dsc_t s_icon_dsc[CANVAS_MAX_PAGES];
static bool         s_icon_ok[CANVAS_MAX_PAGES];

/* batch being processed (worker) */
static bool    s_batch_open  = false;
static uint8_t s_batch_flags = 0;
static uint8_t s_batch_page  = 0;
static uint8_t s_batch_count = 0;

/* rx assembly (TinyUSB task only) */
static uint8_t       s_rx_gen     = 0;
static bool          s_rx_batch   = false;
static canvas_item_t s_rx_rec;
static bool          s_rx_rec_on  = false;

/* page state (LVGL side) */
static int      s_page_count  = 0;
static int      s_sel         = 0;      /* kept across visits */
static bool     s_has_content = false;
static volatile bool s_entering   = false;   /* switching screen up, no local cover */
static volatile bool s_first_done = false;   /* first full / empty batch finished */
static uint32_t s_cover_tick  = 0;

/* LVGL objects */
static lv_obj_t   *s_page       = NULL;   /* content container */
static lv_obj_t   *s_img        = NULL;
static lv_obj_t   *s_empty_lbl  = NULL;
static lv_obj_t   *s_cover      = NULL;
static lv_obj_t   *s_sb_cont    = NULL;   /* sidebar page buttons */
static lv_obj_t   *s_sb_btn[CANVAS_MAX_PAGES];
static lv_timer_t *s_ev_timer   = NULL;
static lv_timer_t *s_hb_timer   = NULL;

/* touch */
static bool     s_touch_down  = false;
static uint32_t s_touch_tick  = 0;
static int16_t  s_touch_x = 0, s_touch_y = 0;

static jpeg_dec_handle_t s_jpeg = NULL;   /* worker only */

static void show_empty(const char *text);
static void rebuild_sidebar(void);

/* -----------------------------------------------------------------------
 * IN reports (LVGL timer flushes s_ev_q; producers: LVGL task, TinyUSB task)
 * ----------------------------------------------------------------------- */
static void ev_push(uint8_t ev, uint8_t a, uint8_t b, uint16_t x, uint16_t y)
{
    if (!s_ev_q) return;
    uint8_t r[8] = { HID_CANVAS_PAGE, ev, a, b, 0, 0, 0, 0 };
    if (ev == HID_CANVAS_EV_TOUCH) {
        r[2] = a;
        r[3] = (uint8_t)x; r[4] = (uint8_t)(x >> 8);
        r[5] = (uint8_t)y; r[6] = (uint8_t)(y >> 8);
    }
    if (xQueueSend(s_ev_q, r, 0) != pdTRUE)
        ESP_LOGW(TAG, "event queue full, dropped ev %u", ev);
}

static void request_page(int page, uint8_t reason)
{
    ev_push(HID_CANVAS_EV_REQUEST, (uint8_t)page, reason, 0, 0);
}

static void ev_timer_cb(lv_timer_t *t)
{
    (void)t;
    uint8_t r[8];
    /* A few per tick; the vendor endpoint takes one report per 1 ms poll. */
    for (int i = 0; i < 4; i++) {
        if (xQueuePeek(s_ev_q, r, 0) != pdTRUE) return;
        if (!usb_hid_canvas_send(r, sizeof(r))) return;   /* busy / not connected: retry next tick */
        xQueueReceive(s_ev_q, r, 0);
    }
}

/* TinyUSB task: the PC asked our mode and we're in Canvas -- it just
 * (re)connected, ask it for the current page (with icons). */
static void sync_cb(void)
{
    if (!s_active) return;
    request_page(s_sel, HID_CANVAS_REQ_SYNC);
}

/* -----------------------------------------------------------------------
 * Receive (TinyUSB task) -- reassemble records, hand them to the worker
 * ----------------------------------------------------------------------- */
static void item_free(canvas_item_t *it)
{
    if (it->data) heap_caps_free(it->data);
    it->data = NULL;
}

static void item_push(canvas_item_t *it)
{
    if (xQueueSend(s_item_q, it, pdMS_TO_TICKS(RX_PUSH_WAIT_MS)) != pdTRUE) {
        ESP_LOGW(TAG, "worker busy, dropped item %d", it->kind);
        item_free(it);
    }
}

static void rx_drop_rec(void)
{
    if (s_rx_rec_on) item_free(&s_rx_rec);
    s_rx_rec_on = false;
}

static void rx_cb(const uint8_t *buf, uint16_t len)
{
    if (!s_active || !s_item_q) { rx_drop_rec(); return; }
    if (len < 2) return;

    uint8_t cmd = buf[0];
    uint8_t gen = buf[1];

    switch (cmd) {
    case HID_CANVAS_CMD_BEGIN: {
        if (len < 5) return;
        rx_drop_rec();
        s_rx_gen   = gen;
        s_rx_batch = true;
        canvas_item_t it = { .kind = ITEM_BEGIN, .flags = buf[2], .page = buf[3], .count = buf[4] };
        item_push(&it);
        break;
    }
    case HID_CANVAS_CMD_REC: {
        if (!s_rx_batch || gen != s_rx_gen || len < 16) return;
        rx_drop_rec();
        canvas_item_t it = {
            .kind = ITEM_REC, .type = buf[2], .enc = buf[3],
            .x = (uint16_t)(buf[4] | (buf[5] << 8)),  .y = (uint16_t)(buf[6] | (buf[7] << 8)),
            .w = (uint16_t)(buf[8] | (buf[9] << 8)),  .h = (uint16_t)(buf[10] | (buf[11] << 8)),
            .len = (uint32_t)buf[12] | ((uint32_t)buf[13] << 8) |
                   ((uint32_t)buf[14] << 16) | ((uint32_t)buf[15] << 24),
        };
        if (it.len == 0 || it.len > REC_MAX_LEN) {
            ESP_LOGW(TAG, "record: bad length %lu", (unsigned long)it.len);
            return;
        }
        it.data = heap_caps_malloc(it.len, MALLOC_CAP_SPIRAM);
        if (!it.data) {
            ESP_LOGW(TAG, "record: PSRAM alloc failed (%lu)", (unsigned long)it.len);
            return;
        }
        s_rx_rec    = it;
        s_rx_rec_on = true;
        break;
    }
    case HID_CANVAS_CMD_DATA: {
        if (!s_rx_rec_on || gen != s_rx_gen) return;
        uint32_t room = s_rx_rec.len - s_rx_rec.got;
        uint32_t n    = (uint32_t)(len - 2) < room ? (uint32_t)(len - 2) : room;
        memcpy(s_rx_rec.data + s_rx_rec.got, buf + 2, n);
        s_rx_rec.got += n;
        if (s_rx_rec.got == s_rx_rec.len) {
            canvas_item_t it = s_rx_rec;
            s_rx_rec_on = false;       /* ownership of data moves to the item */
            item_push(&it);
        }
        break;
    }
    case HID_CANVAS_CMD_END: {
        if (!s_rx_batch || gen != s_rx_gen) return;
        rx_drop_rec();                 /* an unfinished record is just dropped */
        s_rx_batch = false;
        canvas_item_t it = { .kind = ITEM_END };
        item_push(&it);
        break;
    }
    default:
        break;
    }
}

/* -----------------------------------------------------------------------
 * Decoding (worker)
 * ----------------------------------------------------------------------- */
static bool rle_decode(const uint8_t *src, uint32_t len, uint16_t *dst, uint32_t npix)
{
    uint32_t i = 0, o = 0;
    while (i < len && o < npix) {
        uint8_t  n   = src[i++];
        uint32_t cnt = (uint32_t)(n & 0x7F) + 1;
        if (o + cnt > npix) return false;
        if (n & 0x80) {
            if (i + 2 > len) return false;
            uint16_t c = (uint16_t)(src[i] | (src[i + 1] << 8));
            i += 2;
            for (uint32_t k = 0; k < cnt; k++) dst[o++] = c;
        } else {
            if (i + cnt * 2 > len) return false;
            memcpy(&dst[o], &src[i], cnt * 2);
            i += cnt * 2;
            o += cnt;
        }
    }
    return o == npix;
}

/* Decode one record's pixels into a fresh w*h RGB565 buffer (PSRAM,
 * 16-byte aligned for the JPEG decoder). NULL on any error. */
static uint8_t *decode_pixels(const canvas_item_t *it)
{
    uint32_t npix = (uint32_t)it->w * it->h;
    if (npix == 0) return NULL;

    if (it->enc == HID_CANVAS_ENC_JPEG) {
        if (!s_jpeg) {
            jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
            cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
            if (jpeg_dec_open(&cfg, &s_jpeg) != JPEG_ERR_OK) {
                ESP_LOGW(TAG, "jpeg_dec_open failed");
                s_jpeg = NULL;
                return NULL;
            }
        }
        jpeg_dec_io_t io = { .inbuf = it->data, .inbuf_len = (int)it->len };
        jpeg_dec_header_info_t info = { 0 };
        if (jpeg_dec_parse_header(s_jpeg, &io, &info) != JPEG_ERR_OK) {
            ESP_LOGW(TAG, "JPEG header parse failed");
            return NULL;
        }
        if (info.width != it->w || info.height != it->h) {
            ESP_LOGW(TAG, "JPEG is %ux%u, record says %ux%u", info.width, info.height, it->w, it->h);
            return NULL;
        }
        int out_len = 0;
        if (jpeg_dec_get_outbuf_len(s_jpeg, &out_len) != JPEG_ERR_OK || out_len < (int)(npix * 2))
            out_len = (int)(npix * 2);
        uint8_t *out = heap_caps_aligned_alloc(16, (size_t)out_len, MALLOC_CAP_SPIRAM);
        if (!out) return NULL;
        io.outbuf = out;
        if (jpeg_dec_process(s_jpeg, &io) != JPEG_ERR_OK) {
            ESP_LOGW(TAG, "JPEG decode failed");
            heap_caps_free(out);
            return NULL;
        }
        return out;
    }

    uint8_t *out = heap_caps_aligned_alloc(16, npix * 2, MALLOC_CAP_SPIRAM);
    if (!out) return NULL;
    bool ok = false;
    if (it->enc == HID_CANVAS_ENC_RLE) {
        ok = rle_decode(it->data, it->len, (uint16_t *)out, npix);
    } else if (it->enc == HID_CANVAS_ENC_RAW) {
        ok = it->len >= npix * 2;
        if (ok) memcpy(out, it->data, npix * 2);
    }
    if (!ok) {
        ESP_LOGW(TAG, "record decode failed (enc %u, %ux%u, %lu bytes)",
                 it->enc, it->w, it->h, (unsigned long)it->len);
        heap_caps_free(out);
        return NULL;
    }
    return out;
}

static void blit(uint8_t *fb, const uint8_t *px, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    for (uint16_t r = 0; r < h; r++)
        memcpy(fb + ((size_t)(y + r) * CANVAS_W + x) * 2, px + (size_t)r * w * 2, (size_t)w * 2);
}

/* -----------------------------------------------------------------------
 * Worker
 * ----------------------------------------------------------------------- */
static void handle_rect(canvas_item_t *it)
{
    if (it->x + it->w > CANVAS_W || it->y + it->h > CANVAS_H) {
        ESP_LOGW(TAG, "rect %u,%u %ux%u out of bounds", it->x, it->y, it->w, it->h);
        return;
    }
    uint8_t *px = decode_pixels(it);
    if (!px) return;

    if (s_batch_flags & HID_CANVAS_F_FULL) {
        /* Hidden buffer: nobody draws from it, no LVGL lock needed. */
        xSemaphoreTake(s_buf_mtx, portMAX_DELAY);
        if (s_active && s_fb[1 - s_front]) blit(s_fb[1 - s_front], px, it->x, it->y, it->w, it->h);
        xSemaphoreGive(s_buf_mtx);
    } else {
        /* Visible buffer: under the LVGL lock so a half-copied rect is
         * never rendered, then redraw just that area. */
        if (lvgl_port_lock(-1)) {
            xSemaphoreTake(s_buf_mtx, portMAX_DELAY);
            if (s_active && s_fb[s_front] && s_img) {
                blit(s_fb[s_front], px, it->x, it->y, it->w, it->h);
                lv_area_t a = { CANVAS_X + it->x, it->y,
                                CANVAS_X + it->x + it->w - 1, it->y + it->h - 1 };
                lv_obj_invalidate_area(s_img, &a);
            }
            xSemaphoreGive(s_buf_mtx);
            lvgl_port_unlock();
        }
    }
    heap_caps_free(px);
}

static void handle_icon(canvas_item_t *it)
{
    uint16_t slot = it->x;
    if (slot >= CANVAS_MAX_PAGES || it->w > ICON_W || it->h > ICON_H) return;
    uint8_t *px = decode_pixels(it);
    if (!px) return;

    xSemaphoreTake(s_buf_mtx, portMAX_DELAY);
    if (s_active && s_icon_buf[slot]) {
        memcpy(s_icon_buf[slot], px, (size_t)it->w * it->h * 2);
        s_icon_dsc[slot].header.w = it->w;
        s_icon_dsc[slot].header.h = it->h;
        s_icon_dsc[slot].data_size = (uint32_t)it->w * it->h * 2;
        s_icon_ok[slot] = true;
    }
    xSemaphoreGive(s_buf_mtx);
    heap_caps_free(px);
}

static void handle_end(void)
{
    if (!lvgl_port_lock(-1)) return;
    xSemaphoreTake(s_buf_mtx, portMAX_DELAY);

    if (s_active && s_page) {
        if (s_batch_flags & HID_CANVAS_F_EMPTY) {
            s_page_count  = 0;
            s_has_content = false;
            rebuild_sidebar();
            show_empty("No pages\nSet up Canvas in ESDeckPC");
        } else if (s_batch_flags & HID_CANVAS_F_FULL) {
            s_front = 1 - s_front;
            lv_img_set_src(s_img, &s_fb_dsc[s_front]);
            lv_obj_clear_flag(s_img, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
            s_has_content = true;
            if (s_batch_flags & HID_CANVAS_F_ICONS) {
                s_page_count = s_batch_count > CANVAS_MAX_PAGES ? CANVAS_MAX_PAGES : s_batch_count;
                s_sel        = s_batch_page;
                rebuild_sidebar();
            }
        }
        lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
    }

    xSemaphoreGive(s_buf_mtx);
    lvgl_port_unlock();

    if (s_batch_flags & (HID_CANVAS_F_FULL | HID_CANVAS_F_EMPTY)) s_first_done = true;
}

static void worker_task(void *arg)
{
    (void)arg;
    canvas_item_t it;
    for (;;) {
        if (xQueueReceive(s_item_q, &it, portMAX_DELAY) != pdTRUE) continue;
        if (!s_active) { item_free(&it); continue; }

        switch (it.kind) {
        case ITEM_BEGIN:
            s_batch_open  = true;
            s_batch_flags = it.flags;
            s_batch_page  = it.page;
            s_batch_count = it.count;
            if (it.flags & HID_CANVAS_F_ICONS) {
                xSemaphoreTake(s_buf_mtx, portMAX_DELAY);
                memset(s_icon_ok, 0, sizeof(s_icon_ok));
                xSemaphoreGive(s_buf_mtx);
            }
            /* Nothing on screen yet (PC just connected): cover until the
             * page is complete. Not while entering -- the switching screen
             * is already up. */
            if ((it.flags & HID_CANVAS_F_FULL) && !s_has_content && !s_entering && lvgl_port_lock(-1)) {
                if (s_active && s_cover) {
                    lv_obj_clear_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
                    s_cover_tick = lv_tick_get();
                }
                lvgl_port_unlock();
            }
            break;

        case ITEM_REC:
            if (s_batch_open) {
                if (it.type == HID_CANVAS_REC_RECT)      handle_rect(&it);
                else if (it.type == HID_CANVAS_REC_ICON) handle_icon(&it);
            }
            item_free(&it);
            break;

        case ITEM_END:
            if (s_batch_open) handle_end();
            s_batch_open = false;
            break;
        }
    }
}

/* -----------------------------------------------------------------------
 * LVGL side
 * ----------------------------------------------------------------------- */
static void show_empty(const char *text)
{
    if (!s_page) return;
    lv_label_set_text(s_empty_lbl, text);
    lv_obj_clear_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_HIDDEN);
}

/* PC gone: back to "waiting", no page buttons. */
static void go_empty(const char *text)
{
    s_has_content = false;
    s_page_count  = 0;
    rebuild_sidebar();
    show_empty(text);
    lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
}

static void highlight(int idx)
{
    for (int i = 0; i < CANVAS_MAX_PAGES; i++) {
        if (!s_sb_btn[i]) continue;
        lv_obj_set_style_outline_width(s_sb_btn[i], i == idx ? 3 : 0, 0);
    }
}

static bool settings_shown(void)
{
    return s_page && lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN);
}

static void sb_btn_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    ui_settings_deselect();               /* picking a page leaves Settings */
    if (s_page) lv_obj_clear_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    if (idx < 0 || idx >= s_page_count) return;
    highlight(idx);
    if (idx == s_sel) return;
    s_sel = idx;
    request_page(idx, HID_CANVAS_REQ_PAGE);   /* old page stays until the new one is complete */
}

/* Caller holds the LVGL lock (or is the LVGL task) and s_buf_mtx if the
 * worker may be running -- the icon descriptors are read here. */
static void rebuild_sidebar(void)
{
    if (!s_sb_cont) return;
    lv_obj_clean(s_sb_cont);
    memset(s_sb_btn, 0, sizeof(s_sb_btn));

    for (int i = 0; i < s_page_count; i++) {
        lv_obj_t *btn = lv_btn_create(s_sb_cont);
        lv_obj_set_size(btn, ICON_W, ICON_H);
        lv_obj_set_style_bg_color(btn, lv_color_hex(COL_BTN), 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_set_style_clip_corner(btn, true, 0);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_set_style_outline_color(btn, lv_color_hex(COL_SEL), 0);
        lv_obj_set_style_outline_width(btn, 0, 0);
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_add_event_cb(btn, sb_btn_cb, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        if (s_icon_ok[i]) {
            lv_obj_t *img = lv_img_create(btn);
            lv_img_cache_invalidate_src(&s_icon_dsc[i]);   /* same pointer, new pixels */
            lv_img_set_src(img, &s_icon_dsc[i]);
            lv_obj_center(img);
        } else {
            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text_fmt(lbl, "%d", i + 1);
            lv_obj_center(lbl);
        }
        s_sb_btn[i] = btn;
    }
    if (!settings_shown()) highlight(s_sel);
}

static void touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev || !s_has_content) return;

    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int16_t x = (int16_t)(p.x - CANVAS_X), y = (int16_t)p.y;
    if (x < 0) x = 0;
    if (x >= CANVAS_W) x = CANVAS_W - 1;
    if (y < 0) y = 0;
    if (y >= CANVAS_H) y = CANVAS_H - 1;

    if (code == LV_EVENT_PRESSED) {
        s_touch_down = true;
        s_touch_tick = lv_tick_get();
        s_touch_x = x; s_touch_y = y;
        ev_push(HID_CANVAS_EV_TOUCH, HID_CANVAS_TOUCH_DOWN, 0, (uint16_t)x, (uint16_t)y);
    } else if (code == LV_EVENT_PRESSING) {
        if (!s_touch_down) return;
        if (x == s_touch_x && y == s_touch_y) return;
        if (lv_tick_elaps(s_touch_tick) < TOUCH_MOVE_MS) return;
        s_touch_tick = lv_tick_get();
        s_touch_x = x; s_touch_y = y;
        ev_push(HID_CANVAS_EV_TOUCH, HID_CANVAS_TOUCH_MOVE, 0, (uint16_t)x, (uint16_t)y);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        if (!s_touch_down) return;
        s_touch_down = false;
        ev_push(HID_CANVAS_EV_TOUCH, HID_CANVAS_TOUCH_UP, 0, (uint16_t)s_touch_x, (uint16_t)s_touch_y);
    }
}

/* 500 ms: ESDeckPC still there? Loading cover stuck? */
static void hb_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_has_content && lv_tick_elaps(usb_hid_last_pc_rx_tick()) > HEARTBEAT_MS) {
        ESP_LOGI(TAG, "no data from ESDeckPC for %d ms, clearing", HEARTBEAT_MS);
        go_empty("Waiting for ESDeckPC...");
    }
    if (s_cover && !lv_obj_has_flag(s_cover, LV_OBJ_FLAG_HIDDEN) &&
        lv_tick_elaps(s_cover_tick) > COVER_MAX_MS) {
        lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);
        if (!s_has_content) show_empty("Waiting for ESDeckPC...");
    }
}

static lv_obj_t *make_plain(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_set_style_radius(o, 0, 0);
    lv_obj_set_style_pad_all(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void fb_dsc_init(lv_img_dsc_t *d, uint8_t *data, uint16_t w, uint16_t h)
{
    memset(d, 0, sizeof(*d));
    d->header.always_zero = 0;
    d->header.cf = LV_IMG_CF_TRUE_COLOR;
    d->header.w  = w;
    d->header.h  = h;
    d->data_size = (uint32_t)w * h * 2;
    d->data      = data;
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
void ui_canvas_enter(lv_obj_t *sidebar)
{
    if (!s_item_q)  s_item_q  = xQueueCreate(ITEM_Q_LEN, sizeof(canvas_item_t));
    if (!s_ev_q)    s_ev_q    = xQueueCreate(EV_Q_LEN, 8);
    if (!s_buf_mtx) s_buf_mtx = xSemaphoreCreateMutex();
    if (!s_worker)  xTaskCreate(worker_task, "canvas_wk", 6144, NULL, 4, &s_worker);
    xQueueReset(s_ev_q);

    /* Buffers -- Deck's icons etc. are already freed at this point. */
    for (int i = 0; i < 2; i++) {
        s_fb[i] = heap_caps_aligned_alloc(16, CANVAS_FB_BYTES, MALLOC_CAP_SPIRAM);
        if (s_fb[i]) memset(s_fb[i], 0, CANVAS_FB_BYTES);
        fb_dsc_init(&s_fb_dsc[i], s_fb[i], CANVAS_W, CANVAS_H);
        lv_img_cache_invalidate_src(&s_fb_dsc[i]);
    }
    bool fb_ok = s_fb[0] && s_fb[1];
    if (!fb_ok) {
        ESP_LOGE(TAG, "frame buffers: PSRAM alloc failed (free %d B)",
                 heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        for (int i = 0; i < 2; i++) { if (s_fb[i]) heap_caps_free(s_fb[i]); s_fb[i] = NULL; }
    }
    for (int i = 0; i < CANVAS_MAX_PAGES; i++) {
        s_icon_buf[i] = heap_caps_malloc(ICON_W * ICON_H * 2, MALLOC_CAP_SPIRAM);
        fb_dsc_init(&s_icon_dsc[i], s_icon_buf[i], ICON_W, ICON_H);
        s_icon_ok[i] = false;
    }
    s_front       = 0;
    s_page_count  = 0;
    s_has_content = false;
    s_first_done  = false;
    s_batch_open  = false;
    s_touch_down  = false;

    ui_settings_apply_appearance(NULL);   /* Settings: plain look in this mode */

    /* Sidebar: page buttons above the gear */
    s_sb_cont = make_plain(sidebar);
    lv_obj_set_size(s_sb_cont, SIDEBAR_W, SCREEN_H - 80);
    lv_obj_set_pos(s_sb_cont, 0, 0);
    lv_obj_set_style_bg_opa(s_sb_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_top(s_sb_cont, 8, 0);
    lv_obj_set_style_pad_row(s_sb_cont, 16, 0);
    lv_obj_set_layout(s_sb_cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_sb_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_sb_cont, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    /* Content */
    s_page = make_plain(lv_scr_act());
    lv_obj_set_size(s_page, CANVAS_W, CANVAS_H);
    lv_obj_set_pos(s_page, CANVAS_X, 0);
    lv_obj_set_style_bg_color(s_page, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);

    s_img = lv_img_create(s_page);
    lv_obj_set_pos(s_img, 0, 0);
    if (fb_ok) lv_img_set_src(s_img, &s_fb_dsc[s_front]);
    lv_obj_add_flag(s_img, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_img, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(s_img, touch_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_img, touch_cb, LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_img, touch_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_img, touch_cb, LV_EVENT_PRESS_LOST, NULL);

    s_empty_lbl = lv_label_create(s_page);
    lv_obj_set_style_text_color(s_empty_lbl, lv_color_hex(COL_TEXT_DIM), 0);
    lv_obj_set_style_text_font(s_empty_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_empty_lbl);

    s_cover = make_plain(s_page);
    lv_obj_set_size(s_cover, CANVAS_W, CANVAS_H);
    lv_obj_set_style_bg_color(s_cover, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_cover, LV_OPA_COVER, 0);
    lv_obj_t *cl = lv_label_create(s_cover);
    lv_label_set_text(cl, "Loading...");
    lv_obj_set_style_text_color(cl, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_font(cl, &lv_font_montserrat_16, 0);
    lv_obj_center(cl);
    lv_obj_add_flag(s_cover, LV_OBJ_FLAG_HIDDEN);

    show_empty(fb_ok ? "Waiting for ESDeckPC..." : "Not enough memory for Canvas mode");

    lv_obj_t *ctx = ui_get_context_panel();
    lv_obj_move_foreground(sidebar);
    if (ctx) lv_obj_move_foreground(ctx);

    s_ev_timer = lv_timer_create(ev_timer_cb, 10, NULL);
    s_hb_timer = lv_timer_create(hb_timer_cb, 500, NULL);

    if (s_sel >= CANVAS_MAX_PAGES) s_sel = 0;
    if (fb_ok) {
        s_active = true;
        usb_hid_set_canvas_rx_cb(rx_cb);
        usb_hid_set_canvas_sync_cb(sync_cb);
        request_page(s_sel, HID_CANVAS_REQ_ENTER);
    } else {
        s_first_done = true;   /* nothing will arrive: don't keep the switching screen up */
    }

    ESP_LOGI(TAG, "entered canvas mode - PSRAM free: %d B",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}

void ui_canvas_wait_first_frame(uint32_t timeout_ms)
{
    /* No USB, or ESDeckPC not running (it syncs the time every 2 s):
     * nothing is coming, don't hold the switching screen. */
    if (!usb_hid_is_connected()) return;
    if (lv_tick_elaps(usb_hid_last_pc_rx_tick()) > 3000) return;
    s_entering = true;
    uint32_t waited = 0;
    while (!s_first_done && waited < timeout_ms) {
        vTaskDelay(pdMS_TO_TICKS(20));
        waited += 20;
    }
    s_entering = false;
}

void ui_canvas_exit(void)
{
    s_active = false;
    usb_hid_set_canvas_rx_cb(NULL);
    usb_hid_set_canvas_sync_cb(NULL);

    /* Tell the PC to stop capturing. Endpoint may be busy for a poll or
     * two -- retry briefly (the PC also stops on any other mode's traffic). */
    uint8_t leave[8] = { HID_CANVAS_PAGE, HID_CANVAS_EV_LEAVE, 0, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 5 && !usb_hid_canvas_send(leave, sizeof(leave)); i++)
        vTaskDelay(pdMS_TO_TICKS(2));

    if (s_ev_timer) { lv_timer_del(s_ev_timer); s_ev_timer = NULL; }
    if (s_hb_timer) { lv_timer_del(s_hb_timer); s_hb_timer = NULL; }
    if (s_ev_q) xQueueReset(s_ev_q);

    /* Objects first (nothing may still point at the buffers), then buffers. */
    if (s_page)    { lv_obj_del(s_page);    s_page = NULL; }
    if (s_sb_cont) { lv_obj_del(s_sb_cont); s_sb_cont = NULL; }
    s_img = s_empty_lbl = s_cover = NULL;
    memset(s_sb_btn, 0, sizeof(s_sb_btn));

    xSemaphoreTake(s_buf_mtx, portMAX_DELAY);
    for (int i = 0; i < 2; i++) {
        lv_img_cache_invalidate_src(&s_fb_dsc[i]);
        if (s_fb[i]) { heap_caps_free(s_fb[i]); s_fb[i] = NULL; }
    }
    for (int i = 0; i < CANVAS_MAX_PAGES; i++) {
        lv_img_cache_invalidate_src(&s_icon_dsc[i]);
        if (s_icon_buf[i]) { heap_caps_free(s_icon_buf[i]); s_icon_buf[i] = NULL; }
        s_icon_ok[i] = false;
    }
    xSemaphoreGive(s_buf_mtx);

    /* Whatever is still queued goes nowhere (the worker also drops items
     * while inactive). */
    canvas_item_t it;
    while (s_item_q && xQueueReceive(s_item_q, &it, 0) == pdTRUE) item_free(&it);

    s_has_content = false;
    s_page_count  = 0;
    ESP_LOGI(TAG, "exited canvas mode");
}

void ui_canvas_deselect_current(void)
{
    if (!s_page) return;
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    highlight(-1);
}

void ui_canvas_reselect_current(void)
{
    if (!s_page) return;
    lv_obj_clear_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    highlight(s_sel);
}
