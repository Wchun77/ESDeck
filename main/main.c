#include "waveshare_rgb_lcd_port.h"
#include "usb_hid.h"
#include "ui.h"
#include "ui_deck.h"
#include "ui_ota_dialog.h"
#include "fs_manager/fs_sd.h"
#include "usb/usb_manager.h"
#include "ble/ble_manager.h"
#include "boot_anim.h"
#include "nvs_manager/nvs_manager.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

void app_main(void)
{
    heap_caps_malloc_extmem_enable(4096);
    waveshare_esp32_s3_rgb_lcd_init();   /* CH422G 在這裡初始化，EXIO4 拉高 */

    /* esp_new_jpeg 解碼器第一次初始化有明顯耗時(實測約1秒),提早在背景
     * 跟下面的nvs/SD卡初始化重疊進行,boot_anim_play()真正要畫第一幀
     * JPEG時就不用再等這筆一次性成本。 */
    boot_anim_prewarm_jpeg();

    nvs_manager_init();
    fs_sd_init();

    ui_deck_preload_start();   /* load config, kick off background preload task */

    /* icon背景預載的task優先權是3,如果跟boot_anim同時跑,會搶走main
     * task的CPU,讓JPEG開機動畫的解碼被拖慢(實測從~200ms/幀拖到
     * ~780ms/幀)。與其把兩者完全序列化(那樣IMG快取log就不會在動畫
     * 播放期間出現,總開機時間變成兩段相加),這裡改成暫時把main task
     * 自己的優先權拉到比預載task更高,讓動畫解碼在CPU競爭時一定贏,
     * 但預載task還是能利用動畫這邊等待SD I/O、frame pacing delay的
     * 空檔繼續跑,兩者維持平行、只是動畫優先,播完動畫就把優先權還原。 */
    UBaseType_t main_task_prio = uxTaskPriorityGet(NULL);
    vTaskPrioritySet(NULL, main_task_prio + 3);
    boot_anim_play();     /* animation plays while preload task runs */
    vTaskPrioritySet(NULL, main_task_prio);

    /* If a valid update.bin sits on the SD card, ask the user before
     * touching anything. Blocks until answered; "Yes" reboots the
     * device on success and never returns. */
    ui_ota_check_and_prompt();

    ui_deck_preload_wait();
    ESP_LOGI("MAIN", "before usb/ble init - PSRAM free: %d B",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
             
    usb_manager_init();   /* preload task continues in background after this */
    ble_manager_init();   /* host stack up, advertising stays off until the
                            * Settings Bluetooth switch turns it on */
    ESP_LOGI("MAIN", "after usb/ble init - PSRAM free: %d B",
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    if (lvgl_port_lock(-1)) {
        my_ui_init();
        lvgl_port_unlock();
    }
}