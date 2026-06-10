#pragma once

#include <stdint.h>
#include "tinyusb.h"
#include "tusb.h"

/* Monitor OUT report command bytes (PC -> ESP) */
#define HID_MON_CMD_DATA        0x03
#define HID_MON_CMD_TIME        0x04

/* Monitor control IN report bytes (ESP -> PC)
 * page = 0xFF signals a monitor control message, not a button press. */
#define HID_MON_PAGE_CTRL       0xFF
#define HID_MON_BTN_SUBSCRIBE   0x01
#define HID_MON_BTN_UNSUBSCRIBE 0x02

void usb_hid_driver_install(void);

void usb_hid_activate(void);
void usb_hid_deactivate(void);

/* Send button press IN report */
void usb_hid_send(uint8_t page, uint8_t btn);
void usb_hid_release(void);

/* Send monitor subscribe / unsubscribe control IN report */
void usb_hid_monitor_subscribe(void);
void usb_hid_monitor_unsubscribe(void);

/* Register callback invoked when PC sends monitor data (OUT report).
 * Pass NULL to unregister. Called from the TinyUSB task context. */
void usb_hid_set_monitor_cb(void (*cb)(uint8_t cpu_usage, uint8_t cpu_temp,
                                       uint8_t ram_usage, uint8_t gpu_usage));

const tusb_desc_device_t *usb_hid_get_device_desc(void);
const uint8_t            *usb_hid_get_config_desc(void);
