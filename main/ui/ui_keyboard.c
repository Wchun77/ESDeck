#include "ui_keyboard.h"
#include "usb/usb_hid.h"
#include "lvgl.h"
#include "esp_log.h"

#define SCREEN_W    800
#define SCREEN_H    480

/*
 * Grid constants:
 *   KEY_W / KEY_H = 54
 *   GAP           = 4
 *   STEP          = 58  (KEY_W + GAP)
 *   PAD           = 20
 *
 * Panel size:
 *   W = PAD + 11*STEP + KEY_W + PAD = 732
 *   H = PAD +  3*STEP + KEY_H + PAD = 268
 */
#define KB_PAD      20
#define KB_KEY_W    54
#define KB_KEY_H    54
#define KB_GAP      4
#define KB_STEP     58
#define KB_COL(n)   (KB_PAD + (n) * KB_STEP)
#define KB_ROW(n)   (KB_PAD + (n) * KB_STEP)
#define KB_PANEL_W  (KB_PAD + 11 * KB_STEP + KB_KEY_W + KB_PAD)
#define KB_PANEL_H  (KB_PAD +  3 * KB_STEP + KB_KEY_H + KB_PAD)

/* -----------------------------------------------------------------------
 * State
 * ----------------------------------------------------------------------- */
static lv_obj_t *s_screen    = NULL;
static lv_obj_t *s_abc_cont  = NULL;
static lv_obj_t *s_sym_cont  = NULL;
static lv_obj_t *s_tab_abc   = NULL;
static lv_obj_t *s_tab_sym   = NULL;

static lv_obj_t *s_shift_btn    = NULL;
static bool      s_shift_active = false;
static lv_obj_t *s_letter_lbls[26];

static lv_obj_t *s_sym_tab_abc      = NULL;
static lv_obj_t *s_sym_tab_sym      = NULL;
static lv_obj_t *s_sym_shift_btn    = NULL;
static bool      s_sym_shift_active = false;
static lv_obj_t *s_sym_digit_lbls[10];
static lv_obj_t *s_sym_punct_lbls[10];

/* -----------------------------------------------------------------------
 * Key builder helper
 * ----------------------------------------------------------------------- */
static lv_obj_t *kb_btn(lv_obj_t *parent, int x, int y, int w, int h,
                         const char *text, uint8_t keycode, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_btn_create(parent);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3a3a3a), 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x585858), LV_STATE_PRESSED);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, (void *)(uintptr_t)keycode);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_hex(0xeeeeee), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
    lv_obj_center(lbl);
    return btn;
}

#define KB_KEY(parent, col, row, text, kc, cb) \
    kb_btn(parent, KB_COL(col), KB_ROW(row), KB_KEY_W, KB_KEY_H, text, kc, cb)

/* -----------------------------------------------------------------------
 * Shift / sym-shift reset helpers
 * ----------------------------------------------------------------------- */
static void kb_abc_shift_reset(void)
{
    static const char *lower[] = {
        "q","w","e","r","t","y","u","i","o","p",
        "a","s","d","f","g","h","j","k","l",
        "z","x","c","v","b","n","m"
    };
    if (!s_shift_active) return;
    s_shift_active = false;
    if (s_shift_btn) lv_obj_clear_state(s_shift_btn, LV_STATE_CHECKED);
    for (int i = 0; i < 26; i++)
        if (s_letter_lbls[i]) lv_label_set_text(s_letter_lbls[i], lower[i]);
}

static void kb_sym_shift_reset(void)
{
    static const char *dig[]  = { "1","2","3","4","5","6","7","8","9","0" };
    static const char *punc[] = { "`","[","]","\\",";","'",",",".","/" };
    if (!s_sym_shift_active) return;
    s_sym_shift_active = false;
    if (s_sym_shift_btn) lv_obj_clear_state(s_sym_shift_btn, LV_STATE_CHECKED);
    for (int i = 0; i < 10; i++)
        if (s_sym_digit_lbls[i]) lv_label_set_text(s_sym_digit_lbls[i], dig[i]);
    for (int i = 0; i < 9; i++)
        if (s_sym_punct_lbls[i]) lv_label_set_text(s_sym_punct_lbls[i], punc[i]);
}

/* -----------------------------------------------------------------------
 * Key event callbacks
 * ----------------------------------------------------------------------- */
static void kb_key_cb(lv_event_t *e)
{
    uint8_t keycode  = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    uint8_t key_byte = keycode;

    if (s_shift_active) {
        key_byte |= 0x80;
        kb_abc_shift_reset();
    } else if (s_sym_shift_active) {
        key_byte |= 0x80;
        kb_sym_shift_reset();
    }

    usb_hid_send(0x00, key_byte);
    ESP_LOGI("KB", "key=0x%02X shift=%d", keycode, (key_byte >> 7) & 1);
}

