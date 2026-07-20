#include "usb_hid.h"
#include "esp_log.h"
#include <string.h>
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <time.h>
#include <sys/time.h>

static const char *TAG = "USB_HID";

#define TUSB_DESC_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static bool s_active = false;

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
static void (*s_time_cb)(uint8_t hour, uint8_t min, uint8_t sec,
                         uint8_t month, uint8_t day, uint8_t wday) = NULL;

/* Called when PC sends CMD_QUERY; returns true if currently in monitor mode */
static uint8_t (*s_mode_query_cb)(void) = NULL;

/* Called when PC sends CMD_NOWPLAYING_PROGRESS */
static void (*s_nowplaying_cb)(uint32_t position_ms, uint32_t duration_ms,
                               bool playing) = NULL;

/* Called when PC sends CMD_AUDIO_LEVEL */
static void (*s_audio_level_cb)(uint8_t level) = NULL;

void usb_hid_set_monitor_cb(void (*cb)(uint8_t, uint8_t, uint8_t, uint8_t,
                                       uint8_t, uint8_t, uint8_t, uint8_t,
                                       uint8_t, uint8_t, uint8_t, uint8_t,
                                       uint8_t))
{
    s_monitor_data_cb = cb;
}

void usb_hid_set_time_cb(void (*cb)(uint8_t, uint8_t, uint8_t,
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
        .event_cb  = NULL,
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
        if (s_time_cb) {
            struct tm t = {
                .tm_year  = (buffer[1] + 2000) - 1900,
                .tm_mon   = buffer[2] - 1,
                .tm_mday  = buffer[3],
                .tm_hour  = buffer[4],
                .tm_min   = buffer[5],
                .tm_sec   = buffer[6],
                .tm_isdst = -1,
            };
            mktime(&t);

            /* Add 1 second to compensate for the one-tick display lag */
            uint8_t sec_adj  = buffer[6] + 1;
            uint8_t min_adj  = buffer[5];
            uint8_t hour_adj = buffer[4];
            if (sec_adj  >= 60) { sec_adj  = 0; min_adj++;  }
            if (min_adj  >= 60) { min_adj  = 0; hour_adj++; }
            if (hour_adj >= 24) { hour_adj = 0; }

            s_time_cb(hour_adj, min_adj, sec_adj,
                    buffer[2], buffer[3], (uint8_t)t.tm_wday);
        }
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