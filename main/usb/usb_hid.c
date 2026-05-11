#include "usb_hid.h"
#include "esp_log.h"
#include "tinyusb.h"
#include "class/hid/hid_device.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "USB_HID";

#define TUSB_DESC_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)

static bool s_active = false;

static const uint8_t hid_report_descriptor[] = {
    0x06, 0x00, 0xFF,
    0x09, 0x01,
    0xA1, 0x01,
    0x09, 0x02,
    0x15, 0x00,
    0x26, 0xFF, 0x00,
    0x75, 0x08,
    0x95, 0x02,
    0x81, 0x02,
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

/* -----------------------------------------------------------------------
 * Called once at boot - installs PHY + TinyUSB task
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

/* -----------------------------------------------------------------------
 * Called when switching back to HID mode (PHY already running)
 * ----------------------------------------------------------------------- */
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
 * Descriptor getters for usb_manager
 * ----------------------------------------------------------------------- */
const tusb_desc_device_t *usb_hid_get_device_desc(void)
{
    return &s_device_desc;
}

const uint8_t *usb_hid_get_config_desc(void)
{
    return s_config_desc;
}

/* -----------------------------------------------------------------------
 * HID send helpers
 * ----------------------------------------------------------------------- */
void usb_hid_send(uint8_t page, uint8_t btn)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[2] = { page, btn };
    tud_hid_report(0, report, sizeof(report));
}

void usb_hid_release(void)
{
    if (!s_active || !tud_hid_ready()) return;
    uint8_t report[2] = { 0xFF, 0xFF };
    tud_hid_report(0, report, sizeof(report));
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
}

uint16_t tud_hid_get_report_cb(uint8_t instance, uint8_t report_id,
                                hid_report_type_t report_type,
                                uint8_t *buffer, uint16_t reqlen)
{
    return 0;
}