static void kb_fixed_key_cb(lv_event_t *e)
{
    uint8_t keycode = (uint8_t)(uintptr_t)lv_event_get_user_data(e);
    usb_hid_send(0x00, keycode);
    ESP_LOGI("KB", "fixed=0x%02X", keycode);
}

static void kb_shift_cb(lv_event_t *e)
{
    static const char *lower[] = {
        "q","w","e","r","t","y","u","i","o","p",
        "a","s","d","f","g","h","j","k","l",
        "z","x","c","v","b","n","m"
    };
    static const char *upper[] = {
        "Q","W","E","R","T","Y","U","I","O","P",
        "A","S","D","F","G","H","J","K","L",
        "Z","X","C","V","B","N","M"
    };
    s_shift_active = !s_shift_active;
    if (s_shift_btn) {
        if (s_shift_active) lv_obj_add_state(s_shift_btn, LV_STATE_CHECKED);
        else                lv_obj_clear_state(s_shift_btn, LV_STATE_CHECKED);
    }
    const char **labels = s_shift_active ? upper : lower;
    for (int i = 0; i < 26; i++)
        if (s_letter_lbls[i]) lv_label_set_text(s_letter_lbls[i], labels[i]);
}

static void kb_sym_shift_cb(lv_event_t *e)
{
    static const char *dig_n[]  = { "1","2","3","4","5","6","7","8","9","0" };
    static const char *dig_s[]  = { "!","@","#","$","%","^","&","*","(",")" };
    static const char *punc_n[] = { "`","[","]","\\",";","'",",",".","/" };
    static const char *punc_s[] = { "~","{","}","|",":","\""," <",">","?" };

    s_sym_shift_active = !s_sym_shift_active;
    if (s_sym_shift_btn) {
        if (s_sym_shift_active) lv_obj_add_state(s_sym_shift_btn, LV_STATE_CHECKED);
        else                    lv_obj_clear_state(s_sym_shift_btn, LV_STATE_CHECKED);
    }
    const char **dl = s_sym_shift_active ? dig_s  : dig_n;
    const char **pl = s_sym_shift_active ? punc_s : punc_n;
    for (int i = 0; i < 10; i++)
        if (s_sym_digit_lbls[i]) lv_label_set_text(s_sym_digit_lbls[i], dl[i]);
    for (int i = 0; i < 9; i++)
        if (s_sym_punct_lbls[i]) lv_label_set_text(s_sym_punct_lbls[i], pl[i]);
}

