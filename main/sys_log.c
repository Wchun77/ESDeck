#include "sys_log.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define LOG_BUF_SIZE    SYS_LOG_RING_SIZE
#define LOG_LINE_MAX    256   /* one formatted ESP_LOG line, stack buffer */

static char          *s_buf   = NULL;              /* PSRAM, LOG_BUF_SIZE bytes */
static portMUX_TYPE    s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t        s_write_pos = 0;             /* monotonic total bytes written */
static vprintf_like_t  s_orig_vprintf = NULL;

/* Caller already holds s_lock. Splits the write at the wrap point instead
 * of a per-byte loop so the critical section stays short. */
static void ring_write_locked(const char *data, size_t len)
{
    uint32_t pos   = s_write_pos % LOG_BUF_SIZE;
    size_t   first = LOG_BUF_SIZE - pos;
    if (first > len) first = len;

    memcpy(s_buf + pos, data, first);
    if (len > first)
        memcpy(s_buf, data + first, len - first);

    s_write_pos += (uint32_t)len;
}

/* Installed via esp_log_set_vprintf() -- called for every ESP_LOG* line,
 * from whatever task logged it. Formats once, copies into the ring
 * buffer, then always forwards to the previously-installed vprintf so
 * UART0/USB-Serial-JTAG console output is completely unaffected -- this
 * function only ever adds a tap, never replaces the original sink. */
static int log_vprintf(const char *fmt, va_list args)
{
    va_list args_for_ring;
    va_copy(args_for_ring, args);

    char   line[LOG_LINE_MAX];
    int    n = vsnprintf(line, sizeof(line), fmt, args_for_ring);
    va_end(args_for_ring);

    if (n > 0 && s_buf) {
        size_t len = (size_t)n;
        if (len >= sizeof(line)) len = sizeof(line) - 1;   /* vsnprintf truncated it */

        portENTER_CRITICAL(&s_lock);
        ring_write_locked(line, len);
        portEXIT_CRITICAL(&s_lock);
    }

    return s_orig_vprintf ? s_orig_vprintf(fmt, args) : vprintf(fmt, args);
}

void sys_log_init(void)
{
    if (s_buf) return;   /* already initialised */

    s_buf = heap_caps_malloc(LOG_BUF_SIZE, MALLOC_CAP_SPIRAM);
    if (!s_buf) return;   /* degrade gracefully -- console logging still works below */

    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);
}

sys_log_cursor_t sys_log_cursor_start(void)
{
    sys_log_cursor_t c;
    portENTER_CRITICAL(&s_lock);
    c.pos = (s_write_pos > LOG_BUF_SIZE) ? (s_write_pos - LOG_BUF_SIZE) : 0;
    portEXIT_CRITICAL(&s_lock);
    return c;
}

size_t sys_log_read(sys_log_cursor_t *cursor, char *out, size_t out_cap,
                     bool *dropped_out)
{
    if (dropped_out) *dropped_out = false;
    if (!s_buf || !cursor || !out || out_cap <= 1) return 0;

    portENTER_CRITICAL(&s_lock);

    uint32_t oldest = (s_write_pos > LOG_BUF_SIZE) ? (s_write_pos - LOG_BUF_SIZE) : 0;
    if (cursor->pos < oldest) {
        cursor->pos = oldest;          /* fell behind, writer already overwrote it */
        if (dropped_out) *dropped_out = true;
    }

    uint32_t avail = s_write_pos - cursor->pos;
    size_t   want  = (avail < (out_cap - 1)) ? avail : (out_cap - 1);

    uint32_t start = cursor->pos % LOG_BUF_SIZE;
    size_t   first = LOG_BUF_SIZE - start;
    if (first > want) first = want;

    memcpy(out, s_buf + start, first);
    if (want > first)
        memcpy(out + first, s_buf, want - first);

    cursor->pos += (uint32_t)want;

    portEXIT_CRITICAL(&s_lock);

    out[want] = '\0';
    return want;
}
