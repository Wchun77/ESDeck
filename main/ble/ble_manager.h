#pragma once

#include <stdbool.h>

/* -----------------------------------------------------------------------
 * Generic BLE peripheral manager.
 *
 * Scaffold for the notification-mirroring feature: ESP advertises as a
 * connectable BLE peripheral, the phone finds and pairs with it from its
 * own Bluetooth settings (first-time pairing can't be automated -- see
 * design discussion), and bonding is persisted so later reconnects are
 * automatic. This module owns only that much -- advertising on/off plus
 * the connect/disconnect signal. It does NOT implement ANCS yet: once
 * bonded, the actual notification subscription (GATT client role against
 * the phone's own ANCS service) is a separate module layered on top of
 * this same connection.
 *
 * No custom GATT service is exposed -- ESP only needs the standard GAP/GATT
 * services NimBLE registers automatically, since for ANCS the phone is the
 * GATT server and ESP is the client (see ui_toast.h for where the resulting
 * events end up on screen).
 *
 * Requires CONFIG_BT_NIMBLE_ENABLED=y and CONFIG_BT_NIMBLE_NVS_PERSIST=y in
 * sdkconfig (the latter so bonds survive reboot -- without it every power
 * cycle would need re-pairing).
 * ----------------------------------------------------------------------- */

/* Call once at boot (from app_main, alongside the other *_init calls).
 * Brings up the NimBLE host and GAP/GATT services but does NOT start
 * advertising -- matches the "off by default" design for the Settings
 * Bluetooth switch. */
void ble_manager_init(void);

/* Settings' Bluetooth switch calls this.
 * true  -- start advertising (accepts either a fresh pairing or a bonded
 *          phone reconnecting).
 * false -- stop advertising and drop any active connection. */
void ble_manager_set_enabled(bool on);

bool ble_manager_is_enabled(void);
