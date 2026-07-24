#include "sys_clock.h"

#include "esp_timer.h"
#include "lvgl.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <time.h>

/* Raw fields as parsed from the CMD_TIME wire format -- no calendar math
 * here, that all happens in queue_drain_cb() on the LVGL task. Keeping the
 * TinyUSB-task side of this to a plain struct copy matches the existing
 * convention (see ui_monitor.c's own HID callbacks). */
typedef struct {
    uint16_t year;
    uint8_t  month, day, hour, min, sec;
} hid_time_pkt_t;

static QueueHandle_t s_pkt_queue    = NULL;
static lv_timer_t   *s_drain_timer  = NULL;

/* The clock is epoch-seconds-at-an-anchor-instant + however many seconds
 * of hardware-timer-measured monotonic time have elapsed since that
 * instant -- recomputed fresh on every sys_clock_get() call, never
 * accumulated tick-by-tick. An lv_timer only fires "at least every N ms",
 * not "exactly every N ms": mode switches block the LVGL task for
 * hundreds of ms to rebuild pages (image decode, widget churn), so a
 * naive "+1 second per callback firing" tick silently loses whatever real
 * time the task was blocked for -- that's what was actually causing the
 * growing drift on mode switches, not a missing hardware RTC. Deriving
 * the value from esp_timer_get_time() (hardware counter, immune to task
 * scheduling delays) instead of counting callback firings avoids that
 * class of drift entirely. */
static time_t  s_epoch_anchor   = 0;
static int64_t s_mono_anchor_us = 0;
static bool    s_valid          = false;

/* Epoch seconds "right now", derived from the anchor -- not stored/ticked. */
static time_t current_epoch(void)
{
    int64_t elapsed_us = esp_timer_get_time() - s_mono_anchor_us;
    return s_epoch_anchor + (time_t)(elapsed_us / 1000000);
}

static void set_anchor(time_t epoch)
{
    s_epoch_anchor   = epoch;
    s_mono_anchor_us = esp_timer_get_time();
}

/* Drains whatever CMD_TIME packets arrived since the last pass and, if
 * one did, either snaps the clock to it (first sync ever) or re-anchors
 * only when it has drifted from local ticking by more than 1 second
 * (small jitter from the PC-side timer is otherwise ignored so the
 * display doesn't flicker). Runs frequently just to process packets
 * promptly -- it does not itself advance the clock, see current_epoch(). */
static void queue_drain_cb(lv_timer_t *t)
{
    (void)t;

    hid_time_pkt_t pkt;
    bool got = false;
    while (s_pkt_queue && xQueueReceive(s_pkt_queue, &pkt, 0) == pdTRUE)
        got = true;

    if (!got) return;

    struct tm incoming = {
        .tm_year  = pkt.year - 1900,
        .tm_mon   = pkt.month - 1,
        .tm_mday  = pkt.day,
        .tm_hour  = pkt.hour,
        .tm_min   = pkt.min,
        .tm_sec   = pkt.sec + 1,  /* +1s: compensate PC->ESP display lag
                                   * (mktime() below correctly cascades this
                                   * through minute/hour/day/month/year if
                                   * it lands on a boundary). */
        .tm_isdst = -1,
    };
    time_t incoming_epoch = mktime(&incoming);   /* also normalizes the +1s */

    if (!s_valid) {
        set_anchor(incoming_epoch);   /* first sync ever this session: snap directly */
        s_valid = true;
        return;
    }

    long diff = (long)incoming_epoch - (long)current_epoch();
    if (diff < 0) diff = -diff;
    if (diff > 1)
        set_anchor(incoming_epoch);   /* drift correction */
}

void sys_clock_init(void)
{
    if (s_pkt_queue) return;   /* already initialised */
    s_pkt_queue   = xQueueCreate(1, sizeof(hid_time_pkt_t));
    s_drain_timer = lv_timer_create(queue_drain_cb, 200, NULL);
}

void sys_clock_push_hid_time(uint16_t year, uint8_t month, uint8_t day,
                              uint8_t hour, uint8_t min, uint8_t sec)
{
    if (!s_pkt_queue) return;
    hid_time_pkt_t pkt = { year, month, day, hour, min, sec };
    xQueueOverwrite(s_pkt_queue, &pkt);
}

bool sys_clock_is_valid(void)
{
    return s_valid;
}

sys_time_t sys_clock_get(void)
{
    time_t now = current_epoch();
    struct tm tmv;
    localtime_r(&now, &tmv);

    sys_time_t out = {
        .hour  = (uint8_t)tmv.tm_hour,
        .min   = (uint8_t)tmv.tm_min,
        .sec   = (uint8_t)tmv.tm_sec,
        .month = (uint8_t)(tmv.tm_mon + 1),
        .day   = (uint8_t)tmv.tm_mday,
        .wday  = (uint8_t)tmv.tm_wday,
    };
    return out;
}