static void kb_tab_abc_cb(lv_event_t *e)
{
    kb_sym_shift_reset();
    lv_obj_clear_flag(s_abc_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_sym_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_state(s_tab_abc, LV_STATE_CHECKED);
    lv_obj_clear_state(s_tab_sym, LV_STATE_CHECKED);
    if (s_sym_tab_abc) lv_obj_add_state(s_sym_tab_abc, LV_STATE_CHECKED);
    if (s_sym_tab_sym) lv_obj_clear_state(s_sym_tab_sym, LV_STATE_CHECKED);
}

static void kb_tab_sym_cb(lv_event_t *e)
{
    kb_abc_shift_reset();
    lv_obj_add_flag(s_abc_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_sym_cont, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_state(s_tab_abc, LV_STATE_CHECKED);
    lv_obj_add_state(s_tab_sym, LV_STATE_CHECKED);
    if (s_sym_tab_abc) lv_obj_clear_state(s_sym_tab_abc, LV_STATE_CHECKED);
    if (s_sym_tab_sym) lv_obj_add_state(s_sym_tab_sym, LV_STATE_CHECKED);
}

static void keyboard_exit_cb(lv_event_t *e)
{
    if (s_screen) {
        lv_obj_del(s_screen);
        s_screen         = NULL;
        s_abc_cont       = NULL;
        s_sym_cont       = NULL;
        s_tab_abc        = NULL;
        s_tab_sym        = NULL;
        s_shift_btn      = NULL;
        s_shift_active   = false;
        s_sym_tab_abc    = NULL;
        s_sym_tab_sym    = NULL;
        s_sym_shift_btn  = NULL;
        s_sym_shift_active = false;
    }
}

/* -----------------------------------------------------------------------
 * Page builders
 * ----------------------------------------------------------------------- */
static void kb_build_abc(lv_obj_t *cont)
{
    static const uint8_t kc[] = {
        0x14,0x1A,0x08,0x15,0x17,0x1C,0x18,0x0C,0x12,0x13,
        0x04,0x16,0x07,0x09,0x0A,0x0B,0x0D,0x0E,0x0F,
        0x1D,0x1B,0x06,0x19,0x05,0x11,0x10
    };
    static const char *lower[] = {
        "q","w","e","r","t","y","u","i","o","p",
        "a","s","d","f","g","h","j","k","l",
        "z","x","c","v","b","n","m"
    };
    static const int row0[] = { 1,2,3,4,5,6,7,8,9,10 };
    static const int row1[] = { 1,2,3,4,5,6,7,8,9 };
    static const int row2[] = { 2,3,4,5,6,7,8 };

    KB_KEY(cont, 0, 0, "Esc", 0x29, kb_key_cb);
    for (int i = 0; i < 10; i++) {
        lv_obj_t *b = kb_btn(cont, KB_COL(row0[i]), KB_ROW(0),
                             KB_KEY_W, KB_KEY_H, lower[i], kc[i], kb_key_cb);
        s_letter_lbls[i] = lv_obj_get_child(b, 0);
    }
    KB_KEY(cont, 11, 0, LV_SYMBOL_BACKSPACE, 0x2A, kb_fixed_key_cb);

    KB_KEY(cont, 0, 1, "Tab", 0x2B, kb_key_cb);
    for (int i = 0; i < 9; i++) {
        lv_obj_t *b = kb_btn(cont, KB_COL(row1[i]), KB_ROW(1),
                             KB_KEY_W, KB_KEY_H, lower[10+i], kc[10+i], kb_key_cb);
        s_letter_lbls[10+i] = lv_obj_get_child(b, 0);
    }
    kb_btn(cont, KB_COL(10), KB_ROW(1), KB_KEY_W*2+KB_GAP, KB_KEY_H,
           LV_SYMBOL_NEW_LINE, 0x28, kb_fixed_key_cb);

    s_shift_btn = kb_btn(cont, KB_COL(0), KB_ROW(2), KB_KEY_W*2+KB_GAP, KB_KEY_H,
                          LV_SYMBOL_UP " Shift", 0x00, kb_shift_cb);
    lv_obj_add_flag(s_shift_btn, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(s_shift_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_color(s_shift_btn, lv_color_hex(0x777777), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_shift_btn, lv_color_hex(0x0055cc), LV_STATE_CHECKED);
    for (int i = 0; i < 7; i++) {
        lv_obj_t *b = kb_btn(cont, KB_COL(row2[i]), KB_ROW(2),
                             KB_KEY_W, KB_KEY_H, lower[19+i], kc[19+i], kb_key_cb);
        s_letter_lbls[19+i] = lv_obj_get_child(b, 0);
    }
    KB_KEY(cont, 10, 2, LV_SYMBOL_UP, 0x52, kb_fixed_key_cb);

    kb_btn(cont, KB_COL(0), KB_ROW(3), KB_KEY_W*7+KB_GAP*6, KB_KEY_H,
           "Space", 0x2C, kb_fixed_key_cb);

    s_tab_abc = kb_btn(cont, KB_COL(7), KB_ROW(3), KB_KEY_W, KB_KEY_H,
                        "ABC", 0x00, kb_tab_abc_cb);
    lv_obj_add_flag(s_tab_abc, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_state(s_tab_abc, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_tab_abc, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_color(s_tab_abc, lv_color_hex(0x0055cc), LV_STATE_CHECKED);

    s_tab_sym = kb_btn(cont, KB_COL(8), KB_ROW(3), KB_KEY_W, KB_KEY_H,
                        "!@#", 0x00, kb_tab_sym_cb);
    lv_obj_add_flag(s_tab_sym, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(s_tab_sym, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_color(s_tab_sym, lv_color_hex(0x0055cc), LV_STATE_CHECKED);

    KB_KEY(cont,  9, 3, LV_SYMBOL_LEFT,  0x50, kb_fixed_key_cb);
    KB_KEY(cont, 10, 3, LV_SYMBOL_DOWN,  0x51, kb_fixed_key_cb);
    KB_KEY(cont, 11, 3, LV_SYMBOL_RIGHT, 0x4F, kb_fixed_key_cb);
}

static void kb_build_sym(lv_obj_t *cont)
{
    static const uint8_t dig_kc[]  = {
        0x1E,0x1F,0x20,0x21,0x22,0x23,0x24,0x25,0x26,0x27
    };
    static const char *dig_n[] = { "1","2","3","4","5","6","7","8","9","0" };
    static const uint8_t punc_kc[] = {
        0x35,0x2F,0x30,0x31,0x33,0x34,0x36,0x37,0x38
    };
    static const char *punc_n[] = { "`","[","]","\\",";","'",",",".","/" };

    KB_KEY(cont, 0, 0, "Esc", 0x29, kb_key_cb);
    for (int i = 0; i < 10; i++) {
        lv_obj_t *b = kb_btn(cont, KB_COL(i+1), KB_ROW(0),
                             KB_KEY_W, KB_KEY_H, dig_n[i], dig_kc[i], kb_key_cb);
        s_sym_digit_lbls[i] = lv_obj_get_child(b, 0);
    }
    KB_KEY(cont, 11, 0, LV_SYMBOL_BACKSPACE, 0x2A, kb_fixed_key_cb);

    KB_KEY(cont, 0, 1, "Tab", 0x2B, kb_key_cb);
    for (int i = 0; i < 9; i++) {
        lv_obj_t *b = kb_btn(cont, KB_COL(i+1), KB_ROW(1),
                             KB_KEY_W, KB_KEY_H, punc_n[i], punc_kc[i], kb_key_cb);
        s_sym_punct_lbls[i] = lv_obj_get_child(b, 0);
    }
    kb_btn(cont, KB_COL(10), KB_ROW(1), KB_KEY_W*2+KB_GAP, KB_KEY_H,
           LV_SYMBOL_NEW_LINE, 0x28, kb_fixed_key_cb);

    s_sym_shift_btn = kb_btn(cont, KB_COL(0), KB_ROW(2), KB_KEY_W*2+KB_GAP, KB_KEY_H,
                              LV_SYMBOL_UP " Shift", 0x00, kb_sym_shift_cb);
    lv_obj_add_flag(s_sym_shift_btn, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(s_sym_shift_btn, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_color(s_sym_shift_btn, lv_color_hex(0x777777), LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(s_sym_shift_btn, lv_color_hex(0x0055cc), LV_STATE_CHECKED);
    KB_KEY(cont, 10, 2, LV_SYMBOL_UP, 0x52, kb_fixed_key_cb);

    kb_btn(cont, KB_COL(0), KB_ROW(3), KB_KEY_W*7+KB_GAP*6, KB_KEY_H,
           "Space", 0x2C, kb_fixed_key_cb);

    s_sym_tab_abc = kb_btn(cont, KB_COL(7), KB_ROW(3), KB_KEY_W, KB_KEY_H,
                            "ABC", 0x00, kb_tab_abc_cb);
    lv_obj_add_flag(s_sym_tab_abc, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_set_style_bg_color(s_sym_tab_abc, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_color(s_sym_tab_abc, lv_color_hex(0x0055cc), LV_STATE_CHECKED);

    s_sym_tab_sym = kb_btn(cont, KB_COL(8), KB_ROW(3), KB_KEY_W, KB_KEY_H,
                            "!@#", 0x00, kb_tab_sym_cb);
    lv_obj_add_flag(s_sym_tab_sym, LV_OBJ_FLAG_CHECKABLE);
    lv_obj_add_state(s_sym_tab_sym, LV_STATE_CHECKED);
    lv_obj_set_style_bg_color(s_sym_tab_sym, lv_color_hex(0x555555), 0);
    lv_obj_set_style_bg_color(s_sym_tab_sym, lv_color_hex(0x0055cc), LV_STATE_CHECKED);

    KB_KEY(cont,  9, 3, LV_SYMBOL_LEFT,  0x50, kb_fixed_key_cb);
    KB_KEY(cont, 10, 3, LV_SYMBOL_DOWN,  0x51, kb_fixed_key_cb);
    KB_KEY(cont, 11, 3, LV_SYMBOL_RIGHT, 0x4F, kb_fixed_key_cb);
}

/* -----------------------------------------------------------------------
 * Public
 * ----------------------------------------------------------------------- */
void ui_keyboard_show(void)
{
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
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x1e1e1e), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x444444), 0);
    lv_obj_set_style_border_width(panel, 1, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(panel, lv_event_stop_processing, LV_EVENT_CLICKED, NULL);

    s_abc_cont = lv_obj_create(panel);
    lv_obj_set_size(s_abc_cont, KB_PANEL_W, KB_PANEL_H);
    lv_obj_set_pos(s_abc_cont, 0, 0);
    lv_obj_set_style_bg_opa(s_abc_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_abc_cont, 0, 0);
    lv_obj_set_style_pad_all(s_abc_cont, 0, 0);
    lv_obj_clear_flag(s_abc_cont, LV_OBJ_FLAG_SCROLLABLE);
    kb_build_abc(s_abc_cont);

    s_sym_cont = lv_obj_create(panel);
    lv_obj_set_size(s_sym_cont, KB_PANEL_W, KB_PANEL_H);
    lv_obj_set_pos(s_sym_cont, 0, 0);
    lv_obj_set_style_bg_opa(s_sym_cont, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_sym_cont, 0, 0);
    lv_obj_set_style_pad_all(s_sym_cont, 0, 0);
    lv_obj_clear_flag(s_sym_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_sym_cont, LV_OBJ_FLAG_HIDDEN);
    kb_build_sym(s_sym_cont);
}
