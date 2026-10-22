#include "ui_touchpad_config.h"
#include "cfg_common.h"
#include "usb/usb_hid.h"   /* HID_KB_MOD_* */
#include <strings.h>        /* strcasecmp */
#include "app_config.h"
#include "nvs_manager.h"

#include <string.h>
#include <ctype.h>         /* toupper */
#include <stdio.h>
#include <stdlib.h>
#include "esp_log.h"
#include "cJSON.h"

#define TAG  "[TPAD_CFG]"

/* -----------------------------------------------------------------------
 * Hotkey "keys" parsing -- same vocabulary as a Deck hotkey action
 * (ESDeckPC's ActionExecutor VkMap), plus a few extra names. Keep in sync
 * with TouchpadHotkeyKeys in ESDeckPC (Config/TouchpadConfig.cs).
 * ----------------------------------------------------------------------- */
typedef struct { const char *name; uint8_t vk; } key_name_t;

static const key_name_t s_key_names[] = {
    { "enter", 0x0D }, { "tab", 0x09 }, { "esc", 0x1B }, { "del", 0x2E },
    { "backspace", 0x08 }, { "space", 0x20 },
    { "up", 0x26 }, { "down", 0x28 }, { "left", 0x25 }, { "right", 0x27 },
    { "home", 0x24 }, { "end", 0x23 }, { "pgup", 0x21 }, { "pgdn", 0x22 },
    { "insert", 0x2D }, { "prtscr", 0x2C },
    { "f1", 0x70 }, { "f2", 0x71 }, { "f3", 0x72 }, { "f4", 0x73 },
    { "f5", 0x74 }, { "f6", 0x75 }, { "f7", 0x76 }, { "f8", 0x77 },
    { "f9", 0x78 }, { "f10", 0x79 }, { "f11", 0x7A }, { "f12", 0x7B },
};

/* Single character -> VK (US layout), 0 if unknown. */
static uint8_t char_to_vk(char c)
{
    if (c >= 'a' && c <= 'z') return (uint8_t)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return (uint8_t)c;
    if (c >= '0' && c <= '9') return (uint8_t)c;
    switch (c) {
    case '-':  return 0xBD; case '=': return 0xBB;
    case '[':  return 0xDB; case ']': return 0xDD; case '\\': return 0xDC;
    case ';':  return 0xBA; case '\'': return 0xDE;
    case ',':  return 0xBC; case '.': return 0xBE; case '/': return 0xBF;
    case '`':  return 0xC0;
    default:   return 0;
    }
}

/* ["ctrl","c"] -> vk + mods. Exactly one non-modifier key required. */
static bool parse_keys(const cJSON *arr, uint8_t *vk_out, uint8_t *mods_out)
{
    if (!cJSON_IsArray(arr)) return false;

    uint8_t vk = 0, mods = 0;
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsString(it) || !it->valuestring || !it->valuestring[0]) return false;
        const char *s = it->valuestring;

        if      (strcasecmp(s, "ctrl")  == 0) { mods |= HID_KB_MOD_CTRL;  continue; }
        else if (strcasecmp(s, "shift") == 0) { mods |= HID_KB_MOD_SHIFT; continue; }
        else if (strcasecmp(s, "alt")   == 0) { mods |= HID_KB_MOD_ALT;   continue; }
        else if (strcasecmp(s, "win")   == 0) { mods |= HID_KB_MOD_WIN;   continue; }

        uint8_t k = 0;
        if (s[1] == '\0') {
            k = char_to_vk(s[0]);
        } else {
            for (size_t i = 0; i < sizeof(s_key_names) / sizeof(s_key_names[0]); i++) {
                if (strcasecmp(s, s_key_names[i].name) == 0) { k = s_key_names[i].vk; break; }
            }
        }
        if (k == 0 || vk != 0) return false;   /* unknown name, or a second main key */
        vk = k;
    }
    if (vk == 0) return false;

    *vk_out   = vk;
    *mods_out = mods;
    return true;
}

/* Empty label -> the keys themselves, e.g. ["ctrl","c"] -> "Ctrl+C". */
static void keys_to_label(const cJSON *arr, char *out, size_t out_size)
{
    size_t n = 0;
    out[0] = '\0';
    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (!cJSON_IsString(it) || !it->valuestring || !it->valuestring[0]) continue;
        const char *k = it->valuestring;
        if (n > 0 && n + 1 < out_size) out[n++] = '+';
        for (size_t i = 0; k[i] && n + 1 < out_size; i++)
            out[n++] = (char)(i == 0 ? toupper((unsigned char)k[i]) : k[i]);
        out[n] = '\0';
    }
}

