#pragma once

#include "tinyusb.h"
#include "tusb.h"

void usb_msc_class_init(void);
void usb_msc_class_deinit(void);

/* Called once when the PC ejects the drive (or the cable is pulled) --
 * the PC is done with the card, so the device can restart into HID.
 * Runs in the TinyUSB task: no LVGL calls. Without a callback the
 * device restarts by itself shortly after. */
typedef void (*usb_msc_eject_cb_t)(void);
void usb_msc_set_eject_cb(usb_msc_eject_cb_t cb);

const tusb_desc_device_t *usb_msc_get_device_desc(void);
const uint8_t            *usb_msc_get_config_desc(void);
