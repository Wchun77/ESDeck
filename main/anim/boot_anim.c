#include "boot_anim.h"
#include "waveshare_rgb_lcd_port.h"
#include "lvgl_port.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "app_config.h"
#include "fs_manager/fs_sd.h"
#include "nvs_manager/nvs_manager.h"
#include "ui_config.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_jpeg_dec.h"
#include "esp_partition.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>

#define TAG  "[BOOT_ANIM]"

#define SCREEN_W  800
#define SCREEN_H  480
#define CENTER_X  (SCREEN_W / 2)
#define CENTER_Y  (SCREEN_H / 2)

/* 每幀JPEG檔案大小上限,超過就跳過該幀(留很多餘裕,壓縮後的幀通常只有幾十KB) */
#define BOOT_ANIM_MAX_JPEG_SIZE  (256 * 1024)

/* look-ahead預讀深度:背景task最多預先讀幾幀進緩衝區。見下方
 * prefetch_task()註解 -- 4幀 * 256KB上限 = 1MB PSRAM,遠低於整包預讀
 * 需要的容量,而且不需要等全部讀完才能開始播放。 */
#define BOOT_ANIM_PREFETCH_DEPTH  4

/* 解碼輸出buffer雙緩衝:讓「解碼下一幀」跟「上一幀還在被面板掃描/
 * 渲染」這兩件事重疊,而不是每幀都序列化等render完才能開始解碼下一
 * 幀。見play_custom_boot_anim()裡的使用方式。 */
#define BOOT_ANIM_DECODE_BUFFERS  2

/* flash常駐快取:見下方"Flash cache"區塊註解。分區表(partitions.csv)
 * 裡對應的分區label/subtype。 */
#define BOOT_ANIM_FLASH_LABEL           "boot_anim"
#define BOOT_ANIM_FLASH_SUBTYPE         0x40
#define BOOT_ANIM_FLASH_MAGIC           0x544F4F42u   /* 'BOOT' */
#define BOOT_ANIM_FLASH_MAX_FRAMES      200
#define BOOT_ANIM_FLASH_HEADER_RESERVED 4096          /* 一個flash sector,存header+索引 */

/* 等LVGL的渲染task(lvgl_port_task)把剛剛set_src的畫面真的畫完。
 *
 * 不能直接呼叫 lv_refr_now() 自己畫(這個專案的LVGL避免撕裂模式是
 * direct-mode雙緩衝,flush_callback裡等vsync中斷通知的邏輯寫死只發
 * 給lvgl_port_task,別的task呼叫lv_refr_now會卡死),所以改成輪詢
 * lv_disp_t的內部狀態,等它自己非同步畫完為止 -- 不是用猜的固定時間。
 * timeout_ms 是安全上限,萬一渲染真的卡住也不會整個卡死在這裡。 */