/* Hotkeys array, packed: entries that don't parse (no / bad keys) are
 * skipped and the ones after move up. */
static void load_sidebar(const cJSON *root, ui_touchpad_config_t *cfg)
{
    const cJSON *arr = cJSON_GetObjectItem(root, "hotkeys");
    if (!cJSON_IsArray(arr)) return;

    const cJSON *it;
    cJSON_ArrayForEach(it, arr) {
        if (cfg->hotkey_count >= TP_HOTKEY_MAX) {
            ESP_LOGW(TAG, "more than %d hotkeys, extra ignored", TP_HOTKEY_MAX);
            break;
        }
        tp_hotkey_t *hk = &cfg->hotkeys[cfg->hotkey_count];
        memset(hk, 0, sizeof(*hk));
        cfg_str_field(it, "label", hk->label, sizeof(hk->label));
        cfg_str_field(it, "icon",  hk->icon,  sizeof(hk->icon));

        const cJSON *keys = cJSON_GetObjectItem(it, "keys");
        if (!parse_keys(keys, &hk->vk, &hk->mods)) {
            ESP_LOGW(TAG, "hotkey \"%s\": no / invalid keys, skipped", hk->label);
            continue;
        }
        if (!hk->label[0]) keys_to_label(keys, hk->label, sizeof(hk->label));
        cfg->hotkey_count++;
    }
}

/* -----------------------------------------------------------------------
 * Public API
 * ----------------------------------------------------------------------- */
bool ui_touchpad_config_load(ui_touchpad_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));

    char nvs_fname[CFG_FNAME_LEN];
    if (!ui_touchpad_config_nvs_load(nvs_fname, sizeof(nvs_fname))) {
        ESP_LOGW(TAG, "no NVS config, treating as no config");
        return false;
    }

    char path[CFG_FNAME_LEN + 24];
    snprintf(path, sizeof(path), "%s/%s", SD_PATH_CONFIG_TOUCHPAD, nvs_fname);

    char *buf = cfg_read_file(path);
    if (!buf) return false;

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        ESP_LOGW(TAG, "parse failed: %s", path);
        return false;
    }

    cfg_str_field(root, "bg_image", cfg->bg_image, sizeof(cfg->bg_image));
    cfg_str_field(root, "keyboard_icon", cfg->keyboard_icon, sizeof(cfg->keyboard_icon));
    load_sidebar(root, cfg);

    cJSON *settings_obj = cJSON_GetObjectItem(root, "settings");
    if (cJSON_IsObject(settings_obj)) {
        cfg_str_field(settings_obj, "bg_image",  cfg->settings.bg_image,  sizeof(cfg->settings.bg_image));
        cfg_str_field(settings_obj, "side_icon", cfg->settings.side_icon, sizeof(cfg->settings.side_icon));
    }

    cJSON_Delete(root);
    ESP_LOGI(TAG, "loaded: %s bg_image=\"%s\" keyboard_icon=\"%s\" settings.bg_image=\"%s\" settings.side_icon=\"%s\"",
             path, cfg->bg_image, cfg->keyboard_icon, cfg->settings.bg_image, cfg->settings.side_icon);
    return true;
}

bool ui_touchpad_config_nvs_save(const char *filename)
{
    return nvs_manager_set_str(CFG_NVS_NAMESPACE, CFG_NVS_KEY_TOUCHPAD, filename);
}

bool ui_touchpad_config_nvs_load(char *out, size_t out_size)
{
    return nvs_manager_get_str(CFG_NVS_NAMESPACE, CFG_NVS_KEY_TOUCHPAD, out, out_size);
}

touchpad_scan_result_t ui_touchpad_config_scan(void)
{
    return cfg_scan_json_dir(SD_PATH_CONFIG_TOUCHPAD);
}

void ui_touchpad_config_scan_free(touchpad_scan_result_t *res)
{
    /* touchpad_scan_result_t is just an alias of the shared cfg_scan_result_t
     * (see app_config.h) -- kept as its own wrapper for API symmetry with
     * ui_touchpad_config_scan(). */
    cfg_scan_result_free(res);
}
