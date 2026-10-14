/**
 * @file test_cbuf.c
 * @brief Unit tests for the circular buffer (cb.h / cb.c).
 */

#include "unity.h"
#include <string.h>

#include "../src/cb.c"

void test_basics(void)
{
  cbuf_t buffer;
  TEST_ASSERT_TRUE(cbuf_init(&buffer, 200));

  const char *test = "Hello world";
  size_t len = strlen(test);
  for (size_t i = 0; i < len; i++) {
    TEST_ASSERT_TRUE(cbuf_push(&buffer, (uint8_t)test[i]));
  }
  TEST_ASSERT_EQUAL(len, cbuf_count(&buffer));

  for (size_t i = 0; i < len; i++) {
    uint8_t byte = 0;
    TEST_ASSERT_TRUE(cbuf_pop(&buffer, &byte));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)test[i], byte);
  }

  uint8_t byte = 0;
  TEST_ASSERT_FALSE(cbuf_pop(&buffer, &byte));
  TEST_ASSERT_TRUE(cbuf_is_empty(&buffer));

  cbuf_free(&buffer);
}

void test_wraparound(void)
{
  cbuf_t buffer;
  TEST_ASSERT_TRUE(cbuf_init(&buffer, 32));

  // Write/read far more times than the capacity to force wraparound.
  for (int i = 0; i < 5000; i++) {
    for (int j = 0; j < 17; j++) {
      TEST_ASSERT_TRUE(cbuf_push(&buffer, (uint8_t)('A' + j)));
    }
    for (int j = 0; j < 17; j++) {
      uint8_t byte = 0;
      TEST_ASSERT_TRUE(cbuf_pop(&buffer, &byte));
      TEST_ASSERT_EQUAL_UINT8((uint8_t)('A' + j), byte);
    }
    uint8_t byte = 0;
    TEST_ASSERT_FALSE(cbuf_pop(&buffer, &byte));
  }

  cbuf_free(&buffer);
}

void test_overflow(void)
{
  cbuf_t buffer;
  TEST_ASSERT_TRUE(cbuf_init(&buffer, 42));

  for (int i = 0; i < 5000; i++) {
    for (int j = 0; j < 50; j++) {
      TEST_ASSERT_EQUAL_INT(j < 42, cbuf_push(&buffer, (uint8_t)('A' + j)));
    }
    for (int j = 0; j < 50; j++) {
      uint8_t byte = 0;
      bool ok = cbuf_pop(&buffer, &byte);
      if (j < 42) {
        TEST_ASSERT_TRUE(ok);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)('A' + j), byte);
      } else {
        TEST_ASSERT_FALSE(ok);
      }
    }
    TEST_ASSERT_EQUAL(0, cbuf_count(&buffer));
  }

  cbuf_free(&buffer);
}

void run_cbuf_tests(void)
{
  RUN_TEST(test_basics);
  RUN_TEST(test_wraparound);
  RUN_TEST(test_overflow);
}