static void wait_for_render_idle(uint32_t timeout_ms)
{
    lv_disp_t *disp = lv_disp_get_default();
    if (!disp) return;

    TickType_t start = xTaskGetTickCount();
    TickType_t limit = pdMS_TO_TICKS(timeout_ms);

    for (;;) {
        bool idle = false;
        if (lvgl_port_lock(20)) {
            idle = (disp->inv_p == 0) && !disp->rendering_in_progress;
            lvgl_port_unlock();
        }
        if (idle) return;

        if ((xTaskGetTickCount() - start) >= limit) {
            ESP_LOGW(TAG, "wait_for_render_idle: timeout after %u ms", (unsigned)timeout_ms);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

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
 * Multiple animation sets can live side by side under SD_PATH_ASSETS_BOOT,
 * one subfolder each, e.g. assets/boot/xmas/frame_0000.jpg, frame_0001.jpg,
 * ... The active set's folder name is stored in NVS (CFG_NVS_KEY_BOOT_ANIM,
 * settings menu -> Boot Animation); "none"/unset disables the custom
 * animation entirely and falls back to the built-in procedural one below.
 * Keeping each set in its own folder also means swapping animations never
 * mixes leftover frames from a previous, differently-sized set.
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

static void jpeg_dec_teardown(void)
{
    if (s_jpeg_dec) {
        jpeg_dec_close(s_jpeg_dec);
        s_jpeg_dec = NULL;
    }
}

static bool jpeg_dec_setup(void)
{
    if (s_jpeg_dec) return true;

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

/* ---- 暖機:讓 jpeg_dec_open() 的一次性初始化成本跟開機其他工作重疊 ---- */

static SemaphoreHandle_t s_jpeg_prewarm_done = NULL;

static void jpeg_prewarm_task(void *arg)
{
    TickType_t t0 = xTaskGetTickCount();
    jpeg_dec_setup();
    ESP_LOGI(TAG, "jpeg decoder prewarm: %u ms",
             (unsigned)((xTaskGetTickCount() - t0) * portTICK_PERIOD_MS));
    xSemaphoreGive(s_jpeg_prewarm_done);
    vTaskDelete(NULL);
}

void boot_anim_prewarm_jpeg(void)
{
    if (s_jpeg_prewarm_done) return;   /* 已經呼叫過 */

    s_jpeg_prewarm_done = xSemaphoreCreateBinary();
    if (!s_jpeg_prewarm_done) return;

    if (xTaskCreate(jpeg_prewarm_task, "jpeg_prewarm", 4096, NULL, 2, NULL) != pdPASS) {
        vSemaphoreDelete(s_jpeg_prewarm_done);
        s_jpeg_prewarm_done = NULL;
    }
}

/* 等暖機task做完(如果有先呼叫 boot_anim_prewarm_jpeg 的話),確保
 * jpeg_dec_setup() 不會在兩個task同時被呼叫到(competing init)。
 * 沒呼叫過 prewarm 的話這裡直接跳過,decode_jpeg_frame 內部的
 * jpeg_dec_setup() 照樣會自己補做初始化,只是變成同步等待。 */
static void jpeg_prewarm_wait(void)
{
    if (!s_jpeg_prewarm_done) return;
    xSemaphoreTake(s_jpeg_prewarm_done, pdMS_TO_TICKS(3000));
    vSemaphoreDelete(s_jpeg_prewarm_done);
    s_jpeg_prewarm_done = NULL;
}

/* Decode already-in-memory JPEG bytes straight into rgb_buf (must be
 * 16-byte aligned and SCREEN_W*SCREEN_H*2 bytes). Shared by both the
 * SD-streaming path (decode_jpeg_frame, below) and the PSRAM-preload
 * path (play_custom_boot_anim) -- the actual esp_new_jpeg call sequence
 * doesn't care whether the compressed bytes came from a fresh fread or
 * from a blob that was loaded up front. */
static bool decode_jpeg_mem(const uint8_t *data, size_t len, uint8_t *rgb_buf)
{
    if (!jpeg_dec_setup()) return false;

    jpeg_dec_io_t io = { 0 };
    io.inbuf     = (uint8_t *)data;
    io.inbuf_len = (int)len;
    io.outbuf    = rgb_buf;

    jpeg_dec_header_info_t info = { 0 };
    if (jpeg_dec_parse_header(s_jpeg_dec, &io, &info) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "header parse failed");
        return false;
    }
    if (info.width != SCREEN_W || info.height != SCREEN_H) {
        ESP_LOGW(TAG, "size mismatch %ux%u (expect %dx%d)",
                 info.width, info.height, SCREEN_W, SCREEN_H);
        return false;
    }
    if (jpeg_dec_process(s_jpeg_dec, &io) != JPEG_ERR_OK) {
        ESP_LOGW(TAG, "decode failed");
        return false;
    }
    return true;
}

/* -----------------------------------------------------------------------
 * Background look-ahead prefetch (producer/consumer ring buffer)
 *
 * Reading one frame at a time synchronously (in the same task that's
 * decoding/displaying) means every frame's SD read collides with
 * img_preload's own SD reads (icons) *and* stalls the visible frame
 * pacing while it waits -- task priority alone can't fix this because
 * it's a hardware bus contention problem, not a CPU scheduling one.
 * Measured effect: fps settled around 5 even after fixing the earlier
 * CPU-contention bug.
 *
 * A first attempt at fixing this read the *entire* animation into one
 * PSRAM blob before showing frame 0 -- that removed SD contention during
 * playback, but pushed all of it into one big upfront blocking burst,
 * so the screen stayed black for several seconds before anything
 * appeared (worse total wait, even though it's technically hidden behind
 * backlight-off).
 *
 * This replaces that with a small ring buffer (BOOT_ANIM_PREFETCH_DEPTH
 * slots) filled continuously by a background task while the main task
 * plays back whatever's already buffered. Only frame 0 needs to be ready
 * before playback starts, so startup latency is back to roughly what it
 * was before any of this -- but as long as the producer stays ahead of
 * playback, decode is fed straight from RAM the same as the old
 * whole-animation preload, so it's still insulated from SD jitter.
 * ----------------------------------------------------------------------- */

typedef struct {
    uint8_t *data;   /* BOOT_ANIM_MAX_JPEG_SIZE bytes, PSRAM */
    size_t   len;    /* 0 means "end of animation" sentinel */
} prefetch_slot_t;

static prefetch_slot_t   s_pf_slots[BOOT_ANIM_PREFETCH_DEPTH];
static SemaphoreHandle_t s_pf_free_sem   = NULL;   /* counts empty slots */
static SemaphoreHandle_t s_pf_filled_sem = NULL;   /* counts ready slots */
static SemaphoreHandle_t s_pf_exited_sem = NULL;   /* producer task exit signal */
static volatile bool     s_pf_stop       = false;
static char               s_pf_anim_dir[288];
static int                s_pf_consume_slot = 0;

static void prefetch_task(void *arg)
{
    int frame_idx = 0;
    int slot = 0;
    char path[320];

    for (;;) {
        xSemaphoreTake(s_pf_free_sem, portMAX_DELAY);
        if (s_pf_stop) break;

        snprintf(path, sizeof(path), "%s/frame_%04d.jpg", s_pf_anim_dir, frame_idx);

        /* 診斷用:量測「開檔+讀取+關檔」實際花多久,用來確認SD I/O
         * 是不是目前fps的主要瓶頸(而不是憑感覺猜)。 */
        TickType_t io_start = xTaskGetTickCount();

        FILE *f = fopen(path, "rb");
        if (!f || s_pf_stop) {
            if (f) fclose(f);
            s_pf_slots[slot].len = 0;   /* sentinel: no more frames */
            xSemaphoreGive(s_pf_filled_sem);
            break;
        }
        size_t n = fread(s_pf_slots[slot].data, 1, BOOT_ANIM_MAX_JPEG_SIZE, f);
        bool truncated = (n == BOOT_ANIM_MAX_JPEG_SIZE) && !feof(f);
        fclose(f);

        uint32_t io_ms = (xTaskGetTickCount() - io_start) * portTICK_PERIOD_MS;
        ESP_LOGI(TAG, "pf read frame %d: %u ms, %u bytes", frame_idx, (unsigned)io_ms, (unsigned)n);

        if (truncated) {
            /* len==0是"動畫已結束"的專用訊號(見prefetch_next),不能拿
             * 來表示"這幀太大"，否則會被誤判成提早結束整段動畫。保留
             * 截斷後的實際位元組數(>0)，讓消費端照常嘗試解碼──不完整
             * 的JPEG理論上會被esp_new_jpeg判斷解碼失敗，走"跳過這幀,
             * 繼續下一幀"那條路徑，而不是誤觸發"動畫結束"。 */
            ESP_LOGW(TAG, "frame exceeds %u byte slot, likely corrupt: %s",
                     (unsigned)BOOT_ANIM_MAX_JPEG_SIZE, path);
        }
        s_pf_slots[slot].len = n;
        xSemaphoreGive(s_pf_filled_sem);

        slot = (slot + 1) % BOOT_ANIM_PREFETCH_DEPTH;
        frame_idx++;
    }

    xSemaphoreGive(s_pf_exited_sem);
    vTaskDelete(NULL);
}

/* Allocates the ring buffer and starts the background producer task.
 * anim_dir must stay valid for the lifetime of the prefetch session
 * (play_custom_boot_anim keeps it on its own stack, which outlives this). */
static bool prefetch_start(const char *anim_dir)
{
    int allocated = 0;
    for (; allocated < BOOT_ANIM_PREFETCH_DEPTH; allocated++) {
        s_pf_slots[allocated].data = heap_caps_malloc(BOOT_ANIM_MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM);
        s_pf_slots[allocated].len  = 0;
        if (!s_pf_slots[allocated].data) {
            ESP_LOGW(TAG, "PSRAM alloc failed for prefetch slot %d", allocated);
            goto fail;
        }
    }

    snprintf(s_pf_anim_dir, sizeof(s_pf_anim_dir), "%s", anim_dir);
    s_pf_consume_slot = 0;
    s_pf_stop = false;

    s_pf_free_sem   = xSemaphoreCreateCounting(BOOT_ANIM_PREFETCH_DEPTH, BOOT_ANIM_PREFETCH_DEPTH);
    s_pf_filled_sem = xSemaphoreCreateCounting(BOOT_ANIM_PREFETCH_DEPTH, 0);
    s_pf_exited_sem = xSemaphoreCreateBinary();
    if (!s_pf_free_sem || !s_pf_filled_sem || !s_pf_exited_sem) {
        ESP_LOGW(TAG, "semaphore alloc failed for prefetch");
        goto fail;
    }

    if (xTaskCreate(prefetch_task, "boot_anim_pf", 4096, NULL, 3, NULL) != pdPASS) {
        ESP_LOGW(TAG, "prefetch task create failed");
        goto fail;
    }
    return true;

fail:
    if (s_pf_free_sem)   { vSemaphoreDelete(s_pf_free_sem);   s_pf_free_sem = NULL; }
    if (s_pf_filled_sem) { vSemaphoreDelete(s_pf_filled_sem); s_pf_filled_sem = NULL; }
    if (s_pf_exited_sem) { vSemaphoreDelete(s_pf_exited_sem); s_pf_exited_sem = NULL; }
    for (int i = 0; i < allocated; i++) {
        heap_caps_free(s_pf_slots[i].data);
        s_pf_slots[i].data = NULL;
    }
    return false;
}

/* Blocks for the next ready slot, decodes it into rgb_buf, and frees the
 * slot back to the producer. *out_missing mirrors decode_jpeg_frame's old
 * contract: true only means "animation is over", not "this frame broke". */
static bool prefetch_next(uint8_t *rgb_buf, bool *out_missing)
{
    *out_missing = false;

    /* 診斷用:量測消費端在這裡真的卡了多久等背景task把資料生出來。
     * 如果這個數字接近0,代表背景task一直領先、緩衝區沒被吃空,瓶頸
     * 在解碼/顯示端;如果這個數字偏大,代表消費端常常在等SD讀取,
     * 瓶頸確實在I/O(可能是跟img_preload搶頻寬)。 */
    TickType_t wait_start = xTaskGetTickCount();
    if (xSemaphoreTake(s_pf_filled_sem, pdMS_TO_TICKS(5000)) != pdTRUE) {
        ESP_LOGW(TAG, "prefetch stalled, aborting animation");
        *out_missing = true;
        return false;
    }
    uint32_t wait_ms = (xTaskGetTickCount() - wait_start) * portTICK_PERIOD_MS;

    int slot = s_pf_consume_slot;
    size_t len = s_pf_slots[slot].len;
    bool ok = false;
    if (len == 0) {
        *out_missing = true;
    } else {
        TickType_t decode_start = xTaskGetTickCount();
        ok = decode_jpeg_mem(s_pf_slots[slot].data, len, rgb_buf);
        uint32_t decode_ms = (xTaskGetTickCount() - decode_start) * portTICK_PERIOD_MS;
        ESP_LOGI(TAG, "consume: waited %u ms for data, decode %u ms",
                 (unsigned)wait_ms, (unsigned)decode_ms);
    }

    xSemaphoreGive(s_pf_free_sem);
    s_pf_consume_slot = (slot + 1) % BOOT_ANIM_PREFETCH_DEPTH;
    return ok;
}

/* Stops the producer task (if still running) and frees the ring buffer.
 * Safe to call even if the animation ended on its own (producer already
 * self-terminated after hitting the "no more frames" sentinel). */
static void prefetch_stop(void)
{
    if (!s_pf_exited_sem) return;   /* never started, or already torn down */

    s_pf_stop = true;
    /* Wake the producer whether it's blocked waiting for a free slot or
     * about to loop back and check the stop flag -- harmless either way,
     * it exits on the very next check. */
    for (int i = 0; i < BOOT_ANIM_PREFETCH_DEPTH; i++) {
        xSemaphoreGive(s_pf_free_sem);
    }
    xSemaphoreTake(s_pf_exited_sem, pdMS_TO_TICKS(3000));

    vSemaphoreDelete(s_pf_free_sem);   s_pf_free_sem   = NULL;
    vSemaphoreDelete(s_pf_filled_sem); s_pf_filled_sem = NULL;
    vSemaphoreDelete(s_pf_exited_sem); s_pf_exited_sem = NULL;

    for (int i = 0; i < BOOT_ANIM_PREFETCH_DEPTH; i++) {
        if (s_pf_slots[i].data) {
            heap_caps_free(s_pf_slots[i].data);
            s_pf_slots[i].data = NULL;
        }
    }
}

/* 讀NVS裡使用者選定的開機動畫子資料夾名稱,同時回傳原始名稱(給flash
 * 快取比對用)跟組好的SD完整路徑。沒設定過、或使用者選的是"none",
 * 回傳false(交給程序動畫)。 */
static bool get_selected_anim(char *name_out, size_t name_out_size,
                               char *dir_out, size_t dir_out_size)
{
    if (!nvs_manager_get_str(CFG_NVS_NAMESPACE, CFG_NVS_KEY_BOOT_ANIM, name_out, name_out_size)) {
        return false;   /* 從沒選過 */
    }
    if (name_out[0] == '\0' || strcmp(name_out, "none") == 0) {
        return false;
    }
    snprintf(dir_out, dir_out_size, "%s/%s", SD_PATH_ASSETS_BOOT, name_out);
    return true;
}

/* -----------------------------------------------------------------------
 * Flash cache: 常駐版本的開機動畫,完全繞開SD卡
 *
 * 實測顯示目前fps的最大瓶頸是SD讀取(平均~118ms/幀,比decode本身的
 * ~85ms還多),原因是每次fopen在FAT檔案系統上的目錄查找/SD卡指令交握
 * 開銷,加上跟img_preload共用SD實體頻寬。這兩個問題flash都沒有:
 * esp_partition_mmap()把flash內容映射成一段記憶體位址,讀取近乎零成本
 * (不用fopen、不用跟任何人搶SD頻寬)。
 *
 * 代價是flash內容不像SD卡檔案那樣隨插即用──使用者換了一套動畫、但
 * flash裡快取的還是舊的那套時,要先花一次時間把新的整套從SD複製進
 * flash(這次一樣要讀SD,不會比較快),之後開機才吃得到flash的好處。
 * 這次複製只在使用者剛換動畫、flash快取還沒跟上時的那次開機發生,而且
 * 整個過程畫面全黑(backlight還沒開、也不建立任何LVGL物件),避免flash
 * 抹寫時CPU短暫停頓的風險跟LVGL渲染同時發生。
 *
 * header寫在分區最前面的一個sector(BOOT_ANIM_FLASH_HEADER_RESERVED),
 * 而且是整個provision流程裡最後一筆寫入──這樣萬一途中斷電,header的
 * magic/name對不上,下次開機自然會判定快取無效、重新provision,不會
 * 讀到寫一半的損毀資料。
 * ----------------------------------------------------------------------- */

typedef struct {
    uint32_t magic;
    char     name[64];
    uint32_t frame_count;
    uint32_t frame_len[BOOT_ANIM_FLASH_MAX_FRAMES];
} boot_anim_flash_header_t;

static bool flash_cache_valid(const esp_partition_t *part, const char *name,
                               boot_anim_flash_header_t *hdr_out)
{
    if (esp_partition_read(part, 0, hdr_out, sizeof(*hdr_out)) != ESP_OK) return false;
    if (hdr_out->magic != BOOT_ANIM_FLASH_MAGIC) return false;
    if (strncmp(hdr_out->name, name, sizeof(hdr_out->name)) != 0) return false;
    if (hdr_out->frame_count == 0 || hdr_out->frame_count > BOOT_ANIM_FLASH_MAX_FRAMES) return false;
    return true;
}

/* 複製前先用stat()(只讀檔案metadata,不搬資料本體)快速估算整套動畫
 * 需要多少bytes、多少幀,判斷放不放得下這個flash分區。放不下就直接
 * 回傳false,呼叫端會整個跳過provision、原封不動退回SD播放完整版
 * 動畫──不會發生「複製到一半才發現放不下,結果flash裡快取了一份被
 * 截斷的動畫」這種情況。 */
static bool flash_cache_will_fit(const char *anim_dir, size_t budget)
{
    /* static: 這個函式在main task那條本來就很緊的呼叫堆疊上執行(見
     * play_custom_boot_anim()裡的stack overflow說明),避免再疊一份
     * 大buffer上去。 */
    static char path[320];
    struct stat st;
    size_t total = 0;
    int count = 0;

    for (; count < BOOT_ANIM_FLASH_MAX_FRAMES; count++) {
        snprintf(path, sizeof(path), "%s/frame_%04d.jpg", anim_dir, count);
        if (stat(path, &st) != 0) break;   /* 沒有更多幀了 */
        total += (size_t)st.st_size;
        if (total > budget) return false;
    }

    /* 迴圈是因為stat()失敗才停(真的沒幀了)還是撞到幀數上限才停?
     * 撞上限的話,代表還有更多幀會被悄悄丟掉,一樣算放不下。 */
    if (count == BOOT_ANIM_FLASH_MAX_FRAMES) {
        snprintf(path, sizeof(path), "%s/frame_%04d.jpg", anim_dir, count);
        if (stat(path, &st) == 0) return false;
    }
    return true;
}

/* 把anim_dir底下整套frame_%04d.jpg複製進flash分區。純粹搬資料,不解碼
 * 也不碰LVGL/畫面。失敗只代表這次沒快取成功,不影響呼叫端接下來要走
 * 的SD播放路徑,下次開機會再試一次。 */
static bool flash_cache_provision(const esp_partition_t *part, const char *anim_dir, const char *name)
{
    size_t budget = part->size - BOOT_ANIM_FLASH_HEADER_RESERVED;
    if (!flash_cache_will_fit(anim_dir, budget)) {
        ESP_LOGW(TAG, "animation too large for flash cache (budget %u bytes), staying on SD",
                 (unsigned)budget);
        return false;
    }

    ESP_LOGI(TAG, "provisioning flash cache for '%s' ...", name);

    if (esp_partition_erase_range(part, 0, part->size) != ESP_OK) {
        ESP_LOGW(TAG, "flash erase failed");
        return false;
    }

    uint8_t *io_buf = heap_caps_malloc(BOOT_ANIM_MAX_JPEG_SIZE, MALLOC_CAP_SPIRAM);
    if (!io_buf) {
        ESP_LOGW(TAG, "PSRAM alloc failed for flash cache staging buffer");
        return false;
    }

    /* static: 見play_custom_boot_anim()裡的stack overflow說明,這兩個
     * buffer合計超過1KB,不該放在main task本來就很緊的呼叫堆疊上。 */
    static boot_anim_flash_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    static char path[320];
    size_t write_off = BOOT_ANIM_FLASH_HEADER_RESERVED;
    int frame_idx = 0;
    TickType_t t0 = xTaskGetTickCount();

    for (; frame_idx < BOOT_ANIM_FLASH_MAX_FRAMES; frame_idx++) {
        snprintf(path, sizeof(path), "%s/frame_%04d.jpg", anim_dir, frame_idx);
        FILE *f = fopen(path, "rb");
        if (!f) break;   /* 沒有更多幀了,正常結束掃描 */

        size_t n = fread(io_buf, 1, BOOT_ANIM_MAX_JPEG_SIZE, f);
        fclose(f);

        if (write_off + n > part->size) {
            ESP_LOGW(TAG, "flash cache full at frame %d, animation truncated here", frame_idx);
            break;
        }
        if (esp_partition_write(part, write_off, io_buf, n) != ESP_OK) {
            ESP_LOGW(TAG, "flash write failed at frame %d", frame_idx);
            heap_caps_free(io_buf);
            return false;
        }
        hdr.frame_len[frame_idx] = (uint32_t)n;
        write_off += n;
    }
    heap_caps_free(io_buf);

    if (frame_idx == 0) {
        ESP_LOGW(TAG, "no frames found under %s, nothing to cache", anim_dir);
        return false;
    }

    hdr.magic = BOOT_ANIM_FLASH_MAGIC;
    snprintf(hdr.name, sizeof(hdr.name), "%s", name);
    hdr.frame_count = (uint32_t)frame_idx;

    /* header最後才寫,見上面大註解的斷電安全性說明 */
    if (esp_partition_write(part, 0, &hdr, sizeof(hdr)) != ESP_OK) {
        ESP_LOGW(TAG, "flash header write failed");
        return false;
    }

    uint32_t ms = (xTaskGetTickCount() - t0) * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "flash cache provisioned: %d frames, %u bytes, %u ms",
             frame_idx, (unsigned)(write_off - BOOT_ANIM_FLASH_HEADER_RESERVED), (unsigned)ms);
    return true;
}

/* 從flash快取直接播放,完全不碰SD卡。mmap給零成本的讀取,decode仍然
 * 用雙緩衝(見BOOT_ANIM_DECODE_BUFFERS),因為decode/render重疊這件事
 * 跟資料來源是SD還是flash無關,一樣值得做。 */
static bool play_from_flash(const esp_partition_t *part, const boot_anim_flash_header_t *hdr)
{
    const void *mapped = NULL;
    esp_partition_mmap_handle_t mmap_handle;
    if (esp_partition_mmap(part, 0, part->size, ESP_PARTITION_MMAP_DATA, &mapped, &mmap_handle) != ESP_OK) {
        ESP_LOGW(TAG, "flash mmap failed, falling back to SD");
        return false;
    }
    const uint8_t *blob = (const uint8_t *)mapped + BOOT_ANIM_FLASH_HEADER_RESERVED;

    uint8_t *rgb_buf[BOOT_ANIM_DECODE_BUFFERS] = { NULL };
    for (int i = 0; i < BOOT_ANIM_DECODE_BUFFERS; i++) {
        rgb_buf[i] = heap_caps_aligned_alloc(16, SCREEN_W * SCREEN_H * 2, MALLOC_CAP_SPIRAM);
        if (!rgb_buf[i]) {
            ESP_LOGW(TAG, "PSRAM alloc failed, falling back to SD");
            for (int j = 0; j < i; j++) heap_caps_free(rgb_buf[j]);
            esp_partition_munmap(mmap_handle);
            return false;
        }
    }

    int buf_idx = 0;
    size_t offset = 0;

    TickType_t decode0_start = xTaskGetTickCount();
    bool frame0_ok = decode_jpeg_mem(blob + offset, hdr->frame_len[0], rgb_buf[buf_idx]);
    uint32_t decode0_ms = (xTaskGetTickCount() - decode0_start) * portTICK_PERIOD_MS;
    ESP_LOGI(TAG, "frame 0 ready (flash): %s, %u ms", frame0_ok ? "OK" : "FAILED", (unsigned)decode0_ms);

    if (!frame0_ok) {
        for (int i = 0; i < BOOT_ANIM_DECODE_BUFFERS; i++) heap_caps_free(rgb_buf[i]);
        esp_partition_munmap(mmap_handle);
        jpeg_dec_teardown();
        return false;
    }
    offset += hdr->frame_len[0];

    static lv_img_dsc_t dsc;
    dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
    dsc.header.always_zero = 0;
    dsc.header.w           = SCREEN_W;
    dsc.header.h           = SCREEN_H;
    dsc.data_size          = SCREEN_W * SCREEN_H * 2;
    dsc.data               = rgb_buf[buf_idx];

    lv_obj_t *img = NULL;
    if (!lvgl_port_lock(-1)) {
        for (int i = 0; i < BOOT_ANIM_DECODE_BUFFERS; i++) heap_caps_free(rgb_buf[i]);
        esp_partition_munmap(mmap_handle);
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

    wait_for_render_idle(1000);
    vTaskDelay(pdMS_TO_TICKS(100));
    wavesahre_rgb_lcd_bl_on();

    const TickType_t frame_ticks = pdMS_TO_TICKS(1000 / BOOT_ANIM_CUSTOM_FPS);
    const TickType_t anim_start  = xTaskGetTickCount();
    uint32_t frame_idx;

    for (frame_idx = 1; frame_idx < hdr->frame_count; frame_idx++) {
        TickType_t frame_start = xTaskGetTickCount();
        int next_buf = 1 - buf_idx;

        bool ok = decode_jpeg_mem(blob + offset, hdr->frame_len[frame_idx], rgb_buf[next_buf]);
        if (ok) {
            if (lvgl_port_lock(-1)) {
                dsc.data = rgb_buf[next_buf];
                lv_img_cache_invalidate_src(&dsc);
                lv_img_set_src(img, &dsc);
                lvgl_port_unlock();
            }
            buf_idx = next_buf;
        }
        offset += hdr->frame_len[frame_idx];

        TickType_t elapsed = xTaskGetTickCount() - frame_start;
        if (elapsed < frame_ticks) {
            vTaskDelay(frame_ticks - elapsed);
        }
    }

    uint32_t total_ms = (xTaskGetTickCount() - anim_start) * portTICK_PERIOD_MS;
    if (total_ms > 0) {
        ESP_LOGI(TAG, "%u frames in %u ms (~%.1f fps actual, target %d fps) [flash]",
                 (unsigned)frame_idx, (unsigned)total_ms,
                 frame_idx * 1000.0f / total_ms, BOOT_ANIM_CUSTOM_FPS);
    }

    vTaskDelay(pdMS_TO_TICKS(200));
    if (lvgl_port_lock(-1)) {
        lv_obj_clean(lv_scr_act());
        lvgl_port_unlock();
    }

    for (int i = 0; i < BOOT_ANIM_DECODE_BUFFERS; i++) heap_caps_free(rgb_buf[i]);
    esp_partition_munmap(mmap_handle);
    jpeg_dec_teardown();
    return true;
}

/* Returns true if a custom animation was found and played (screen is left
 * cleared and backlight already on). Returns false if there's no custom
 * animation configured, in which case the caller should fall back to the
 * built-in procedural animation. */
static bool play_from_sd(const char *anim_dir)
{
    /* esp_new_jpeg 建議輸出buffer要16 byte對齊。開兩塊輪流用,見上面
     * BOOT_ANIM_DECODE_BUFFERS的註解。 */
    uint8_t *rgb_buf[BOOT_ANIM_DECODE_BUFFERS] = { NULL };
    for (int i = 0; i < BOOT_ANIM_DECODE_BUFFERS; i++) {
        rgb_buf[i] = heap_caps_aligned_alloc(16, SCREEN_W * SCREEN_H * 2, MALLOC_CAP_SPIRAM);
        if (!rgb_buf[i]) {
            ESP_LOGW(TAG, "PSRAM alloc failed, falling back to built-in animation");
            for (int j = 0; j < i; j++) heap_caps_free(rgb_buf[j]);
            return false;
        }
    }

    jpeg_prewarm_wait();   /* 如果main.c有先呼叫boot_anim_prewarm_jpeg,這裡通常瞬間返回 */

    if (!prefetch_start(anim_dir)) {
        ESP_LOGW(TAG, "prefetch setup failed, falling back to built-in animation");
        for (int i = 0; i < BOOT_ANIM_DECODE_BUFFERS; i++) heap_caps_free(rgb_buf[i]);
        return false;
    }

    int buf_idx = 0;   /* frame 0一律用buffer 0開局 */

    /* 診斷用:量測第一幀實際等待+解碼耗時(背景task這時才剛開始讀,
     * 這裡就是新版真正的「動畫出現前延遲」數字) */
    TickType_t decode0_start = xTaskGetTickCount();
    bool frame0_missing = false;
    bool frame0_ok = prefetch_next(rgb_buf[buf_idx], &frame0_missing);
    uint32_t decode0_ms = (xTaskGetTickCount() - decode0_start) * portTICK_PERIOD_MS;
    if (!frame0_missing) {
        ESP_LOGI(TAG, "frame 0 ready: %s, %u ms", frame0_ok ? "OK" : "FAILED", (unsigned)decode0_ms);
    }

    if (!frame0_ok) {
        /* frame0_missing == 沒放自訂動畫,交給原本的程序動畫;
         * 否則是解碼失敗,一樣退回內建動畫 */
        prefetch_stop();
        for (int i = 0; i < BOOT_ANIM_DECODE_BUFFERS; i++) heap_caps_free(rgb_buf[i]);
        jpeg_dec_teardown();
        return false;
    }

    static lv_img_dsc_t dsc;   /* static: lv_img_set_src keeps a pointer to this */
    dsc.header.cf          = LV_IMG_CF_TRUE_COLOR;
    dsc.header.always_zero = 0;
    dsc.header.w           = SCREEN_W;
    dsc.header.h           = SCREEN_H;
    dsc.data_size          = SCREEN_W * SCREEN_H * 2;
    dsc.data               = rgb_buf[buf_idx];

    lv_obj_t *img = NULL;
    if (!lvgl_port_lock(-1)) {
        prefetch_stop();
        for (int i = 0; i < BOOT_ANIM_DECODE_BUFFERS; i++) heap_caps_free(rgb_buf[i]);
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

    /* lv_img_set_src 只是標記待重繪,真正畫進面板framebuffer是另一個LVGL
     * task(lvgl_port_task)非同步做的。這裡背光還是關的、畫面對使用者
     * 不可見,所以可以放心等它真的畫完,不用擔心使用者看到等待過程。
     *
     * 注意:這塊板子用bounce buffer搬運畫面,vsync通知是「一段bounce
     * buffer搬完」就觸發,不是「整張畫面搬完」才觸發,inv_p==0不完全
     * 保證畫面100%更新完畢。輪詢先抓到大致完成的時間點,再加一段安全
     * 餘裕(涵蓋幾個畫面更新週期,16MHz pixel clock下一張約25ms)確保
     * 真的搬完 -- 反正背光還沒開,這段時間使用者看不到,不影響體感。 */
    wait_for_render_idle(1000);
    vTaskDelay(pdMS_TO_TICKS(100));
    wavesahre_rgb_lcd_bl_on();   /* 第一幀確定畫完了,這時才開背光 */

    const TickType_t frame_ticks = pdMS_TO_TICKS(1000 / BOOT_ANIM_CUSTOM_FPS);
    const TickType_t anim_start  = xTaskGetTickCount();
    int frame_idx = 1;

    for (;; frame_idx++) {
        TickType_t frame_start = xTaskGetTickCount();

        int next_buf = 1 - buf_idx;

        /* 這裡原本加了一個雙緩衝重用前的防禦性wait_for_render_idle,
         * 但實測發現它幾乎每幀都超時、根本沒在保護該保護的東西:
         * disp->inv_p是全域旗標,測的是「上一輪剛發出的invalidate有沒
         * 有被馬上處理」,不是「這塊buffer上次的畫面有沒有真的畫完」,
         * 兩者是不同問題,而且前者幾乎必然還沒完成(渲染task本來就還
         * 沒機會跑)。結果變成每幀白白多吃約50ms,反而抵銷了雙緩衝的
         * 效果。真正的安全網是時間差本身:每塊buffer重用前間隔了完整
         * 一輪frame cycle(~130ms),遠大於面板實際掃描一張畫面的時間
         * (16MHz pixel clock下約25ms),不需要額外檢查就已經夠安全。 */

        bool missing = false;
        bool ok = prefetch_next(rgb_buf[next_buf], &missing);

        if (ok) {
            if (lvgl_port_lock(-1)) {
                dsc.data = rgb_buf[next_buf];
                lv_img_cache_invalidate_src(&dsc);
                lv_img_set_src(img, &dsc);
                lvgl_port_unlock();
            }
            buf_idx = next_buf;
            /* 不再像單緩衝版那樣等這幀真的畫完才進下一輪 -- 這幀的
             * render時間會跟下一幀的解碼+SD等待重疊,這是雙緩衝的重點。 */
        } else if (missing) {
            break;   /* 沒有下一幀了,動畫播完 */
        }
        /* 檔案/資料存在但解碼失敗就跳過這一幀,不中斷整段動畫 */

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

    prefetch_stop();
    for (int i = 0; i < BOOT_ANIM_DECODE_BUFFERS; i++) heap_caps_free(rgb_buf[i]);
    jpeg_dec_teardown();
    return true;
}

/* Returns true if a custom animation was found and played (screen is left
 * cleared and backlight already on). Returns false if there's no custom
 * animation configured, in which case the caller should fall back to the
 * built-in procedural animation.
 *
 * 判斷順序:flash快取命中 -> 直接從flash播(全程不碰SD);沒命中(第一次
 * 選這套動畫,或使用者剛換了一套) -> 需要SD,先嘗試把這套動畫複製進
 * flash(複製期間畫面全黑,失敗也不影響這次播放,只是下次開機還是走
 * 這條路),再照舊從SD播放。 */
static bool play_custom_boot_anim(void)
{
    /* main task(app_main)的stack只有3584 bytes(CONFIG_ESP_MAIN_TASK_
     * STACK_SIZE),boot_anim_flash_header_t光是frame_len陣列就佔872
     * bytes,這裡跟flash_cache_provision()裡同一個struct疊在一起放
     * stack上,再加上esp_partition_erase_range/write本身呼叫深度,
     * 曾經直接把main task的stack炸掉(stack overflow panic)。這幾個
     * 大型buffer全部改成static,移到.bss,不占用呼叫堆疊 -- boot_anim
     * 全程單執行緒、沒有重入疑慮,static在這裡是安全的。 */
    static char name[64];
    static char anim_dir[288];
    if (!get_selected_anim(name, sizeof(name), anim_dir, sizeof(anim_dir))) {
        return false;   /* 使用者選了 None,或還沒選過 */
    }

    jpeg_prewarm_wait();   /* 如果main.c有先呼叫boot_anim_prewarm_jpeg,這裡通常瞬間返回 */

    const esp_partition_t *part = esp_partition_find_first(
        ESP_PARTITION_TYPE_DATA, (esp_partition_subtype_t)BOOT_ANIM_FLASH_SUBTYPE,
        BOOT_ANIM_FLASH_LABEL);
    if (!part) {
        ESP_LOGW(TAG, "flash cache partition '%s' not found, SD-only this boot",
                 BOOT_ANIM_FLASH_LABEL);
    }

    static boot_anim_flash_header_t hdr;
    memset(&hdr, 0, sizeof(hdr));
    if (part && flash_cache_valid(part, name, &hdr)) {
        if (play_from_flash(part, &hdr)) return true;
        ESP_LOGW(TAG, "flash playback failed, falling back to SD");
    }

    if (!fs_sd_status()) return false;

    if (part) {
        if (!flash_cache_provision(part, anim_dir, name)) {
            ESP_LOGW(TAG, "flash cache provisioning failed, will retry next boot");
        }
    }

    return play_from_sd(anim_dir);
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