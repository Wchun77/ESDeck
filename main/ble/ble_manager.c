#include "ble_manager.h"
#include "ui_toast.h"
#include "ui_font_cjk.h"
#include "lvgl.h"

#include "esp_log.h"
#include "esp_timer.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <assert.h>

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_gatt.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "BLE_MGR";

/* store/config/ble_store_config.h only declares read/write/delete --
 * ble_store_config_init() is genuinely missing from that header (upstream
 * nimble inconsistency). Espressif's own bleprph example works around it
 * the same way: a plain local forward declaration instead of a header. */
void ble_store_config_init(void);

#define DEVICE_NAME  "ESDeck"

static bool     s_enabled     = false;
static uint8_t  s_own_addr_type;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;

static int gap_event_handler(struct ble_gap_event *event, void *arg);

/* -----------------------------------------------------------------------
 * Toast hookup -- same ui_toast_push() call the USB HID test signal used
 * (see usb_hid.c), now the real source. lv_async_call() is required: GAP
 * events fire from the NimBLE host task, not the LVGL task.
 * ----------------------------------------------------------------------- */
static void toast_ble_connected_cb(void *arg)    { (void)arg; ui_toast_push("BLE Connected", 1, NULL, NULL); }
static void toast_ble_disconnected_cb(void *arg) { (void)arg; ui_toast_push("BLE Disconnected", 1, NULL, NULL); }

/* -----------------------------------------------------------------------
 * Advertising
 *
 * Includes an ANCS Service Solicitation (Bluetooth AD type 0x15, 128-bit
 * UUID). ESP is a Notification Consumer here -- it wants to *use* ANCS,
 * which the phone (Notification Provider) exposes -- it does not offer
 * ANCS itself, so a Service UUID list (type 0x06/0x07, "I offer this
 * service") would be the wrong direction. Soliciting the UUID instead
 * ("I'm looking for a peer that offers this service") is also what makes
 * iOS treat a plain, non-MFi peripheral as an ANCS-capable accessory and
 * list it under Settings > Bluetooth at all -- required because ANCS
 * pairing can only happen through that system UI; a CoreBluetooth app
 * (nRF Connect included) cannot grant ANCS access no matter how it
 * connects (confirmed by Apple: developer.apple.com/forums/thread/63223).
 *
 * struct ble_hs_adv_fields has no dedicated field for Service
 * Solicitation, so this is assembled by hand: ble_hs_adv_set_fields()
 * encodes the normal fields (flags, name) into a local buffer without
 * touching the radio, the Solicitation AD structure is appended after
 * it, then the combined buffer goes to ble_gap_adv_set_data() -- the same
 * call ble_gap_adv_set_fields() makes internally for the simple case.
 * ----------------------------------------------------------------------- */

/* ANCS service UUID 7905F431-B5CE-4E99-A40F-4B1E122D00D0, byte-reversed
 * for the BLE wire format (least-significant byte first). */
static const uint8_t s_ancs_uuid128[16] = {
    0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4,
    0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79,
};

static void ble_manager_advertise(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_gap_adv_params adv_params;
    uint8_t adv_buf[BLE_HS_ADV_MAX_SZ];
    uint8_t adv_len;
    int rc;

    memset(&fields, 0, sizeof(fields));
    /* General discoverable + classic BR/EDR not supported (BLE only). */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    /* tx_pwr_lvl deliberately dropped (was set here before) -- flags (3
     * bytes) + name (8 bytes) + the 18-byte ANCS solicitation appended
     * below already total 29 of the legacy advertising PDU's 31-byte
     * budget; tx power's 3 more bytes would overflow it. Not worth
     * losing the solicitation over. */

    rc = ble_hs_adv_set_fields(&fields, adv_buf, &adv_len, sizeof(adv_buf));
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_adv_set_fields failed: %d", rc);
        return;
    }

    if ((size_t)adv_len + 2 + sizeof(s_ancs_uuid128) > sizeof(adv_buf)) {
        ESP_LOGE(TAG, "adv buffer too small for ANCS solicitation");
        return;
    }
    adv_buf[adv_len++] = 1 + sizeof(s_ancs_uuid128);   /* AD length: type + value */
    adv_buf[adv_len++] = BLE_HS_ADV_TYPE_SOL_UUIDS128;
    memcpy(&adv_buf[adv_len], s_ancs_uuid128, sizeof(s_ancs_uuid128));
    adv_len += sizeof(s_ancs_uuid128);

    rc = ble_gap_adv_set_data(adv_buf, adv_len);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_data failed: %d", rc);
        return;
    }

    memset(&adv_params, 0, sizeof(adv_params));
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER,
                            &adv_params, gap_event_handler, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    }
}

/* -----------------------------------------------------------------------
 * ANCS client (Notification Consumer)
 *
 * Everything else in this file is GAP peripheral/advertising code -- ESP
 * as the thing being connected *to*. ANCS runs the other way: once
 * connected, ESP acts as a GATT *client* against the phone's ANCS, which
 * makes the phone the GATT server (Notification Provider) here.
 *
 * Discovery/subscribe chain, kicked off from BLE_GAP_EVENT_CONNECT below:
 *   1. find the ANCS service by UUID (may legitimately not be present --
 *      the ANCS spec says so explicitly, since ANCS is unpublished
 *      whenever no notifications exist yet)
 *   2. find its Notification Source characteristic within that service
 *   3. find that characteristic's CCCD (Client Characteristic
 *      Configuration Descriptor, UUID 0x2902) -- this is the standard
 *      on/off switch for notifications, looked up properly via discovery
 *      rather than assumed to be "value handle + 1"
 *   4. write 0x0001 to the CCCD to subscribe
 *
 * Step 4 is the one that matters for pairing: the ANCS spec requires
 * every one of its characteristics to be encrypted/authenticated to
 * access ("All these characteristics require authorization for
 * access"). Nothing before this point (advertising with the Service
 * Solicitation, the phone connecting) ever touches an authenticated
 * attribute, so nothing before this point could ever make iOS show its
 * system pairing dialog -- that dialog is what a *failed* access to an
 * encrypted attribute triggers, not anything to do with whether the
 * device was visible in a scan list. If the CCCD write's first attempt
 * bounces off "insufficient authentication" before pairing has had a
 * chance to run, BLE_GAP_EVENT_ENC_CHANGE below retries it once the link
 * actually becomes encrypted.
 * ----------------------------------------------------------------------- */
