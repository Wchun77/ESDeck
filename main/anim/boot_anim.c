#include "boot_anim.h"
#include "waveshare_rgb_lcd_port.h"
#include "lvgl_port.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"
#include "fs_manager/fs_sd.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#define TAG  "[BOOT_ANIM]"

#define SCREEN_W  800
#define SCREEN_H  480
#define CENTER_X  (SCREEN_W / 2)
#define CENTER_Y  (SCREEN_H / 2)

/* 每幀JPEG檔案大小上限,超過就跳過該幀(留很多餘裕,壓縮後的幀通常只有幾十KB) */
#define BOOT_ANIM_MAX_JPEG_SIZE  (256 * 1024)

static lv_obj_t *s_ring1    = NULL;
static lv_obj_t *s_ring2    = NULL;
static lv_obj_t *s_ring3    = NULL;
static lv_obj_t *s_label    = NULL;
static lv_obj_t *s_sub      = NULL;
static lv_obj_t *s_bar      = NULL;
static lv_obj_t *s_bar_bg   = NULL;
static lv_obj_t *s_overlay  = NULL;

/* -----------------------------------------------------------------------
 * Animation callbacks
 * ----------------------------------------------------------------------- */
static void ring_size_cb(void *obj, int32_t v)
{
    lv_obj_set_size((lv_obj_t *)obj, v, v);
    lv_obj_center((lv_obj_t *)obj);
}

static void ring_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_border_opa((lv_obj_t *)obj, v, 0);
}

static void label_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_text_opa((lv_obj_t *)obj, v, 0);
}

static void bar_width_cb(void *obj, int32_t v)
{
    lv_obj_set_width((lv_obj_t *)obj, v);
}

static void overlay_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, v, 0);
}

/* -----------------------------------------------------------------------
 * Helper: create a ring (circle outline)
 * ----------------------------------------------------------------------- */
static lv_obj_t *create_ring(lv_obj_t *parent, int size, lv_color_t color,
                               int border_w, lv_opa_t opa)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_set_size(obj, size, size);
    lv_obj_center(obj);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(obj, color, 0);
    lv_obj_set_style_border_width(obj, border_w, 0);
    lv_obj_set_style_border_opa(obj, opa, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}

/* -----------------------------------------------------------------------
 * Build the boot screen (call inside lvgl lock)
 * ----------------------------------------------------------------------- */
static void build_boot_screen(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* Rings */
    s_ring3 = create_ring(scr, 20,  lv_color_hex(0x00AAFF), 1, LV_OPA_30);
    s_ring2 = create_ring(scr, 20,  lv_color_hex(0x00CCFF), 2, LV_OPA_60);
    s_ring1 = create_ring(scr, 20,  lv_color_hex(0x00EEFF), 3, LV_OPA_COVER);

    /* Main label */
    s_label = lv_label_create(scr);
    lv_label_set_text(s_label, "DECK");
    lv_obj_set_style_text_font(s_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_label, lv_color_hex(0x00EEFF), 0);
    lv_obj_set_style_text_opa(s_label, LV_OPA_TRANSP, 0);
    lv_obj_align(s_label, LV_ALIGN_CENTER, 0, -20);

    /* Sub label */
    s_sub = lv_label_create(scr);
    lv_label_set_text(s_sub, "STREAM CONTROLLER");
    lv_obj_set_style_text_font(s_sub, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_sub, lv_color_hex(0x4488AA), 0);
    lv_obj_set_style_text_opa(s_sub, LV_OPA_TRANSP, 0);
    lv_obj_align(s_sub, LV_ALIGN_CENTER, 0, 25);

    /* Progress bar background */
    s_bar_bg = lv_obj_create(scr);
    lv_obj_set_size(s_bar_bg, 400, 3);
    lv_obj_set_pos(s_bar_bg, (SCREEN_W - 400) / 2, SCREEN_H / 2 + 50); 
    lv_obj_set_style_bg_color(s_bar_bg, lv_color_hex(0x112233), 0);
    lv_obj_set_style_border_width(s_bar_bg, 0, 0);
    lv_obj_set_style_radius(s_bar_bg, 2, 0);
    lv_obj_clear_flag(s_bar_bg, LV_OBJ_FLAG_SCROLLABLE);

    /* Progress bar fill */
    s_bar = lv_obj_create(scr);
    lv_obj_set_size(s_bar, 0, 3);
    lv_obj_set_pos(s_bar, (SCREEN_W - 400) / 2, SCREEN_H / 2 + 50); 
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(0x00EEFF), 0);
    lv_obj_set_style_border_width(s_bar, 0, 0);
    lv_obj_set_style_radius(s_bar, 2, 0);
    lv_obj_clear_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);

    /* Fade-out overlay */
    s_overlay = lv_obj_create(scr);
    lv_obj_set_size(s_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
}

