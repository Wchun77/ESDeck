#include "usb_msc.h"
#include "tinyusb_msc.h"
#include "fs_manager/fs_flash.h"
#include "fs_manager/fs_sd.h"
#include "esp_partition.h"
#include "wear_levelling.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "esp_log.h"
#include "stdlib.h"

static const char *TAG = "USB_MSC";

#define PARTITION_LABEL  "storage"

#define TUSB_MSC_DESC_TOTAL_LEN  (TUD_CONFIG_DESC_LEN + TUD_MSC_DESC_LEN)

static tinyusb_msc_storage_handle_t s_flash_hdl  = NULL;
static tinyusb_msc_storage_handle_t s_sd_hdl     = NULL;
static wl_handle_t                  s_wl_handle   = WL_INVALID_HANDLE;

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
        .callback     = NULL,
        .callback_arg = NULL,
    };
    ESP_ERROR_CHECK(tinyusb_msc_install_driver(&drv_cfg));

    /* Flash LUN */
    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA,
        ESP_PARTITION_SUBTYPE_DATA_FAT,
        PARTITION_LABEL);
    ESP_ERROR_CHECK(wl_mount(part, &s_wl_handle));

    const tinyusb_msc_storage_config_t flash_cfg = {
        .medium.wl_handle = s_wl_handle,
        .mount_point      = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    ESP_ERROR_CHECK(tinyusb_msc_new_storage_spiflash(&flash_cfg, &s_flash_hdl));
    ESP_LOGI(TAG, "Flash LUN ready");

    /* SD LUN - reuse card handle from fs_sd */
    sdmmc_card_t *card = fs_sd_get_card();
    if (card == NULL) {
        ESP_LOGW(TAG, "SD not available, skipping SD LUN");
        return;
    }
    ESP_LOGI(TAG, "SD card: %s, %lluMB", card->cid.name, (uint64_t)card->csd.capacity * card->csd.sector_size / (1024 * 1024));

    const tinyusb_msc_storage_config_t sd_cfg = {
        .medium.card = card,
        .mount_point = TINYUSB_MSC_STORAGE_MOUNT_USB,
    };
    esp_err_t err = tinyusb_msc_new_storage_sdmmc(&sd_cfg, &s_sd_hdl);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD storage init failed: %s, skipping SD LUN", esp_err_to_name(err));
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
    if (s_flash_hdl) {
        tinyusb_msc_delete_storage(s_flash_hdl);
        s_flash_hdl = NULL;
    }
    tinyusb_msc_uninstall_driver();

    if (s_wl_handle != WL_INVALID_HANDLE) {
        wl_unmount(s_wl_handle);
        s_wl_handle = WL_INVALID_HANDLE;
    }
    ESP_LOGI(TAG, "MSC class deinit done");
}
