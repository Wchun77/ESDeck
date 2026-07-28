#include "ui_log_view.h"
#include "ui.h"
#include "sys_log.h"
#include "lvgl.h"
#include "esp_heap_caps.h"
#include <stdio.h>
#include <string.h>

/* -----------------------------------------------------------------------
 * Every earlier approach here (one giant label, then multiple page-sized
 * labels) kept hitting lv_txt_get_size's "integer overflow while
 * calculating text height" -- this LVGL build has LV_USE_LARGE_COORD off,
 * so lv_coord_t only has a 13-bit effective range (LV_COORD_MAX == 8191).
 * Any approach that hands LVGL enough accumulated text/height to
 * approach that -- even spread across several page objects, since it's
 * the *container's* total scrollable height that matters, not any one
 * label -- was always going to run into this eventually; there just
 * isn't 8191px of room for a few hundred lines of scrollback rendered as
 * real, currently-on-screen-sized LVGL content.
 *
 * This version doesn't render scrollback at all. The full backlog lives
 * in a plain byte buffer (s_store) plus a line-offset index (s_lines),
 * completely outside of LVGL -- no coordinate limit applies to a plain
 * C array. Only the lines that actually fit in the visible window
 * (VISIBLE_LINES, computed from the font's line height) are ever handed
 * to a single small label, rebuilt on every scroll step or new line.
 * That label's content never exceeds one screen's worth, so it's nowhere
 * near the coordinate ceiling regardless of how much history exists.
 *
 * Scrolling is manual (dragging accumulates pixel delta -> shifts
 * s_top_line by whole lines -> re-render), not LVGL's native
 * content-height scrolling -- native scrolling is exactly the mechanism
 * that needs the content to fit inside lv_coord_t, which is what this
 * whole redesign is avoiding.
 * ----------------------------------------------------------------------- */

#define REFRESH_PERIOD_MS   300
#define READ_CHUNK_SIZE     2048
#define TITLEBAR_H          44
#define STORE_CAPACITY      SYS_LOG_RING_SIZE  /* no point exceeding what sys_log itself ever retains */
#define MAX_LINES           2000               /* defensive cap; STORE_CAPACITY runs out first in practice */
#define MAX_UNBROKEN_LINE   1024                /* force a synthetic line break past this many bytes with
                                                   * no real '\n' -- see append_raw()'s comment */
#define LABEL_CONTENT_W     (SCREEN_W - 24 - 8)  /* must match s_spangroup's set width, see ui_log_view_show() --
                                                   * used to clip lines to one visual row, see render_window() */

static lv_obj_t         *s_screen        = NULL;
static lv_obj_t         *s_cont          = NULL;
static lv_obj_t         *s_spangroup     = NULL;   /* one lv_span_t child per visible log line, see render_window() */
static lv_timer_t       *s_refresh_timer = NULL;
static sys_log_cursor_t  s_cursor;

static char     *s_store          = NULL;   /* PSRAM, STORE_CAPACITY bytes */
static uint32_t  *s_lines         = NULL;   /* PSRAM, MAX_LINES uint32_t -- s_lines[i] = start offset of complete line i */
static size_t     s_store_len     = 0;      /* bytes used in s_store */
static size_t     s_cur_line_start = 0;     /* offset where the still-in-progress (unterminated) line begins */
static size_t     s_line_count    = 0;      /* number of *complete* lines currently indexed */

static size_t     s_top_line      = 0;      /* index of the first line in the visible window */
static bool        s_following    = true;   /* auto-follow newest content unless the user scrolled up */
static int         s_visible_lines = 24;    /* computed at open time from the font's line height */
static lv_coord_t  s_line_height   = 18;

static int32_t     s_drag_accum_px = 0;

static void refresh_cb(lv_timer_t *t);
static void render_window(void);

