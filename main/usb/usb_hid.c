#include "usb_hid.h"
#include "esp_log.h"
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
 * Feature report (PC -> ESP): 8 bytes [cmd, d0, d1, d2, d3, rsv x3]
 *
 * PC sends data via SetReport (Control transfer, Feature type).
 * No OUT interrupt endpoint needed; original descriptor shape preserved
 * so Windows HID enumeration is not affected.
 */
static const uint8_t hid_report_descriptor[] = {
    0x06, 0x00, 0xFF,   /* Usage Page (Vendor 0xFF00) */
    0x09, 0x01,         /* Usage (0x01) */
    0xA1, 0x01,         /* Collection (Application) */

    /* IN: 8 bytes (page + btn + 6 reserved) */
    0x09, 0x02,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x08,         /* Report Count (8) */
    0x81, 0x02,         /* Input (Data, Var, Abs) */

    /* Feature: 8 bytes (cmd + 4 data + 3 reserved) */
    0x09, 0x03,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x08,         /* Report Count (8) */
    0xB1, 0x02,         /* Feature (Data, Var, Abs) */

    0xC0                /* End Collection */
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

/* -----------------------------------------------------------------------
 * Monitor Feature report callback — registered by ui_monitor on enter/exit
 * ----------------------------------------------------------------------- */
static void (*s_monitor_data_cb)(uint8_t cpu_usage, uint8_t cpu_temp,
                                 uint8_t ram_usage, uint8_t gpu_usage) = NULL;

void usb_hid_set_monitor_cb(void (*cb)(uint8_t, uint8_t, uint8_t, uint8_t))
{
    s_monitor_data_cb = cb;
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
            .string            = NULL,
            .string_count      = 0,
            .full_speed_config = s_config_desc,
            .high_speed_config = NULL,
        },
        .event_cb  = NULL,
        .event_arg = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));
    s_active = true;
    ESP_LOGI(TAG, "HID driver installed");
}

void usb_hid_activate(void)
{
    s_active = true;
    ESP_LOGI(TAG, "HID activated");
}

void usb_hid_deactivate(void)
{
    s_active = false;
    ESP_LOGI(TAG, "HID deactivated");
}

/* -----------------------------------------------------------------------
 * Descriptor getters
 * ----------------------------------------------------------------------- */
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
    /* Accept Feature (SetReport via Control transfer) only */
    if (report_type != HID_REPORT_TYPE_FEATURE) return;
    if (bufsize < 1) return;

    uint8_t cmd = buffer[0];

    if (cmd == HID_MON_CMD_DATA && bufsize >= 5) {
        if (s_monitor_data_cb) {
            s_monitor_data_cb(buffer[1], buffer[2], buffer[3], buffer[4]);
        }
    }
    else if (cmd == HID_MON_CMD_TIME && bufsize >= 7) {
        struct tm t = {
            .tm_year  = (buffer[1] + 2000) - 1900,
            .tm_mon   = buffer[2] - 1,
            .tm_mday  = buffer[3],
            .tm_hour  = buffer[4],
            .tm_min   = buffer[5],
            .tm_sec   = buffer[6],
            .tm_isdst = -1,
        };
        time_t epoch = mktime(&t);
        struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
        settimeofday(&tv, NULL);
        ESP_LOGI(TAG, "time synced: %04d-%02d-%02d %02d:%02d:%02d",
                 buffer[1] + 2000, buffer[2], buffer[3],
                 buffer[4], buffer[5], buffer[6]);
    }
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen)
{
    return 0;
}