#define ANCS_SVC_UUID \
    BLE_UUID128_DECLARE(0xD0, 0x00, 0x2D, 0x12, 0x1E, 0x4B, 0x0F, 0xA4, \
                         0x99, 0x4E, 0xCE, 0xB5, 0x31, 0xF4, 0x05, 0x79)
#define ANCS_NOTIFICATION_SOURCE_UUID \
    BLE_UUID128_DECLARE(0xBD, 0x1D, 0xA2, 0x99, 0xE6, 0x25, 0x58, 0x8C, \
                         0xD9, 0x42, 0x01, 0x63, 0x0D, 0x12, 0xBF, 0x9F)
/* Control Point (write) -- ESP uses this to ask for a notification's
 * actual text (Get Notification Attributes). No CCCD -- write-only. */
#define ANCS_CONTROL_POINT_UUID \
    BLE_UUID128_DECLARE(0xD9, 0xD9, 0xAA, 0xFD, 0xBD, 0x9B, 0x21, 0x98, \
                         0xA8, 0x49, 0xE1, 0x45, 0xF3, 0xD8, 0xD1, 0x69)
/* Data Source (notify) -- the Control Point request's response arrives
 * here, potentially split across several GATT notifications. */
#define ANCS_DATA_SOURCE_UUID \
    BLE_UUID128_DECLARE(0xFB, 0x7B, 0x7C, 0xCE, 0x6A, 0xB3, 0x44, 0xBE, \
                         0xB5, 0x4B, 0xD6, 0x24, 0xE9, 0xC6, 0xEA, 0x22)
#define ANCS_CCCD_UUID16   0x2902

static uint16_t s_ancs_svc_end_handle;
static uint16_t s_ancs_ns_val_handle;
static uint16_t s_ancs_ns_cccd_handle;
static uint16_t s_ancs_cp_val_handle;   /* Control Point -- write, no CCCD */
static uint16_t s_ancs_ds_val_handle;
static uint16_t s_ancs_ds_cccd_handle;

/* Which characteristic a chained ancs_disc_dsc_cb() call is currently
 * looking for the CCCD of -- passed through as its void *arg. */
typedef enum { ANCS_DSC_TARGET_NS, ANCS_DSC_TARGET_DS } ancs_dsc_target_t;

static int ancs_cccd_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                               struct ble_gatt_attr *attr, void *arg)
{
    (void)attr;
    const char *which = (const char *)arg;   /* string literal, static storage -- safe across the async gap */

    if (error->status == 0) {
        ESP_LOGI(TAG, "ANCS: subscribed to %s", which);
        return 0;
    }

    if (error->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ||
        error->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC)) {
        /* This is the expected first-attempt failure -- every ANCS
         * characteristic requires an encrypted/authenticated link and
         * this one isn't yet. NimBLE does *not* auto-initiate pairing on
         * this error by itself; the app has to explicitly ask for it.
         * This call is what actually makes iOS show its system pairing
         * dialog. Once it completes, BLE_GAP_EVENT_ENC_CHANGE fires and
         * retries both CCCD writes (see gap_event_handler()). */
        ESP_LOGI(TAG, "ANCS: %s CCCD write needs auth (status=%d) -- "
                      "initiating pairing", which, error->status);
        int rc = ble_gap_security_initiate(conn_handle);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "ANCS: ble_gap_security_initiate failed: %d", rc);
        }
    } else {
        ESP_LOGW(TAG, "ANCS: %s CCCD write failed; status=%d", which, error->status);
    }
    return 0;
}

static void ancs_write_cccd(uint16_t conn_handle, uint16_t cccd_handle, const char *which)
{
    static const uint8_t value[2] = { 0x01, 0x00 };   /* enable notifications */
    int rc;

    if (cccd_handle == 0) return;   /* not discovered (yet) -- harmless no-op */

    rc = ble_gattc_write_flat(conn_handle, cccd_handle,
                              value, sizeof(value), ancs_cccd_write_cb, (void *)which);
    if (rc != 0) {
        ESP_LOGE(TAG, "ANCS: %s CCCD write kickoff failed: %d", which, rc);
    }
}

/* Descriptor search range end for the characteristic at val_handle --
 * stops right before whichever other known ANCS characteristic comes
 * next (a characteristic declaration attribute always sits exactly one
 * handle before its own value attribute, hence "-2" to land just before
 * the next one's declaration), or the service's own end_handle if
 * val_handle belongs to the last characteristic. Only meaningful once
 * all three characteristics have been discovered (see ancs_disc_chr_cb()'s
 * terminal branch, the only place this is called from) -- needed so a
 * broad scan for one characteristic's CCCD can't wander into a
 * different characteristic's descriptors further down the table. */
static uint16_t ancs_dsc_range_end(uint16_t val_handle)
{
    uint16_t end = s_ancs_svc_end_handle;
    uint16_t others[3] = { s_ancs_ns_val_handle, s_ancs_cp_val_handle, s_ancs_ds_val_handle };

    for (int i = 0; i < 3; i++) {
        uint16_t h = others[i];
        if (h != 0 && h > val_handle && (uint16_t)(h - 2) < end) {
            end = (uint16_t)(h - 2);
        }
    }
    return end;
}

static int ancs_disc_dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                             uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                             void *arg)
{
    (void)chr_val_handle;
    ancs_dsc_target_t target    = (ancs_dsc_target_t)(intptr_t)arg;
    uint16_t          *cccd_slot = (target == ANCS_DSC_TARGET_NS) ? &s_ancs_ns_cccd_handle
                                                                   : &s_ancs_ds_cccd_handle;
    const char        *which     = (target == ANCS_DSC_TARGET_NS) ? "Notification Source" : "Data Source";

    if (error->status == 0 && dsc != NULL) {
        if (dsc->uuid.u.type == BLE_UUID_TYPE_16 &&
            ble_uuid_u16(&dsc->uuid.u) == ANCS_CCCD_UUID16) {
            *cccd_slot = dsc->handle;
        }
        return 0;
    }

    /* Descriptor discovery for this one characteristic finished
     * (error->status == BLE_HS_EDONE, or any other terminal status). */
    if (*cccd_slot != 0) {
        ancs_write_cccd(conn_handle, *cccd_slot, which);
    } else {
        ESP_LOGW(TAG, "ANCS: %s CCCD not found", which);
    }

    /* Chain into Data Source's descriptor discovery now that Notification
     * Source's is fully done -- sequential rather than concurrent, to
     * avoid relying on NimBLE to correctly queue two GATT client
     * procedures issued back to back on the same connection. */
    if (target == ANCS_DSC_TARGET_NS && s_ancs_ds_val_handle != 0) {
        int rc = ble_gattc_disc_all_dscs(conn_handle, s_ancs_ds_val_handle,
                                          ancs_dsc_range_end(s_ancs_ds_val_handle),
                                          ancs_disc_dsc_cb, (void *)(intptr_t)ANCS_DSC_TARGET_DS);
        if (rc != 0) {
            ESP_LOGE(TAG, "ANCS: Data Source disc_all_dscs failed: %d", rc);
        }
    }
    return 0;
}

