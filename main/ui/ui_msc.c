#include "ui_msc.h"
#include "usb/usb_manager.h"
#include "ui_settings.h"
#include "ui_monitor.h"
#include "ui_media.h"
#include "lvgl.h"

#define SCREEN_W  800
#define SCREEN_H  480

/* -----------------------------------------------------------------------
 * MSC mode landing screen
 * ----------------------------------------------------------------------- */
static void show_msc_screen(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x111111), 0);

    lv_obj_t *cont = lv_obj_create(scr);
    lv_obj_set_size(cont, 400, 200);
    lv_obj_center(cont);
    lv_obj_set_style_bg_color(cont, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_width(cont, 0, 0);
    lv_obj_set_style_radius(cont, 12, 0);
    lv_obj_set_layout(cont, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(cont, 20, 0);
    lv_obj_set_style_pad_row(cont, 12, 0);

    lv_obj_t *icon = lv_label_create(cont);
    lv_label_set_text(icon, LV_SYMBOL_USB);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(0xffffff), 0);

    lv_obj_t *title = lv_label_create(cont);
    lv_label_set_text(title, "USB Mass Storage Mode");
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);

    lv_obj_t *sub = lv_label_create(cont);
    lv_label_set_text(sub, "SD card connected\nRestart to return to HID mode");
    lv_obj_set_style_text_color(sub, lv_color_hex(0x888888), 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);
}

static void msc_screen_timer_cb(lv_timer_t *t)
{
    show_msc_screen();
    lv_timer_del(t);
}

static void msc_start_timer_cb(lv_timer_t *t)
{
    lv_timer_del(t);
    usb_manager_request_msc();
    lv_timer_create(msc_screen_timer_cb, 800, NULL);
}

/* -----------------------------------------------------------------------
 * Confirm dialog
 * ----------------------------------------------------------------------- */
static lv_obj_t *s_overlay = NULL;

static void dialog_confirm_cb(lv_event_t *e)
{
    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }

    /* Exit whichever mode is currently active cleanly before MSC's
     * show_msc_screen() wipes the screen with lv_obj_clean() -- without
     * this, a mode's own periodic timer (e.g. Media's s_media_timer)
     * keeps running against widget pointers that just got freed out from
     * under it, and the next tick crashes touching freed LVGL objects.
     * Deck has no such timer today so it's harmless to skip, but Monitor
     * and Media both do. */
    ui_mode_t mode = ui_settings_get_mode();
    if (mode == UI_MODE_MONITOR) {
        ui_monitor_exit();
    } else if (mode == UI_MODE_MEDIA) {
        ui_media_exit();
    }

    lv_obj_t *black = lv_obj_create(lv_scr_act());
    lv_obj_set_size(black, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(black, 0, 0);
    lv_obj_set_style_bg_color(black, lv_color_black(), 0);
    lv_obj_set_style_border_width(black, 0, 0);
    lv_obj_set_style_radius(black, 0, 0);
    lv_timer_create(msc_start_timer_cb, 150, NULL);
}

static void dialog_cancel_cb(lv_event_t *e)
{
    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
}

void ui_msc_show_confirm_dialog(void)
{
    s_overlay = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_overlay, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_overlay, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_set_style_radius(s_overlay, 0, 0);
    lv_obj_clear_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(s_overlay);
    lv_obj_set_size(box, 460, 220);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_radius(box, 12, 0);
    lv_obj_set_style_pad_all(box, 24, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, "Switch to MSC Mode");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xffffff), 0);
    lv_obj_set_pos(title, 0, 0);

    lv_obj_t *div = lv_obj_create(box);
    lv_obj_set_size(div, 412, 1);
    lv_obj_set_pos(div, 0, 28);
    lv_obj_set_style_bg_color(div, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(div, 0, 0);
    lv_obj_set_style_radius(div, 0, 0);
    lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *body = lv_label_create(box);
    lv_label_set_text(body,
        "SD card will be available to PC.\n"
        "Device must restart to return to HID mode.");
    lv_obj_set_style_text_color(body, lv_color_hex(0x999999), 0);
    lv_obj_set_style_text_font(body, &lv_font_montserrat_14, 0);
    lv_obj_set_pos(body, 0, 44);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, 412);

    lv_obj_t *btn_row = lv_obj_create(box);
    lv_obj_set_size(btn_row, 412, 48);
    lv_obj_set_pos(btn_row, 0, 128);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_layout(btn_row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(btn_row, 12, 0);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *btn_cancel = lv_btn_create(btn_row);
    lv_obj_set_size(btn_cancel, 120, 40);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_color(btn_cancel, lv_color_hex(0x4a4a4a), LV_STATE_PRESSED);
    lv_obj_set_style_border_color(btn_cancel, lv_color_hex(0x555555), 0);
    lv_obj_set_style_border_width(btn_cancel, 1, 0);
    lv_obj_set_style_radius(btn_cancel, 6, 0);
    lv_obj_add_event_cb(btn_cancel, dialog_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(btn_cancel, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_t *lbl_cancel = lv_label_create(btn_cancel);
    lv_label_set_text(lbl_cancel, "Cancel");
    lv_obj_set_style_text_color(lbl_cancel, lv_color_hex(0xcccccc), 0);
    lv_obj_center(lbl_cancel);

    lv_obj_t *btn_confirm = lv_btn_create(btn_row);
    lv_obj_set_size(btn_confirm, 120, 40);
    lv_obj_set_style_bg_color(btn_confirm, lv_color_hex(0x0055cc), 0);
    lv_obj_set_style_bg_color(btn_confirm, lv_color_hex(0x0044aa), LV_STATE_PRESSED);
    lv_obj_set_style_border_width(btn_confirm, 0, 0);
    lv_obj_set_style_radius(btn_confirm, 6, 0);
    lv_obj_add_event_cb(btn_confirm, dialog_confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(btn_confirm, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_t *lbl_confirm = lv_label_create(btn_confirm);
    lv_label_set_text(lbl_confirm, "Switch");
    lv_obj_set_style_text_color(lbl_confirm, lv_color_hex(0xffffff), 0);
    lv_obj_center(lbl_confirm);
}
