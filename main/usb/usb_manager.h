#pragma once

typedef enum {
    USB_MODE_HID = 0,
    USB_MODE_MSC,
} usb_mode_t;

void       usb_manager_init(void);
void       usb_manager_request_msc(void);
usb_mode_t usb_manager_get_mode(void);
