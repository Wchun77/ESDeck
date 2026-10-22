#include "usb_hid.h"
#include "esp_log.h"
#include <string.h>
#include <stdlib.h>
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "lvgl.h"
#include "lvgl_port.h"
#include "ui_toast.h"

static const char *TAG = "USB_HID";

#define TUSB_DESC_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN * 2)

/* HID instances / interfaces:
 *   0 = ESDeck vendor HID (Deck / Monitor / Media / time sync / ESDeckPC paths)
 *   1 = standard keyboard (Report ID 1) + mouse (Report ID 2), used when
 *       the input mode is "USB HID" (usb_hid_set_native_input()) -- Windows
 *       sees a real keyboard / mouse: no ESDeckPC needed, works with
 *       elevated windows, UAC and games that reject injected input. */
#define HID_ITF_VENDOR   0
#define HID_ITF_INPUT    1
#define RID_KEYBOARD     1
#define RID_MOUSE        2

static bool s_active = false;

/* Every vendor IN report is HID_VENDOR_IN_LEN bytes on the wire (the
 * descriptor has no variable-length reports): callers build their short
 * report and this zero-pads it. Same return as tud_hid_n_report() --
 * false if the endpoint is busy. */
static bool vendor_send(const uint8_t *data, size_t len)
{
    uint8_t buf[HID_VENDOR_IN_LEN] = {0};
    memcpy(buf, data, len < sizeof(buf) ? len : sizeof(buf));
    return tud_hid_n_report(HID_ITF_VENDOR, 0, buf, sizeof(buf));
}

/* Forward decl -- defined near the bottom alongside the toast hookup it
 * exists for, but tusb_cfg (below) needs to reference it by name. */
static void usb_hid_event_cb(tinyusb_event_t *event, void *arg);

/* Settings page's live HID status indicator (see ui_settings.c). Fired
 * from the same lv_async_call() hop as the connect/disconnect toast, so
 * it's already safe to touch LVGL objects from here. */
static usb_hid_conn_cb_t s_conn_cb = NULL;

/*
 * HID report descriptor — Vendor-defined (Usage Page 0xFF00).
 *
 * IN      report (ESP -> PC): 64 bytes [page, btn/cmd, data...], zero-padded
 *                             (HID_VENDOR_IN_LEN; was 8 -- widened for
 *                             the chunked Deck action transfer, page 0xFB)
 * Feature report (PC -> ESP): 64 bytes [cmd, data...]
 *
 * PC sends data via SetReport (Control transfer, Feature type).
 */
