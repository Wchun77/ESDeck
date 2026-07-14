#include "waveshare_rgb_lcd_port.h"
#include "usb_hid.h"
#include "ui.h"
#include "ui_img_pool.h"
#include "ui_ota_dialog.h"
#include "fs_manager/fs_sd.h"
#include "usb/usb_manager.h"
#include "boot_anim.h"
#include "nvs_manager/nvs_manager.h"

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

    /* boot_anim_play() 要在 ui_preload_start() 之前執行:preload的背景
     * task優先權比較高(3),如果跟boot_anim同時跑,會一直搶走CPU,讓
     * JPEG開機動畫的解碼被拖慢(實測從~200ms/幀拖到~780ms/幀)。開機
     * 動畫播完之後再讓icon預載開始跑,兩邊就不會互搶。 */
    boot_anim_play();
    ui_preload_start();   /* load config, kick off background preload task */

    /* If a valid update.bin sits on the SD card, ask the user before
     * touching anything. Blocks until answered; "Yes" reboots the
     * device on success and never returns. */
    ui_ota_check_and_prompt();

    ui_preload_wait();
    usb_manager_init();   /* preload task continues in background after this */

    if (lvgl_port_lock(-1)) {
        my_ui_init();
        lvgl_port_unlock();
    }
}