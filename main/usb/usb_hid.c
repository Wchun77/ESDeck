#include "usb_hid.h"
#include "esp_log.h"
#include <string.h>
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "ui_toast.h"

static const char *TAG = "USB_HID";

#define TUSB_DESC_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static bool s_active = false;

/* Forward decl -- defined near the bottom alongside the toast hookup it
 * exists for, but tusb_cfg (below) needs to reference it by name. */
static void usb_hid_event_cb(tinyusb_event_t *event, void *arg);

/*
 * HID report descriptor — Vendor-defined (Usage Page 0xFF00).
 *
 * IN      report (ESP -> PC): 8 bytes [page, btn, rsv x6]
 * Feature report (PC -> ESP): 8 bytes [cmd, d0..d6]
 *
 * PC sends data via SetReport (Control transfer, Feature type).
 */
static const uint8_t hid_report_descriptor[] = {
    0x06, 0x00, 0xFF,
    0x09, 0x01,
    0xA1, 0x01,

    /* IN: 8 bytes */
    0x09, 0x02,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x08,
    0x81, 0x02,

    /* Feature: 64 bytes */
    0x09, 0x03,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x40,   /* Report Count = 64 */
    0xB1, 0x02,

    0xC0
};

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_DESC_TOTAL_LEN, 0, 100),
    TUD_HID_DESCRIPTOR(0, 0, false, sizeof(hid_report_descriptor), 0x81, 16, 10),
};

static const tusb_desc_device_t s_device_desc = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0,
    .bDeviceSubClass    = 0,
    .bDeviceProtocol    = 0,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x303A,
    .idProduct          = 0x4004,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

static const char *s_string_desc[] = {
    "\x09\x04",  // 0: Language (English)
    "Hank",      // 1: Manufacturer
    "ESDeck",    // 2: Product name  ← 改這裡
    "000001",    // 3: Serial number
};

/* -----------------------------------------------------------------------
 * Callbacks registered by upper layers
 * ----------------------------------------------------------------------- */

/* Called when PC sends CMD_DATA */
static void (*s_monitor_data_cb)(uint8_t cpu_usage, uint8_t cpu_temp,
                                 uint8_t ram_usage, uint8_t gpu_usage,
                                 uint8_t gpu_temp,  uint8_t gpu_vram,
                                 uint8_t cpu_freq,  uint8_t net_up,
                                 uint8_t net_down,  uint8_t disk_usage,
                                 uint8_t cpu_power, uint8_t gpu_power,
                                 uint8_t ssd_life) = NULL;

/* Called when PC sends CMD_TIME */
static void (*s_time_cb)(uint16_t year, uint8_t month, uint8_t day,
                         uint8_t hour, uint8_t min, uint8_t sec) = NULL;

/* Called when PC sends CMD_QUERY; returns true if currently in monitor mode */
static uint8_t (*s_mode_query_cb)(void) = NULL;

/* Called when PC sends CMD_NOWPLAYING_PROGRESS */
static void (*s_nowplaying_cb)(uint32_t position_ms, uint32_t duration_ms,
                               bool playing) = NULL;

/* Called when PC sends CMD_AUDIO_LEVEL */
static void (*s_audio_level_cb)(uint8_t level) = NULL;

/* Called when a cover/info image finishes reassembly + decode */
static void (*s_nowplaying_img_cb)(uint8_t kind, uint8_t *rgb565_data,
                                   uint16_t w, uint16_t h) = NULL;

/* -----------------------------------------------------------------------
 * Cover / info image reassembly (CMD_NOWPLAYING_IMG_START/CHUNK/END)
 *
 * PC sends raw JPEG bytes chunked across many Feature reports on a single
 * control pipe (blocking SetFeature calls, one at a time from PC's own
 * worker thread), so ordering is guaranteed -- chunks are just appended
 * sequentially, no chunk index needed. generation_id guards against a
 * stray leftover chunk/END from a superseded transfer landing on a new
 * one (PC bumps it whenever it restarts a transfer for that kind, e.g. on
 * track change).
 *
 * One staging slot per kind (HID_MEDIA_IMG_KIND_COUNT) so a cover update
 * and an info-strip update can be in flight independently. Raw JPEG bytes
 * live in a PSRAM staging buffer sized for the worst case either kind
 * needs; on END, decoded via esp_new_jpeg straight into a fresh PSRAM
 * RGB565 buffer sized exactly for that kind and handed off to
 * s_nowplaying_img_cb (ownership transfers -- the callback frees it).
 * ----------------------------------------------------------------------- */