static void log_view_close(void)
{
    if (s_refresh_timer) {
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = NULL;
    }
    if (s_screen) {
        lv_obj_del(s_screen);   /* deletes s_cont/s_spangroup too, they're children of it */
        s_screen    = NULL;
        s_cont      = NULL;
        s_spangroup = NULL;
    }
    if (s_store)  { heap_caps_free(s_store);  s_store  = NULL; }
    if (s_lines)  { heap_caps_free(s_lines);  s_lines  = NULL; }

    s_store_len      = 0;
    s_cur_line_start = 0;
    s_line_count      = 0;
    s_top_line        = 0;
    s_following       = true;
    s_drag_accum_px   = 0;
}

/* Closing used to be "tap anywhere," which turned out to trigger far too
 * easily -- any incidental tap (including the tail end of the 5-second
 * hold gesture that opens this screen, or the start of a drag-scroll)
 * could close it. An explicit X button in the title bar (see
 * ui_log_view_show()) is the only way out now, so this can just close
 * directly with no guard logic needed. */
static void close_cb(lv_event_t *e)
{
    (void)e;
    log_view_close();
}

/* LVGL's built-in fonts here (montserrat_*) only cover plain ASCII --
 * some things that end up in the log stream (box-drawing table dumps
 * etc.) contain multi-byte UTF-8 codepoints outside that range. Drawing
 * one of those logs its own warning every time (lv_draw_sw_letter: glyph
 * dsc. not found), which -- since this viewer is displaying the very log
 * stream those warnings get written to -- turns into repeated warning
 * spam for as long as the offending characters stay on screen. Replacing
 * each non-ASCII codepoint with a single '?' before it ever reaches the
 * store avoids LVGL ever attempting to draw an unsupported glyph. Safe to
 * do in place: output position never overtakes input position.
 *
 * A claimed multi-byte sequence is only consumed as a whole (one '?' for
 * the whole codepoint) after checking it actually has that many bytes
 * available AND they look like real continuation bytes (0x80-0xBF).
 * Without that check, a lead byte with no valid continuation -- which
 * does happen in practice: sys_log.c's log_vprintf() formats each
 * ESP_LOG call into a fixed-size stack buffer, and a single call whose
 * output is longer than that (e.g. tusb_desc's box-drawing descriptor
 * table dump, one ESP_LOGI call for the whole multi-line table) gets cut
 * off wherever that limit lands, sometimes mid-codepoint -- would blindly
 * skip ahead into whatever comes next in the stream and eat it as if it
 * were part of that sequence. That "whatever comes next" is frequently
 * the very next, unrelated ESP_LOG line's leading bytes (including its
 * '\n' if the cut lands right before one), which is exactly what
 * produced lines that failed to break and showed up in the wrong color
 * -- real content was being silently swallowed as bogus continuation
 * bytes. An invalid/incomplete sequence now only ever costs its own
 * single lead byte, leaving every byte after it untouched. */
static size_t sanitize_ascii_inplace(char *buf, size_t len)
{
    size_t oi = 0;
    for (size_t i = 0; i < len; ) {
        unsigned char c = (unsigned char)buf[i];
        if (c < 0x80) {
            buf[oi++] = (char)c;
            i++;
            continue;
        }

        size_t seq_len;
        if      ((c & 0xE0) == 0xC0) seq_len = 2;
        else if ((c & 0xF0) == 0xE0) seq_len = 3;
        else if ((c & 0xF8) == 0xF0) seq_len = 4;
        else                          seq_len = 1;   /* stray/invalid lead byte */

        bool valid = (i + seq_len <= len);
        for (size_t k = 1; valid && k < seq_len; k++)
            if (((unsigned char)buf[i + k] & 0xC0) != 0x80) valid = false;

        i += valid ? seq_len : 1;
        buf[oi++] = '?';
    }
    buf[oi] = '\0';
    return oi;
}

/* Drops the oldest n complete lines from the index and compacts s_store
 * so the freed space is reclaimed at the front. Adjusts s_top_line so
 * whatever the user was looking at doesn't jump around. */