/* -----------------------------------------------------------------------
 * Start all animations (call inside lvgl lock)
 * ----------------------------------------------------------------------- */
static void start_animations(void)
{
    lv_anim_t a;

    /* Ring 1: 20 -> 200px, fade out opacity */
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_ring1);
    lv_anim_set_exec_cb(&a, ring_size_cb);
    lv_anim_set_values(&a, 20, 200);
    lv_anim_set_time(&a, 600);
    lv_anim_set_delay(&a, 0);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_set_exec_cb(&a, ring_opa_cb);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 600);
    lv_anim_start(&a);

    /* Ring 2: delayed, larger */
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_ring2);
    lv_anim_set_exec_cb(&a, ring_size_cb);
    lv_anim_set_values(&a, 20, 280);
    lv_anim_set_time(&a, 700);
    lv_anim_set_delay(&a, 150);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_set_exec_cb(&a, ring_opa_cb);
    lv_anim_set_values(&a, LV_OPA_60, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 700);
    lv_anim_start(&a);

    /* Ring 3: delayed, largest */
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_ring3);
    lv_anim_set_exec_cb(&a, ring_size_cb);
    lv_anim_set_values(&a, 20, 360);
    lv_anim_set_time(&a, 800);
    lv_anim_set_delay(&a, 300);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);

    lv_anim_set_exec_cb(&a, ring_opa_cb);
    lv_anim_set_values(&a, LV_OPA_30, LV_OPA_TRANSP);
    lv_anim_set_time(&a, 800);
    lv_anim_start(&a);

    /* Main label fade in */
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_label);
    lv_anim_set_exec_cb(&a, label_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, 400);
    lv_anim_set_delay(&a, 400);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);

    /* Sub label fade in */
    lv_anim_set_var(&a, s_sub);
    lv_anim_set_delay(&a, 600);
    lv_anim_start(&a);

    /* Progress bar sweep */
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_bar);
    lv_anim_set_exec_cb(&a, bar_width_cb);
    lv_anim_set_values(&a, 0, 400);
    lv_anim_set_time(&a, 800);
    lv_anim_set_delay(&a, 700);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    /* Fade-out overlay */
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_overlay);
    lv_anim_set_exec_cb(&a, overlay_opa_cb);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_time(&a, 300);
    lv_anim_set_delay(&a, 1800);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in);
    lv_anim_start(&a);
}

/* -----------------------------------------------------------------------
 * Custom boot animation (JPEG frame sequence from SD card)
 *
 * Frames: SD_PATH_ASSETS_BOOT/frame_0000.jpg, frame_0001.jpg, ...
 *
 * Decoded with Espressif's esp_new_jpeg (SIMD-accelerated on S3), which
 * is meaningfully faster than the classic TJpgDec-family decoders --
 * LVGL's own SJPG decoder (used for the .jpg backgrounds elsewhere in
 * this app, see ui_img_pool.c) is TJpgDec-based and measured ~400ms for
 * a single 800x480 frame here, far too slow for a smooth animation.
 *
 * This is a completely separate decoder/component from LVGL's SJPG and
 * from the (unrelated, older) esp_jpeg/TJpgDec package that hung
 * indefinitely on this hardware in an earlier attempt -- it does not
 * touch or replace the SJPG decoder LVGL uses for backgrounds/icons,
 * so those keep working exactly as before.
 *
 * This also replaces the original raw-RGB565 .bin approach, which was
 * bottlenecked by SD read throughput (~930KB/s measured vs. ~9MB/s
 * needed for 750KB raw frames at 12fps). JPEG shrinks each frame down
 * to tens of KB so the SD read is no longer the limiting factor.
 * ----------------------------------------------------------------------- */