#define HID_IMG_MAX_JPEG_SIZE (60 * 1024)

typedef struct {
    bool     active;        /* true from START until END/reset */
    uint8_t  generation;
    uint8_t *buf;            /* PSRAM staging buffer, lazily allocated */
    uint32_t total_size;     /* announced in START */
    uint32_t received;
} img_recv_state_t;

static img_recv_state_t  s_img_recv[HID_MEDIA_IMG_KIND_COUNT];
static jpeg_dec_handle_t s_img_jpeg_dec = NULL;

static bool img_kind_dims(uint8_t kind, uint16_t *w, uint16_t *h)
{
    switch (kind) {
        case HID_MEDIA_IMG_KIND_COVER: *w = HID_MEDIA_IMG_COVER_W; *h = HID_MEDIA_IMG_COVER_H; return true;
        case HID_MEDIA_IMG_KIND_INFO:  *w = HID_MEDIA_IMG_INFO_W;  *h = HID_MEDIA_IMG_INFO_H;  return true;
        default: return false;
    }
}

static bool img_jpeg_dec_setup(void)
{
    if (s_img_jpeg_dec) return true;

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;

    if (jpeg_dec_open(&cfg, &s_img_jpeg_dec) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "img jpeg_dec_open failed");
        s_img_jpeg_dec = NULL;
        return false;
    }
    return true;
}

static void img_recv_reset(uint8_t kind)
{
    s_img_recv[kind].active   = false;
    s_img_recv[kind].received = 0;
}

static void img_recv_start(uint8_t generation, uint8_t kind, uint32_t total_size)
{
    if (kind >= HID_MEDIA_IMG_KIND_COUNT) return;

    if (total_size == 0) {
        /* Sentinel, not an error: PC has no image for this kind right now
         * (e.g. NowPlayingWatcher lost focus/session) -- cancel whatever
         * transfer might be in flight and tell the callback to clear
         * whatever's currently displayed, rather than starting a
         * transfer that will never get chunks. */
        img_recv_reset(kind);
        if (s_nowplaying_img_cb) s_nowplaying_img_cb(kind, NULL, 0, 0);
        return;
    }
    if (total_size > HID_IMG_MAX_JPEG_SIZE) {
        ESP_LOGW(TAG, "img start: bad total_size %lu for kind %u", (unsigned long)total_size, kind);
        return;
    }

    img_recv_state_t *st = &s_img_recv[kind];
    if (!st->buf) {
        st->buf = heap_caps_malloc(HID_IMG_MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM);
        if (!st->buf) {
            ESP_LOGW(TAG, "img start: PSRAM alloc failed for kind %u", kind);
            return;
        }
    }

    st->active     = true;
    st->generation = generation;
    st->total_size = total_size;
    st->received   = 0;
}

static void img_recv_chunk(uint8_t generation, uint8_t kind,
                            const uint8_t *data, uint16_t data_len)
{
    if (kind >= HID_MEDIA_IMG_KIND_COUNT) return;
    img_recv_state_t *st = &s_img_recv[kind];
    if (!st->active || !st->buf || st->generation != generation) return;   /* stale/unknown transfer */

    /* The Feature report is a fixed 64-byte payload per the HID descriptor
     * (no variable-length reports), so data_len is always
     * HID_FEATURE_PAYLOAD_SIZE-3 here regardless of how many bytes of the
     * *last* chunk are real JPEG data vs. the PC's zero-padding tail --
     * bufsize on this side can't tell them apart. total_size (announced
     * in START) is the authority on how many bytes actually belong to
     * this transfer, so clamp to whatever's still needed instead of
     * trusting data_len -- copying the last chunk's padding tail in
     * would both corrupt the JPEG and spuriously overflow past
     * total_size. */
    if (st->received >= st->total_size) return;   /* nothing left to take, ignore trailing padding chunks */

    uint32_t remaining = st->total_size - st->received;
    uint32_t take = ((uint32_t)data_len < remaining) ? data_len : remaining;

    memcpy(st->buf + st->received, data, take);
    st->received += take;
}