static const uint8_t hid_report_descriptor[] = {
    0x06, 0x00, 0xFF,
    0x09, 0x01,
    0xA1, 0x01,

    /* IN: 64 bytes (HID_VENDOR_IN_LEN) */
    0x09, 0x02,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x40,   /* Report Count = 64 */
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

/*
 * Interface 1 report descriptor: boot-style keyboard (TinyUSB's standard
 * one) + a 5-button mouse with high-resolution wheel / horizontal pan.
 *
 * Hi-res scrolling uses the HID "Resolution Multiplier" (Generic Desktop
 * 0x48): Windows reads the feature report and, if it supports it, sets the
 * multiplier to max (physical 120) -- then 1 wheel count = 1/120 notch
 * (= 1 WHEEL_DELTA unit), same smooth scrolling ESDeckPC gives. If the
 * host never sets it, counts are whole notches and the flush code falls
 * back to coarse steps (see s_res_mult).
 *
 * Mouse input report (ID 2): buttons(5 bits + 3 pad) X Y wheel pan, all
 * int8 -- the layout TinyUSB's tud_hid_n_mouse_report() sends.
 * Mouse feature report (ID 2): wheel multiplier (2 bits), pan multiplier
 * (2 bits), 4 bits padding.
 */
static const uint8_t hid_input_report_descriptor[] = {
    TUD_HID_REPORT_DESC_KEYBOARD(HID_REPORT_ID(RID_KEYBOARD)),

    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x02,             /* Usage (Mouse) */
    0xA1, 0x01,             /* Collection (Application) */
    0x85, RID_MOUSE,        /*   Report ID */
    0x09, 0x01,             /*   Usage (Pointer) */
    0xA1, 0x00,             /*   Collection (Physical) */
    0x05, 0x09,             /*     Usage Page (Button) */
    0x19, 0x01, 0x29, 0x05, /*     Buttons 1..5 (L R M Back Forward) */
    0x15, 0x00, 0x25, 0x01, /*     Logical 0..1 */
    0x95, 0x05, 0x75, 0x01, /*     5 x 1 bit */
    0x81, 0x02,             /*     Input (Data,Var,Abs) */
    0x95, 0x01, 0x75, 0x03, /*     1 x 3 bits padding */
    0x81, 0x01,             /*     Input (Const) */
    0x05, 0x01,             /*     Usage Page (Generic Desktop) */
    0x09, 0x30, 0x09, 0x31, /*     X, Y */
    0x15, 0x81, 0x25, 0x7F, /*     Logical -127..127 */
    0x75, 0x08, 0x95, 0x02, /*     2 x 8 bits */
    0x81, 0x06,             /*     Input (Data,Var,Rel) */

    0xA1, 0x02,             /*     Collection (Logical) -- wheel */
    0x09, 0x48,             /*       Usage (Resolution Multiplier) */
    0x15, 0x00, 0x25, 0x01, /*       Logical 0..1 */
    0x35, 0x01, 0x45, 0x78, /*       Physical 1..120 */
    0x75, 0x02, 0x95, 0x01, /*       1 x 2 bits */
    0xB1, 0x02,             /*       Feature (Data,Var,Abs) */
    0x35, 0x00, 0x45, 0x00, /*       Physical reset */
    0x09, 0x38,             /*       Usage (Wheel) */
    0x15, 0x81, 0x25, 0x7F, /*       Logical -127..127 */
    0x75, 0x08, 0x95, 0x01, /*       1 x 8 bits */
    0x81, 0x06,             /*       Input (Data,Var,Rel) */
    0xC0,                   /*     End Collection */

    0xA1, 0x02,             /*     Collection (Logical) -- pan */
    0x09, 0x48,             /*       Usage (Resolution Multiplier) */
    0x15, 0x00, 0x25, 0x01, /*       Logical 0..1 */
    0x35, 0x01, 0x45, 0x78, /*       Physical 1..120 */
    0x75, 0x02, 0x95, 0x01, /*       1 x 2 bits */
    0xB1, 0x02,             /*       Feature (Data,Var,Abs) */
    0x75, 0x04,             /*       1 x 4 bits padding */
    0xB1, 0x01,             /*       Feature (Const) */
    0x35, 0x00, 0x45, 0x00, /*       Physical reset */
    0x05, 0x0C,             /*       Usage Page (Consumer) */
    0x0A, 0x38, 0x02,       /*       Usage (AC Pan) */
    0x15, 0x81, 0x25, 0x7F, /*       Logical -127..127 */
    0x75, 0x08, 0x95, 0x01, /*       1 x 8 bits */
    0x81, 0x06,             /*       Input (Data,Var,Rel) */
    0xC0,                   /*     End Collection */

    0xC0,                   /*   End Collection (Physical) */
    0xC0,                   /* End Collection (Application) */
};

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUSB_DESC_TOTAL_LEN, 0, 100),
    /* 64-byte packets, 1 ms poll: a Deck action (chunked, page 0xFB) of a
     * few hundred bytes goes out in a few ms. */
    TUD_HID_DESCRIPTOR(HID_ITF_VENDOR, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(hid_report_descriptor), 0x81, HID_VENDOR_IN_LEN, 1),
    /* 1 ms poll for the pointer -- real mice use 1..8 ms */
    TUD_HID_DESCRIPTOR(HID_ITF_INPUT, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(hid_input_report_descriptor), 0x82, 16, 1),
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
    /* 0x4005 (was 0x4004): the interface layout changed (vendor HID +
     * keyboard/mouse), and Windows caches descriptors per VID/PID --
     * a new PID makes it enumerate the composite device cleanly. ESDeckPC
     * accepts both. */
    .idProduct          = 0x4005,
    /* 0x0101: vendor IN report widened 8 -> 64 bytes; bumped so Windows
     * doesn't reuse a cached descriptor set. */
    .bcdDevice          = 0x0101,
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
 * Connection state -- self-tracked, NOT read from tud_mounted().
 *
 * tud_mounted() (managed_components/espressif__tinyusb/src/device/usbd.c)
 * actually just checks cfg_num != 0, which is only cleared on
 * DCD_EVENT_UNPLUGGED -- and on this port (DWC2, dcd_dwc2.c) that event
 * fires solely on a GOTGINT_SEDET "VBUS session end" interrupt, i.e. a
 * real physical unplug. usb_hid_force_disconnect()'s tud_disconnect()
 * only toggles the D+/D- pull resistors (dcd_disconnect() -- confirmed a
 * real, working implementation, not a stub); VBUS from the host cable
 * never drops, so no DCD_EVENT_UNPLUGGED is ever raised and tud_mounted()
 * stays stuck true forever after a self-initiated soft disconnect. There
 * is no TinyUSB callback for a software-initiated soft-disconnect on this
 * port -- so the state has to be tracked here instead, updated from two
 * places: the real attach/detach event below (actual cable plug/unplug),
 * and directly inside usb_hid_force_disconnect()/force_connect() (since
 * those are self-inflicted and no corresponding hardware event will ever
 * follow to confirm them).
 * ----------------------------------------------------------------------- */
static bool s_conn_state = false;

static void set_conn_state(bool connected)
{
    s_conn_state = connected;
    if (s_conn_cb) s_conn_cb(connected);
}

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
 * ----------------------------------------------------------------------- */
static void toast_hid_connected_cb(void *arg)
{
    (void)arg;
    ui_toast_push("HID Connected", 1, NULL, NULL);
    set_conn_state(true);
}
static void toast_hid_disconnected_cb(void *arg)
{
    (void)arg;
    ui_toast_push("HID Disconnected", 1, NULL, NULL);
    set_conn_state(false);
}

