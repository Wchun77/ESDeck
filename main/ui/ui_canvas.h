#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "lvgl.h"
#include "app_config.h"   /* cfg_scan_result_t */

/*
 * Canvas mode -- pages rendered by ESDeckPC.
 *
 * ESDeckPC renders an HTML page into the content area (720 x 480) and
 * sends the changed parts over HID (protocol: "Canvas mode" in usb_hid.h);
 * this module only shows them and sends touches back. The sidebar holds up
 * to CANVAS_MAX_PAGES page buttons, their icons also drawn by the PC, plus
 * the gear (ui.c). Without ESDeckPC there is nothing to show: the content
 * area says so and the sidebar has no page buttons.
 *
 * Two frame buffers (PSRAM, freed on exit): a page switch is received into
 * the hidden one and swapped in when complete, so a page never appears
 * half drawn; small updates on the same page go straight onto the visible
 * one.
 *
 * Entry (same contract as the other modes):
 *   1. caller tears down whatever mode was active
 *   2. ui_canvas_enter(sidebar) -- asks the PC for the current page
 *   3. optionally ui_canvas_wait_first_frame() from a non-LVGL task while
 *      the switching screen is up, then hide it
 */
#define CANVAS_MAX_PAGES  4

void ui_canvas_enter(lv_obj_t *sidebar);
void ui_canvas_exit(void);

/* Blocks (not on the LVGL task) until the first page has arrived, the PC
 * reported it has nothing, or timeout_ms passed. Returns immediately when
 * USB isn't connected or ESDeckPC isn't running. */
void ui_canvas_wait_first_frame(uint32_t timeout_ms);

/* Settings over / away from the content area -- same pair as the other modes. */
void ui_canvas_deselect_current(void);
void ui_canvas_reselect_current(void);

/* Select Config in Canvas mode -- the configs are on the PC.
 * request: ask ESDeckPC for its list (false = not in Canvas mode).
 * poll (LVGL task): 0 = not complete yet, 1 = *out filled (free with
 *   cfg_scan_result_free(); count 0 = the PC has none) and *active_idx set
 *   (-1 = none in use), -1 = out of memory.
 * select: make that config active on the PC; with Settings open, Settings
 *   stays up until the new config's first page has arrived. */
#define CANVAS_CFG_LIST_MAX  32
bool ui_canvas_request_config_list(void);
int  ui_canvas_config_list_poll(cfg_scan_result_t *out, int *active_idx);
void ui_canvas_select_config(const char *name);