/* COVER (photographic thumbnail) decodes fine as JPEG. INFO (flat-color
 * background + sharp text edges) showed visible JPEG blocking artifacts
 * around the glyphs -- lossy chroma subsampling doesn't handle hard edges
 * well -- so INFO is sent as PNG instead (lossless, and this content
 * compresses to almost nothing as PNG anyway). Two different decoders,
 * same downstream contract: fill a fresh PSRAM RGB565 buffer sized
 * exactly want_w*want_h*2 and return it via *out_buf. */
static bool decode_jpeg_to_rgb565(const uint8_t *data, size_t len,
                                   uint16_t want_w, uint16_t want_h, uint8_t **out_buf)
{
    if (!img_jpeg_dec_setup()) return false;

    jpeg_dec_io_t io = { 0 };
    io.inbuf     = (uint8_t *)data;
    io.inbuf_len = (int)len;

    jpeg_dec_header_info_t info = { 0 };
    if (jpeg_dec_parse_header(s_img_jpeg_dec, &io, &info) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "img end: JPEG header parse failed");
        return false;
    }
    if (info.width != want_w || info.height != want_h) {
        ESP_LOGW(TAG, "img end: JPEG size mismatch %ux%u (expect %ux%u)",
                 info.width, info.height, want_w, want_h);
        return false;
    }

    uint8_t *rgb_buf = heap_caps_aligned_alloc(16, (size_t)want_w * want_h * 2, MALLOC_CAP_SPIRAM);
    if (!rgb_buf) {
        ESP_LOGW(TAG, "img end: PSRAM alloc failed for JPEG decode output");
        return false;
    }
    io.outbuf = rgb_buf;

    if (jpeg_dec_process(s_img_jpeg_dec, &io) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "img end: JPEG decode failed");
        heap_caps_free(rgb_buf);
        return false;
    }

    *out_buf = rgb_buf;
    return true;
}

/* Decodes via LVGL's own registered decoders (LV_USE_PNG=y pulls in
 * lodepng) instead of a standalone lib like esp_new_jpeg above -- there's
 * no equivalent bare PNG decoder component already vendored in this
 * project, and LVGL's decoder abstraction already knows how to sniff a
 * raw in-memory buffer via LV_IMG_SRC_VARIABLE. Unlike esp_new_jpeg (a
 * standalone lib untouched by LVGL's own state), lv_img_decoder_* touches
 * LVGL's internal image cache, which the rendering thread also touches --
 * wrapped in lvgl_port_lock() since this runs on the TinyUSB task, not
 * the LVGL thread.
 *
 * LVGL's bundled lodepng-based decoder (lv_png.c) always decodes to
 * native color depth *plus an explicit per-pixel alpha byte*
 * (LV_IMG_CF_TRUE_COLOR_ALPHA layout -- 3 bytes/px at this project's 16-bit
 * color depth: RGB565 LE + alpha), which is exactly what we want here and
 * is kept as-is (not flattened to opaque RGB565) -- the whole point of
 * sending INFO as a PNG with a real alpha channel is so the text has a
 * *transparent* background on the ESP side (composited by LVGL at
 * display time), not a solid rectangle that would look wrong once Media
 * gets a real background image behind it. header.cf on the way in has to
 * be left as LV_IMG_CF_UNKNOWN (the zero-init default): lv_png.c's
 * decoder_info only fills in TRUE_COLOR_ALPHA when the caller's cf is
 * falsy, otherwise it just echoes back whatever cf you passed in
 * unchanged. Caller must use LV_IMG_CF_TRUE_COLOR_ALPHA (not
 * TRUE_COLOR) and w*h*3 bytes when building the lv_img_dsc_t for
 * whatever this hands back -- see ui_media.c's apply_info_image(). */
