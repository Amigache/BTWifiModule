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
#include <stddef.h>
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
 * @brief Connect to an address previously seen by a scan.
 *
 * The peer address type is only known for devices kept in the last scan list.
 * When the address is unknown (for instance because the peer was off during
 * the last scan) no attempt is started: the caller must rescan instead of
 * waiting for a connection event that will never arrive.
 *
 * @param addr  address in display order.
 * @return `true` if a connection attempt was started, `false` otherwise.
 */
bool bt_client_connect(bt_addr_t addr);

/**
 * @brief Abort a connection attempt that is still in progress.
 *
 * Does nothing when a connection is already established or when the host has
 * no way to cancel the attempt (Bluedroid); in that case the stack's own
 * connect timeout reports the failure.
 */
void bt_client_abort_connect(void);

/** Send the reset command to the headtracker (if connected to one). */
void bt_client_dohtreset(void);

/* --- Runtime diagnostics --------------------------------------------------- */

/** Runtime counters for the central (trainer master) data path. */
typedef struct {
  uint32_t notifications;      //< framed notifications forwarded
  uint32_t malformed;          //< notifications dropped (empty or unframed)
  uint32_t uart_packets;       //< frames written to the UART
  uint32_t uart_bytes;         //< bytes written to the UART
  uint32_t uart_write_max_us;  //< max UART write duration (us)
  uint32_t max_rx_to_uart_us;  //< max BLE -> UART latency (us)
  uint32_t avg_rx_to_uart_us;  //< average BLE -> UART latency (us)
  uint32_t max_gap_ms;         //< max gap between notifications (ms)
  uint32_t gaps_over_100_ms;   //< notification gaps over 100 ms
} bt_client_diag_t;

/** Snapshot the runtime counters into `d` (torn reads are acceptable). */
void bt_client_diag_get(bt_client_diag_t *d);

/* --- Thread-safe access to the scanned-device list ------------------------ */

/** @return number of devices kept from the last scan. */
uint8_t bt_client_scanned_count(void);

/**
 * @brief Copy device `index` into `addr_out`/`type_out`.
 * @return `true` if the index is valid.
 */
bool bt_client_scanned_get(uint8_t index, bt_addr_t addr_out, uint8_t *type_out);

/* --- Bounded BLE -> UART notification queue ------------------------------- */

/** Trainer frames buffered between the BLE callback and the UART task. */
#define BT_CLIENT_QUEUE_LEN 4

/** Maximum payload bytes per queued trainer frame (frames are <= ~20 bytes). */
#define BT_CLIENT_QUEUE_PAYLOAD 64

/**
 * @brief Enqueue a trainer frame for forwarding to the UART.
 *
 * Non-blocking; called from the BLE notification callback. If the queue is
 * full the oldest frame is evicted (latest-frame-wins) so a BLE burst cannot
 * pile up behind the UART and add unbounded latency.
 *
 * @param data  frame bytes.
 * @param len   frame length (must be <= BT_CLIENT_QUEUE_PAYLOAD).
 */
void bt_client_notify_push(const uint8_t *data, size_t len);

/**
 * @brief Dequeue one trainer frame (non-blocking).
 * @param[out] data  buffer for the frame bytes.
 * @param[in]  cap   capacity of `data`.
 * @param[out] len   frame length written.
 * @return `true` if a frame was dequeued.
 */
bool bt_client_notify_pop(uint8_t *data, size_t cap, size_t *len);

/** @return number of frames evicted because the queue was full. */
uint32_t bt_client_notify_superseded(void);