static void usb_hid_event_cb(tinyusb_event_t *event, void *arg)
{
    (void)arg;
    if (!s_active) return;
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

void usb_hid_set_conn_cb(usb_hid_conn_cb_t cb) { s_conn_cb = cb; }

bool usb_hid_is_connected(void) { return s_conn_state; }

/* Soft USB disconnect / reconnect (toggles the pull-up) -- exposed as two
 * separate one-shot actions rather than a single "reconnect" that does
 * both, so the Settings page's HID Status row can be a real toggle: tap
 * while Connected -> disconnect only (and stay disconnected -- this also
 * makes the PC side see it drop, e.g. to manually test that path), tap
 * again while Disconnected -> connect only. Both are plain TinyUSB calls,
 * safe to call directly from the LVGL task (unlike the boot-time pair in
 * usb_hid_driver_install(), neither blocks).
 *
 * Asymmetric on purpose:
 * - Disconnect: set_conn_state(false) is called directly, immediately.
 *   No real hardware event will ever confirm this on this port (see the
 *   comment above s_conn_state) -- it's genuinely already true the moment
 *   dcd_disconnect() asserts the soft-disconnect bit, so there's nothing
 *   to wait for.
 * - Connect: state is NOT flipped here. Unlike disconnect, mount
 *   detection isn't broken -- tud_mount_cb() (-> TINYUSB_EVENT_ATTACHED
 *   -> toast_hid_connected_cb() -> set_conn_state(true) above) fires on
 *   genuine SET_CONFIGURATION success, independent of the VBUS-only
 *   DCD_EVENT_UNPLUGGED issue, so it's left to actually confirm the host
 *   finished re-enumerating (typically 1-3s later) instead of claiming
 *   Connected before that's true. */
void usb_hid_force_disconnect(void) { tud_disconnect(); set_conn_state(false); }
void usb_hid_force_connect(void)    { tud_connect(); }

const tusb_desc_device_t *usb_hid_get_device_desc(void) { return &s_device_desc; }
const uint8_t            *usb_hid_get_config_desc(void) { return s_config_desc;  }

/* -----------------------------------------------------------------------
 * IN report helpers
 * ----------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
 * Deck action transfer (page HID_ACTION_PAGE, see usb_hid.h). LVGL thread
 * only: usb_hid_send_action() queues a copy, act_timer_cb() sends one
 * chunk per tick while anything is queued (the endpoint takes one packet
 * per 1 ms poll anyway), then deletes its timer.
 * ----------------------------------------------------------------------- */
#define ACT_QUEUE_LEN   4
#define ACT_HDR_LEN     6
#define ACT_CHUNK_LEN   (HID_VENDOR_IN_LEN - ACT_HDR_LEN)

typedef struct {
    uint8_t *data;
    uint16_t len;
    uint8_t  msg_id;
} act_msg_t;

static act_msg_t   s_act_q[ACT_QUEUE_LEN];
static uint8_t     s_act_head    = 0;
static uint8_t     s_act_count   = 0;
static uint16_t    s_act_off     = 0;    /* bytes of the head message already sent */
static uint8_t     s_act_seq     = 0;
static uint8_t     s_act_next_id = 0;
static lv_timer_t *s_act_timer   = NULL;

static void act_pop(void)
{
    free(s_act_q[s_act_head].data);
    s_act_q[s_act_head].data = NULL;
    s_act_head = (s_act_head + 1) % ACT_QUEUE_LEN;
    s_act_count--;
    s_act_off = 0;
    s_act_seq = 0;
}

static void act_timer_cb(lv_timer_t *t)
{
    (void)t;
    if (!s_active) {
        /* Unplugged / HID off: whatever is queued can't go anywhere. */
        while (s_act_count) act_pop();
    }
    if (s_act_count && tud_hid_n_ready(HID_ITF_VENDOR)) {
        const act_msg_t *msg = &s_act_q[s_act_head];
        uint16_t n = msg->len - s_act_off;
        if (n > ACT_CHUNK_LEN) n = ACT_CHUNK_LEN;

        uint8_t flags = 0;
        if (s_act_off == 0)            flags |= HID_ACTION_FLAG_FIRST;
        if (s_act_off + n >= msg->len) flags |= HID_ACTION_FLAG_LAST;

        uint8_t r[HID_VENDOR_IN_LEN] = {
            HID_ACTION_PAGE, msg->msg_id, s_act_seq, flags,
            (uint8_t)(msg->len & 0xFF), (uint8_t)(msg->len >> 8),
        };
        memcpy(r + ACT_HDR_LEN, msg->data + s_act_off, n);

        if (vendor_send(r, sizeof(r))) {
            s_act_off += n;
            s_act_seq++;
            if (s_act_off >= msg->len) act_pop();
        }
    }
    if (s_act_count == 0 && s_act_timer) {
        lv_timer_del(s_act_timer);
        s_act_timer = NULL;
    }
}

bool usb_hid_send_action(const char *data, size_t len)
{
    if (!s_active || !data || len == 0) return false;
    if (len > HID_ACTION_MAX_LEN) {
        ESP_LOGW(TAG, "action too long (%u B), not sent", (unsigned)len);
        return false;
    }
    if (s_act_count >= ACT_QUEUE_LEN) {
        ESP_LOGW(TAG, "action queue full, dropped");
        return false;
    }

    uint8_t *copy = heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!copy) copy = malloc(len);
    if (!copy) return false;
    memcpy(copy, data, len);

    act_msg_t *msg = &s_act_q[(s_act_head + s_act_count) % ACT_QUEUE_LEN];
    msg->data   = copy;
    msg->len    = (uint16_t)len;
    msg->msg_id = s_act_next_id++;
    s_act_count++;

    if (!s_act_timer) s_act_timer = lv_timer_create(act_timer_cb, 1, NULL);
    act_timer_cb(s_act_timer);   /* first chunk right away if the endpoint is free */
    return true;
}

/* -----------------------------------------------------------------------
 * Firmware update over HID (HID_OTA_*). Chunks arrive in the TinyUSB task
 * (tud_hid_set_report_cb) and go into a stream buffer; the worker started
 * by the UI (ui_ota_dialog.c) drains it with usb_hid_ota_read(). Flow
 * control is the PC's job: it never has more than HID_OTA_WINDOW bytes
 * beyond the last ACK in flight, and the buffer holds twice that, so the
 * USB side never has to wait (it can't block the TinyUSB task).
 * ----------------------------------------------------------------------- */
#define OTA_SB_SIZE      (HID_OTA_WINDOW * 2)
#define OTA_ACK_STEP     4096
#define OTA_READ_TMO_MS  5000

static usb_hid_ota_begin_cb_t s_ota_begin_cb = NULL;
static StreamBufferHandle_t   s_ota_sb       = NULL;
static StaticStreamBuffer_t   s_ota_sb_ctl;
static uint8_t               *s_ota_sb_mem   = NULL;
static volatile bool          s_ota_active   = false;
static volatile uint8_t       s_ota_err      = 0;     /* set by the USB side, read by the worker */
static uint32_t               s_ota_total    = 0;
static uint32_t               s_ota_rx       = 0;     /* USB side */
static uint16_t               s_ota_seq      = 0;     /* USB side: next expected chunk */
static uint32_t               s_ota_used     = 0;     /* worker side */
static uint32_t               s_ota_acked    = 0;     /* worker side */

