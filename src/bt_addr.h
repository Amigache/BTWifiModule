/**
 * @file bt_addr.h
 * @brief Pure Bluetooth address conversion helpers (no ESP-IDF dependencies,
 *        so they are host-testable).
 *
 * The firmware works with addresses in *display order* (most significant byte
 * first), which is how they appear in the AT commands (`Central:<12 hex>`).
 * The NimBLE host reports addresses in *wire order* (little-endian), so they
 * must be reversed with bt_addr_reverse() before comparing or displaying.
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/** Bluetooth address length (bytes). */
#define BT_ADDR_LEN 6

/** Length of the text representation (12 hex digits + NUL terminator). */
#define BT_ADDR_STR_LEN 13

/** Bluetooth address in display order. */
typedef uint8_t bt_addr_t[BT_ADDR_LEN];

/**
 * @brief Convert a text address ("AABBCCDDEEFF") into bytes.
 *
 * @param dest  output buffer (@ref BT_ADDR_LEN bytes).
 * @param str   string of 12 hex digits (no separators).
 *
 * @note Input is not validated; non-hex characters are interpreted via
 *       `strtoul`, so ensure clean input.
 */
void bt_addr_from_str(bt_addr_t dest, const char *str);

/**
 * @brief Format an address as 12 uppercase hex digits.
 *
 * @param dest  output buffer of at least @ref BT_ADDR_STR_LEN bytes.
 * @param src   input address.
 * @return `dest` (for chaining in printf/strcpy).
 */
char *bt_addr_to_str(char dest[BT_ADDR_STR_LEN], const bt_addr_t src);

/**
 * @brief Reverse the byte order of an address (wire <-> display order).
 *
 * @param src  input address.
 * @param dst  output address (may alias `src`).
 */
void bt_addr_reverse(const bt_addr_t src, bt_addr_t dst);

/**
 * @brief Report whether an address is all zeroes (no value).
 *
 * @param addr  address to check.
 * @return `true` if all @ref BT_ADDR_LEN bytes are zero.
 */
bool bt_addr_is_zero(const bt_addr_t addr);
