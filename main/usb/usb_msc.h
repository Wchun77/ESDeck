#pragma once

#include "tinyusb.h"
#include "tusb.h"

void usb_msc_class_init(void);
void usb_msc_class_deinit(void);

const tusb_desc_device_t *usb_msc_get_device_desc(void);
const uint8_t            *usb_msc_get_config_desc(void);
