#pragma once

#include <stdint.h>
#include "tinyusb.h"
#include "tusb.h"

void usb_hid_driver_install(void);

void usb_hid_activate(void);
void usb_hid_deactivate(void);

void usb_hid_send(uint8_t page, uint8_t btn);
void usb_hid_release(void);

const tusb_desc_device_t *usb_hid_get_device_desc(void);
const uint8_t            *usb_hid_get_config_desc(void);
