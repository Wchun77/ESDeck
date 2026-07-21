#include "ble_manager.h"
#include "ui_toast.h"
#include "lvgl.h"

#include "esp_log.h"
#include <string.h>
#include <assert.h>

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
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
 * ----------------------------------------------------------------------- */
static void ble_manager_advertise(void)
{
    struct ble_hs_adv_fields fields;
    struct ble_gap_adv_params adv_params;
    int rc;

    memset(&fields, 0, sizeof(fields));
    /* General discoverable + classic BR/EDR not supported (BLE only). */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;

    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
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
