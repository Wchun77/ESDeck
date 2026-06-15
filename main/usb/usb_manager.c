#include "usb_manager.h"
#include "usb_hid.h"
#include "usb_msc.h"
#include "fs_manager/fs_sd.h"
#include "descriptors_control.h"
#include "tinyusb.h"
#include "tusb.h"
#include "lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"

static const char *TAG = "USB_MGR";

#define EVT_REQ_MSC  BIT0

static EventGroupHandle_t s_evt_grp  = NULL;
static usb_mode_t         s_cur_mode = USB_MODE_HID;

static void usb_manager_task(void *arg)
{
    for (;;) {
        xEventGroupWaitBits(s_evt_grp, EVT_REQ_MSC, pdTRUE, pdFALSE, portMAX_DELAY);

        if (s_cur_mode == USB_MODE_MSC) continue;

        ESP_LOGI(TAG, "Switching to MSC (one-way)");

        usb_hid_deactivate();

        // 等黑畫面刷完（由按鈕那邊負責填黑，這裡只等）
        vTaskDelay(pdMS_TO_TICKS(300));

        tud_disconnect();

        fs_sd_unmount_fat();

        const tinyusb_desc_config_t desc = {
            .device            = usb_msc_get_device_desc(),
            .qualifier         = NULL,
            .string            = NULL,
            .string_count      = 0,
            .full_speed_config = usb_msc_get_config_desc(),
            .high_speed_config = NULL,
        };
        tinyusb_descriptors_set(TINYUSB_PORT_FULL_SPEED_0, &desc);

        usb_msc_class_init();
        tud_connect();

        s_cur_mode = USB_MODE_MSC;
        ESP_LOGI(TAG, "MSC ready (restart to return to HID)");
    }
}

void usb_manager_init(void)
{
    s_evt_grp = xEventGroupCreate();
    usb_hid_driver_install();
    xTaskCreatePinnedToCore(usb_manager_task, "usb_mgr", 4096, NULL, 5, NULL, 1);
}

void usb_manager_request_msc(void)
{
    xEventGroupSetBits(s_evt_grp, EVT_REQ_MSC);
}

usb_mode_t usb_manager_get_mode(void)
{
    return s_cur_mode;
}