#include "ui_keyboard.h"
#include "usb/usb_hid.h"
#include "lvgl.h"
#include "esp_log.h"
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static const char *TAG = "KB";

#define SCREEN_W    800
#define SCREEN_H    480

/*
 * 60% layout, 5 rows x 15 units. Widths are in quarter units (4 = 1u) so
 * 1.25u / 1.5u / 1.75u / 2.25u keys fit the same grid.
 *
 *   1u step  = 52 px (48 key + 4 gap)
 *   key px   = w4 * 13 - KB_GAP
 *   panel W  = KB_PAD*2 + 15*52 - KB_GAP = 792
 *   panel H  = KB_PAD*2 +  5*52 - KB_GAP = 272
 *
 * Slightly smaller keys than the old 54 px grid -- 15 columns of 54 px
 * would not fit 800 px.
 */
#define KB_PAD       8
#define KB_GAP       4
#define KB_UNIT4     13                                   /* px per quarter unit */
#define KB_STEP      (KB_UNIT4 * 4)                       /* 52 */
#define KB_KEY_H     (KB_STEP - KB_GAP)                   /* 48 */
#define KB_COLS_U    15
#define KB_ROWS      5
#define KB_PANEL_W   (KB_PAD * 2 + KB_COLS_U * KB_STEP - KB_GAP)
#define KB_PANEL_H   (KB_PAD * 2 + KB_ROWS * KB_STEP - KB_GAP)

#define KB_DOUBLE_TAP_MS  350   /* second tap on a modifier within this = lock */
#define KB_TIMER_MS        10   /* HID flush + auto-repeat tick */
#define KB_REPEAT_DELAY_MS 400  /* hold this long before auto-repeat starts */
#define KB_REPEAT_RATE_MS  35   /* then one repeat per this */

/* Colors */
#define COL_PANEL      0x1e1e1e
#define COL_KEY        0x3a3a3a
#define COL_KEY_MOD    0x2c2c2c    /* modifiers / editing keys */
#define COL_KEY_PRESS  0x585858
#define COL_ACTIVE     0x0055cc    /* one-shot modifier */
#define COL_LOCKED     0x2a7fff    /* locked modifier */
#define COL_TEXT       0xeeeeee
#define COL_TEXT_DIM   0xaaaaaa

/* -----------------------------------------------------------------------
 * Key table
 *
 * vk / fn_vk are Windows virtual-key codes -- the PC side will feed them
 * to SendInput as VK (not Unicode) so the Windows IME can turn them into
 * Chinese. The "IME" key (old Caps position) sends a plain, non-sticky
 * Shift tap -- that's what toggles Chinese/English in the Windows IME.
 * ----------------------------------------------------------------------- */
typedef enum {
    KK_NORMAL,      /* plain key: lbl / shift_lbl / fn_lbl */
    KK_LETTER,      /* a-z: upper case while Shift is active */
    KK_MOD,         /* sticky modifier, mod = MOD_* bit */
    KK_GAP,         /* empty space, no button */
} key_kind_t;

/* Same bit values as HID_KB_MOD_* in usb_hid.h, so mods can be sent as-is
 * after masking off MOD_FN. */
#define MOD_SHIFT  HID_KB_MOD_SHIFT
#define MOD_CTRL   HID_KB_MOD_CTRL
#define MOD_ALT    HID_KB_MOD_ALT
#define MOD_WIN    HID_KB_MOD_WIN
#define MOD_FN     0x10   /* local layer only, never sent */
#define MOD_SEND_MASK (MOD_SHIFT | MOD_CTRL | MOD_ALT | MOD_WIN)

typedef struct {
    uint8_t      w4;          /* width in quarter units; 0 = end of row */
    key_kind_t   kind;
    uint8_t      vk;
    const char  *lbl;
    const char  *shift_lbl;   /* NULL = same as lbl */
    uint8_t      fn_vk;       /* 0 = no Fn function */
    const char  *fn_lbl;
    uint8_t      mod;         /* KK_MOD only */
} kb_key_t;