void usb_hid_set_ota_begin_cb(usb_hid_ota_begin_cb_t cb) { s_ota_begin_cb = cb; }

/* Status IN report. tries > 1 (worker task only) retries for a while if
 * the endpoint is busy -- a lost ACK would leave the PC waiting. From the
 * TinyUSB task (BEGIN errors) use 1: waiting there would stall the very
 * task that completes the transfer; the PC times out instead. */
static void ota_status_n(uint8_t st, uint32_t val, int tries)
{
    uint8_t r[6] = { HID_OTA_PAGE, st,
                     (uint8_t)val, (uint8_t)(val >> 8), (uint8_t)(val >> 16), (uint8_t)(val >> 24) };
    for (int i = 0; i < tries; i++) {
        if (s_active && vendor_send(r, sizeof(r))) return;
        if (i + 1 < tries) vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGW(TAG, "OTA status 0x%02X not sent", st);
}

static void ota_status(uint8_t st, uint32_t val) { ota_status_n(st, val, 100); }

static void ota_begin(uint32_t size)
{
    if (s_ota_active) { ota_status_n(HID_OTA_ST_ERROR, HID_OTA_ERR_BUSY, 1); return; }
    if (size == 0 || size > 0x400000) { ota_status_n(HID_OTA_ST_ERROR, HID_OTA_ERR_SIZE, 1); return; }
    if (!s_ota_begin_cb) { ota_status_n(HID_OTA_ST_ERROR, HID_OTA_ERR_NO_UI, 1); return; }

    if (!s_ota_sb) {
        s_ota_sb_mem = heap_caps_malloc(OTA_SB_SIZE + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_ota_sb_mem) { ota_status_n(HID_OTA_ST_ERROR, HID_OTA_ERR_FLASH, 1); return; }
        s_ota_sb = xStreamBufferCreateStatic(OTA_SB_SIZE, 1, s_ota_sb_mem, &s_ota_sb_ctl);
    }
    xStreamBufferReset(s_ota_sb);

    s_ota_total = size;
    s_ota_rx = s_ota_used = s_ota_acked = 0;
    s_ota_seq = 0;
    s_ota_err = 0;
    s_ota_active = true;
    ESP_LOGI(TAG, "OTA from PC: %lu bytes", (unsigned long)size);
    s_ota_begin_cb(size);
}

static void ota_data(const uint8_t *buf, uint16_t len)
{
    if (!s_ota_active || s_ota_err || len < 3) return;
    uint16_t seq = (uint16_t)(buf[0] | (buf[1] << 8));
    if (seq != s_ota_seq) { s_ota_err = HID_OTA_ERR_SEQ; return; }
    s_ota_seq++;

    uint32_t n = len - 2;
    if (n > HID_OTA_CHUNK) n = HID_OTA_CHUNK;
    if (n > s_ota_total - s_ota_rx) n = s_ota_total - s_ota_rx;   /* drop the last chunk's padding */
    if (n == 0) return;
    if (xStreamBufferSend(s_ota_sb, buf + 2, n, 0) != n) { s_ota_err = HID_OTA_ERR_OVERFLOW; return; }
    s_ota_rx += n;
}

bool usb_hid_ota_active(void) { return s_ota_active; }

void usb_hid_ota_erased(void)
{
    ota_status(HID_OTA_ST_ERASED, 0);
}

int usb_hid_ota_read(uint8_t *buf, size_t max)
{
    if (!s_ota_active) return -1;
    if (s_ota_used >= s_ota_total) return 0;

    size_t want = s_ota_total - s_ota_used;
    if (want > max) want = max;

    /* Wait for data in short slices so an error flagged by the USB side
     * (or an ABORT) is noticed quickly; give up after OTA_READ_TMO_MS of
     * silence (PC gone). */
    size_t n = 0;
    for (int waited = 0; n == 0; waited += 100) {
        if (s_ota_err) return -1;
        if (waited >= OTA_READ_TMO_MS) { ESP_LOGE(TAG, "OTA: no data from PC"); return -1; }
        n = xStreamBufferReceive(s_ota_sb, buf, want, pdMS_TO_TICKS(100));
    }

    s_ota_used += n;
    if (s_ota_used - s_ota_acked >= OTA_ACK_STEP || s_ota_used == s_ota_total) {
        s_ota_acked = s_ota_used;
        ota_status(HID_OTA_ST_ACK, s_ota_used);
    }
    return (int)n;
}

void usb_hid_ota_finish(bool ok)
{
    uint8_t err = s_ota_err ? s_ota_err : HID_OTA_ERR_FLASH;
    ota_status(ok ? HID_OTA_ST_DONE : HID_OTA_ST_ERROR, ok ? 0 : err);
    s_ota_active = false;
}

void usb_hid_monitor_subscribe(void)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[8] = { HID_MON_PAGE_CTRL, HID_MON_BTN_SUBSCRIBE, 0, 0, 0, 0, 0, 0 };
    vendor_send(report, sizeof(report));
    ESP_LOGI(TAG, "monitor subscribe sent");
}

void usb_hid_monitor_unsubscribe(void)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[8] = { HID_MON_PAGE_CTRL, HID_MON_BTN_UNSUBSCRIBE, 0, 0, 0, 0, 0, 0 };
    vendor_send(report, sizeof(report));
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
        case 3:  btn = HID_MON_BTN_MODE_TOUCHPAD; name = "touchpad"; break;
        default: btn = HID_MON_BTN_MODE_DECK;    name = "deck";    break;
    }

    uint8_t report[8] = { HID_MON_PAGE_CTRL, btn, 0, 0, 0, 0, 0, 0 };
    vendor_send(report, sizeof(report));
    ESP_LOGI(TAG, "mode reply: %s", name);
}

