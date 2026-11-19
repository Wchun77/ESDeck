#include "usb_msc.h"
#include "tinyusb_msc.h"
#include "fs_manager/fs_sd.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "stdlib.h"

static const char *TAG = "USB_MSC";

#define TUSB_MSC_DESC_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static tinyusb_msc_storage_handle_t s_sd_hdl = NULL;
static usb_msc_eject_cb_t           s_eject_cb = NULL;
static bool                         s_ejected  = false;

void usb_msc_set_eject_cb(usb_msc_eject_cb_t cb) { s_eject_cb = cb; }

static void restart_timer_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

/* esp_tinyusb hands the card back to the app when the host sends SCSI
 * START STOP UNIT with eject (Windows "Eject"), and when the cable is
 * pulled (tud_umount_cb). Either way the PC is done: every event after
 * MOUNT_START means that hand-back happened (mounted, failed, or no FAT
 * found -- formatting is off, see do_not_format below). */
static void msc_event_cb(tinyusb_msc_storage_handle_t handle, tinyusb_msc_event_t *event, void *arg)
{
    (void)handle; (void)arg;
    if (event->id == TINYUSB_MSC_EVENT_MOUNT_START || s_ejected) return;
    s_ejected = true;
    ESP_LOGI(TAG, "drive ejected by host (event %d), restarting", (int)event->id);

    if (s_eject_cb) {
        s_eject_cb();
        return;
    }
    /* no UI hook: restart a moment later, after this SCSI command completes */
    const esp_timer_create_args_t args = { .callback = restart_timer_cb, .name = "msc_rst" };
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) esp_timer_start_once(t, 500 * 1000);
    else esp_restart();
}

static const uint8_t s_config_desc[] = {
    TUD_CONFIG_DESCRIPTOR(1, 1, 0, TUSB_MSC_DESC_TOTAL_LEN, 0, 100),
    TUD_MSC_DESCRIPTOR(0, 0, 0x02, 0x82, 64),
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
    .idProduct          = 0x4005,
    .bcdDevice          = 0x0100,
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

const tusb_desc_device_t *usb_msc_get_device_desc(void)
{
    return &s_device_desc;
}

const uint8_t *usb_msc_get_config_desc(void)
{
    return s_config_desc;
}

void usb_msc_class_init(void)
{
    const tinyusb_msc_driver_config_t drv_cfg = {
        .callback     = msc_event_cb,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_msc_install_driver(&drv_cfg));

    sdmmc_card_t *card = fs_sd_get_card();
    if (card == NULL) {
        ESP_LOGW(TAG, "SD not available, MSC has no LUN");
        return;
    }
    ESP_LOGI(TAG, "SD card: %s, %lluMB", card->cid.name,
             (uint64_t)card->csd.capacity * card->csd.sector_size / (1024 * 1024));

    const tinyusb_msc_storage_config_t sd_cfg = {
        .medium.card = card,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
        /* On eject esp_tinyusb mounts the card for the app, and by default
         * formats it if no FAT is found. Never format the user's card. */
        .fat_fs.do_not_format = true,
    };
    esp_err_t err = tinyusb_msc_new_storage_sdmmc(&sd_cfg, &s_sd_hdl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD storage init failed: %s", esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG, "SD LUN ready");
}

void usb_msc_class_deinit(void)
{
    if (s_sd_hdl) {
        tinyusb_msc_delete_storage(s_sd_hdl);
        s_sd_hdl = NULL;
    }
    tinyusb_msc_uninstall_driver();
    ESP_LOGI(TAG, "MSC class deinit done");
}
