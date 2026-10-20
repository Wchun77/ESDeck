#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "app_config.h"     /* CFG_FNAME_LEN, CFG_BG_LEN, cfg_scan_result_t */
#include "ui_settings.h"   /* ui_settings_appearance_t */

/* Touchpad mode appearance -- same schema as Media's (ui_media_config.h),
 * loaded from whichever config/touchpad/<name>.json is selected in NVS:
 *
 *   {
 *       "bg_image": "desk.jpg",
 *       "keyboard_icon": "kb.png",
 *       "sidebar_mode": "hotkeys",          // "none" (default) | "wheel" | "hotkeys"
 *       "hotkeys": [                        // up to TP_HOTKEY_MAX, only for "hotkeys"
 *           { "label": "Copy", "icon": "copy.png", "keys": ["ctrl", "c"] }
 *       ],
 *       "settings": {
 *           "bg_image": "panel_bg.jpg",
 *           "side_icon": "mouse.png"
 *       }
 *   }
 *
 * All fields optional. keyboard_icon is the sidebar keyboard button's
 * image (under assets/side_icons, same as any side_icon); empty or
 * missing file = LV_SYMBOL_KEYBOARD glyph.
 *
 * Sidebar middle area (between the keyboard button and the gear):
 *   none    -- empty (default)
 *   wheel   -- scroll wheel strip
 *   hotkeys -- up to TP_HOTKEY_MAX buttons, each one key combo. "keys" uses
 *              the same names as a Deck "hotkey" action (modifiers ctrl /
 *              shift / alt / win + one key: a-z, 0-9, punctuation, enter,
 *              tab, esc, del, backspace, space, arrows, home, end, pgup,
 *              pgdn, insert, prtscr, f1-f12). Parsed here into a VK code +
 *              modifier mask and sent through the keyboard report path, so
 *              no PC-side config is involved. Entries whose keys don't parse
 *              are skipped. Button shows icon (assets/side_icons) if the file
 *              exists, else label text. */
#define TP_HOTKEY_MAX        4
#define TP_HOTKEY_LABEL_LEN  12

typedef enum {
    TP_SIDEBAR_NONE = 0,
    TP_SIDEBAR_WHEEL,
    TP_SIDEBAR_HOTKEYS,
} tp_sidebar_mode_t;

typedef struct {
    char    label[TP_HOTKEY_LABEL_LEN];
    char    icon[UI_SETTINGS_SIDE_ICON_LEN];
    uint8_t vk;      /* Windows virtual-key code of the main key */
    uint8_t mods;    /* HID_KB_MOD_* (usb_hid.h) */
} tp_hotkey_t;

typedef struct {
    char bg_image[CFG_BG_LEN];         /* content bg, under SD_PATH_ASSETS_BG; empty = flat color */
    char keyboard_icon[UI_SETTINGS_SIDE_ICON_LEN]; /* under SD_PATH_ASSETS_SIDE_ICON; empty = glyph */
    tp_sidebar_mode_t sidebar_mode;
    tp_hotkey_t       hotkeys[TP_HOTKEY_MAX];
    uint8_t           hotkey_count;
    ui_settings_appearance_t settings; /* Settings overlay's own bg_image + side_icon */
} ui_touchpad_config_t;

/* Always zeroes *cfg first; returns false (cfg left all-empty) when no
 * config is selected in NVS or the file is missing/invalid -- treat that
 * as "no config", not an error. */
bool ui_touchpad_config_load(ui_touchpad_config_t *cfg);

bool ui_touchpad_config_nvs_save(const char *filename);
bool ui_touchpad_config_nvs_load(char *out, size_t out_size);

/* Scan SD_PATH_CONFIG_TOUCHPAD for *.json. count = -1 if the directory
 * does not exist. Free with ui_touchpad_config_scan_free(). */
typedef cfg_scan_result_t touchpad_scan_result_t;
touchpad_scan_result_t ui_touchpad_config_scan(void);
void                   ui_touchpad_config_scan_free(touchpad_scan_result_t *res);
