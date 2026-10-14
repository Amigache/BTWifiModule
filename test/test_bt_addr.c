/**
 * @file test_bt_addr.c
 * @brief Unit tests for Bluetooth address conversion
 *        (bt_addr.h / bt_addr.c).
 */

#include "unity.h"
#include <string.h>

#include "../src/bt_addr.c"

void test_to_str(void)
{
  bt_addr_t a = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF};
  char s[BT_ADDR_STR_LEN];
  TEST_ASSERT_EQUAL_STRING("AABBCCDDEEFF", bt_addr_to_str(s, a));
}

void test_from_str(void)
{
  bt_addr_t a;
  bt_addr_from_str(a, "AABBCCDDEEFF");
  char s[BT_ADDR_STR_LEN];
  TEST_ASSERT_EQUAL_STRING("AABBCCDDEEFF", bt_addr_to_str(s, a));
}

void test_roundtrip(void)
{
  bt_addr_t a, b;
  bt_addr_from_str(a, "010203040506");
  char s[BT_ADDR_STR_LEN];
  bt_addr_to_str(s, a);
  bt_addr_from_str(b, s);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(a, b, BT_ADDR_LEN);
}

void test_reverse(void)
{
  bt_addr_t a = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  bt_addr_t r;
  bt_addr_reverse(a, r);
  TEST_ASSERT_EQUAL_UINT8(0x06, r[0]);
  TEST_ASSERT_EQUAL_UINT8(0x01, r[5]);

  // Reversing twice returns the original address.
  bt_addr_t back;
  bt_addr_reverse(r, back);
  TEST_ASSERT_EQUAL_UINT8_ARRAY(a, back, BT_ADDR_LEN);
}

void test_reverse_in_place(void)
{
  bt_addr_t a = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  bt_addr_reverse(a, a);
  TEST_ASSERT_EQUAL_UINT8(0x06, a[0]);
  TEST_ASSERT_EQUAL_UINT8(0x01, a[5]);
}

void test_is_zero(void)
{
  bt_addr_t z = {0};
  TEST_ASSERT_TRUE(bt_addr_is_zero(z));

  bt_addr_t nz = {0, 0, 0, 0, 0, 1};
  TEST_ASSERT_FALSE(bt_addr_is_zero(nz));
}

void run_bt_addr_tests(void)
{
  RUN_TEST(test_to_str);
  RUN_TEST(test_from_str);
  RUN_TEST(test_roundtrip);
  RUN_TEST(test_reverse);
  RUN_TEST(test_reverse_in_place);
  RUN_TEST(test_is_zero);
}