#define K(w, vk, l, sl)          { w, KK_NORMAL, vk, l, sl, 0, NULL, 0 }
#define KF(w, vk, l, sl, fv, fl) { w, KK_NORMAL, vk, l, sl, fv, fl, 0 }
#define KL(c, vk)                { 4, KK_LETTER, vk, c, NULL, 0, NULL, 0 }
#define KM(w, vk, l, m)          { w, KK_MOD, vk, l, NULL, 0, NULL, m }
#define KGAP(w)                  { w, KK_GAP, 0, NULL, NULL, 0, NULL, 0 }
#define KEND                     { 0 }

static const kb_key_t s_row0[] = {
    KF(4, 0x1B, "Esc", NULL, 0xC0, "`"),
    KF(4, '1', "1", "!", 0x70, "F1"),  KF(4, '2', "2", "@", 0x71, "F2"),
    KF(4, '3', "3", "#", 0x72, "F3"),  KF(4, '4', "4", "$", 0x73, "F4"),
    KF(4, '5', "5", "%", 0x74, "F5"),  KF(4, '6', "6", "^", 0x75, "F6"),
    KF(4, '7', "7", "&", 0x76, "F7"),  KF(4, '8', "8", "*", 0x77, "F8"),
    KF(4, '9', "9", "(", 0x78, "F9"),  KF(4, '0', "0", ")", 0x79, "F10"),
    KF(4, 0xBD, "-", "_", 0x7A, "F11"), KF(4, 0xBB, "=", "+", 0x7B, "F12"),
    KF(8, 0x08, LV_SYMBOL_BACKSPACE, NULL, 0x2E, "Del"),
    KEND
};
static const kb_key_t s_row1[] = {
    K(6, 0x09, "Tab", NULL),
    KL("q", 'Q'), KL("w", 'W'), KL("e", 'E'), KL("r", 'R'), KL("t", 'T'),
    KL("y", 'Y'), KL("u", 'U'), KL("i", 'I'), KL("o", 'O'), KL("p", 'P'),
    K(4, 0xDB, "[", "{"), K(4, 0xDD, "]", "}"),
    K(6, 0xDC, "\\", "|"),
    KEND
};
static const kb_key_t s_row2[] = {
    K(7, 0xA0, "IME", NULL),            /* plain Shift tap = IME Chinese/English */
    KL("a", 'A'), KL("s", 'S'), KL("d", 'D'), KL("f", 'F'), KL("g", 'G'),
    KL("h", 'H'), KL("j", 'J'), KL("k", 'K'), KL("l", 'L'),
    K(4, 0xBA, ";", ":"), K(4, 0xDE, "'", "\""),
    K(9, 0x0D, "Enter", NULL),
    KEND
};
static const kb_key_t s_row3[] = {
    KM(9, 0xA0, "Shift", MOD_SHIFT),
    KL("z", 'Z'), KL("x", 'X'), KL("c", 'C'), KL("v", 'V'), KL("b", 'B'),
    KL("n", 'N'), KL("m", 'M'),
    K(4, 0xBC, ",", "<"), K(4, 0xBE, ".", ">"), K(4, 0xBF, "/", "?"),
    KGAP(3),                                              /* up arrow sits right above down */
    KF(4, 0x26, LV_SYMBOL_UP, NULL, 0x21, "PgUp"),
    KGAP(4),
    KEND
};
static const kb_key_t s_row4[] = {
    KM(5, 0xA2, "Ctrl", MOD_CTRL),
    KM(5, 0x5B, "Win",  MOD_WIN),
    KM(5, 0xA4, "Alt",  MOD_ALT),
    K(25, 0x20, "", NULL),
    KM(4, 0x00, "Fn",   MOD_FN),
    KM(4, 0xA3, "Ctrl", MOD_CTRL),
    KF(4, 0x25, LV_SYMBOL_LEFT,  NULL, 0x24, "Home"),
    KF(4, 0x28, LV_SYMBOL_DOWN,  NULL, 0x22, "PgDn"),
    KF(4, 0x27, LV_SYMBOL_RIGHT, NULL, 0x23, "End"),
    KEND
};

