/**
 * @file test_main.c
 * @brief Single Unity test runner for all host-testable modules.
 *
 * PlatformIO's Unity framework links every file under test/ into one binary,
 * so there must be exactly one main(). Each module test file exposes a
 * `run_xxx_tests()` function invoked here between UNITY_BEGIN/UNITY_END.
 */

#include <unity.h>

extern void run_cbuf_tests(void);
extern void run_at_command_tests(void);
extern void run_bt_addr_tests(void);

int main(void)
{
  UNITY_BEGIN();
  run_cbuf_tests();
  run_at_command_tests();
  run_bt_addr_tests();
  return UNITY_END();
}