static void drop_oldest_lines(size_t n)
{
    if (n > s_line_count) n = s_line_count;
    if (n == 0) return;

    size_t new_start = (n < s_line_count) ? s_lines[n] : s_cur_line_start;

    memmove(s_store, s_store + new_start, s_store_len - new_start);
    s_store_len      -= new_start;
    s_cur_line_start -= new_start;

    for (size_t i = n; i < s_line_count; i++)
        s_lines[i - n] -= (uint32_t)new_start;
    s_line_count -= n;

    s_top_line = (s_top_line >= n) ? (s_top_line - n) : 0;
}

/* Appends already-sanitized bytes to the store, indexing every complete
 * ('\n'-terminated) line as it's found. Makes room by dropping the
 * oldest lines first if the store or line index is full.
 *
 * MAX_UNBROKEN_LINE exists for the same reason the earlier page-based
 * design needed a byte backstop alongside its line-count one: not every
 * write into sys_log's ring buffer ends in '\n' (something building up
 * one printed row across several partial writes, e.g. the box-drawing
 * table dumps that started all of this) -- without a forced break, a
 * long enough stretch of content with no real newline would just grow
 * s_cur_line_start's line forever and it would never become a
 * *complete*, indexed, droppable line, defeating the trim logic above. */
static void append_raw(const char *data, size_t len)
{
    if (len == 0) return;

    while (s_store_len + len > STORE_CAPACITY) {
        if (s_line_count == 0) break;   /* nothing droppable left; append will just truncate below */
        drop_oldest_lines(s_line_count > 64 ? 64 : s_line_count);
    }

    size_t copy_len = len;
    if (s_store_len + copy_len > STORE_CAPACITY)
        copy_len = STORE_CAPACITY - s_store_len;
    if (copy_len == 0) return;

    memcpy(s_store + s_store_len, data, copy_len);
    size_t scan_start = s_store_len;
    s_store_len += copy_len;

    for (size_t i = scan_start; i < s_store_len; i++) {
        bool forced = (i - s_cur_line_start + 1) >= MAX_UNBROKEN_LINE;
        if (s_store[i] == '\n' || forced) {
            if (s_line_count >= MAX_LINES)
                drop_oldest_lines(MAX_LINES / 8);
            s_lines[s_line_count++] = (uint32_t)s_cur_line_start;
            s_cur_line_start = i + 1;
        }
    }
}

/* Content length of line li, not counting its trailing '\n' (if any).
 * Zero means the line is blank -- skipped entirely on screen, see
 * render_window() and follow_to_bottom(). Some log sources (NimBLE's port
 * layer, notably) print a genuine empty line after many of their
 * messages; those are real content, not a rendering bug, but they're not
 * worth a wasted row in a screen that only fits ~24 lines. */
static size_t line_content_len(size_t li)
{
    size_t line_start = s_lines[li];
    size_t line_end    = (li + 1 < s_line_count) ? s_lines[li + 1] : s_cur_line_start;
    size_t line_len    = line_end - line_start;
    bool has_nl = (line_len > 0 && s_store[line_start + line_len - 1] == '\n');
    return has_nl ? line_len - 1 : line_len;
}

/* Picks s_top_line so the *visible* (non-blank) lines ending at the
 * bottom of the backlog fill the screen, walking backward from the
 * newest line and skipping blanks -- a plain "s_line_count -
 * s_visible_lines" would under-fill the screen whenever blank lines fall
 * within that range. */
static void follow_to_bottom(void)
{
    size_t shown = 0;
    size_t li = s_line_count;
    while (li > 0 && shown < (size_t)s_visible_lines) {
        li--;
        if (line_content_len(li) > 0) shown++;
    }
    s_top_line = li;
}

