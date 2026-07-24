#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "tinyusb.h"
#include "tusb.h"

/* Feature report payload size (bytes). Must match the HID descriptor
 * Report Count and the PC-side REPORT_SIZE (which adds 1 for Report ID). */
#define HID_FEATURE_PAYLOAD_SIZE 64

/* Monitor OUT report command bytes (PC -> ESP) */
#define HID_MON_CMD_DATA         0x03
#define HID_MON_CMD_TIME         0x04
#define HID_MON_CMD_QUERY        0x05

/* Monitor control IN report bytes (ESP -> PC)
 * page = 0xFF signals a monitor control message, not a button press. */
#define HID_MON_PAGE_CTRL        0xFF
#define HID_MON_BTN_SUBSCRIBE    0x01
#define HID_MON_BTN_UNSUBSCRIBE  0x02
#define HID_MON_BTN_MODE_DECK    0x03
#define HID_MON_BTN_MODE_MONITOR 0x04
#define HID_MON_BTN_MODE_MEDIA   0x05

/* Media OUT report command byte (PC -> ESP), continuing after the monitor
 * cmd range above (0x03-0x05). Payload: position_ms(4B LE) +
 * duration_ms(4B LE) + playing(1B, 0/1). Numeric only for now -- title/
 * artist need a PC-rendered image pipeline (no CJK font on-device, see
 * project notes §4.1) and are not part of this protocol yet. */
#define HID_MEDIA_CMD_NOWPLAYING_PROGRESS  0x06

/* Media OUT report command byte (PC -> ESP): system output audio level for
 * the sidebar VU-meter bar. Payload: level(1B, 0-100). Simple single-value
 * VU meter for now (project notes §5 "簡單版") -- FFT band splitting for a
 * real spectrum bar is a later step. Sent at whatever cadence the PC side
 * chooses; ESP just displays the latest value it has, same
 * received/timeout convention as CMD_NOWPLAYING_PROGRESS. */
#define HID_MEDIA_CMD_AUDIO_LEVEL           0x07

/* Media OUT report commands (PC -> ESP), continued: cover art / title-
 * artist image transfer. Chunked over the same 64-byte Feature report
 * channel, reassembled and decoded on the ESP into a fixed-size RGB565
 * buffer per kind -- PC must resize/encode to exactly the target
 * dimensions below before sending; anything else is rejected. COVER is
 * decoded as JPEG (photographic thumbnail, esp_new_jpeg); INFO is decoded
 * as PNG (flat background + sharp text edges -- JPEG's lossy chroma
 * subsampling showed visible blocking artifacts around glyphs, PNG is
 * lossless and this content compresses to almost nothing anyway). See
 * project notes §4.3/§4.4. */
#define HID_MEDIA_CMD_NOWPLAYING_IMG_START  0x08
#define HID_MEDIA_CMD_NOWPLAYING_IMG_CHUNK  0x09
#define HID_MEDIA_CMD_NOWPLAYING_IMG_END    0x0A

/* Image "kind" byte, sent with START/CHUNK/END -- two independent transfer
 * slots so a cover update and an info-strip update can be in flight at the
 * same time without stepping on each other. */
#define HID_MEDIA_IMG_KIND_COVER  0
#define HID_MEDIA_IMG_KIND_INFO   1
#define HID_MEDIA_IMG_KIND_COUNT  2

/* Fixed target dimensions per kind (PC must encode to exactly this size,
 * ESP rejects a decoded JPEG whose header reports anything else). COVER
 * matches ui_media.c's cover art box; INFO is a single strip holding both
 * the title (top) and artist (bottom) lines, rendered PC-side since the
 * ESP has no CJK font on-device. */
#define HID_MEDIA_IMG_COVER_W  220
#define HID_MEDIA_IMG_COVER_H  220
#define HID_MEDIA_IMG_INFO_W   480
#define HID_MEDIA_IMG_INFO_H   70

/* Media control IN report bytes (ESP -> PC)
 * page = 0xFE signals a Media control message -- parallel, independent
 * namespace from the monitor 0xFF channel above (not a button press). */
#define HID_MEDIA_PAGE_CTRL       0xFE
#define HID_MEDIA_BTN_SUBSCRIBE   0x01
#define HID_MEDIA_BTN_UNSUBSCRIBE 0x02

/* Media control IN report bytes (ESP -> PC), continued: playback control
 * (ESP button press -> PC media session). One-shot commands, not gated by
 * subscribe state -- pressing these only does anything meaningful while
 * the ESP UI has them enabled (real data flowing), see ui_media.c. */
#define HID_MEDIA_BTN_PLAY_PAUSE  0x03
#define HID_MEDIA_BTN_NEXT        0x04
#define HID_MEDIA_BTN_PREV        0x05

/* Media control IN report bytes (ESP -> PC), continued: seek. Sent once on
 * slider release, not a live drag stream. Payload after the btn byte is
 * position_ms(4B LE): { HID_MEDIA_PAGE_CTRL, HID_MEDIA_BTN_SEEK,
 * pos_b0, pos_b1, pos_b2, pos_b3, 0, 0 }. */
#define HID_MEDIA_BTN_SEEK        0x06

void usb_hid_driver_install(void);

void usb_hid_activate(void);
void usb_hid_deactivate(void);

/* True if the device is currently attached/mounted. Tracked in software
 * (not read from tud_mounted()) -- see the s_conn_state comment in
 * usb_hid.c: on this port, tud_mounted() only clears on a real VBUS
 * session-end interrupt, so it never notices a self-initiated soft
 * disconnect (usb_hid_force_disconnect() below). Safe to call from
 * anywhere including the LVGL task. */
bool usb_hid_is_connected(void);

