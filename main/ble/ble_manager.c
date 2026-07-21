#include "ble_manager.h"
#include "ui_toast.h"
#include "lvgl.h"

#include "esp_log.h"
#include <string.h>
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
static void toast_ble_connected_cb(void *arg)    { (void)arg; ui_toast_push("BLE Connected", 1, NULL); }
static void toast_ble_disconnected_cb(void *arg) { (void)arg; ui_toast_push("BLE Disconnected", 1, NULL); }

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
#define ANCS_CCCD_UUID16   0x2902

static uint16_t s_ancs_svc_end_handle;
static uint16_t s_ancs_ns_val_handle;
static uint16_t s_ancs_ns_cccd_handle;

static int ancs_write_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                          struct ble_gatt_attr *attr, void *arg)
{
    (void)attr; (void)arg;

    if (error->status == 0) {
        ESP_LOGI(TAG, "ANCS: subscribed to Notification Source");
        return 0;
    }

    if (error->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_AUTHEN) ||
        error->status == BLE_HS_ATT_ERR(BLE_ATT_ERR_INSUFFICIENT_ENC)) {
        /* This is the expected first-attempt failure -- Notification
         * Source requires an encrypted/authenticated link and this one
         * isn't yet. NimBLE does *not* auto-initiate pairing on this
         * error by itself; the app has to explicitly ask for it. This
         * call is what actually makes iOS show its system pairing
         * dialog. Once it completes, BLE_GAP_EVENT_ENC_CHANGE fires and
         * ancs_write_cccd() retries this write. */
        ESP_LOGI(TAG, "ANCS: CCCD write needs auth (status=%d) -- "
                      "initiating pairing", error->status);
        int rc = ble_gap_security_initiate(conn_handle);
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGE(TAG, "ANCS: ble_gap_security_initiate failed: %d", rc);
        }
    } else {
        ESP_LOGW(TAG, "ANCS: CCCD write failed; status=%d", error->status);
    }
    return 0;
}

static void ancs_write_cccd(uint16_t conn_handle)
{
    static const uint8_t value[2] = { 0x01, 0x00 };   /* enable notifications */
    int rc;

    if (s_ancs_ns_cccd_handle == 0) return;

    rc = ble_gattc_write_flat(conn_handle, s_ancs_ns_cccd_handle,
                              value, sizeof(value), ancs_write_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ANCS: CCCD write kickoff failed: %d", rc);
    }
}

static int ancs_disc_dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                             uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                             void *arg)
{
    (void)chr_val_handle; (void)arg;

    if (error->status == 0 && dsc != NULL) {
        if (dsc->uuid.u.type == BLE_UUID_TYPE_16 &&
            ble_uuid_u16(&dsc->uuid.u) == ANCS_CCCD_UUID16) {
            s_ancs_ns_cccd_handle = dsc->handle;
        }
        return 0;
    }

    /* Descriptor discovery finished (error->status == BLE_HS_EDONE, or any
     * other terminal status). */
    if (s_ancs_ns_cccd_handle != 0) {
        ancs_write_cccd(conn_handle);
    } else {
        ESP_LOGW(TAG, "ANCS: Notification Source CCCD not found");
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
        }
        return 0;
    }

    /* Characteristic discovery finished. */
    if (s_ancs_ns_val_handle != 0) {
        int rc = ble_gattc_disc_all_dscs(conn_handle, s_ancs_ns_val_handle,
                                          s_ancs_svc_end_handle,
                                          ancs_disc_dsc_cb, NULL);
        if (rc != 0) {
            ESP_LOGE(TAG, "ANCS: disc_all_dscs failed: %d", rc);
        }
    } else {
        ESP_LOGW(TAG, "ANCS: Notification Source characteristic not found");
    }
    return 0;
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
             * or a bonded phone reconnected) -- (re)try the CCCD write in
             * case the first attempt bounced off "insufficient
             * authentication" before pairing had run. Harmless no-op if
             * we're already subscribed, or if discovery hasn't found the
             * CCCD handle yet (ancs_write_cccd() is a no-op in that case
             * -- the original write attempt from ancs_disc_dsc_cb() is
             * still the one that kicked off pairing to begin with). */
            ancs_write_cccd(event->enc_change.conn_handle);
        }
        return 0;

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