/* ESP_LOG's default (color-disabled) format is "E (timestamp) tag: msg\n"
 * -- the level letter is always the first character of the line. Colors
 * mirror what idf.py monitor shows with ANSI colors on (see esp_log.h's
 * own LOG_COLOR_E/W/I/D), so this looks like what anyone used to a
 * normal serial monitor already expects. Anything that doesn't start
 * with a recognized level letter (box-drawing dumps, the "[...dropped]"
 * marker, plain printf output with no ESP_LOG prefix) falls back to a
 * dim neutral gray rather than guessing.
 *
 * Returns a real lv_color_t (was a "RRGGBB" hex string, back when this
 * got spliced into a #RRGGBB recolor tag -- see render_window()'s
 * comment for why that whole approach was replaced with per-line spans,
 * each with a real style color instead of a parsed one). */
static lv_color_t level_color(const char *line, size_t len)
{
    if (len == 0) return lv_color_hex(0x999999);
    switch (line[0]) {
        case 'E': return lv_color_hex(0xff5555);   /* error */
        case 'W': return lv_color_hex(0xffcc00);   /* warn */
        case 'I': return lv_color_hex(0x55ff55);   /* info */
        case 'D': return lv_color_hex(0x55ffff);   /* debug */
        case 'V': return lv_color_hex(0x999999);   /* verbose */
        default:  return lv_color_hex(0x999999);
    }
}

/* Largest prefix of txt[0..len) (in characters) whose rendered pixel
 * width is <= max_w, found by binary search over lv_txt_get_width() --
 * a handful of measurements per call, only for lines that need it (the
 * common case of a short line that already fits skips straight past). */
static size_t clamp_to_width(const char *txt, size_t len, lv_coord_t max_w)
{
    if (max_w <= 0) return 0;
    if (lv_txt_get_width(txt, (uint32_t)len, &lv_font_montserrat_14, 0, LV_TEXT_FLAG_NONE) <= max_w)
        return len;

    size_t lo = 0, hi = len;
    while (lo < hi) {
        size_t mid = lo + (hi - lo + 1) / 2;
        if (lv_txt_get_width(txt, (uint32_t)mid, &lv_font_montserrat_14, 0, LV_TEXT_FLAG_NONE) <= max_w)
            lo = mid;
        else
            hi = mid - 1;
    }
    return lo;
}

/* Rebuilds the visible span list from lines [s_top_line, s_top_line +
 * s_visible_lines) -- always a small, bounded slice regardless of how
 * much history exists in s_store. One lv_span_t per log line, colored
 * via a real style property (lv_style_set_text_color on the span's own
 * style) instead of an earlier design that built one giant string for a
 * single recolor-enabled label with hand-rolled #RRGGBB...# tags spliced
 * around each line.
 *
 * That earlier approach had a whole class of bugs baked into its
 * premise: a literal '#' in log content can't be represented at all once
 * label recolor is on (lv_draw_label.c's per-letter command-state parser
 * consumes every '#' as a delimiter in every state, so there's no way to
 * escape one), and hand-assembling one big buffer with careful
 * open/close/escape byte-accounting for every tag left room for a real
 * one-byte out-of-bounds write. Spans sidestep all of that -- color is a
 * real lv_style_t property per span, never parsed out of the text, so
 * there's no tag syntax to build, close, or escape.
 *
 * Line separation between spans needs its own explanation, since it's
 * *not* simply "put '\n' at the end of each span's text" -- a span
 * ending in "...content\n" with nothing after it is exactly the case
 * lv_span.c's own line-assembly can't distinguish from "this span's text
 * just ran out, go pull more from the next span to finish this same
 * visual line": internally it checks whether the byte right after the
 * detected line-break is this span's own NUL terminator, and if so
 * (which it always is here, since the '\n' is the very last thing before
 * it) treats it as *not* a definitive end-of-line, keeps scanning into
 * the next span, and concatenates it onto the same row instead of
 * starting a new one -- exactly the "text never breaks, everything
 * piles onto the same lines" symptom this replaced. The fix is to
 * prefix each span *after* the first with '\n' instead of suffixing the
 * previous one: the '\n' then has real content (this span's own line)
 * following it within the *same* span's string, so the NUL-adjacency
 * check no longer misfires, and the line-break is recognized as
 * definitive.
 *
 * Each line is still measured against LABEL_CONTENT_W and clamped via
 * clamp_to_width() before becoming a span -- not to protect a tag parser
 * anymore, just so one of our own stored "lines" always maps to exactly
 * one visual row, which is what s_visible_lines/s_top_line's line-count
 * scroll math assumes. */