void usb_hid_media_subscribe(void)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[8] = { HID_MEDIA_PAGE_CTRL, HID_MEDIA_BTN_SUBSCRIBE, 0, 0, 0, 0, 0, 0 };
    vendor_send(report, sizeof(report));
    ESP_LOGI(TAG, "media subscribe sent");
}

void usb_hid_media_unsubscribe(void)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[8] = { HID_MEDIA_PAGE_CTRL, HID_MEDIA_BTN_UNSUBSCRIBE, 0, 0, 0, 0, 0, 0 };
    vendor_send(report, sizeof(report));
    ESP_LOGI(TAG, "media unsubscribe sent");
}

void usb_hid_media_play_pause(void)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[8] = { HID_MEDIA_PAGE_CTRL, HID_MEDIA_BTN_PLAY_PAUSE, 0, 0, 0, 0, 0, 0 };
    vendor_send(report, sizeof(report));
    ESP_LOGI(TAG, "media play/pause sent");
}

void usb_hid_media_next(void)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[8] = { HID_MEDIA_PAGE_CTRL, HID_MEDIA_BTN_NEXT, 0, 0, 0, 0, 0, 0 };
    vendor_send(report, sizeof(report));
    ESP_LOGI(TAG, "media next sent");
}

void usb_hid_media_prev(void)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[8] = { HID_MEDIA_PAGE_CTRL, HID_MEDIA_BTN_PREV, 0, 0, 0, 0, 0, 0 };
    vendor_send(report, sizeof(report));
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
    vendor_send(report, sizeof(report));
    ESP_LOGI(TAG, "media seek sent: %lu ms", (unsigned long)position_ms);
}

/* -----------------------------------------------------------------------
 * Native input (interface 1: standard keyboard + mouse) -- helpers.
 * The flush functions that use the touchpad / keyboard queues are further
 * down, after those queues (tp_flush_native(), kb_flush_native()).
 * ----------------------------------------------------------------------- */
static bool    s_native   = false;   /* usb_hid_set_native_input() */

/* Mouse feature report (ID 2): bits 0-1 wheel multiplier, bits 2-3 pan
 * multiplier, as set by the host. Non-zero = host turned hi-res on, so one
 * count = 1/120 notch; zero = counts are whole notches. Written from the
 * TinyUSB task (SET_REPORT), read on the LVGL task -- a single byte, no
 * lock needed. */
static volatile uint8_t s_res_mult = 0;

static void native_set_report(uint8_t report_id, hid_report_type_t type,
                              uint8_t const *buf, uint16_t len)
{
    if (type == HID_REPORT_TYPE_FEATURE && report_id == RID_MOUSE && len >= 1) {
        s_res_mult = buf[0] & 0x0F;
        ESP_LOGI(TAG, "mouse resolution multiplier: wheel %s, pan %s",
                 (s_res_mult & 0x03) ? "hi-res" : "notch",
                 (s_res_mult & 0x0C) ? "hi-res" : "notch");
    }
    /* Keyboard LED output report (ID 1) -- nothing to show, ignored. */
}

static uint16_t native_get_report(uint8_t report_id, hid_report_type_t type,
                                  uint8_t *buf, uint16_t reqlen)
{
    if (reqlen < 1) return 0;
    if (type == HID_REPORT_TYPE_FEATURE && report_id == RID_MOUSE) {
        buf[0] = s_res_mult;
        return 1;
    }
    /* Anything else: an all-zero report of the requested size (max 8). */
    uint16_t n = reqlen < 8 ? reqlen : 8;
    memset(buf, 0, n);
    return n;
}

/* Windows virtual-key code -> HID keyboard usage (US layout). Only the keys
 * the mini keyboard / sidebar hotkeys can produce. 0 = none. Modifier VKs
 * are handled separately (vk_to_modifier()). */
static uint8_t vk_to_usage(uint8_t vk)
{
    if (vk >= 'A' && vk <= 'Z') return (uint8_t)(0x04 + (vk - 'A'));
    if (vk >= '1' && vk <= '9') return (uint8_t)(0x1E + (vk - '1'));
    if (vk == '0')              return 0x27;
    if (vk >= 0x70 && vk <= 0x7B) return (uint8_t)(0x3A + (vk - 0x70));   /* F1..F12 */
    switch (vk) {
    case 0x0D: return 0x28;   /* Enter */
    case 0x1B: return 0x29;   /* Esc */
    case 0x08: return 0x2A;   /* Backspace */
    case 0x09: return 0x2B;   /* Tab */
    case 0x20: return 0x2C;   /* Space */
    case 0xBD: return 0x2D;   /* - */
    case 0xBB: return 0x2E;   /* = */
    case 0xDB: return 0x2F;   /* [ */
    case 0xDD: return 0x30;   /* ] */
    case 0xDC: return 0x31;   /* \ */
    case 0xBA: return 0x33;   /* ; */
    case 0xDE: return 0x34;   /* ' */
    case 0xC0: return 0x35;   /* ` */
    case 0xBC: return 0x36;   /* , */
    case 0xBE: return 0x37;   /* . */
    case 0xBF: return 0x38;   /* / */
    case 0x14: return 0x39;   /* Caps Lock */
    case 0x2C: return 0x46;   /* Print Screen */
    case 0x2D: return 0x49;   /* Insert */
    case 0x24: return 0x4A;   /* Home */
    case 0x21: return 0x4B;   /* Page Up */
    case 0x2E: return 0x4C;   /* Delete */
    case 0x23: return 0x4D;   /* End */
    case 0x22: return 0x4E;   /* Page Down */
    case 0x27: return 0x4F;   /* Right */
    case 0x25: return 0x50;   /* Left */
    case 0x28: return 0x51;   /* Down */
    case 0x26: return 0x52;   /* Up */
    default:   return 0;
    }
}

