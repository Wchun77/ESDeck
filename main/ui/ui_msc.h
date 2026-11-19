#pragma once

/* Show the MSC confirm dialog (Switch / Cancel). */
void ui_msc_show_confirm_dialog(void);

/* Hook up the PC side: HID_SYS_CMD_STORAGE switches to Storage Mode, and
 * the PC ejecting the drive restarts the device. Call once at boot. */
void ui_msc_init(void);