static bool decode_png_to_rgb565(const uint8_t *data, size_t len,
                                  uint16_t want_w, uint16_t want_h, uint8_t **out_buf)
{
    if (!lvgl_port_lock(pdMS_TO_TICKS(1000))) {
        ESP_LOGW(TAG, "img end: lvgl lock timeout for PNG decode");
        return false;
    }

    lv_img_dsc_t raw = { 0 };
    raw.data_size = (uint32_t)len;
    raw.data      = data;

    lv_img_decoder_dsc_t dec;
    memset(&dec, 0, sizeof(dec));
    if (lv_img_decoder_open(&dec, &raw, lv_color_white(), 0) != LV_RES_OK) {
        ESP_LOGW(TAG, "img end: PNG decode open failed");
        lvgl_port_unlock();
        return false;
    }

    uint32_t w = dec.header.w;
    uint32_t h = dec.header.h;

    if (w != want_w || h != want_h || dec.header.cf != LV_IMG_CF_TRUE_COLOR_ALPHA) {
        ESP_LOGW(TAG, "img end: PNG format mismatch %lux%lu cf=%d (expect %ux%u)",
                 (unsigned long)w, (unsigned long)h, dec.header.cf, want_w, want_h);
        lv_img_decoder_close(&dec);
        lvgl_port_unlock();
        return false;
    }

    size_t stride = (size_t)w * 3;   /* RGB565 LE + alpha byte per px, kept in full */
    uint8_t *buf  = heap_caps_aligned_alloc(16, stride * h, MALLOC_CAP_SPIRAM);
    if (!buf) {
        ESP_LOGW(TAG, "img end: PSRAM alloc failed for PNG decode output");
        lv_img_decoder_close(&dec);
        lvgl_port_unlock();
        return false;
    }

    bool ok = true;
    if (dec.img_data) {
        memcpy(buf, dec.img_data, stride * h);
    } else {
        for (lv_coord_t y = 0; y < (lv_coord_t)h && ok; y++) {
            if (lv_img_decoder_read_line(&dec, 0, y, (lv_coord_t)w,
                                          buf + (size_t)y * stride) != LV_RES_OK) {
                ESP_LOGW(TAG, "img end: PNG read_line failed at row %d", y);
                ok = false;
            }
        }
    }

    lv_img_decoder_close(&dec);
    lvgl_port_unlock();

    if (!ok) {
        heap_caps_free(buf);
        return false;
    }

    *out_buf = buf;
    return true;
}

static void img_recv_end(uint8_t generation, uint8_t kind)
{
    if (kind >= HID_MEDIA_IMG_KIND_COUNT) return;
    img_recv_state_t *st = &s_img_recv[kind];
    if (!st->active || !st->buf || st->generation != generation) return;

    if (st->received != st->total_size) {
        ESP_LOGW(TAG, "img end: size mismatch for kind %u (%lu/%lu), discarding",
                 kind, (unsigned long)st->received, (unsigned long)st->total_size);
        img_recv_reset(kind);
        return;
    }

    uint16_t want_w, want_h;
    img_kind_dims(kind, &want_w, &want_h);

    uint8_t *rgb_buf = NULL;
    bool ok = (kind == HID_MEDIA_IMG_KIND_INFO)
              ? decode_png_to_rgb565(st->buf, st->received, want_w, want_h, &rgb_buf)
              : decode_jpeg_to_rgb565(st->buf, st->received, want_w, want_h, &rgb_buf);

    img_recv_reset(kind);

    if (!ok) return;

    if (s_nowplaying_img_cb) {
        s_nowplaying_img_cb(kind, rgb_buf, want_w, want_h);
    } else {
        heap_caps_free(rgb_buf);
    }
}

void usb_hid_set_monitor_cb(void (*cb)(uint8_t, uint8_t, uint8_t, uint8_t,
                                       uint8_t, uint8_t, uint8_t, uint8_t,
                                       uint8_t, uint8_t, uint8_t, uint8_t,
                                       uint8_t))
{
    s_monitor_data_cb = cb;
}