static const kb_key_t *const s_rows[KB_ROWS] = { s_row0, s_row1, s_row2, s_row3, s_row4 };

#define KB_MAX_KEYS  72

/* -----------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */
static lv_obj_t        *s_screen = NULL;
static lv_obj_t        *s_btn[KB_MAX_KEYS];
static lv_obj_t        *s_lbl[KB_MAX_KEYS];
static const kb_key_t  *s_def[KB_MAX_KEYS];
static int              s_key_count = 0;

/* Last applied label text / bg color per key -- refresh_keys() only
 * touches widgets whose look actually changes, so tapping Ctrl redraws two
 * buttons instead of invalidating every label on the panel. */
static const char      *s_cur_txt[KB_MAX_KEYS];
static uint32_t         s_cur_col[KB_MAX_KEYS];

static uint8_t  s_mod_oneshot = 0;    /* released after the next normal key */
static uint8_t  s_mod_locked  = 0;    /* held until tapped again */
static uint8_t  s_last_mod_tap     = 0;

/* Currently held (pressed) normal key -- single touch, so at most one. The
 * vk/mods captured at press time are reused for repeats and the release,
 * so a modifier/Fn change mid-hold can't produce a mismatched key-up. */
static bool        s_down        = false;
static uint8_t     s_down_vk     = 0;
static uint8_t     s_down_mods   = 0;
static bool        s_down_repeat = false;   /* this key auto-repeats */
static uint32_t    s_down_tick   = 0;
static uint32_t    s_repeat_tick = 0;
static lv_timer_t *s_timer       = NULL;
static uint32_t s_last_mod_tap_ms  = 0;

static uint8_t active_mods(void) { return s_mod_oneshot | s_mod_locked; }

/* -----------------------------------------------------------------------
 * Visual refresh
 * ----------------------------------------------------------------------- */
static const char *const s_upper[26] = {
    "A","B","C","D","E","F","G","H","I","J","K","L","M",
    "N","O","P","Q","R","S","T","U","V","W","X","Y","Z",
};

static void set_label(int i, const char *txt)
{
    if (s_cur_txt[i] == txt) return;
    s_cur_txt[i] = txt;
    lv_label_set_text_static(s_lbl[i], txt);
}

static void set_mod_look(int i, uint32_t col, bool locked)
{
    if (s_cur_col[i] == col) return;
    s_cur_col[i] = col;
    lv_obj_set_style_bg_color(s_btn[i], lv_color_hex(col), 0);
    lv_obj_set_style_border_width(s_btn[i], locked ? 2 : 0, 0);
}

static void refresh_keys(void)
{
    uint8_t mods  = active_mods();
    bool    shift = (mods & MOD_SHIFT) != 0;
    bool    fn    = (mods & MOD_FN) != 0;

    for (int i = 0; i < s_key_count; i++) {
        const kb_key_t *k = s_def[i];

        if (k->kind == KK_LETTER) {
            set_label(i, shift ? s_upper[k->vk - 'A'] : k->lbl);
        } else if (k->kind == KK_NORMAL) {
            const char *txt = k->lbl;
            if (fn && k->fn_lbl)            txt = k->fn_lbl;
            else if (shift && k->shift_lbl) txt = k->shift_lbl;
            set_label(i, txt);
        } else if (k->kind == KK_MOD) {
            if (s_mod_locked & k->mod)       set_mod_look(i, COL_LOCKED, true);
            else if (s_mod_oneshot & k->mod) set_mod_look(i, COL_ACTIVE, false);
            else                             set_mod_look(i, COL_KEY_MOD, false);
        }
    }
}

