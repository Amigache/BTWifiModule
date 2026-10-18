/**
 * @file main.c
 * @brief Entry point: initializes NVS, loads the configuration and starts the
 *        UART task that drives the module.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "settings.h"
#include "terminal.h"

/** Shared NVS handle (used by settings.c). */
nvs_handle_t nvs_flsh_btw;

void app_main(void)
{
#if defined(BOOT_DEBUG_DELAY_MS)
  /* Give the host time to open the USB console before the first log. */
  vTaskDelay(pdMS_TO_TICKS(BOOT_DEBUG_DELAY_MS));
#endif
  /* Initialize NVS. */
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(nvs_open("btwifi", NVS_READWRITE, &nvs_flsh_btw));

  /* Load the persistent configuration before starting the UART link. */
  settings_init();
  settings_load();

  /* UART task: parses AT, forwards traffic and runs the state machines. */
  TaskHandle_t tUartHnd = NULL;
  xTaskCreate(runUARTHead, "UART", 4096, NULL, tskIDLE_PRIORITY + 2, &tUartHnd);
  configASSERT(tUartHnd);
}