static jpeg_dec_handle_t s_jpeg_dec     = NULL;
static uint8_t          *s_jpeg_in_buf  = NULL;   /* reused compressed-frame read buffer */

static void jpeg_dec_teardown(void)
{
    if (s_jpeg_dec) {
        jpeg_dec_close(s_jpeg_dec);
        s_jpeg_dec = NULL;
    }
    if (s_jpeg_in_buf) {
        heap_caps_free(s_jpeg_in_buf);
        s_jpeg_in_buf = NULL;
    }
}

static bool jpeg_dec_setup(void)
{
    if (s_jpeg_dec && s_jpeg_in_buf) return true;

    s_jpeg_in_buf = heap_caps_malloc(BOOT_ANIM_MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_jpeg_in_buf) {
        ESP_LOGW(TAG, "PSRAM alloc failed for jpeg input buffer");
        return false;
    }

    jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
    cfg.output_type = JPEG_PIXEL_FORMAT_RGB565_LE;
    /* 顏色若上機後不對(偏色/像反相片),把上面改成 JPEG_PIXEL_FORMAT_RGB565_BE 再試 */

    if (jpeg_dec_open(&cfg, &s_jpeg_dec) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "jpeg_dec_open failed");
        jpeg_dec_teardown();
        return false;
    }
    return true;
}

/* Decode one JPEG file straight into rgb_buf (must be 16-byte aligned and
 * SCREEN_W*SCREEN_H*2 bytes). Returns false (and logs a warning) on any
 * I/O, size-mismatch or decode error -- caller decides whether that's
 * fatal (frame 0) or just a dropped frame (mid-sequence). */
static bool decode_jpeg_frame(const char *sd_path, uint8_t *rgb_buf)
{
    if (!jpeg_dec_setup()) return false;

    FILE *f = fopen(sd_path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "cannot open: %s", sd_path);
        return false;
    }
    size_t n = fread(s_jpeg_in_buf, 1, BOOT_ANIM_MAX_JPEG_SIZE, f);
    bool truncated = (n == BOOT_ANIM_MAX_JPEG_SIZE) && !feof(f);
    fclose(f);

    if (truncated) {
        ESP_LOGW(TAG, "frame exceeds %u byte cap, skipped: %s",
                 (unsigned)BOOT_ANIM_MAX_JPEG_SIZE, sd_path);
        return false;
    }

    jpeg_dec_io_t io = { 0 };
    io.inbuf     = s_jpeg_in_buf;
    io.inbuf_len = (int)n;
    io.outbuf    = rgb_buf;

    jpeg_dec_header_info_t info = { 0 };
    if (jpeg_dec_parse_header(s_jpeg_dec, &io, &info) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "header parse failed: %s", sd_path);
        return false;
    }
    if (info.width != SCREEN_W || info.height != SCREEN_H) {
        ESP_LOGW(TAG, "size mismatch %ux%u (expect %dx%d): %s",
                 info.width, info.height, SCREEN_W, SCREEN_H, sd_path);
        return false;
    }
    if (jpeg_dec_process(s_jpeg_dec, &io) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "decode failed: %s", sd_path);
        return false;
    }
    return true;
}

/* Returns true if a custom animation was found and played (screen is left
 * cleared and backlight already on). Returns false if there's no custom
 * animation on the SD card (or PSRAM/decoding failed on frame 0), in which
 * case the caller should fall back to the built-in procedural animation. */