static int ancs_disc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                             const struct ble_gatt_chr *chr, void *arg)
{
    (void)arg;

    if (error->status == 0 && chr != NULL) {
        if (ble_uuid_cmp(&chr->uuid.u, ANCS_NOTIFICATION_SOURCE_UUID) == 0) {
            s_ancs_ns_val_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, ANCS_CONTROL_POINT_UUID) == 0) {
            s_ancs_cp_val_handle = chr->val_handle;
        } else if (ble_uuid_cmp(&chr->uuid.u, ANCS_DATA_SOURCE_UUID) == 0) {
            s_ancs_ds_val_handle = chr->val_handle;
        }
        return 0;
    }

    /* Characteristic discovery finished -- Control Point is write-only
     * (no CCCD to look up). Notification Source and Data Source both
     * need their own CCCD found+subscribed; chained through
     * ancs_disc_dsc_cb() (NS first, which kicks off DS once it's done)
     * so only one descriptor-discovery procedure is ever in flight. */
    if (s_ancs_cp_val_handle == 0) {
        ESP_LOGW(TAG, "ANCS: Control Point characteristic not found");
    }

    if (s_ancs_ns_val_handle != 0) {
        int rc = ble_gattc_disc_all_dscs(conn_handle, s_ancs_ns_val_handle,
                                          ancs_dsc_range_end(s_ancs_ns_val_handle),
                                          ancs_disc_dsc_cb, (void *)(intptr_t)ANCS_DSC_TARGET_NS);
        if (rc != 0) {
            ESP_LOGE(TAG, "ANCS: Notification Source disc_all_dscs failed: %d", rc);
        }
    } else {
        ESP_LOGW(TAG, "ANCS: Notification Source characteristic not found");
        /* NS is what would normally chain into DS's descriptor discovery
         * -- without it, kick DS off directly so a phone that's somehow
         * missing NS but has DS doesn't silently get nothing. */
        if (s_ancs_ds_val_handle != 0) {
            int rc = ble_gattc_disc_all_dscs(conn_handle, s_ancs_ds_val_handle,
                                              ancs_dsc_range_end(s_ancs_ds_val_handle),
                                              ancs_disc_dsc_cb, (void *)(intptr_t)ANCS_DSC_TARGET_DS);
            if (rc != 0) {
                ESP_LOGE(TAG, "ANCS: Data Source disc_all_dscs failed: %d", rc);
            }
        }
    }
    return 0;
}

/* -----------------------------------------------------------------------
 * Get Notification Attributes -- Notification Source only carries a
 * compact 8-byte event (no text), so the actual title/message has to be
 * separately requested via Control Point and read back via Data Source.
 *
 * Only one request is ever allowed in flight at a time, enforced by
 * s_ancs_request_busy (see ancs_on_notification_source_notify()) -- ANCS
 * bursts an Added event for *every* notification the phone currently has
 * active right after a fresh subscribe (its way of syncing initial
 * state), and firing a fresh Get Notification Attributes write for each
 * one back to back, with no pacing, is what first crashed this: NimBLE's
 * ATT layer has a bounded number of outstanding command buffers per
 * connection, and slamming ~15 Write Without Response commands into it
 * in a few milliseconds overran that pool badly enough to hit a hard
 * assert deep in ble_att_tx_with_conn() rather than a clean error
 * return. Dropping the rest of a burst (rather than queueing it) is fine
 * for a best-effort display feature -- a real queue/UID-tracking table
 * would be a lot of machinery for what's ultimately a transient toast.
 * A one-shot timeout (ancs_request_timeout_cb()) clears the busy flag if
 * a response never arrives (dropped packet, Data Source not actually
 * subscribed, etc.), so a single lost response can't wedge every
 * notification after it for the rest of the connection.
 *
 * Response layout, once reassembled in s_ds_buf by
 * ancs_on_data_source_notify():
 *   CommandID(1) + NotificationUID(4) +
 *   repeated, in the same order requested: AttributeID(1) + Length(2, LE) + Data(Length bytes)
 * ----------------------------------------------------------------------- */
#define ANCS_CMD_GET_NOTIF_ATTRS   0x00
#define ANCS_CMD_GET_APP_ATTRS     0x01
#define ANCS_ATTR_APP_IDENTIFIER   0x00
#define ANCS_ATTR_TITLE            0x01
#define ANCS_ATTR_MESSAGE          0x03
#define ANCS_APP_ATTR_DISPLAY_NAME 0x00
#define ANCS_TITLE_MAX_LEN         32
#define ANCS_MESSAGE_MAX_LEN       96   /* headroom matches ui_toast's TOAST_LABEL_LEN */
#define ANCS_EVENT_ID_ADDED        0
#define ANCS_APP_ID_LEN            64
#define ANCS_APP_NAME_MAX_LEN      24

/* Must cover the worst case of "%s\n%s: %s" with app_name[24], title[64]
 * and message[128] (see ancs_push_toast_now()) --
 * (24-1) + 1 + (64-1) + 2 + (128-1) + 1 = 217, rounded up to 224 for a
 * little slack. This buffer is filled through a pointer-parameter helper
 * rather than snprintf() called directly on the local arrays, so GCC's
 * -Wformat-truncation can no longer verify this bound itself (it loses
 * the arrays' declared sizes once they decay to plain char* parameters)
 * -- the 217-byte worst case above is a manual derivation, not a
 * compiler-checked one. Runtime truncation itself is harmless regardless
 * (this text gets UTF-8-safely truncated again by ui_toast.c). */
#define ANCS_TOAST_TEXT_LEN        224

static uint8_t s_ds_buf[256];
static size_t  s_ds_len;

/* Which ANCS Control Point command a response currently being reassembled
 * in s_ds_buf belongs to -- Get Notification Attributes and Get App
 * Attributes have different response layouts (see
 * ancs_parse_notif_attrs_response() / ancs_parse_app_attrs_response()),
 * and only one request is ever in flight at a time (s_ancs_request_busy),
 * so this alone is enough to pick the right parser. */
