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

void usb_hid_driver_install(void);

void usb_hid_activate(void);
void usb_hid_deactivate(void);

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

/* Register callback invoked when PC sends CMD_TIME.
 * Pass NULL to unregister. Called from the TinyUSB task context. */
void usb_hid_set_time_cb(void (*cb)(uint8_t hour, uint8_t min, uint8_t sec,
                                    uint8_t month, uint8_t day, uint8_t wday));

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

const tusb_desc_device_t *usb_hid_get_device_desc(void);
const uint8_t            *usb_hid_get_config_desc(void);