void usb_hid_set_time_cb(void (*cb)(uint16_t, uint8_t, uint8_t,
                                    uint8_t, uint8_t, uint8_t))
{
    s_time_cb = cb;
}

void usb_hid_set_mode_query_cb(uint8_t (*cb)(void))
{
    s_mode_query_cb = cb;
}

void usb_hid_set_nowplaying_cb(void (*cb)(uint32_t, uint32_t, bool))
{
    s_nowplaying_cb = cb;
}

void usb_hid_set_audio_level_cb(void (*cb)(uint8_t))
{
    s_audio_level_cb = cb;
}

void usb_hid_set_nowplaying_img_cb(void (*cb)(uint8_t, uint8_t *, uint16_t, uint16_t))
{
    s_nowplaying_img_cb = cb;
}

/* -----------------------------------------------------------------------
 * Driver install / activate / deactivate
 * ----------------------------------------------------------------------- */
void usb_hid_driver_install(void)
{
    const tinyusb_config_t tusb_cfg = {
        .port = TINYUSB_PORT_FULL_SPEED_0,
        .phy = {
            .skip_setup      = false,
            .self_powered    = false,
            .vbus_monitor_io = -1,
        },
        .task = {
            .size     = 4096,
            .priority = 5,
            .xCoreID  = 0,
        },
        .descriptor = {
            .device            = &s_device_desc,
            .qualifier         = NULL,
            .string            = s_string_desc,
            .string_count      = sizeof(s_string_desc) / sizeof(s_string_desc[0]),
            .full_speed_config = s_config_desc,
            .high_speed_config = NULL,
        },
        .event_cb  = usb_hid_event_cb,
        .event_arg = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    /* On reset-while-USB-connected, the ROM bootloader briefly puts the
     * USB Serial/JTAG peripheral on the bus (same GPIO19/20 as OTG).
     * When TinyUSB switches the mux, Windows sees a glitch rather than a
     * clean disconnect → reconnect, leaving the HID driver in a bad state.
     * Force an explicit disconnect/reconnect so Windows re-enumerates cleanly. */
    tud_disconnect();
    vTaskDelay(pdMS_TO_TICKS(500));
    tud_connect();

    s_active = true;
    ESP_LOGI(TAG, "HID driver installed");
}

void usb_hid_activate(void)   { s_active = true;  ESP_LOGI(TAG, "HID activated");   }
void usb_hid_deactivate(void) { s_active = false; ESP_LOGI(TAG, "HID deactivated"); }

/* -----------------------------------------------------------------------
 * TinyUSB attach/detach event -- via the esp_tinyusb wrapper's event_cb,
 * not raw tud_mount_cb/tud_umount_cb.
 *
 * managed_components/espressif__esp_tinyusb/tinyusb.c already defines
 * tud_mount_cb()/tud_umount_cb() itself (it forwards them to whatever
 * event_cb is set in tinyusb_config_t) -- redefining those two here too
 * would be (and was) a duplicate-symbol link error. The wrapper's own
 * event_cb hook is the intended extension point instead; wired below via
 * tusb_cfg.event_cb.
 *
 * Right now this is purely a stand-in "device connected/disconnected"
 * signal to exercise ui_toast's queue/animation/dismiss behavior
 * end-to-end (plug/unplug the USB cable) before real BLE notifications
 * exist -- see ui_toast.h. Once BLE lands, its own connect/disconnect
 * events push through the same ui_toast_push() call.
 * ----------------------------------------------------------------------- */
static void toast_hid_connected_cb(void *arg)    { (void)arg; ui_toast_push("HID Connected", 1, NULL); }
static void toast_hid_disconnected_cb(void *arg) { (void)arg; ui_toast_push("HID Disconnected", 1, NULL); }

static void usb_hid_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    /* Called from the TinyUSB task, not the LVGL task -- hop over via
     * lv_async_call() same as every other cross-task UI call in this
     * codebase (see ui_settings.c's enter_monitor_task pattern). */
    switch (event->id) {
    case TINYUSB_EVENT_ATTACHED:
        lv_async_call(toast_hid_connected_cb, NULL);
        break;
    case TINYUSB_EVENT_DETACHED:
        lv_async_call(toast_hid_disconnected_cb, NULL);
        break;
    default:
        break;
    }
}