/* -----------------------------------------------------------------------
 * Key handling
 *
 * Normal keys send a real down on PRESSED and up on RELEASED/PRESS_LOST
 * (HID page 0xFC, see usb_hid.h), with auto-repeat generated here while
 * held -- injected keys get no typematic repeat from Windows. Modifiers
 * are sticky (tap = next key only, double tap = lock) and travel with
 * each key event; ESDeckPC wraps the key with the matching modifier
 * down/up.
 * ----------------------------------------------------------------------- */
static void key_release(void)
{
    if (!s_down) return;
    s_down = false;
    usb_hid_kb_key(false, s_down_vk, s_down_mods);
    usb_hid_kb_flush();

    /* One-shot modifiers apply to exactly one key. */
    s_last_mod_tap = 0;
    if (s_mod_oneshot) {
        s_mod_oneshot = 0;
        refresh_keys();
    }
}

static void key_press(const kb_key_t *k)
{
    if (s_down) key_release();   /* shouldn't happen with single touch */

    uint8_t mods = active_mods();
    bool    fn   = (mods & MOD_FN) != 0;
    uint8_t vk   = (fn && k->fn_vk) ? k->fn_vk : k->vk;
    if (vk == 0) return;

    s_down        = true;
    s_down_vk     = vk;
    s_down_mods   = mods & MOD_SEND_MASK;
    s_down_repeat = (vk != 0xA0);   /* the IME Shift tap must never repeat */
    s_down_tick   = lv_tick_get();
    s_repeat_tick = s_down_tick;

    usb_hid_kb_key(true, s_down_vk, s_down_mods);
    usb_hid_kb_flush();
    ESP_LOGD(TAG, "down vk=0x%02X mods=0x%02X", s_down_vk, s_down_mods);
}

static void mod_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_key_count) return;
    const kb_key_t *k = s_def[idx];

    uint32_t now = lv_tick_get();
    bool double_tap = (s_last_mod_tap == k->mod) &&
                      (lv_tick_elaps(s_last_mod_tap_ms) < KB_DOUBLE_TAP_MS);

    if (s_mod_locked & k->mod) {
        s_mod_locked &= (uint8_t)~k->mod;               /* locked -> off */
    } else if ((s_mod_oneshot & k->mod) && double_tap) {
        s_mod_oneshot &= (uint8_t)~k->mod;              /* one-shot -> locked */
        s_mod_locked  |= k->mod;
    } else if (s_mod_oneshot & k->mod) {
        s_mod_oneshot &= (uint8_t)~k->mod;              /* one-shot -> off */
    } else {
        s_mod_oneshot |= k->mod;                        /* off -> one-shot */
    }

    s_last_mod_tap    = k->mod;
    s_last_mod_tap_ms = now;
    refresh_keys();
}

static void key_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_key_count) return;

    if (code == LV_EVENT_PRESSED) {
        key_press(s_def[idx]);
    } else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) {
        key_release();
    }
}

/* 10 ms: auto-repeat for the held key + push queued key events out. Keeps
 * running after the keyboard closes until the queue drains, so a final
 * key-up is never stranded; then deletes itself. */
static void kb_timer_cb(lv_timer_t *t)
{
    if (s_down && s_down_repeat &&
        lv_tick_elaps(s_down_tick) >= KB_REPEAT_DELAY_MS &&
        lv_tick_elaps(s_repeat_tick) >= KB_REPEAT_RATE_MS) {
        s_repeat_tick = lv_tick_get();
        /* Only when the queue is empty -- never build up a backlog of
         * repeats that keeps typing after the finger lifts. */
        if (!usb_hid_kb_pending())
            usb_hid_kb_key(true, s_down_vk, s_down_mods);
    }

    usb_hid_kb_flush();

    if (!s_screen && !usb_hid_kb_pending()) {
        lv_timer_del(t);
        s_timer = NULL;
    }
}

/* -----------------------------------------------------------------------
 * Build
 * ----------------------------------------------------------------------- */