static bool play_custom_boot_anim(void)
{
    if (!fs_sd_status()) return false;

    char path[320];
    snprintf(path, sizeof(path), "%s/frame_0000.jpg", SD_PATH_ASSETS_BOOT);

    FILE *probe = fopen(path, "rb");
    if (!probe) return false;   /* 沒放自訂動畫,交給原本的程序動畫 */
    fclose(probe);

    /* esp_new_jpeg 建議輸出buffer要16 byte對齊 */
    uint8_t *rgb_buf = heap_caps_aligned_alloc(16, SCREEN_W * SCREEN_H * 2, MALLOC_CAP_SPIRAM);
    if (!rgb_buf) {
        ESP_LOGW(TAG, "PSRAM alloc failed, falling back to built-in animation");
        return false;
    }

    /* 診斷用:量測第一幀實際解碼耗時 */
    TickType_t decode0_start = xTaskGetTickCount();
    bool frame0_ok = decode_jpeg_frame(path, rgb_buf);
    uint32_t decode0_ms = (xTaskGetTickCount() - decode0_start) * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "frame 0 decode: %s, %u ms",
             frame0_ok ? "OK" : "FAILED", (unsigned)decode0_ms);

    if (!frame0_ok) {
        heap_caps_free(rgb_buf);
        jpeg_dec_teardown();
        return false;
    }

    static lv_img_dsc_t dsc;   /* static: lv_img_set_src keeps a pointer to this */
    dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
    dsc.header.always_zero = 0;
    dsc.header.w           = SCREEN_W;
    dsc.header.h           = SCREEN_H;
    dsc.data_size          = SCREEN_W * SCREEN_H * 2;
    dsc.data               = rgb_buf;

    lv_obj_t *img = NULL;
    if (!lvgl_port_lock(-1)) {
        heap_caps_free(rgb_buf);
        jpeg_dec_teardown();
        return false;
    }
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    img = lv_img_create(scr);
    lv_img_set_src(img, &dsc);
    lv_obj_center(img);
    lvgl_port_unlock();

    wavesahre_rgb_lcd_bl_on();   /* 第一幀已經畫出來了,這時才開背光 */

    const TickType_t frame_ticks = pdMS_TO_TICKS(1000 / BOOT_ANIM_CUSTOM_FPS);
    const TickType_t anim_start  = xTaskGetTickCount();
    int frame_idx = 1;

    for (;; frame_idx++) {
        snprintf(path, sizeof(path), "%s/frame_%04d.jpg", SD_PATH_ASSETS_BOOT, frame_idx);
        FILE *probe_next = fopen(path, "rb");
        if (!probe_next) break;   /* 讀不到下一幀,動畫播完 */
        fclose(probe_next);

        TickType_t frame_start = xTaskGetTickCount();

        if (decode_jpeg_frame(path, rgb_buf)) {
            if (lvgl_port_lock(-1)) {
                lv_img_cache_invalidate_src(&dsc);
                lv_img_set_src(img, &dsc);
                lvgl_port_unlock();
            }
        }
        /* 解碼失敗就跳過這一幀,不中斷整段動畫 */

        TickType_t elapsed = xTaskGetTickCount() - frame_start;
        if (elapsed < frame_ticks) {
            vTaskDelay(frame_ticks - elapsed);
        }
    }

    uint32_t total_ms = (xTaskGetTickCount() - anim_start) * portTICK_PERIOD_MS;
    if (total_ms > 0) {
        ESP_LOGI(TAG, "%d frames in %u ms (~%.1f fps actual, target %d fps)",
                 frame_idx, (unsigned)total_ms,
                 frame_idx * 1000.0f / total_ms, BOOT_ANIM_CUSTOM_FPS);
    }

    vTaskDelay(pdMS_TO_TICKS(200));

    if (lvgl_port_lock(-1)) {
        lv_obj_clean(lv_scr_act());
        lvgl_port_unlock();
    }

    heap_caps_free(rgb_buf);
    jpeg_dec_teardown();
    return true;
}

/* -----------------------------------------------------------------------
 * Public entry point
 * ----------------------------------------------------------------------- */
void boot_anim_play(void)
{
    if (play_custom_boot_anim()) return;

    /* 沒有自訂動畫(或PSRAM/解碼失敗) -> 用原本內建的程序動畫 */
    if (!lvgl_port_lock(-1)) return;
    build_boot_screen();
    start_animations();
    lvgl_port_unlock();

    vTaskDelay(pdMS_TO_TICKS(150));  // 等第一幀畫完
    wavesahre_rgb_lcd_bl_on();      // 這時才開背光

    vTaskDelay(pdMS_TO_TICKS(2150));

    if (!lvgl_port_lock(-1)) return;
    lv_obj_clean(lv_scr_act());
    lvgl_port_unlock();
}