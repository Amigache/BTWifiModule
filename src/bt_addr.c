/**
 * @file bt_addr.c
 * @brief Implementation of bt_addr.h (Bluetooth address conversion).
 */

#include "bt_addr.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void bt_addr_from_str(bt_addr_t dest, const char *str)
{
  for (int i = 0; i < BT_ADDR_LEN; i++) {
    char hex[3] = {str[2 * i], str[2 * i + 1], '\0'};
    dest[i] = (uint8_t)strtoul(hex, NULL, 16);
  }
}

char *bt_addr_to_str(char dest[BT_ADDR_STR_LEN], const bt_addr_t src)
{
  snprintf(dest, BT_ADDR_STR_LEN, "%02X%02X%02X%02X%02X%02X", src[0], src[1], src[2],
           src[3], src[4], src[5]);
  return dest;
}

void bt_addr_reverse(const bt_addr_t src, bt_addr_t dst)
{
  uint8_t tmp[BT_ADDR_LEN];
  for (int i = 0; i < BT_ADDR_LEN; i++) {
    tmp[i] = src[BT_ADDR_LEN - 1 - i];
  }
  memcpy(dst, tmp, BT_ADDR_LEN);
}

bool bt_addr_is_zero(const bt_addr_t addr)
{
  for (int i = 0; i < BT_ADDR_LEN; i++) {
    if (addr[i] != 0) return false;
  }
  return true;
}