static void render_window(void)
{
    if (s_line_count == 0) {
        while (lv_spangroup_get_child_cnt(s_spangroup) > 0) {
            lv_spangroup_del_span(s_spangroup, lv_spangroup_get_child(s_spangroup, 0));
        }
        lv_spangroup_refr_mode(s_spangroup);
        return;
    }

    size_t max_top = (s_line_count > (size_t)s_visible_lines) ? (s_line_count - s_visible_lines) : 0;
    if (s_top_line > max_top) s_top_line = max_top;

    /* Delete every existing child and rebuild the window from scratch on
     * every call, rather than trying to reuse/patch the previous span
     * list by index. The reuse approach that used to live here (matching
     * spans up by index and only adding/removing the difference) was the
     * source of an intermittent blank-region-at-the-bottom bug: it's easy
     * for the reused-vs-recreated bookkeeping to drift out of sync with
     * what's actually in the spangroup after enough add/remove churn
     * across refresh ticks and drag steps, silently leaving fewer visible
     * rows than s_visible_lines calls for. A handful of spans (at most
     * s_visible_lines, ~24-30) is cheap enough to delete and recreate
     * every 300ms/drag-step that the correctness win is worth the extra
     * alloc/free -- there's no perceptible cost at this scale. */
    while (lv_spangroup_get_child_cnt(s_spangroup) > 0) {
        lv_spangroup_del_span(s_spangroup, lv_spangroup_get_child(s_spangroup, 0));
    }

    /* One line's worth of scratch space -- leading '\n' (1) + clamped
     * content + "..." (3) + NUL (1). lv_span_set_text() copies this into
     * its own heap allocation, so reusing the same buffer every
     * iteration is safe. */
    static char line_buf[256];

    /* Walk forward from s_top_line, skipping blank lines entirely (see
     * line_content_len()'s comment) rather than giving each one its own
     * empty row -- shown counts actual rendered rows, li is the raw line
     * index (blank or not); the loop stops once the screen is full or the
     * backlog runs out, whichever comes first. */
    size_t shown = 0;
    for (size_t li = s_top_line; li < s_line_count && shown < (size_t)s_visible_lines; li++) {
        size_t content_len = line_content_len(li);
        if (content_len == 0) continue;   /* blank line -- skip, no row spent on it */

        size_t line_start = s_lines[li];

        static const char *ellipsis = "...";
        lv_coord_t ellipsis_w = lv_txt_get_width(ellipsis, 3, &lv_font_montserrat_14, 0, LV_TEXT_FLAG_NONE);
        size_t disp_len = content_len;
        bool clipped = false;
        if (lv_txt_get_width(s_store + line_start, (uint32_t)content_len, &lv_font_montserrat_14, 0, LV_TEXT_FLAG_NONE) > LABEL_CONTENT_W) {
            disp_len = clamp_to_width(s_store + line_start, content_len, LABEL_CONTENT_W - ellipsis_w);
            clipped = true;
        }

        size_t max_disp = sizeof(line_buf) - 5;   /* leading '\n' + "..." + NUL */
        if (disp_len > max_disp) disp_len = max_disp;   /* defensive; LABEL_CONTENT_W-fitting content is always far under this */

        size_t out_pos = 0;
        if (shown > 0) line_buf[out_pos++] = '\n';   /* see the function comment above -- ends the *previous* span's line */
        memcpy(line_buf + out_pos, s_store + line_start, disp_len);
        out_pos += disp_len;
        if (clipped) {
            line_buf[out_pos++] = '.';
            line_buf[out_pos++] = '.';
            line_buf[out_pos++] = '.';
        }
        line_buf[out_pos] = '\0';

        lv_span_t *span = lv_spangroup_new_span(s_spangroup);
        lv_span_set_text(span, line_buf);
        lv_style_set_text_color(&span->style, level_color(s_store + line_start, content_len));
        shown++;
    }

    lv_spangroup_refr_mode(s_spangroup);
}

