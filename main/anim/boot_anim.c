#include "boot_anim.h"
#include "waveshare_rgb_lcd_port.h"
#include "lvgl_port.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SCREEN_W  800
#define SCREEN_H  480
#define CENTER_X  (SCREEN_W / 2)
#define CENTER_Y  (SCREEN_H / 2)

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
 * Public entry point
 * ----------------------------------------------------------------------- */
void boot_anim_play(void)
{
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