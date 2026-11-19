/**
 * @file status_led.h
 * @brief Status indication on the on-board RGB LED.
 *
 * Only the RadioMaster Wireless Module (`PCB_RM_WIRELESS`) carries a status LED
 * (WS2812-compatible TX1812MJA5-1515 on GPIO18, through R15). On every other
 * board the functions are no-ops, so callers do not need board conditionals.
 */

#pragma once

#include <stdbool.h>

/**
 * @brief Initialize the LED driver and show the "powered, no link" colour.
 *
 * Must be called once from the task that later calls @ref status_led_update;
 * the driver is not thread-safe. A hardware failure only leaves the LED off.
 */
void status_led_init(void);

/**
 * @brief Refresh the LED from the current link state.
 *
 * Colours: red = powered but idle, green = radio UART active, blue = BLE peer
 * connected, cyan = both. The LED is only written when the colour changes.
 *
 * @param radio_active true while the radio UART has recent traffic
 * @param bt_connected true while a BLE peer is connected (either role)
 */
void status_led_update(bool radio_active, bool bt_connected);
