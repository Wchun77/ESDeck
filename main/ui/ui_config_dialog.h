#pragma once

/* Select Config dialog, one per mode -- the *.json files under that mode's
 * config folder. On confirm each saves the selection to NVS and calls its
 * mode's ui_settings_*_reload(). */
void ui_deck_config_dialog_show(void);
void ui_monitor_config_dialog_show(void);

/* Same dialog, Media's config list (the *.json files under config/media).
 * On confirm, saves the selection to NVS and calls ui_settings_media_reload()
 * -- same lightweight exit/re-enter pattern Monitor's version uses. */
void ui_media_config_dialog_show(void);

/* Same dialog, Touchpad's config list (the *.json files under config/touchpad). On confirm,
 * saves to NVS and calls ui_settings_touchpad_reload(). */
void ui_touchpad_config_dialog_show(void);