static void build_key(lv_obj_t *panel, const kb_key_t *k, int x, int y)
{
    if (s_key_count >= KB_MAX_KEYS) return;
    int idx = s_key_count++;

    bool special = (k->kind == KK_MOD ||
                    k->vk == 0x1B || k->vk == 0x08 || k->vk == 0x09 || k->vk == 0x0D ||
                    k->vk == 0xA0);

    lv_obj_t *btn = lv_btn_create(panel);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, k->w4 * KB_UNIT4 - KB_GAP, KB_KEY_H);
    lv_obj_set_style_bg_color(btn, lv_color_hex(special ? COL_KEY_MOD : COL_KEY), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(COL_KEY_PRESS), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_border_color(btn, lv_color_hex(0xffffff), 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_PRESS_LOCK);
    if (k->kind == KK_MOD)
        lv_obj_add_event_cb(btn, mod_cb, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    else
        lv_obj_add_event_cb(btn, key_cb, LV_EVENT_ALL, (void *)(intptr_t)idx);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text_static(lbl, k->lbl);
    /* Single characters / symbols a bit larger than word labels. */
    bool small_word = strlen(k->lbl) > 1 && (unsigned char)k->lbl[0] < 0x80;
    lv_obj_set_style_text_font(lbl, small_word ? &lv_font_montserrat_14 : &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(special ? COL_TEXT_DIM : COL_TEXT), 0);
    lv_obj_center(lbl);

    s_btn[idx] = btn;
    s_lbl[idx] = lbl;
    s_def[idx] = k;
    s_cur_txt[idx] = k->lbl;
    s_cur_col[idx] = special ? COL_KEY_MOD : COL_KEY;
}

static void build_keys(lv_obj_t *panel)
{
    s_key_count = 0;
    for (int r = 0; r < KB_ROWS; r++) {
        int x = KB_PAD;
        int y = KB_PAD + r * KB_STEP;
        for (const kb_key_t *k = s_rows[r]; k->w4; k++) {
            if (k->kind != KK_GAP) build_key(panel, k, x, y);
            x += k->w4 * KB_UNIT4;
        }
    }
}

static void keyboard_exit_cb(lv_event_t *e)
{
    (void)e;
    if (!s_screen) return;
    key_release();          /* never leave a key held on the PC */
    lv_obj_del(s_screen);
    s_screen = NULL;
    s_key_count = 0;
    /* Nothing stays latched once the keyboard is gone. */
    s_mod_oneshot = s_mod_locked = 0;
    s_last_mod_tap = 0;
}

/* -----------------------------------------------------------------------
 * Public
 * ----------------------------------------------------------------------- */
bool ui_keyboard_is_open(void)
{
    return s_screen != NULL;
}

void ui_keyboard_show(void)
{
    if (s_screen) return;   /* already open */

    lv_obj_t *scr = lv_scr_act();

    s_screen = lv_obj_create(scr);
    lv_obj_set_size(s_screen, SCREEN_W, SCREEN_H);
    lv_obj_set_pos(s_screen, 0, 0);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(s_screen, LV_OPA_70, 0);
    lv_obj_set_style_border_width(s_screen, 0, 0);
    lv_obj_set_style_radius(s_screen, 0, 0);
    lv_obj_clear_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_screen, keyboard_exit_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *panel = lv_obj_create(s_screen);
    lv_obj_set_size(panel, KB_PANEL_W, KB_PANEL_H);
    lv_obj_center(panel);
    lv_obj_set_style_bg_color(panel, lv_color_hex(COL_PANEL), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    /* Taps on the panel itself (gaps between keys) must not close it. */
    lv_obj_add_event_cb(panel, lv_event_stop_processing, LV_EVENT_CLICKED, NULL);

    build_keys(panel);
    refresh_keys();

    if (!s_timer) s_timer = lv_timer_create(kb_timer_cb, KB_TIMER_MS, NULL);
}