/* Force a soft USB disconnect / reconnect (toggles the pull-up) without
 * physically unplugging the cable. Two separate one-shot actions rather
 * than a combined "reconnect" so a UI toggle (Settings page's HID Status
 * row) can call whichever one matches the current state -- disconnect
 * stays disconnected until connect is called explicitly, useful for
 * manually testing the PC side's disconnect handling. Both are
 * non-blocking, safe to call from the LVGL task. usb_hid_is_connected()
 * reflects usb_hid_force_disconnect() immediately; after
 * usb_hid_force_connect() it stays false until the host actually
 * finishes re-enumerating (real TINYUSB_EVENT_ATTACHED, ~1-3s later). */
void usb_hid_force_disconnect(void);
void usb_hid_force_connect(void);

/* Register callback invoked whenever the device attaches/detaches (see
 * TINYUSB_EVENT_ATTACHED/DETACHED below). Pass NULL to unregister.
 * Already hopped onto the LVGL task via lv_async_call() before this
 * fires, same as the connect/disconnect toast it's called alongside --
 * safe to touch LVGL objects directly. */
typedef void (*usb_hid_conn_cb_t)(bool connected);
void usb_hid_set_conn_cb(usb_hid_conn_cb_t cb);

/* Send button press IN report */
void usb_hid_send(uint8_t page, uint8_t btn);
void usb_hid_release(void);

/* Send monitor subscribe / unsubscribe control IN report */
void usb_hid_monitor_subscribe(void);
void usb_hid_monitor_unsubscribe(void);

/* Send media subscribe / unsubscribe control IN report */
void usb_hid_media_subscribe(void);
void usb_hid_media_unsubscribe(void);

/* Send media playback control IN report (ESP -> PC) */
void usb_hid_media_play_pause(void);
void usb_hid_media_next(void);
void usb_hid_media_prev(void);
void usb_hid_media_seek(uint32_t position_ms);

/* Reply to a CMD_QUERY with current UI mode.
 * mode: 0=deck, 1=monitor, 2=media (matches ui_mode_t in ui_settings.h --
 * usb_hid.c intentionally does not #include ui_settings.h to avoid a
 * layering dependency, so the caller passes the raw enum value). */
void usb_hid_reply_mode(uint8_t mode);

/* Register callback invoked when PC sends monitor data (OUT report).
 * Pass NULL to unregister. Called from the TinyUSB task context. */
void usb_hid_set_monitor_cb(void (*cb)(uint8_t cpu_usage, uint8_t cpu_temp,
                                       uint8_t ram_usage, uint8_t gpu_usage,
                                       uint8_t gpu_temp,  uint8_t gpu_vram,
                                       uint8_t cpu_freq,  uint8_t net_up,
                                       uint8_t net_down,  uint8_t disk_usage,
                                       uint8_t cpu_power, uint8_t gpu_power,
                                       uint8_t ssd_life));

/* Register callback invoked when PC sends CMD_TIME. Pass NULL to
 * unregister. Called from the TinyUSB task context -- raw field pass-
 * through only, no calendar math here (year/month/day/hour/min/sec as
 * sent by the PC; see sys_clock.c for lag compensation, drift correction
 * and wday derivation). Unlike usb_hid_set_monitor_cb(), this is
 * typically registered once at boot (my_ui_init() -> sys_clock_init()),
 * not per UI mode -- the PC may send CMD_TIME while any mode is active. */
void usb_hid_set_time_cb(void (*cb)(uint16_t year, uint8_t month, uint8_t day,
                                    uint8_t hour, uint8_t min, uint8_t sec));

/* Register callback invoked when PC sends CMD_QUERY.
 * Must return the current UI mode: 0=deck, 1=monitor, 2=media. */
void usb_hid_set_mode_query_cb(uint8_t (*cb)(void));

/* Register callback invoked when PC sends CMD_NOWPLAYING_PROGRESS.
 * Pass NULL to unregister. Called from the TinyUSB task context. */
void usb_hid_set_nowplaying_cb(void (*cb)(uint32_t position_ms,
                                          uint32_t duration_ms,
                                          bool playing));

/* Register callback invoked when PC sends CMD_AUDIO_LEVEL.
 * Pass NULL to unregister. Called from the TinyUSB task context. */
void usb_hid_set_audio_level_cb(void (*cb)(uint8_t level));

/* Register callback invoked when a cover/info image (see
 * HID_MEDIA_CMD_NOWPLAYING_IMG_*) finishes reassembly and decode.
 * kind is HID_MEDIA_IMG_KIND_COVER/INFO, w/h are the fixed dimensions for
 * that kind, and rgb565_data is a heap_caps_malloc(MALLOC_CAP_SPIRAM)
 * buffer. Ownership transfers to the callback -- it must eventually
 * heap_caps_free() it once done (see ui_media.c). Buffer format/size
 * differs by kind (COVER is opaque JPEG -> plain RGB565, w*h*2 bytes;
 * INFO is PNG with alpha -> RGB565+alpha, w*h*3 bytes -- see usb_hid.c's
 * decode_jpeg_to_rgb565()/decode_png_to_rgb565()).
 * rgb565_data is NULL (w/h both 0) as a "clear" sentinel when PC sends
 * CMD_NOWPLAYING_IMG_START with total_size=0 -- e.g. Now Playing lost
 * focus/session and has nothing to show for that kind anymore.
 * Pass NULL to unregister. Called from the TinyUSB task context -- no
 * LVGL calls here, same rule as the other callbacks. */
void usb_hid_set_nowplaying_img_cb(void (*cb)(uint8_t kind, uint8_t *rgb565_data,
                                              uint16_t w, uint16_t h));

const tusb_desc_device_t *usb_hid_get_device_desc(void);
const uint8_t            *usb_hid_get_config_desc(void);