/* Modifier VKs -> HID modifier bit (0 = not a modifier key). */
static uint8_t vk_to_modifier(uint8_t vk)
{
    switch (vk) {
    case 0xA2: case 0x11: return KEYBOARD_MODIFIER_LEFTCTRL;
    case 0xA3:            return KEYBOARD_MODIFIER_RIGHTCTRL;
    case 0xA0: case 0x10: return KEYBOARD_MODIFIER_LEFTSHIFT;
    case 0xA1:            return KEYBOARD_MODIFIER_RIGHTSHIFT;
    case 0xA4: case 0x12: return KEYBOARD_MODIFIER_LEFTALT;
    case 0xA5:            return KEYBOARD_MODIFIER_RIGHTALT;
    case 0x5B:            return KEYBOARD_MODIFIER_LEFTGUI;
    case 0x5C:            return KEYBOARD_MODIFIER_RIGHTGUI;
    default:              return 0;
    }
}

/* HID_KB_MOD_* (the queue's mods byte) -> HID modifier bits. */
static uint8_t mods_to_modifier(uint8_t mods)
{
    uint8_t m = 0;
    if (mods & HID_KB_MOD_CTRL)  m |= KEYBOARD_MODIFIER_LEFTCTRL;
    if (mods & HID_KB_MOD_SHIFT) m |= KEYBOARD_MODIFIER_LEFTSHIFT;
    if (mods & HID_KB_MOD_ALT)   m |= KEYBOARD_MODIFIER_LEFTALT;
    if (mods & HID_KB_MOD_WIN)   m |= KEYBOARD_MODIFIER_LEFTGUI;
    return m;
}

/* -----------------------------------------------------------------------
 * Touchpad reports (see usb_hid.h). All called from the LVGL thread only.
 * ----------------------------------------------------------------------- */
#define TP_BTN_QUEUE_LEN  8

static int32_t s_tp_dx    = 0;
static int32_t s_tp_dy    = 0;
static int32_t s_tp_wheel = 0;
static int32_t s_tp_hwheel = 0;

static uint8_t s_tp_btn_q[TP_BTN_QUEUE_LEN];
static uint8_t s_tp_btn_head  = 0;
static uint8_t s_tp_btn_count = 0;
static uint8_t s_tp_btn_last  = 0;   /* newest queued (or current) mask */

void usb_hid_tp_move(int dx, int dy)  { s_tp_dx += dx; s_tp_dy += dy; }
void usb_hid_tp_wheel(int pixels)     { s_tp_wheel += pixels; }
void usb_hid_tp_hwheel(int pixels)    { s_tp_hwheel += pixels; }
void usb_hid_tp_reset_motion(void)    { s_tp_dx = s_tp_dy = s_tp_wheel = s_tp_hwheel = 0; }

void usb_hid_tp_buttons(uint8_t mask)
{
    if (mask == s_tp_btn_last) return;

    if (s_tp_btn_count < TP_BTN_QUEUE_LEN) {
        s_tp_btn_q[(s_tp_btn_head + s_tp_btn_count) % TP_BTN_QUEUE_LEN] = mask;
        s_tp_btn_count++;
    } else {
        /* Full: replace the newest entry so the final state is still right. */
        s_tp_btn_q[(s_tp_btn_head + s_tp_btn_count - 1) % TP_BTN_QUEUE_LEN] = mask;
    }
    s_tp_btn_last = mask;
}

