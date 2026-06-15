#include "waveshare_rgb_lcd_port.h"
#include "usb_hid.h"
#include "ui.h"
#include "ui_img_pool.h"
#include "fs_manager/fs_sd.h"
#include "usb/usb_manager.h"
#include "boot_anim.h"
#include "nvs_manager/nvs_manager.h"

void app_main(void)
{
    heap_caps_malloc_extmem_enable(4096);
    waveshare_esp32_s3_rgb_lcd_init();   /* CH422G 在這裡初始化，EXIO4 拉高 */
    nvs_manager_init();
    fs_sd_init();
    ui_preload_start();   /* load config, kick off background preload task */
    boot_anim_play();     /* animation plays while preload task runs */
    ui_preload_wait();
    usb_manager_init();   /* preload task continues in background after this */

    if (lvgl_port_lock(-1)) {
        my_ui_init();
        lvgl_port_unlock();
    }
}