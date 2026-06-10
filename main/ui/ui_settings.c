#include "ui_settings.h"
#include "ui_msc.h"
#include "ui_config_dialog.h"
#include "ui_keyboard.h"
#include "lvgl.h"

#define SCREEN_W  800
#define SCREEN_H  480

static lv_obj_t *s_panel   = NULL;
static lv_obj_t *s_overlay = NULL;

/* -----------------------------------------------------------------------
 * Item callbacks
 * ----------------------------------------------------------------------- */
static void hide_menu(void)
{
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    if (s_overlay) {
        lv_obj_del(s_overlay);
        s_overlay = NULL;
    }
}

static void item_msc_cb(lv_event_t *e)
{
    hide_menu();
    ui_msc_show_confirm_dialog();
}

static void item_config_cb(lv_event_t *e)
{
    hide_menu();
    ui_config_dialog_show();
}

static void item_keyboard_cb(lv_event_t *e)
{
    hide_menu();
    ui_keyboard_show();
}

static void overlay_cb(lv_event_t *e)
{
    hide_menu();
}

/* -----------------------------------------------------------------------
 * Helper: create a single menu item button
 * ----------------------------------------------------------------------- */
static void add_item(lv_obj_t *panel, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *item = lv_btn_create(panel);
    lv_obj_set_width(item, LV_PCT(100));
    lv_obj_set_style_bg_color(item, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_bg_color(item, lv_color_hex(0x3a3a3a), LV_STATE_PRESSED);
    lv_obj_set_style_radius(item, 4, 0);
    lv_obj_add_event_cb(item, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(item);
    lv_label_set_text(lbl, text);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
}

/* -----------------------------------------------------------------------
 * Public
 * ----------------------------------------------------------------------- */
lv_obj_t *ui_settings_build(lv_obj_t *scr)
{
    s_panel = lv_obj_create(scr);
    lv_obj_set_size(s_panel, 220, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_panel, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_border_width(s_panel, 1, 0);
    lv_obj_set_style_border_color(s_panel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_radius(s_panel, 8, 0);
    lv_obj_set_style_pad_all(s_panel, 8, 0);
    lv_obj_set_layout(s_panel, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);

    add_item(s_panel, "Switch to MSC mode", item_msc_cb);
    add_item(s_panel, "Select Config",      item_config_cb);
    add_item(s_panel, "Keyboard Mode",      item_keyboard_cb);

    lv_obj_update_layout(s_panel);
    lv_obj_set_pos(s_panel,
                   80 + 8,
                   SCREEN_H - lv_obj_get_height(s_panel) - 8);
    return s_panel;
}

void ui_settings_toggle(void)
{
    if (lv_obj_has_flag(s_panel, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_t *scr = lv_scr_act();
        s_overlay = lv_obj_create(scr);
        lv_obj_set_size(s_overlay, SCREEN_W, SCREEN_H);
        lv_obj_set_pos(s_overlay, 0, 0);
        lv_obj_set_style_bg_opa(s_overlay, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_overlay, 0, 0);
        lv_obj_add_event_cb(s_overlay, overlay_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_move_foreground(s_panel);
        lv_obj_clear_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    } else {
        hide_menu();
    }
}