const tusb_desc_device_t *usb_hid_get_device_desc(void) { return &s_device_desc; }
const uint8_t            *usb_hid_get_config_desc(void) { return s_config_desc;  }

/* -----------------------------------------------------------------------
 * IN report helpers
 * ----------------------------------------------------------------------- */
void usb_hid_send(uint8_t page, uint8_t btn)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[8] = { page, btn, 0, 0, 0, 0, 0, 0 };
    tud_hid_report(0, report, sizeof(report));
}

void usb_hid_release(void)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[8] = { 0xFF, 0xFF, 0, 0, 0, 0, 0, 0 };
    tud_hid_report(0, report, sizeof(report));
}

void usb_hid_monitor_subscribe(void)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[8] = { HID_MON_PAGE_CTRL, HID_MON_BTN_SUBSCRIBE, 0, 0, 0, 0, 0, 0 };
    tud_hid_report(0, report, sizeof(report));
    ESP_LOGI(TAG, "monitor subscribe sent");
}

void usb_hid_monitor_unsubscribe(void)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[8] = { HID_MON_PAGE_CTRL, HID_MON_BTN_UNSUBSCRIBE, 0, 0, 0, 0, 0, 0 };
    tud_hid_report(0, report, sizeof(report));
    ESP_LOGI(TAG, "monitor unsubscribe sent");
}

void usb_hid_reply_mode(uint8_t mode)
{
    if (!s_active || !tud_hid_ready()) return;

    uint8_t btn;
    const char *name;
    switch (mode) {
        case 1:  btn = HID_MON_BTN_MODE_MONITOR; name = "monitor"; break;
        case 2:  btn = HID_MON_BTN_MODE_MEDIA;   name = "media";   break;
        default: btn = HID_MON_BTN_MODE_DECK;    name = "deck";    break;
    }

    uint8_t report[8] = { HID_MON_PAGE_CTRL, btn, 0, 0, 0, 0, 0, 0 };
    tud_hid_report(0, report, sizeof(report));
    ESP_LOGI(TAG, "mode reply: %s", name);
}

void usb_hid_media_subscribe(void)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[8] = { HID_MEDIA_PAGE_CTRL, HID_MEDIA_BTN_SUBSCRIBE, 0, 0, 0, 0, 0, 0 };
    tud_hid_report(0, report, sizeof(report));
    ESP_LOGI(TAG, "media subscribe sent");
}

void usb_hid_media_unsubscribe(void)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[8] = { HID_MEDIA_PAGE_CTRL, HID_MEDIA_BTN_UNSUBSCRIBE, 0, 0, 0, 0, 0, 0 };
    tud_hid_report(0, report, sizeof(report));
    ESP_LOGI(TAG, "media unsubscribe sent");
}

void usb_hid_media_play_pause(void)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[8] = { HID_MEDIA_PAGE_CTRL, HID_MEDIA_BTN_PLAY_PAUSE, 0, 0, 0, 0, 0, 0 };
    tud_hid_report(0, report, sizeof(report));
    ESP_LOGI(TAG, "media play/pause sent");
}

void usb_hid_media_next(void)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[8] = { HID_MEDIA_PAGE_CTRL, HID_MEDIA_BTN_NEXT, 0, 0, 0, 0, 0, 0 };
    tud_hid_report(0, report, sizeof(report));
    ESP_LOGI(TAG, "media next sent");
}

void usb_hid_media_prev(void)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[8] = { HID_MEDIA_PAGE_CTRL, HID_MEDIA_BTN_PREV, 0, 0, 0, 0, 0, 0 };
    tud_hid_report(0, report, sizeof(report));
    ESP_LOGI(TAG, "media prev sent");
}