typedef enum { ANCS_REQ_NONE, ANCS_REQ_NOTIF_ATTRS, ANCS_REQ_APP_ATTRS } ancs_req_type_t;
static ancs_req_type_t s_ancs_req_type = ANCS_REQ_NONE;

/* Title/message held here while a Get Notification Attributes response's
 * app identifier is being resolved to a display name via a *second*,
 * chained Control Point request (Get App Attributes) -- see
 * ancs_parse_notif_attrs_response(). Only ever one notification's worth
 * live at a time, same reasoning as s_ds_buf/s_ancs_request_busy. */
static char s_pending_title[64];
static char s_pending_message[128];
static bool s_pending_valid;

/* App display names rarely change and there are only ever a handful of
 * apps actively posting notifications on a real phone (Messages, Mail,
 * a couple of chat apps) -- a small round-robin cache avoids re-running
 * the Get App Attributes round trip for every single notification from
 * the same app. */
#define ANCS_APP_CACHE_SIZE 8
typedef struct {
    char app_id[ANCS_APP_ID_LEN];
    char display_name[ANCS_APP_NAME_MAX_LEN];
    bool valid;
} ancs_app_cache_entry_t;
static ancs_app_cache_entry_t s_app_cache[ANCS_APP_CACHE_SIZE];
static int s_app_cache_next;

static const char *ancs_app_cache_lookup(const char *app_id)
{
    for (int i = 0; i < ANCS_APP_CACHE_SIZE; i++) {
        if (s_app_cache[i].valid && strcmp(s_app_cache[i].app_id, app_id) == 0) {
            return s_app_cache[i].display_name;
        }
    }
    return NULL;
}

static void ancs_app_cache_store(const char *app_id, const char *display_name)
{
    ancs_app_cache_entry_t *e = &s_app_cache[s_app_cache_next];
    snprintf(e->app_id, sizeof(e->app_id), "%s", app_id);
    snprintf(e->display_name, sizeof(e->display_name), "%s", display_name);
    e->valid = true;
    s_app_cache_next = (s_app_cache_next + 1) % ANCS_APP_CACHE_SIZE;
}

/* Serializes Get Notification Attributes / Get App Attributes requests --
 * see the file-header comment above for why this exists. */
static bool               s_ancs_request_busy;
static esp_timer_handle_t s_ancs_request_timeout_timer;

#define ANCS_REQUEST_TIMEOUT_US   (4 * 1000 * 1000)

/* Waits (via a repeating LVGL timer, not a block) for the CJK font
 * preload task to finish before showing a toast that needs it -- see
 * ui_font_cjk.h's comment on ui_font_cjk_try_get(). Without this, a
 * notification arriving before the ~13s preload finishes (only possible
 * right after boot) would show with CJK glyphs missing from whatever
 * fallback font gets used instead -- exactly what "glyph dsc. not found"
 * warnings in the log are. Capped at ANCS_FONT_WAIT_MAX_MS so a
 * genuinely failed/missing font (UI_FONT_CJK_UNAVAILABLE) or an
 * unexpectedly stuck load doesn't block this notification from ever
 * showing at all -- it just shows without CJK styling as a last resort. */
#define ANCS_FONT_WAIT_STEP_MS   300
#define ANCS_FONT_WAIT_MAX_MS    15000

typedef struct {
    char    *text;
    uint32_t waited_ms;
} ancs_toast_wait_ctx_t;

static void ancs_toast_wait_timer_cb(lv_timer_t *timer)
{
    ancs_toast_wait_ctx_t *ctx = (ancs_toast_wait_ctx_t *)timer->user_data;
    const lv_font_t *font = NULL;
    ui_font_cjk_status_t status = ui_font_cjk_try_get(&font);

    ctx->waited_ms += ANCS_FONT_WAIT_STEP_MS;

    if (status == UI_FONT_CJK_LOADING && ctx->waited_ms < ANCS_FONT_WAIT_MAX_MS) {
        return;   /* keep waiting -- timer repeats on its own */
    }

    ui_toast_push(ctx->text, 1, NULL, font);   /* font may be NULL here (UNAVAILABLE or timed-out LOADING) */
    free(ctx->text);
    free(ctx);
    lv_timer_del(timer);
}

static void ancs_toast_push_cb(void *arg)
{
    char *text = (char *)arg;
    const lv_font_t *font = NULL;
    ui_font_cjk_status_t status = ui_font_cjk_try_get(&font);

    if (status != UI_FONT_CJK_LOADING) {
        /* READY (font already loaded, common case after boot) or
         * UNAVAILABLE (no CJK font on SD -- no point waiting) both
         * resolve immediately. */
        ui_toast_push(text, 1, NULL, font);
        free(text);
        return;
    }

    ancs_toast_wait_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!ctx) {
        ui_toast_push(text, 1, NULL, NULL);   /* OOM -- show it anyway, best effort */
        free(text);
        return;
    }
    ctx->text = text;   /* ownership moves to the timer context */
    ctx->waited_ms = 0;
    lv_timer_create(ancs_toast_wait_timer_cb, ANCS_FONT_WAIT_STEP_MS, ctx);
}

/* Builds and queues the actual toast text -- app_name may be NULL/empty
 * (cache miss that timed out, or a notification with no AppIdentifier at
 * all), in which case the toast just falls back to title/message alone
 * as before. See ANCS_TOAST_TEXT_LEN's comment for the buffer-size
 * derivation this relies on. */
static void ancs_push_toast_now(const char *app_name, const char *title, const char *message)
{
    char *text = malloc(ANCS_TOAST_TEXT_LEN);
    if (!text) return;

    bool has_app   = app_name && app_name[0];
    bool has_title = title && title[0];
    bool has_msg   = message && message[0];

    if (has_app && has_title && has_msg) {
        snprintf(text, ANCS_TOAST_TEXT_LEN, "%s\n%s: %s", app_name, title, message);
    } else if (has_app && has_title) {
        snprintf(text, ANCS_TOAST_TEXT_LEN, "%s\n%s", app_name, title);
    } else if (has_app && has_msg) {
        snprintf(text, ANCS_TOAST_TEXT_LEN, "%s\n%s", app_name, message);
    } else if (has_title && has_msg) {
        snprintf(text, ANCS_TOAST_TEXT_LEN, "%s\n%s", title, message);
    } else if (has_title) {
        snprintf(text, ANCS_TOAST_TEXT_LEN, "%s", title);
    } else if (has_msg) {
        snprintf(text, ANCS_TOAST_TEXT_LEN, "%s", message);
    } else if (has_app) {
        snprintf(text, ANCS_TOAST_TEXT_LEN, "%s", app_name);
    } else {
        free(text);
        return;
    }

    lv_async_call(ancs_toast_push_cb, text);
}

