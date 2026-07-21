#pragma once

/* Show the Select Config dialog.
 * On confirm, tears down the current deck, loads the selected config,
 * and rebuilds the deck via a background preload task. */
void ui_config_dialog_show(void);
void ui_monitor_config_dialog_show(void);

/* Same dialog, Media's config list (the *.json files under config/media).
 * On confirm, saves the selection to NVS and calls ui_settings_media_reload()
 * -- same lightweight exit/re-enter pattern Monitor's version uses. */
void ui_media_config_dialog_show(void);