/* Drag-to-scroll, entirely manual -- see file header comment for why this
 * isn't native LVGL content scrolling. Dragging up reveals newer content
 * (top_line increases); dragging down reveals older content. Scrolling
 * back down to the bottom resumes auto-follow. */
static void cont_pressing_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_get_act();
    if (!indev) return;

    lv_point_t vect;
    lv_indev_get_vect(indev, &vect);
    if (vect.y == 0) return;

    s_drag_accum_px -= vect.y;   /* drag finger up (negative vect.y) -> reveal newer -> top_line increases */

    bool moved = false;
    while (s_drag_accum_px >= s_line_height) {
        s_drag_accum_px -= s_line_height;
        if (s_top_line + s_visible_lines < s_line_count) { s_top_line++; moved = true; }
    }
    while (s_drag_accum_px <= -s_line_height) {
        s_drag_accum_px += s_line_height;
        if (s_top_line > 0) { s_top_line--; moved = true; }
    }

    if (!moved) return;

    s_following = (s_top_line + (size_t)s_visible_lines >= s_line_count);
    render_window();
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;

    char chunk[READ_CHUNK_SIZE];
    bool dropped;
    bool appended = false;

    for (;;) {
        size_t n = sys_log_read(&s_cursor, chunk, sizeof(chunk), &dropped);
        if (n == 0) break;
        appended = true;

        if (dropped) {
            static const char *marker = "[... older log lines dropped ...]\n";
            append_raw(marker, strlen(marker));
        }

        n = sanitize_ascii_inplace(chunk, n);
        append_raw(chunk, n);
    }

    if (!appended) return;

    if (s_following) follow_to_bottom();
    render_window();
}

