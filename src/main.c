/*
 * BTWifiModule — ESP32-C3 BLE Trainer / Telemetry bridge with WiFi dashboard
 *
 * Emulates FrSky BLE modules (Peripheral/Central/Telemetry roles).
 * Provides a SoftAP web dashboard for configuration, BLE scanning,
 * OTA firmware updates, and real-time status via WebSocket.
 */

#include "bt.h"
#include "defines.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "settings.h"
#include "terminal.h"
#include "webserver.h"

nvs_handle_t nvs_flsh_btw;

void app_main(void)
{
  TaskHandle_t tUartHnd = NULL;
  xTaskCreate(runUARTHead, "UART", 4096, NULL, tskIDLE_PRIORITY + 2, &tUartHnd);
  configASSERT(tUartHnd);

  esp_err_t ret;

  /* Initialize NVS. */
  ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);

  ESP_ERROR_CHECK(nvs_open("btwifi", NVS_READWRITE, &nvs_flsh_btw));

  loadSettings();

  webserver_start();
}