/* Ends the current request/chain -- called once a toast has been pushed
 * (or given up on) and there's nothing further to wait for. */
static void ancs_request_timeout_cb(void *arg);

static void ancs_finish_request(void)
{
    s_ancs_request_busy = false;
    s_ancs_req_type      = ANCS_REQ_NONE;
    s_pending_valid       = false;
    if (s_ancs_request_timeout_timer) esp_timer_stop(s_ancs_request_timeout_timer);
}

/* (Re)arms the 4-second timeout after a Control Point write is
 * successfully queued -- shared by both legs of the chain
 * (ancs_request_attributes() / ancs_request_app_attributes()). */
static void ancs_arm_timeout(void)
{
    if (!s_ancs_request_timeout_timer) {
        const esp_timer_create_args_t args = {
            .callback = ancs_request_timeout_cb,
            .name     = "ancs_req_timeout",
        };
        esp_timer_create(&args, &s_ancs_request_timeout_timer);
    }
    esp_timer_stop(s_ancs_request_timeout_timer);   /* in case a previous one is still armed */
    esp_timer_start_once(s_ancs_request_timeout_timer, ANCS_REQUEST_TIMEOUT_US);
}

static void ancs_request_timeout_cb(void *arg)
{
    (void)arg;
    if (!s_ancs_request_busy) return;

    ESP_LOGW(TAG, "ANCS: %s response timed out, giving up on it",
             s_ancs_req_type == ANCS_REQ_APP_ATTRS ? "GetAppAttributes" : "GetNotificationAttributes");
    s_ds_len = 0;

    /* If this was the app-name leg of the chain, the notification itself
     * was already fully fetched -- still show it, just without a resolved
     * app name, rather than silently dropping it because the *second*
     * request timed out. */
    if (s_ancs_req_type == ANCS_REQ_APP_ATTRS && s_pending_valid) {
        ancs_push_toast_now(NULL, s_pending_title, s_pending_message);
    }
    ancs_finish_request();
}

static void ancs_request_app_attributes(uint16_t conn_handle, const char *app_id);

/* Strips Unicode bidi/directional-formatting control characters in place
 * (UTF-8, 3 bytes each, all in the E2 80/81 xx range). iOS wraps
 * dynamically-inserted text (contact names, message bodies -- especially
 * from apps like LINE when the notification carries richer content, e.g. a
 * camera/sticker reaction) in these to keep bidirectional text rendering
 * correct regardless of surrounding context; plain-text notifications often
 * don't get them. They carry no visible glyph by design, but this font
 * doesn't have (and shouldn't need) placeholder glyphs for them, hence the
 * "glyph dsc. not found for U+2068/U+2069" warnings -- stripping them here
 * means the toast only ever sees actual displayable text.
 *
 * Covers LRM/RLM (U+200E/U+200F), LRE/RLE/PDF/LRO/RLO (U+202A-U+202E), and
 * LRI/RLI/FSI/PDI (U+2066-U+2069) -- the ones actually seen from ANCS in
 * practice are FSI/PDI, but the whole bidi-control block shares the same
 * "no visible glyph, safe to drop" property. */
static void ancs_strip_bidi_controls(char *s)
{
    unsigned char *r = (unsigned char *)s;
    unsigned char *w = (unsigned char *)s;

    while (*r) {
        if (r[0] == 0xE2 && r[1] == 0x80 && (r[2] == 0x8E || r[2] == 0x8F ||
                                              (r[2] >= 0xAA && r[2] <= 0xAE))) {
            r += 3;
            continue;
        }
        if (r[0] == 0xE2 && r[1] == 0x81 && r[2] >= 0xA6 && r[2] <= 0xA9) {
            r += 3;
            continue;
        }
        *w++ = *r++;
    }
    *w = '\0';
}

/* Parses a Get Notification Attributes response (CommandID 0x00) from the
 * top of s_ds_buf every time new bytes arrive, rather than tracking
 * partial-attribute state across calls -- simpler, and cheap given these
 * responses only ever run to a couple hundred bytes. Returns true once
 * fully parsed and acted on (the caller resets s_ds_len to 0 in that
 * case, ready for the next leg of the chain or the next request). */
static bool ancs_parse_notif_attrs_response(void)
{
    if (s_ds_len < 5) return false;   /* CommandID(1) + NotificationUID(4) not fully in yet */

    /* NotificationUID isn't cross-checked against the request that
     * triggered it -- only one Get Notification Attributes request is
     * ever in flight (see the comment above), so whatever response
     * arrives can only belong to that one request. */
    size_t pos = 5;

    static const uint8_t want_ids[3] = {
        ANCS_ATTR_APP_IDENTIFIER, ANCS_ATTR_TITLE, ANCS_ATTR_MESSAGE
    };
    char app_id[ANCS_APP_ID_LEN] = { 0 };
    char title[64]    = { 0 };
    char message[128] = { 0 };
    char   *slots[3] = { app_id, title, message };
    size_t  caps[3]  = { sizeof(app_id), sizeof(title), sizeof(message) };

    for (int i = 0; i < 3; i++) {
        if (pos + 3 > s_ds_len) return false;   /* AttributeID(1) + Length(2) not fully in yet */

        uint8_t  attr_id  = s_ds_buf[pos];
        uint16_t attr_len = (uint16_t)s_ds_buf[pos + 1] | ((uint16_t)s_ds_buf[pos + 2] << 8);
        pos += 3;

        if (pos + attr_len > s_ds_len) return false;   /* value not fully in yet */

        if (attr_id == want_ids[i]) {
            size_t n = ((size_t)attr_len < caps[i] - 1) ? attr_len : caps[i] - 1;
            memcpy(slots[i], &s_ds_buf[pos], n);
            slots[i][n] = '\0';
        } else {
            ESP_LOGW(TAG, "ANCS: unexpected attribute order (got 0x%02X, wanted 0x%02X)",
                     attr_id, want_ids[i]);
        }
        pos += attr_len;
    }

    ancs_strip_bidi_controls(title);
    ancs_strip_bidi_controls(message);

    ESP_LOGI(TAG, "ANCS: notification from \"%s\": \"%s\" / \"%s\"", app_id, title, message);

    /* Title is usually the human-readable sender (contact name, or the
     * app's own display name for some apps); Message is the body.
     * AppIdentifier is the bundle ID (e.g. "jp.naver.line") -- not
     * user-friendly shown raw, so it's resolved to a real display name
     * ("LINE") via a chained Get App Attributes request instead of being
     * shown directly. Cache hit means that round trip isn't needed. */
    if (app_id[0]) {
        const char *cached = ancs_app_cache_lookup(app_id);
        if (cached) {
            ancs_push_toast_now(cached, title, message);
            ancs_finish_request();
        } else {
            snprintf(s_pending_title, sizeof(s_pending_title), "%s", title);
            snprintf(s_pending_message, sizeof(s_pending_message), "%s", message);
            s_pending_valid = true;
            ancs_request_app_attributes(s_conn_handle, app_id);
        }
    } else {
        ancs_push_toast_now(NULL, title, message);
        ancs_finish_request();
    }

    return true;
}

