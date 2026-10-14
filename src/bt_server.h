/**
 * @file bt_server.h
 * @brief Peripheral role (GATT server): service 0xFFF0, characteristic 0xFFF6.
 *
 * Exposes a GATT service with a NOTIFY characteristic used to forward the bytes
 * the radio sends to the module (S.Port/CRSF telemetry or slave trainer), and
 * that a central can write to.
 *
 * Supports both Bluedroid and NimBLE (per `USE_NIMBLE`).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/** `true` while a BLE central is connected. */
extern volatile bool bt_server_connected;

/** Initialize the GATT peripheral and start advertising. */
void bt_server_init(void);

/**
 * @brief Send data over the notification characteristic.
 *
 * @param data  bytes to send.
 * @param len   number of bytes.
 * @return 0 on success, non-zero if not connected or the send failed.
 */
int bt_server_send_data(const uint8_t *data, int len);

/**
 * @brief Rebuild and re-apply the advertising data (e.g. after changing the
 *        name via AT+NAME).
 */
void bt_server_refresh_advertising(void);
