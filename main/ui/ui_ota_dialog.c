#include "ui_ota_dialog.h"
#include "ota_manager/ota_manager.h"
#include "lvgl_port.h"
#include "waveshare_rgb_lcd_port.h"   /* backlight on/off */
#include "lvgl.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <stdio.h>
#include <string.h>

#define TAG       "[OTA_UI]"
#define SCREEN_W  800
#define SCREEN_H  480

static SemaphoreHandle_t s_done_sem  = NULL;
static ota_scan_result_t s_result;

static lv_obj_t *s_dim        = NULL;
static lv_obj_t *s_box        = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_bar_bg     = NULL;
static lv_obj_t *s_bar        = NULL;
static lv_obj_t *s_pct_lbl    = NULL;

/* -----------------------------------------------------------------------
 * Confirm dialog
 * ----------------------------------------------------------------------- */
static void close_confirm_dialog(void)
{
    if (s_dim) {
        lv_obj_del(s_dim);   /* deletes s_box as a child too */
        s_dim = NULL;
        s_box = NULL;
    }
}

static void on_confirm_no(lv_event_t *e)
{
    close_confirm_dialog();
    xSemaphoreGive(s_done_sem);
}

/* Forward decl */
static void start_update_flow(void);

static void on_confirm_yes(lv_event_t *e)
{
    close_confirm_dialog();
    start_update_flow();
}