static int32_t tp_clamp(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void tp_flush_native(void);

void usb_hid_tp_flush(void)
{
    if (s_native) { tp_flush_native(); return; }

    bool have_btn = s_tp_btn_count > 0;
    bool have_move = have_btn || s_tp_dx || s_tp_dy || s_tp_wheel;
    if (!have_move && s_tp_hwheel == 0) return;
    if (!s_active || !tud_hid_ready()) return;

    if (!have_move) {
        /* Horizontal wheel only -- its own report type (see usb_hid.h). */
        int32_t hw = tp_clamp(s_tp_hwheel, INT8_MIN, INT8_MAX);
        uint8_t hrep[8] = { HID_TP_PAGE, HID_TP_CMD_HWHEEL, (uint8_t)(int8_t)hw, s_tp_btn_last, 0, 0, 0, 0 };
        if (vendor_send(hrep, sizeof(hrep))) s_tp_hwheel -= hw;
        return;
    }

    int32_t dx = tp_clamp(s_tp_dx, INT16_MIN, INT16_MAX);
    int32_t dy = tp_clamp(s_tp_dy, INT16_MIN, INT16_MAX);
    int32_t wh = tp_clamp(s_tp_wheel, INT8_MIN, INT8_MAX);
    uint8_t mask = have_btn ? s_tp_btn_q[s_tp_btn_head] : s_tp_btn_last;

    uint8_t report[8] = {
        HID_TP_PAGE, HID_TP_CMD_MOVE,
        (uint8_t)(dx & 0xFF), (uint8_t)((dx >> 8) & 0xFF),
        (uint8_t)(dy & 0xFF), (uint8_t)((dy >> 8) & 0xFF),
        (uint8_t)(int8_t)wh, mask
    };

    /* vendor_send() fails if the endpoint got claimed in between (e.g.
     * a mode reply from the TinyUSB task) -- keep everything for the next
     * flush instead of losing it. */
    if (!vendor_send(report, sizeof(report))) return;

    s_tp_dx    -= dx;
    s_tp_dy    -= dy;
    s_tp_wheel -= wh;
    if (have_btn) {
        s_tp_btn_head = (s_tp_btn_head + 1) % TP_BTN_QUEUE_LEN;
        s_tp_btn_count--;
    }
}

/* -----------------------------------------------------------------------
 * Keyboard reports (see usb_hid.h). LVGL thread only.
 * ----------------------------------------------------------------------- */
#define KB_Q_LEN  32

typedef struct { uint8_t cmd, vk, mods; } kb_evt_t;

static kb_evt_t s_kb_q[KB_Q_LEN];
static uint8_t  s_kb_head  = 0;
static uint8_t  s_kb_count = 0;

bool usb_hid_kb_key(bool down, uint8_t vk, uint8_t mods)
{
    /* Last slot is reserved for key-ups so a release can always be queued. */
    uint8_t limit = down ? (KB_Q_LEN - 1) : KB_Q_LEN;
    if (s_kb_count >= limit) {
        ESP_LOGW(TAG, "kb queue full, dropped %s vk=0x%02X", down ? "down" : "up", vk);
        return false;
    }
    kb_evt_t *ev = &s_kb_q[(s_kb_head + s_kb_count) % KB_Q_LEN];
    ev->cmd  = down ? HID_KB_CMD_DOWN : HID_KB_CMD_UP;
    ev->vk   = vk;
    ev->mods = mods;
    s_kb_count++;
    return true;
}

bool usb_hid_kb_pending(void) { return s_kb_count > 0; }

static void kb_flush_native(void);

void usb_hid_kb_flush(void)
{
    if (s_native) { kb_flush_native(); return; }
    if (s_kb_count == 0) return;
    if (!s_active || !tud_hid_ready()) return;

    const kb_evt_t *ev = &s_kb_q[s_kb_head];
    uint8_t report[8] = { HID_KB_PAGE, ev->cmd, ev->vk, ev->mods, 0, 0, 0, 0 };

    /* Endpoint may get claimed in between (TinyUSB task) -- keep the event
     * for the next flush instead of losing it. */
    if (!vendor_send(report, sizeof(report))) return;

    s_kb_head = (s_kb_head + 1) % KB_Q_LEN;
    s_kb_count--;
}

/* -----------------------------------------------------------------------
 * Native input -- flush (interface 1). LVGL thread, same as the vendor
 * flushes above; one report per call, nothing dropped if the endpoint is
 * busy (state stays queued / accumulated for the next call).
 * ----------------------------------------------------------------------- */

/* Finger pixels -> wheel counts. Matches ESDeckPC's WheelPerPixel = 5
 * (24 px per notch): hi-res = 5 counts (1/120 notch each) per px, else
 * whole notches every 24 px. */
#define NATIVE_WHEEL_PX_PER_NOTCH  24
#define NATIVE_WHEEL_HIRES_PER_PX  5
/* Pointer gain on the native path = ESDeckPC's TouchpadInput.Sensitivity
 * (2.0), so Cursor Speed 1.00x feels the same in both input modes. */
#define NATIVE_POINTER_GAIN        2

/* Take up to one report's worth out of a px accumulator; returns counts. */
static int8_t take_wheel(int32_t *acc, bool hires)
{
    if (*acc == 0) return 0;
    if (hires) {
        int32_t px = tp_clamp(*acc, -127 / NATIVE_WHEEL_HIRES_PER_PX, 127 / NATIVE_WHEEL_HIRES_PER_PX);
        *acc -= px;
        return (int8_t)(px * NATIVE_WHEEL_HIRES_PER_PX);
    }
    int32_t notches = tp_clamp(*acc / NATIVE_WHEEL_PX_PER_NOTCH, -127, 127);
    *acc -= notches * NATIVE_WHEEL_PX_PER_NOTCH;   /* remainder waits for more px */
    return (int8_t)notches;
}

static void tp_flush_native(void)
{
    bool have_btn = s_tp_btn_count > 0;
    bool have_wh  = (s_res_mult & 0x03) ? (s_tp_wheel != 0)
                                        : (s_tp_wheel >= NATIVE_WHEEL_PX_PER_NOTCH || s_tp_wheel <= -NATIVE_WHEEL_PX_PER_NOTCH);
    bool have_pan = (s_res_mult & 0x0C) ? (s_tp_hwheel != 0)
                                        : (s_tp_hwheel >= NATIVE_WHEEL_PX_PER_NOTCH || s_tp_hwheel <= -NATIVE_WHEEL_PX_PER_NOTCH);
    if (!have_btn && !s_tp_dx && !s_tp_dy && !have_wh && !have_pan) return;
    if (!s_active || !tud_hid_n_ready(HID_ITF_INPUT)) return;

    /* Take at most what still fits int8 after the gain. */
    int32_t dx = tp_clamp(s_tp_dx, -127 / NATIVE_POINTER_GAIN, 127 / NATIVE_POINTER_GAIN);
    int32_t dy = tp_clamp(s_tp_dy, -127 / NATIVE_POINTER_GAIN, 127 / NATIVE_POINTER_GAIN);
    int32_t wacc = s_tp_wheel, hacc = s_tp_hwheel;
    int8_t  wh  = take_wheel(&wacc, (s_res_mult & 0x03) != 0);
    int8_t  pan = take_wheel(&hacc, (s_res_mult & 0x0C) != 0);
    /* HID_TP_BTN_* bits are already the HID mouse button bits
     * (1 L, 2 R, 4 M, 8 button 4 / back, 16 button 5 / forward). */
    uint8_t mask = have_btn ? s_tp_btn_q[s_tp_btn_head] : s_tp_btn_last;

    if (!tud_hid_n_mouse_report(HID_ITF_INPUT, RID_MOUSE, mask,
                                (int8_t)(dx * NATIVE_POINTER_GAIN),
                                (int8_t)(dy * NATIVE_POINTER_GAIN), wh, pan)) return;

    s_tp_dx -= dx;
    s_tp_dy -= dy;
    s_tp_wheel  = wacc;
    s_tp_hwheel = hacc;
    if (have_btn) {
        s_tp_btn_head = (s_tp_btn_head + 1) % TP_BTN_QUEUE_LEN;
        s_tp_btn_count--;
    }
}

/* Keyboard state as the host currently sees it. The queue carries
 * down/up events (one key at a time from the mini keyboard / hotkeys);
 * this turns them into 6-key-rollover reports. */
static uint8_t s_nk_keys[6];
static uint8_t s_nk_mod = 0;

static void kb_flush_native(void)
{
    if (s_kb_count == 0) return;
    if (!s_active || !tud_hid_n_ready(HID_ITF_INPUT)) return;

    const kb_evt_t *ev = &s_kb_q[s_kb_head];
    uint8_t keys[6], mod;
    memcpy(keys, s_nk_keys, sizeof(keys));
    mod = s_nk_mod;

    uint8_t usage = vk_to_usage(ev->vk);
    uint8_t vkmod = vk_to_modifier(ev->vk);

    if (ev->cmd == HID_KB_CMD_DOWN) {
        /* Held modifiers come with the key; a modifier VK (IME Shift tap)
         * is just its modifier bit. A repeat DOWN of a held key changes
         * nothing -- the OS does its own typematic repeat for real keys. */
        mod |= mods_to_modifier(ev->mods) | vkmod;
        if (usage) {
            bool have = false;
            for (int i = 0; i < 6; i++) if (keys[i] == usage) have = true;
            for (int i = 0; i < 6 && !have; i++) if (!keys[i]) { keys[i] = usage; have = true; }
        }
    } else {
        if (usage) for (int i = 0; i < 6; i++) if (keys[i] == usage) keys[i] = 0;
        mod &= (uint8_t)~vkmod;
        /* Modifiers were sent along with this key -- with no key left
         * down they go up too (same as ESDeckPC's KeyboardInput). Only for
         * a real key: releasing a modifier VK (a Touchpad top button set
         * to Ctrl, say) drops just its own bit, so another modifier still
         * held stays down. */
        bool any = false;
        for (int i = 0; i < 6; i++) if (keys[i]) any = true;
        if (usage && !any) mod = 0;
    }

    if (!tud_hid_n_keyboard_report(HID_ITF_INPUT, RID_KEYBOARD, mod, keys)) return;

    memcpy(s_nk_keys, keys, sizeof(keys));
    s_nk_mod = mod;
    s_kb_head = (s_kb_head + 1) % KB_Q_LEN;
    s_kb_count--;
}

bool usb_hid_get_native_input(void) { return s_native; }

void usb_hid_set_native_input(bool native)
{
    if (native == s_native) return;

    /* Release whatever the old path still holds, best effort. */
    if (s_native) {
        if (s_active && tud_hid_n_ready(HID_ITF_INPUT))
            tud_hid_n_mouse_report(HID_ITF_INPUT, RID_MOUSE, 0, 0, 0, 0, 0);
        if (s_active && tud_hid_n_ready(HID_ITF_INPUT))
            tud_hid_n_keyboard_report(HID_ITF_INPUT, RID_KEYBOARD, 0, NULL);
        memset(s_nk_keys, 0, sizeof(s_nk_keys));
        s_nk_mod = 0;
    }
    /* Vendor path: ESDeckPC releases on its side when it sees no more
     * input; queued events are dropped rather than replayed on the new path. */
    s_kb_count = 0;
    s_tp_btn_count = 0;
    s_tp_btn_last  = 0;
    s_tp_dx = s_tp_dy = s_tp_wheel = s_tp_hwheel = 0;

    s_native = native;
    ESP_LOGI(TAG, "input mode: %s", native ? "USB HID (keyboard + mouse)" : "ESDeckPC (vendor reports)");
}

/* -----------------------------------------------------------------------
 * TinyUSB HID callbacks
 * ----------------------------------------------------------------------- */
uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
    return (instance == HID_ITF_INPUT) ? hid_input_report_descriptor : hid_report_descriptor;
}

