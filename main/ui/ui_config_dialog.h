#pragma once

/* Show the Select Config dialog.
 * On confirm, tears down the current deck, loads the selected config,
 * and rebuilds the deck via a background preload task. */
void ui_config_dialog_show(void);
void ui_monitor_config_dialog_show(void);
