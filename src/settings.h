/**
 * @file settings.h
 * @brief Persistent module configuration (NVS) with protected access.
 *
 * The @ref settings struct is shared between the UART task and the Bluetooth
 * stack tasks. Flash writes are serialized: BT tasks only *request* a save
 * (settings_request_save) and the UART task performs it in its loop
 * (settings_process), guaranteeing a single writer and avoiding blocking NVS
 * operations from the BT context.
 */

#pragma once

#include <stdbool.h>

#include "board.h"
#include "bt_addr.h"

/** Struct persisted in NVS as a single blob. */
typedef struct {
  role_t role;                    //< current Bluetooth role
  char name[LEN_BLUETOOTH_NAME];  //< advertised name (AT+NAME)
  char rmtbtaddr[BT_ADDR_STR_LEN];//< saved remote address (auto-connect)
} settings_t;

extern settings_t settings;

/** `true` once the configuration has been loaded (or invalidated) from NVS. */
extern volatile bool settings_ok;

/**
 * @brief Create the protection mutex. Must be called once before using the
 *        rest of the module.
 */
void settings_init(void);

/** Load the configuration from NVS and set @ref settings_ok. */
void settings_load(void);

/**
 * @brief Persist the configuration to NVS (blocking, safe from any task).
 *        Skips redundant writes when nothing changed.
 */
void settings_save(void);

/**
 * @brief Request a deferred save (non-blocking). Use from Bluetooth stack
 *        callbacks.
 */
void settings_request_save(void);

/** Flush a pending save, if any. Call from the main loop. */
void settings_process(void);
