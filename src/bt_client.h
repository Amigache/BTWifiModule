/**
 * @file bt_client.h
 * @brief Central role (GATT client): scan, connect and auto-connect to a BLE
 *        trainer/headtracker.
 *
 * In central mode (trainer master) the module scans, connects to the saved
 * device and forwards the trainer notifications to the radio over UART.
 *
 * Supports both Bluedroid and NimBLE (per `USE_NIMBLE`).
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "bt_addr.h"

// `esp_bd_addr_t` is provided by ESP-IDF under Bluedroid; under NimBLE it is
// defined here.
#if defined(USE_NIMBLE)
  #include <stdint.h>
  typedef uint8_t esp_bd_addr_t[6];
#else
  #include "esp_bt_defs.h"
#endif

/** Maximum number of distinct addresses kept during a scan. */
#define MAX_BLE_ADDRESSES 20

/** Minimum RSSI (dBm) to consider a device during the scan. */
#define MIN_BLE_RSSI -95

/** Board/trainer type detected from its GATT characteristics. */
typedef enum {
  BLE_BOARD_UNKNOWN,
  BLE_BOARD_FRSKY_CC2540,
  BLE_BOARD_FRSKY_PARA,
  BLE_BOARD_HEADTRACKER,
  BLE_BOARD_FLYSKY,
  BLE_BOARD_HM10,
  BLE_BOARD_COUNT
} ble_board_type;

/** Device found during the scan (address in display order). */
typedef struct {
  bt_addr_t addr;
  uint8_t type;  //< BLE address type (BLE_ADDR_TYPE_*)
} bt_scanned_dev_t;

/** Board type names (index = ble_board_type). */
extern char *ble_board_type_names[BLE_BOARD_COUNT];

/* --- Connection state ----------------------------------------------------- */
extern volatile bool bt_client_connected;
extern volatile bool bt_client_scan_complete;
extern volatile bool bt_client_validslavefound;
extern volatile bool bt_client_ht_reset;
extern volatile bool bt_client_forwardHt;   //< permission to forward trainer frames
extern volatile ble_board_type bt_client_board_type;

/** Initialize the GATT central (register callbacks and configure MTU). */
void bt_client_init(void);

/** Disconnect from the remote device, if connected. */
void bt_client_disconnect(void);

/** Start a time-limited scan (for AT+DISC?). */
void bt_client_start_scan(void);

/** Stop the ongoing scan. */
void bt_client_scan_stop(void);

/** @return `true` if there is a valid saved address to auto-connect to. */
bool bt_client_has_saved_address(void);

/**
 * @brief Scan indefinitely until the saved device is found, then connect.
 * @return `false` if not ready or already connected.
 */
bool bt_client_start_autoconnect(void);

/**
 * @brief Connect to an address (which must be in the last scan list).
 * @param addr  address in display order.
 */
void bt_client_connect(bt_addr_t addr);

/** Send the reset command to the headtracker (if connected to one). */
void bt_client_dohtreset(void);

/* --- Thread-safe access to the scanned-device list ------------------------ */

/** @return number of devices kept from the last scan. */
uint8_t bt_client_scanned_count(void);

/**
 * @brief Copy device `index` into `addr_out`/`type_out`.
 * @return `true` if the index is valid.
 */
bool bt_client_scanned_get(uint8_t index, bt_addr_t addr_out, uint8_t *type_out);