void tud_hid_set_report_cb(uint8_t instance, uint8_t report_id,
                            hid_report_type_t report_type,
                            uint8_t const *buffer, uint16_t bufsize)
{
    if (instance == HID_ITF_INPUT) {
        native_set_report(report_id, report_type, buffer, bufsize);
        return;
    }
    if (report_type != HID_REPORT_TYPE_FEATURE) return;
    if (bufsize < 1) return;

    uint8_t cmd = buffer[0];

    /* Firmware update running: only the update itself, time sync and the
     * mode query get through -- no Monitor stats / Media progress, level or
     * images to redraw (and decode) behind the update screen. */
    if (s_ota_active &&
        cmd != HID_OTA_CMD_DATA && cmd != HID_OTA_CMD_BEGIN && cmd != HID_OTA_CMD_ABORT &&
        cmd != HID_MON_CMD_TIME && cmd != HID_MON_CMD_QUERY)
        return;

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
    else if (cmd == HID_OTA_CMD_DATA) {
        ota_data(buffer + 1, bufsize - 1);
    }
    else if (cmd == HID_OTA_CMD_BEGIN && bufsize >= 5) {
        ota_begin((uint32_t)buffer[1] | ((uint32_t)buffer[2] << 8) |
                  ((uint32_t)buffer[3] << 16) | ((uint32_t)buffer[4] << 24));
    }
    else if (cmd == HID_OTA_CMD_ABORT) {
        if (s_ota_active) s_ota_err = HID_OTA_ERR_FLASH;
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
    if (instance == HID_ITF_INPUT)
        return native_get_report(report_id, report_type, buffer, reqlen);

    if (report_type == HID_REPORT_TYPE_FEATURE ||
        report_type == HID_REPORT_TYPE_INPUT) {
        uint16_t len = reqlen < 8 ? reqlen : 8;
        memset(buffer, 0, len);
        return len;
    }
    return 0;
}