void usb_hid_media_seek(uint32_t position_ms)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[8] = {
        HID_MEDIA_PAGE_CTRL, HID_MEDIA_BTN_SEEK,
        (uint8_t)(position_ms & 0xFF),
        (uint8_t)((position_ms >> 8) & 0xFF),
        (uint8_t)((position_ms >> 16) & 0xFF),
        (uint8_t)((position_ms >> 24) & 0xFF),
        0, 0
    };
    tud_hid_report(0, report, sizeof(report));
    ESP_LOGI(TAG, "media seek sent: %lu ms", (unsigned long)position_ms);
}

/* -----------------------------------------------------------------------
 * TinyUSB HID callbacks
 * ----------------------------------------------------------------------- */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return hid_report_descriptor;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize)
{
    if (report_type != HID_REPORT_TYPE_FEATURE) return;
    if (bufsize < 1) return;

    uint8_t cmd = buffer[0];

    if (cmd == HID_MON_CMD_DATA && bufsize >= 15) {
        if (s_monitor_data_cb)
            s_monitor_data_cb(buffer[1],  buffer[2],  buffer[3],  buffer[4],
                              buffer[5],  buffer[6],  buffer[7],  buffer[8],
                              buffer[9],  buffer[10], buffer[11], buffer[12],
                              buffer[13]);
    }
    else if (cmd == HID_MON_CMD_TIME && bufsize >= 7) {
        /* Raw field pass-through only -- no calendar math here (lag
         * compensation, drift correction, wday derivation all live in
         * sys_clock.c, the only place that owns the running clock). */
        if (s_time_cb)
            s_time_cb((uint16_t)(buffer[1] + 2000), buffer[2], buffer[3],
                      buffer[4], buffer[5], buffer[6]);
    }
    else if (cmd == HID_MON_CMD_QUERY) {
        uint8_t mode = s_mode_query_cb ? s_mode_query_cb() : 0;
        usb_hid_reply_mode(mode);
    }
    else if (cmd == HID_MEDIA_CMD_NOWPLAYING_PROGRESS && bufsize >= 10) {
        if (s_nowplaying_cb) {
            uint32_t position_ms = (uint32_t)buffer[1]
                                  | ((uint32_t)buffer[2] << 8)
                                  | ((uint32_t)buffer[3] << 16)
                                  | ((uint32_t)buffer[4] << 24);
            uint32_t duration_ms = (uint32_t)buffer[5]
                                  | ((uint32_t)buffer[6] << 8)
                                  | ((uint32_t)buffer[7] << 16)
                                  | ((uint32_t)buffer[8] << 24);
            bool playing = buffer[9] != 0;
            s_nowplaying_cb(position_ms, duration_ms, playing);
        }
    }
    else if (cmd == HID_MEDIA_CMD_AUDIO_LEVEL && bufsize >= 2) {
        if (s_audio_level_cb)
            s_audio_level_cb(buffer[1]);
    }
    else if (cmd == HID_MEDIA_CMD_NOWPLAYING_IMG_START && bufsize >= 5) {
        uint8_t  generation = buffer[1];
        uint8_t  kind       = buffer[2];
        uint32_t total_size = (uint32_t)buffer[3] | ((uint32_t)buffer[4] << 8);
        img_recv_start(generation, kind, total_size);
    }
    else if (cmd == HID_MEDIA_CMD_NOWPLAYING_IMG_CHUNK && bufsize >= 3) {
        uint8_t generation = buffer[1];
        uint8_t kind       = buffer[2];
        img_recv_chunk(generation, kind, buffer + 3, bufsize - 3);
    }
    else if (cmd == HID_MEDIA_CMD_NOWPLAYING_IMG_END && bufsize >= 3) {
        uint8_t generation = buffer[1];
        uint8_t kind       = buffer[2];
        img_recv_end(generation, kind);
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen)
{
    if (report_type == HID_REPORT_TYPE_FEATURE ||
        report_type == HID_REPORT_TYPE_INPUT) {
        uint16_t len = reqlen < 8 ? reqlen : 8;
        memset(buffer, 0, len);
        return len;
    }
    return 0;
}