static void build_confirm_dialog(void)
{
    lv_obj_t *scr = lv_scr_act();

    s_dim = lv_obj_create(scr);
    lv_obj_set_size(s_dim, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_dim, 0, 0);
    lv_obj_set_style_bg_color(s_dim, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_dim, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_dim, 0, 0);
    lv_obj_set_style_radius(s_dim, 0, 0);
    lv_obj_clear_flag(s_dim, LV_OBJ_FLAG_SCROLLABLE);

    s_box = lv_obj_create(s_dim);
    lv_obj_set_size(s_box, 480, 220);
    lv_obj_center(s_box);
    lv_obj_set_style_bg_color(s_box, lv_color_hex(0x1e1e1e), 0);
    lv_obj_set_style_border_color(s_box, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(s_box, 1, 0);
    lv_obj_set_style_radius(s_box, 12, 0);
    lv_obj_set_style_pad_all(s_box, 20, 0);
    lv_obj_clear_flag(s_box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_box);
    lv_label_set_text(title, "Firmware Update Found");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 0, 0);

    char msg[256 + 64];
    snprintf(msg, sizeof(msg), "%.255s\n(%u KB)\n\nInstall this update now?",
             s_result.filename, (unsigned)(s_result.size / 1024));

    lv_obj_t *body = lv_label_create(s_box);
    lv_label_set_text(body, msg);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(body, lv_color_hex(0xcccccc), 0);
    lv_obj_set_width(body, 440);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_align(body, LV_ALIGN_TOP_LEFT, 0, 40);

    lv_obj_t *btn_no = lv_btn_create(s_box);
    lv_obj_set_size(btn_no, 140, 44);
    lv_obj_align(btn_no, LV_ALIGN_BOTTOM_RIGHT, -160, 0);
    lv_obj_set_style_bg_color(btn_no, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_color(btn_no, lv_color_hex(0x4a4a4a), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn_no, 0, 0);
    lv_obj_set_style_radius(btn_no, 6, 0);
    lv_obj_add_event_cb(btn_no, on_confirm_no, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(btn_no, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_t *lbl_no = lv_label_create(btn_no);
    lv_label_set_text(lbl_no, "Not Now");
    lv_obj_set_style_text_color(lbl_no, lv_color_hex(0xcccccc), 0);
    lv_obj_center(lbl_no);

    lv_obj_t *btn_yes = lv_btn_create(s_box);
    lv_obj_set_size(btn_yes, 140, 44);
    lv_obj_align(btn_yes, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    lv_obj_set_style_bg_color(btn_yes, lv_color_hex(0x0055cc), 0);
    lv_obj_set_style_bg_color(btn_yes, lv_color_hex(0x0066ee), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn_yes, 0, 0);
    lv_obj_set_style_radius(btn_yes, 6, 0);
    lv_obj_add_event_cb(btn_yes, on_confirm_yes, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(btn_yes, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_t *lbl_yes = lv_label_create(btn_yes);
    lv_label_set_text(lbl_yes, "Update");
    lv_obj_set_style_text_color(lbl_yes, lv_color_hex(0xffffff), 0);
    lv_obj_center(lbl_yes);
}

/* -----------------------------------------------------------------------
 * Progress screen
 * ----------------------------------------------------------------------- */
static void build_progress_screen(void)
{
    lv_obj_t *scr = lv_scr_act();

    s_dim = lv_obj_create(scr);
    lv_obj_set_size(s_dim, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_dim, 0, 0);
    lv_obj_set_style_bg_color(s_dim, lv_color_hex(0x111111), 0);
    lv_obj_set_style_bg_opa(s_dim, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_dim, 0, 0);
    lv_obj_set_style_radius(s_dim, 0, 0);
    lv_obj_clear_flag(s_dim, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_dim);
    lv_label_set_text(title, "Updating Firmware");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -60);

    s_status_lbl = lv_label_create(s_dim);
    lv_label_set_text(s_status_lbl, "Preparing update (erasing flash)...");
    lv_obj_set_style_text_font(s_status_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0x888888), 0);
    lv_obj_align(s_status_lbl, LV_ALIGN_CENTER, 0, -25);

    /* Bar stays hidden during the erase phase -- there's nothing meaningful
     * to animate yet, and an idle/static screen hides that one unavoidable
     * pause far better than a bar that looks stuck at 0%. */
    lv_obj_t *bar_bg = lv_obj_create(s_dim);
    lv_obj_set_size(bar_bg, 400, 8);
    lv_obj_align(bar_bg, LV_ALIGN_CENTER, 0, 10);
    lv_obj_set_style_bg_color(bar_bg, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_width(bar_bg, 0, 0);
    lv_obj_set_style_radius(bar_bg, 4, 0);
    lv_obj_clear_flag(bar_bg, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar_bg, LV_OBJ_FLAG_HIDDEN);
    s_bar_bg = bar_bg;

    s_bar = lv_bar_create(bar_bg);
    lv_obj_set_size(s_bar, 400, 8);
    lv_obj_center(s_bar);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x00aaff), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 4, LV_PART_INDICATOR);
    lv_bar_set_range(s_bar, 0, 100);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);

    s_pct_lbl = lv_label_create(s_dim);
    lv_label_set_text(s_pct_lbl, "0%");
    lv_obj_set_style_text_font(s_pct_lbl, &lv_font_montserrat_14, 0);
    lv_obj_set_style_text_color(s_pct_lbl, lv_color_hex(0x00aaff), 0);
    lv_obj_align(s_pct_lbl, LV_ALIGN_CENTER, 0, 35);
    lv_obj_add_flag(s_pct_lbl, LV_OBJ_FLAG_HIDDEN);
}

/* -----------------------------------------------------------------------
 * Called once erase is done -- switch from the static "preparing" message
 * to the live progress bar. Also runs on the worker task.
 * ----------------------------------------------------------------------- */
static void on_ota_erase_done(void *user_data)
{
    if (!lvgl_port_lock(-1)) return;

    if (s_status_lbl) lv_label_set_text(s_status_lbl, "Please do not power off...");
    if (s_bar_bg)      lv_obj_clear_flag(s_bar_bg, LV_OBJ_FLAG_HIDDEN);
    if (s_pct_lbl)     lv_obj_clear_flag(s_pct_lbl, LV_OBJ_FLAG_HIDDEN);

    lvgl_port_unlock();
}

/* -----------------------------------------------------------------------
 * ota_apply_update progress callback -- called from the worker task,
 * NOT the LVGL task, so it must lock before touching LVGL objects.
 * ----------------------------------------------------------------------- */
static void on_ota_progress(uint8_t percent, void *user_data)
{
    if (!lvgl_port_lock(pdMS_TO_TICKS(200))) return;

    if (s_bar)     lv_bar_set_value(s_bar, percent, LV_ANIM_OFF);
    if (s_pct_lbl) {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u%%", percent);
        lv_label_set_text(s_pct_lbl, buf);
    }

    lvgl_port_unlock();
}

/* -----------------------------------------------------------------------
 * Worker task
 * ----------------------------------------------------------------------- */
static void ota_apply_task(void *arg)
{
    /* Make sure the "Updating Firmware" screen is fully on the panel
     * before any flash erase/write starts: flash and PSRAM share the MSPI
     * bus, so once erasing begins the LCD can't be fed reliably and a
     * half-switched frame (old dialog + new screen) would stay visible.
     * This is a plain task, so it may wait on lvgl_port_task (an
     * lv_refr_now() from the LVGL event callback only filled one of the
     * two direct-mode buffers). */
    lvgl_port_wait_render_idle(1000);
    vTaskDelay(pdMS_TO_TICKS(100));

    bool ok = ota_apply_update(s_result.path, s_result.size,
                               on_ota_erase_done, on_ota_progress, NULL);

    if (ok) {
        if (lvgl_port_lock(-1)) {
            lv_label_set_text(s_status_lbl, "Done -- rebooting...");
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_restart();
        /* not reached */
    }

    ESP_LOGE(TAG, "OTA update failed, keeping file for retry on next boot");
    if (lvgl_port_lock(-1)) {
        lv_label_set_text(s_status_lbl, "Update failed. Will retry on next boot.");
        lv_obj_set_style_text_color(s_status_lbl, lv_color_hex(0xff6666), 0);
        lvgl_port_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(2500));

    if (lvgl_port_lock(-1)) {
        if (s_dim) {
            lv_obj_del(s_dim);
            s_dim = NULL;
        }
        s_status_lbl = NULL;
        s_bar_bg      = NULL;
        s_bar         = NULL;
        s_pct_lbl     = NULL;
        lvgl_port_unlock();
    }

    xSemaphoreGive(s_done_sem);
    vTaskDelete(NULL);
}

static void start_update_flow(void)
{
    build_progress_screen();
    /* Runs in an LVGL event callback: just build the screen and return so
     * lvgl_port_task renders it; ota_apply_task waits for that before it
     * touches flash. */
    xTaskCreate(ota_apply_task, "ota_apply", 8192, NULL, 3, NULL);
}

/* -----------------------------------------------------------------------
 * Public
 * ----------------------------------------------------------------------- */
void ui_ota_check_and_prompt(void)
{
    ota_scan_result_t r = ota_check_update();
    if (!r.found) return;

    ESP_LOGI(TAG, "Update file found: %s (%u bytes)", r.filename, (unsigned)r.size);
    s_result = r;

    s_done_sem = xSemaphoreCreateBinary();
    if (!s_done_sem) {
        ESP_LOGE(TAG, "Failed to create semaphore");
        return;
    }

    /* Runs before the boot animation, so the backlight is still off
     * (waveshare_esp32_s3_rgb_lcd_init() keeps it off until something has
     * been drawn, to hide the uninitialized framebuffer). Black screen +
     * dialog first, let LVGL render it, then light up. */
    if (!lvgl_port_lock(-1)) {
        vSemaphoreDelete(s_done_sem);
        s_done_sem = NULL;
        return;
    }
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    build_confirm_dialog();
    lvgl_port_unlock();

    /* Same as boot_anim's custom animation: wait for LVGL to really draw
     * it, plus a margin -- the VSYNC notify fires per bounce-buffer chunk,
     * not per whole frame, so "idle" isn't 100% "on the panel" yet. A
     * fixed short delay wasn't enough: the backlight came on over a
     * still-white framebuffer for a moment. */
    lvgl_port_wait_render_idle(1000);
    vTaskDelay(pdMS_TO_TICKS(100));
    wavesahre_rgb_lcd_bl_on();

    xSemaphoreTake(s_done_sem, portMAX_DELAY);
    vSemaphoreDelete(s_done_sem);
    s_done_sem = NULL;

    /* Declined (or the update failed): dialog is gone, screen is black.
     * Backlight off again so boot_anim_play() starts from the same state
     * as a boot without an update file -- it turns the backlight on once
     * its own first frame is drawn. */
    vTaskDelay(pdMS_TO_TICKS(100));
    wavesahre_rgb_lcd_bl_off();
}