/* Parses a Get App Attributes response (CommandID 0x01):
 *   CommandID(1) + AppIdentifier (NUL-terminated UTF-8 string) +
 *   AttributeID(1) + Length(2, LE) + Data(Length bytes)
 * Only ever requests one attribute (DisplayName), so there's exactly one
 * AttributeID/Length/Data group to read, unlike the notification-attributes
 * response's fixed 3. */
static bool ancs_parse_app_attrs_response(void)
{
    if (s_ds_len < 2) return false;   /* CommandID(1) + at least a NUL not fully in yet */

    size_t str_end = 0;
    bool   found_nul = false;
    for (size_t i = 1; i < s_ds_len; i++) {
        if (s_ds_buf[i] == 0x00) { str_end = i; found_nul = true; break; }
    }
    if (!found_nul) return false;   /* AppIdentifier string not fully in yet */

    size_t pos = str_end + 1;
    if (pos + 3 > s_ds_len) return false;   /* AttributeID(1) + Length(2) not fully in yet */

    uint8_t  attr_id  = s_ds_buf[pos];
    uint16_t attr_len = (uint16_t)s_ds_buf[pos + 1] | ((uint16_t)s_ds_buf[pos + 2] << 8);
    pos += 3;
    if (pos + attr_len > s_ds_len) return false;   /* value not fully in yet */

    char display_name[ANCS_APP_NAME_MAX_LEN] = { 0 };
    if (attr_id == ANCS_APP_ATTR_DISPLAY_NAME) {
        size_t n = ((size_t)attr_len < sizeof(display_name) - 1) ? attr_len : sizeof(display_name) - 1;
        memcpy(display_name, &s_ds_buf[pos], n);
        display_name[n] = '\0';
    }

    ESP_LOGI(TAG, "ANCS: app display name: \"%s\"", display_name);

    if (display_name[0] && s_pending_valid) {
        /* s_pending_title/message were stashed with the app_id this name
         * belongs to, but the app_id string itself wasn't kept -- reread
         * it straight out of the request bytes instead of adding another
         * static buffer just to round-trip it. Cheap: str_end bounds it
         * inside s_ds_buf's own AppIdentifier echo, which the spec
         * guarantees matches what was requested. */
        ancs_app_cache_store((const char *)&s_ds_buf[1], display_name);
    }

    if (s_pending_valid) {
        ancs_push_toast_now(display_name[0] ? display_name : NULL,
                             s_pending_title, s_pending_message);
    }
    ancs_finish_request();
    return true;
}

static bool ancs_try_parse_response(void)
{
    if (s_ds_len < 1) return false;
    return (s_ancs_req_type == ANCS_REQ_APP_ATTRS)
               ? ancs_parse_app_attrs_response()
               : ancs_parse_notif_attrs_response();
}

static void ancs_on_data_source_notify(const uint8_t *data, uint16_t len)
{
    if (len == 0) return;

    /* Diagnostic -- confirms whether Data Source is delivering anything at
     * all, independent of whether ancs_try_parse_response() below manages
     * to make sense of it. Every "response timed out" case so far has left
     * this ambiguous: no way to tell "phone never sent a response" apart
     * from "phone sent one but we mis-parsed/mis-routed it". */
    ESP_LOGI(TAG, "ANCS: Data Source notify, %u bytes (buffered so far: %u)",
             (unsigned)len, (unsigned)(s_ds_len + len));

    if (s_ds_len + len > sizeof(s_ds_buf)) {
        ESP_LOGW(TAG, "ANCS: Data Source response overflow (%u + %u > %u), dropping",
                 (unsigned)s_ds_len, (unsigned)len, (unsigned)sizeof(s_ds_buf));
        s_ds_len = 0;
        if (s_pending_valid) {
            /* Notification text itself was already fetched before this
             * overflowed leg (app-name lookup) -- still show it. */
            ancs_push_toast_now(NULL, s_pending_title, s_pending_message);
        }
        ancs_finish_request();
        return;
    }
    memcpy(&s_ds_buf[s_ds_len], data, len);
    s_ds_len += len;

    /* Only the buffer itself is unconditionally consumed here once fully
     * parsed -- busy/timer lifecycle is owned by whichever parse path
     * handled it: ancs_finish_request() if the chain is done (toast
     * pushed or given up on), or a fresh ancs_request_app_attributes()
     * call if this was Get Notification Attributes handing off to the
     * app-name leg (that call re-arms both itself, for the next
     * response). */
    if (ancs_try_parse_response()) {
        s_ds_len = 0;
    }
}

/* Write-with-response callback for the Control Point write -- diagnostic
 * only, doesn't touch s_ancs_request_busy (the timeout timer / Data Source
 * response is still what clears that). Switched from
 * ble_gattc_write_no_rsp_flat() to ble_gattc_write_flat() specifically so
 * this fires at all: every previous attempt used Write Without Response,
 * which gets zero ATT-level feedback either way, so a request that the
 * phone silently rejected (e.g. because Control Point's actual GATT
 * property is "Write" and not "Write Without Response" -- ANCS's spec text
 * is not 100% certain from memory, and no NimBLE headers are available
 * here to check its discovered property bits) would look identical, from
 * this code's point of view, to one that was accepted but never answered.
 * This makes that ambiguity visible in the log instead. */
