/**
 * @file test_at_command.c
 * @brief Unit tests for the AT command parser (at_command.h / at_command.c).
 */

#include "unity.h"
#include <string.h>

#include "../src/at_command.c"

void test_role0(void)
{
  char line[] = "+ROLE0\r\n";
  at_command_parse_t r = at_command_parse(line);
  TEST_ASSERT_EQUAL(AT_CMD_ROLE0, r.command);
}

void test_role1_with_at_prefix(void)
{
  char line[] = "AT+ROLE1\n";
  at_command_parse_t r = at_command_parse(line);
  TEST_ASSERT_EQUAL(AT_CMD_ROLE1, r.command);
}

void test_con_argument(void)
{
  char line[] = "+CONAABBCCDDEEFF\r\n";
  at_command_parse_t r = at_command_parse(line);
  TEST_ASSERT_EQUAL(AT_CMD_CON, r.command);
  TEST_ASSERT_EQUAL_STRING("AABBCCDDEEFF", r.argument);
}

void test_name_argument(void)
{
  char line[] = "AT+NAMETX16S\r\n";
  at_command_parse_t r = at_command_parse(line);
  TEST_ASSERT_EQUAL(AT_CMD_NAME, r.command);
  TEST_ASSERT_EQUAL_STRING("TX16S", r.argument);
}

void test_txpw_argument(void)
{
  char line[] = "AT+TXPW2\r\n";
  at_command_parse_t r = at_command_parse(line);
  TEST_ASSERT_EQUAL(AT_CMD_TXPW, r.command);
  TEST_ASSERT_EQUAL_STRING("2", r.argument);
}

void test_disc(void)
{
  char line[] = "AT+DISC?\r\n";
  at_command_parse_t r = at_command_parse(line);
  TEST_ASSERT_EQUAL(AT_CMD_DISC, r.command);
}

void test_clear(void)
{
  char line[] = "AT+CLEAR\r\n";
  at_command_parse_t r = at_command_parse(line);
  TEST_ASSERT_EQUAL(AT_CMD_CLEAR, r.command);
}

void test_baud_argument(void)
{
  char line[] = "AT+BAUD4\r\n";
  at_command_parse_t r = at_command_parse(line);
  TEST_ASSERT_EQUAL(AT_CMD_BAUD, r.command);
  TEST_ASSERT_EQUAL_STRING("4", r.argument);
}

void test_htreset(void)
{
  char line[] = "AT+HTRESET\r\n";
  at_command_parse_t r = at_command_parse(line);
  TEST_ASSERT_EQUAL(AT_CMD_HTRESET, r.command);
}

void test_unknown(void)
{
  char line[] = "AT+BOGUS\r\n";
  at_command_parse_t r = at_command_parse(line);
  TEST_ASSERT_EQUAL(AT_CMD_UNKNOWN, r.command);
}

void test_trailing_whitespace_stripped(void)
{
  char line[] = "+NAMEfoo   ";
  at_command_parse_t r = at_command_parse(line);
  TEST_ASSERT_EQUAL(AT_CMD_NAME, r.command);
  TEST_ASSERT_EQUAL_STRING("foo", r.argument);
}

void test_null_input(void)
{
  at_command_parse_t r = at_command_parse(NULL);
  TEST_ASSERT_EQUAL(AT_CMD_UNKNOWN, r.command);
}

void test_empty_input(void)
{
  char line[] = "";
  at_command_parse_t r = at_command_parse(line);
  TEST_ASSERT_EQUAL(AT_CMD_UNKNOWN, r.command);
}

void run_at_command_tests(void)
{
  RUN_TEST(test_role0);
  RUN_TEST(test_role1_with_at_prefix);
  RUN_TEST(test_con_argument);
  RUN_TEST(test_name_argument);
  RUN_TEST(test_txpw_argument);
  RUN_TEST(test_disc);
  RUN_TEST(test_clear);
  RUN_TEST(test_baud_argument);
  RUN_TEST(test_htreset);
  RUN_TEST(test_unknown);
  RUN_TEST(test_trailing_whitespace_stripped);
  RUN_TEST(test_null_input);
  RUN_TEST(test_empty_input);
}
