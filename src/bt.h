/**
 * @file bt.h
 * @brief Common Bluetooth stack initialization and control (Bluedroid or
 *        NimBLE, selected via the `USE_NIMBLE` macro).
 *
 * The concrete roles (peripheral/central) are initialized in bt_server.c and
 * bt_client.c; this module only starts/stops the controller and host.
 */

#pragma once

#include "bt_addr.h"

// `esp_bd_addr_t` is provided by ESP-IDF under Bluedroid; under NimBLE it is
// defined here as an equivalent 6-byte array.
#if defined(USE_NIMBLE)
  #include <stdint.h>
  typedef uint8_t esp_bd_addr_t[6];
#else
  #include "esp_bt_defs.h"
#endif

/** Pause (ms) before restarting the BT stack after a role change. */
#define BT_PAUSE_BEFORE_RESTART 250

/** BLE connection parameters (interval x1.25ms, timeout x10ms). */
#define BT_CON_INT_MIN 10
#define BT_CON_INT_MAX 10
#define BT_CON_TIMEOUT 70

/** Local and remote addresses (in display order). */
extern esp_bd_addr_t localbtaddress;
extern esp_bd_addr_t rmtbtaddress;

/** Initialize the Bluetooth controller and host (BLE). */
void bt_init(void);

/** Disable and release the Bluetooth stack. */
void bt_disable(void);

/**
 * @brief Set the Bluetooth device name.
 *
 * @param name  new name (copied with a length limit).
 */
void bt_set_name(const char *name);

#if defined(USE_NIMBLE)
/** NimBLE host task (runs nimble_port_run). */
void bt_host_task(void *param);
#endif