static int ancs_cp_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                             struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle; (void)attr; (void)arg;
    if (error->status == 0) {
        ESP_LOGI(TAG, "ANCS: Control Point write acked by phone");
    } else {
        ESP_LOGW(TAG, "ANCS: Control Point write rejected; status=%d", error->status);
    }
    return 0;
}

static void ancs_request_attributes(uint16_t conn_handle, uint32_t uid)
{
    if (s_ancs_cp_val_handle == 0) return;   /* Control Point not discovered (yet) */

    uint8_t req[12];
    size_t  n = 0;
    req[n++] = ANCS_CMD_GET_NOTIF_ATTRS;
    req[n++] = (uint8_t)(uid);
    req[n++] = (uint8_t)(uid >> 8);
    req[n++] = (uint8_t)(uid >> 16);
    req[n++] = (uint8_t)(uid >> 24);
    req[n++] = ANCS_ATTR_APP_IDENTIFIER;
    req[n++] = ANCS_ATTR_TITLE;
    req[n++] = (uint8_t)(ANCS_TITLE_MAX_LEN);
    req[n++] = (uint8_t)(ANCS_TITLE_MAX_LEN >> 8);
    req[n++] = ANCS_ATTR_MESSAGE;
    req[n++] = (uint8_t)(ANCS_MESSAGE_MAX_LEN);
    req[n++] = (uint8_t)(ANCS_MESSAGE_MAX_LEN >> 8);

    /* Diagnostic -- dump the exact bytes being sent so the request layout
     * (CommandID + UID + repeated AttributeID[+MaxLen]) can be eyeballed
     * against the ANCS spec directly from the log, rather than trusting
     * this code's own idea of what it built. */
    char hex[3 * sizeof(req) + 1];
    for (size_t i = 0; i < n; i++) {
        snprintf(&hex[i * 3], 4, "%02X ", req[i]);
    }
    ESP_LOGI(TAG, "ANCS: GetNotificationAttributes request, handle=%u uid=%lu bytes=[ %s]",
             s_ancs_cp_val_handle, (unsigned long)uid, hex);

    s_ds_len = 0;   /* fresh reassembly buffer for this request's response */
    s_ancs_request_busy = true;
    s_ancs_req_type = ANCS_REQ_NOTIF_ATTRS;

    /* Write Request (with response) instead of Write Without Response --
     * see ancs_cp_write_cb()'s comment above for why. This still doesn't
     * confirm the phone actually processed the command semantically (only
     * that the ATT-layer write itself was accepted), but it turns a
     * previously-silent failure mode into a logged one. */
    int rc = ble_gattc_write_flat(conn_handle, s_ancs_cp_val_handle, req, n,
                                   ancs_cp_write_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ANCS: GetNotificationAttributes write failed: %d", rc);
        s_ancs_request_busy = false;   /* never actually went out -- don't block the next one */
        s_ancs_req_type = ANCS_REQ_NONE;
        return;
    }

    ancs_arm_timeout();
}

/* Second leg of the chain kicked off from ancs_parse_notif_attrs_response()
 * on an app-name cache miss -- same request/response machinery
 * (s_ds_buf/s_ds_len, s_ancs_request_busy, the timeout timer), just a
 * different Control Point command and response layout. app_id must still
 * be valid when this returns (it's read out of a local array on the
 * caller's stack) -- it isn't stored anywhere here since
 * ancs_parse_app_attrs_response() rereads it out of the echoed response
 * bytes instead of needing it passed back in. */
static void ancs_request_app_attributes(uint16_t conn_handle, const char *app_id)
{
    if (s_ancs_cp_val_handle == 0) {
        ancs_push_toast_now(NULL, s_pending_title, s_pending_message);
        ancs_finish_request();
        return;
    }

    size_t id_len = strlen(app_id);
    if (id_len > ANCS_APP_ID_LEN - 1) id_len = ANCS_APP_ID_LEN - 1;

    uint8_t req[ANCS_APP_ID_LEN + 4];
    size_t  n = 0;
    req[n++] = ANCS_CMD_GET_APP_ATTRS;
    memcpy(&req[n], app_id, id_len);
    n += id_len;
    req[n++] = 0x00;   /* AppIdentifier is NUL-terminated per spec */
    req[n++] = ANCS_APP_ATTR_DISPLAY_NAME;

    ESP_LOGI(TAG, "ANCS: GetAppAttributes request, handle=%u app_id=\"%s\"",
             s_ancs_cp_val_handle, app_id);

    s_ds_len = 0;
    s_ancs_request_busy = true;
    s_ancs_req_type = ANCS_REQ_APP_ATTRS;

    int rc = ble_gattc_write_flat(conn_handle, s_ancs_cp_val_handle, req, n,
                                   ancs_cp_write_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "ANCS: GetAppAttributes write failed: %d", rc);
        /* App name lookup failed to even go out -- still show the
         * notification itself, just without a resolved app name. */
        ancs_push_toast_now(NULL, s_pending_title, s_pending_message);
        ancs_finish_request();
        return;
    }

    ancs_arm_timeout();
}

static void ancs_on_notification_source_notify(uint16_t conn_handle, const uint8_t *data, uint16_t len)
{
    if (len < 8) return;

    uint8_t event_id = data[0];
    /* EventFlags (data[1]) and Category/CategoryCount (data[2]/data[3])
     * not used yet -- could gate which categories fetch text later. */
    uint32_t uid = (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
                   ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);

    if (event_id != ANCS_EVENT_ID_ADDED) return;   /* only fetch text for new notifications */

    if (s_ancs_request_busy) {
        /* Already fetching text for an earlier notification -- see the
         * file-header comment above ancs_request_attributes() for why
         * this guard exists (initial-sync burst crashed NimBLE without
         * it). Best-effort: this one's text just won't show. */
        /* LOGD, not LOGI -- the initial-sync burst alone drops 100+ of
         * these in under a second (see the file-header comment), and at
         * LOGI that alone drowns out everything else in the log viewer.
         * Compiles to a no-op under the project's default log level. */
        ESP_LOGD(TAG, "ANCS: dropping notification uid=%lu, still fetching a previous one",
                 (unsigned long)uid);
        return;
    }

    ancs_request_attributes(conn_handle, uid);
}