void ui_log_view_show(void)
{
    log_view_close();   /* guard against a stray double-open */

    s_store = heap_caps_malloc(STORE_CAPACITY, MALLOC_CAP_SPIRAM);
    s_lines = heap_caps_malloc(MAX_LINES * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!s_store || !s_lines) {
        if (s_store) { heap_caps_free(s_store); s_store = NULL; }
        if (s_lines) { heap_caps_free(s_lines); s_lines = NULL; }
        return;   /* can't open without scratch memory -- nothing to show */
    }

    s_screen = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_screen, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_screen, 0, 0);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_radius(s_screen, 0, 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    /* Title bar -- the only way to close this screen is the X button
     * here. Tap-anywhere-to-close (the earlier design) fired far too
     * easily: the tail end of the 5-second hold gesture that opens this
     * screen, the start of a drag-scroll, anything incidental. */
    lv_obj_t *titlebar = lv_obj_create(s_screen);
    lv_obj_set_size(titlebar, SCREEN_W, TITLEBAR_H);
    lv_obj_set_pos(titlebar, 0, 0);
    lv_obj_set_style_bg_color(titlebar, lv_color_hex(0x1a1a1a), 0);
    lv_obj_set_style_border_width(titlebar, 0, 0);
    lv_obj_set_style_radius(titlebar, 0, 0);
    lv_obj_set_style_pad_all(titlebar, 0, 0);
    lv_obj_clear_flag(titlebar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title_lbl = lv_label_create(titlebar);
    lv_label_set_text(title_lbl, "Log");
    lv_obj_set_style_text_color(title_lbl, lv_color_hex(0xcccccc), 0);
    lv_obj_set_style_text_font(title_lbl, &lv_font_montserrat_20, 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 16, 0);

    lv_obj_t *close_btn = lv_btn_create(titlebar);
    lv_obj_set_size(close_btn, TITLEBAR_H - 8, TITLEBAR_H - 8);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(close_btn, lv_color_hex(0x2a2a2a), 0);
    lv_obj_set_style_radius(close_btn, 8, 0);
    lv_obj_add_event_cb(close_btn, close_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *close_lbl = lv_label_create(close_btn);
    lv_label_set_text(close_lbl, LV_SYMBOL_CLOSE);
    lv_obj_center(close_lbl);

    s_cont = lv_obj_create(s_screen);
    lv_obj_set_size(s_cont, SCREEN_W - 24, SCREEN_H - TITLEBAR_H - 24);
    lv_obj_set_pos(s_cont, 12, TITLEBAR_H + 12);
    lv_obj_set_style_bg_opa(s_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cont, 0, 0);
    lv_obj_set_style_pad_all(s_cont, 4, 0);
    /* Not scrollable -- this view is manually scrolled (see
     * cont_pressing_cb()), native content-height scrolling is what all
     * the earlier lv_coord_t overflow trouble came from. */
    lv_obj_clear_flag(s_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_cont, cont_pressing_cb, LV_EVENT_PRESSING, NULL);

    s_spangroup = lv_spangroup_create(s_cont);
    lv_coord_t spangroup_h = SCREEN_H - TITLEBAR_H - 24 - 8;
    lv_obj_set_size(s_spangroup, SCREEN_W - 24 - 8, spangroup_h);
    /* FIXED, not BREAK -- we do our own line wrapping/clipping
     * (clamp_to_width() in render_window()) and hand it exactly
     * s_visible_lines spans, so the spangroup itself doesn't need to
     * (and shouldn't) do any wrapping of its own; it just draws within
     * this fixed box. */
    lv_spangroup_set_mode(s_spangroup, LV_SPAN_MODE_FIXED);
    lv_spangroup_set_overflow(s_spangroup, LV_SPAN_OVERFLOW_CLIP);
    lv_spangroup_set_align(s_spangroup, LV_TEXT_ALIGN_LEFT);
    lv_obj_set_style_text_color(s_spangroup, lv_color_hex(0xcccccc), 0);   /* base/fallback -- every span sets its own color anyway */
    lv_obj_set_style_text_font(s_spangroup, &lv_font_montserrat_14, 0);
    lv_obj_set_style_pad_all(s_spangroup, 0, 0);
    /* lv_spangroup, unlike lv_label, does not clear LV_OBJ_FLAG_CLICKABLE
     * in its constructor -- it inherits the plain lv_obj default of
     * clickable-by-default (see lv_obj_create's obj->flags init). Left
     * alone, that makes it the deepest clickable hit under the touch
     * point, so indev delivers PRESSING straight to the spangroup and
     * cont_pressing_cb (registered on s_cont, its parent) never fires --
     * this, not span churn, was why drag-scrolling stopped working after
     * switching from a label. */
    lv_obj_clear_flag(s_spangroup, LV_OBJ_FLAG_CLICKABLE);

    s_line_height   = lv_font_get_line_height(&lv_font_montserrat_14);
    s_visible_lines = (int)(spangroup_h / (s_line_height > 0 ? s_line_height : 18));
    if (s_visible_lines < 1) s_visible_lines = 1;

    s_cursor = sys_log_cursor_start();
    for (;;) {
        char   chunk[READ_CHUNK_SIZE];
        bool   dropped;
        size_t n = sys_log_read(&s_cursor, chunk, sizeof(chunk), &dropped);
        if (n == 0) break;
        if (dropped) {
            static const char *marker = "[... older log lines dropped ...]\n";
            append_raw(marker, strlen(marker));
        }
        n = sanitize_ascii_inplace(chunk, n);
        append_raw(chunk, n);
    }

    follow_to_bottom();
    render_window();

    s_refresh_timer = lv_timer_create(refresh_cb, REFRESH_PERIOD_MS, NULL);
}