static int ancs_disc_svc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                             const struct ble_gatt_svc *service, void *arg)
{
    (void)arg;

    if (error->status != 0 || service == NULL) {
        if (error->status != BLE_HS_EDONE) {
            ESP_LOGW(TAG, "ANCS: service discovery ended early; status=%d "
                          "(iOS doesn't guarantee ANCS is always published)",
                     error->status);
        }
        return 0;
    }

    s_ancs_svc_end_handle = service->end_handle;
    int rc = ble_gattc_disc_all_chrs(conn_handle, service->start_handle,
                                      service->end_handle,
                                      ancs_disc_chr_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ANCS: disc_all_chrs failed: %d", rc);
    }
    return 0;
}

static void ancs_start_discovery(uint16_t conn_handle)
{
    s_ancs_svc_end_handle = 0;
    s_ancs_ns_val_handle  = 0;
    s_ancs_ns_cccd_handle = 0;
    s_ancs_cp_val_handle  = 0;
    s_ancs_ds_val_handle  = 0;
    s_ancs_ds_cccd_handle = 0;
    s_ds_len              = 0;   /* also drop any in-flight Data Source reassembly */
    s_ancs_req_type       = ANCS_REQ_NONE;
    s_pending_valid       = false;
    /* App name cache deliberately left alone -- display names don't
     * change across a reconnect of the same phone. */
    s_ancs_request_busy   = false;
    if (s_ancs_request_timeout_timer) esp_timer_stop(s_ancs_request_timeout_timer);

    int rc = ble_gattc_disc_svc_by_uuid(conn_handle, ANCS_SVC_UUID,
                                         ancs_disc_svc_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ANCS: disc_svc_by_uuid failed: %d", rc);
    }
}

/* -----------------------------------------------------------------------
 * GAP events
 * ----------------------------------------------------------------------- */
static int gap_event_handler(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "connected; handle=%d", s_conn_handle);
            lv_async_call(toast_ble_connected_cb, NULL);
            ancs_start_discovery(s_conn_handle);
        } else {
            ESP_LOGW(TAG, "connect failed; status=%d", event->connect.status);
            if (s_enabled) ble_manager_advertise();
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "disconnected; reason=%d", event->disconnect.reason);
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        lv_async_call(toast_ble_disconnected_cb, NULL);
        /* NimBLE stops advertising once a connection is accepted -- restart
         * it so the phone (or a future second phone) can reconnect without
         * the switch needing to be toggled off/on again. */
        if (s_enabled) ble_manager_advertise();
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        /* Advertising duration/timeout elapsed (BLE_HS_FOREVER means this
         * normally shouldn't fire, but handle it defensively). */
        if (s_enabled) ble_manager_advertise();
        return 0;

    case BLE_GAP_EVENT_ENC_CHANGE:
        ESP_LOGI(TAG, "encryption change; status=%d", event->enc_change.status);
        if (event->enc_change.status == 0) {
            /* Link just became encrypted (initial pairing just completed,
             * or a bonded phone reconnected) -- (re)try both CCCD writes
             * in case either's first attempt bounced off "insufficient
             * authentication" before pairing had run. Harmless no-op for
             * whichever's already subscribed, or whose CCCD handle isn't
             * discovered yet (ancs_write_cccd() no-ops on a zero handle
             * -- the original write attempt from ancs_disc_dsc_cb() is
             * still what kicked off pairing to begin with). */
            ancs_write_cccd(event->enc_change.conn_handle, s_ancs_ns_cccd_handle, "Notification Source");
            ancs_write_cccd(event->enc_change.conn_handle, s_ancs_ds_cccd_handle, "Data Source");
        }
        return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint16_t handle = event->notify_rx.attr_handle;

        if (handle != s_ancs_ns_val_handle && handle != s_ancs_ds_val_handle) {
            /* Genuinely unexpected -- a notify/indicate on a handle that's
             * neither Notification Source nor Data Source. Worth keeping
             * at INFO since it should basically never happen; if Data
             * Source responses ever start arriving on the wrong handle,
             * this is what would catch it. The matched-handle case below
             * doesn't need its own line -- NS already logs per-event at
             * LOGD, DS logs its own byte count in
             * ancs_on_data_source_notify(). */
            ESP_LOGI(TAG, "ANCS: NOTIFY_RX on unrecognized handle=%u indication=%d (ns=%u ds=%u)",
                     handle, (int)event->notify_rx.indication,
                     s_ancs_ns_val_handle, s_ancs_ds_val_handle);
        }

        if (handle == s_ancs_ns_val_handle) {
            uint8_t  buf[8];
            uint16_t out_len = 0;
            if (ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof(buf), &out_len) == 0) {
                ancs_on_notification_source_notify(event->notify_rx.conn_handle, buf, out_len);
            }
        } else if (handle == s_ancs_ds_val_handle) {
            uint8_t  chunk[sizeof(s_ds_buf)];
            uint16_t out_len = 0;
            if (ble_hs_mbuf_to_flat(event->notify_rx.om, chunk, sizeof(chunk), &out_len) == 0) {
                ancs_on_data_source_notify(chunk, out_len);
            }
        }
        return 0;
    }

    default:
        return 0;
    }
}

/* -----------------------------------------------------------------------
 * Host lifecycle
 * ----------------------------------------------------------------------- */
static void on_reset(int reason)
{
    ESP_LOGW(TAG, "host reset; reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    assert(rc == 0);

    rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_hs_id_infer_auto failed: %d", rc);
        return;
    }

    if (s_enabled) ble_manager_advertise();
}

static void host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

/* -----------------------------------------------------------------------
 * Public
 * ----------------------------------------------------------------------- */
void ble_manager_init(void)
{
    esp_err_t ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %d", ret);
        return;
    }

    ble_hs_cfg.reset_cb       = on_reset;
    ble_hs_cfg.sync_cb        = on_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    /* Bonding, Just Works (no PIN/keyboard on this device). */
    ble_hs_cfg.sm_io_cap        = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding       = 1;
    ble_hs_cfg.sm_mitm          = 0;
    ble_hs_cfg.sm_sc            = 1;
    ble_hs_cfg.sm_our_key_dist  = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_svc_gap_device_name_set(DEVICE_NAME);
    assert(rc == 0);

    /* NVS-backed bond storage -- CONFIG_BT_NIMBLE_NVS_PERSIST must be on,
     * otherwise this still compiles but bonds won't survive reboot. */
    ble_store_config_init();

    nimble_port_freertos_init(host_task);
}

void ble_manager_set_enabled(bool on)
{
    if (on == s_enabled) return;
    s_enabled = on;

    if (on) {
        ble_manager_advertise();
    } else {
        ble_gap_adv_stop();
        if (s_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(s_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
}

bool ble_manager_is_enabled(void)
{
    return s_enabled;
}
