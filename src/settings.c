/**
 * @file settings.c
 * @brief Implementation of settings.h (NVS persistence + protected access).
 */

#include "settings.h"

#include <string.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LOG_SET "SETTINGS"

/** NVS blob key that stores the settings_t struct. */
#define NVS_STRUCT_KEY "skey"

/** NVS handle opened in main.c. */
extern nvs_handle_t nvs_flsh_btw;

settings_t settings = {.role = ROLE_UNKNOWN};
volatile bool settings_ok = false;

static SemaphoreHandle_t s_mutex = NULL;
static volatile bool s_dirty = false;

void settings_init(void)
{
  if (s_mutex == NULL) {
    s_mutex = xSemaphoreCreateMutex();
    configASSERT(s_mutex != NULL);
  }
}

static void lock(void)
{
  if (s_mutex != NULL) {
    xSemaphoreTake(s_mutex, portMAX_DELAY);
  }
}

static void unlock(void)
{
  if (s_mutex != NULL) {
    xSemaphoreGive(s_mutex);
  }
}

/** Read the NVS blob into `out`. Returns true if the data is valid. */
static bool read_from_nvs(settings_t *out)
{
  size_t length = sizeof(settings_t);
  esp_err_t ret = nvs_get_blob(nvs_flsh_btw, NVS_STRUCT_KEY, (void *)out, &length);
  if (ret == ESP_OK && length == sizeof(settings_t)) {
    return true;
  }
  if (ret == ESP_OK) {
    ESP_LOGE(LOG_SET, "Incorrect settings length (%u != %u)", (unsigned)length,
             (unsigned)sizeof(settings_t));
  } else if (ret != ESP_ERR_NVS_NOT_FOUND) {
    ESP_LOGE(LOG_SET, "Error reading settings: %s", esp_err_to_name(ret));
  }
  memset(out, 0, sizeof(settings_t));
  return false;
}

/** Write the struct to NVS. Compares against the stored value to skip
 *  redundant writes (reduces flash wear). */
static void write_to_nvs(const settings_t *in)
{
  settings_t current;
  if (read_from_nvs(&current) && memcmp(&current, in, sizeof(settings_t)) == 0) {
    ESP_LOGD(LOG_SET, "Settings unchanged, skipping write");
    return;
  }

  esp_err_t err = nvs_set_blob(nvs_flsh_btw, NVS_STRUCT_KEY, (const void *)in, sizeof(settings_t));
  if (err == ESP_OK) {
    err = nvs_commit(nvs_flsh_btw);
  }

  if (err == ESP_OK) {
    ESP_LOGI(LOG_SET, "Settings written");
  } else {
    ESP_LOGE(LOG_SET, "Unable to write settings: %s", esp_err_to_name(err));
  }
}

void settings_load(void)
{
  settings_init();

  lock();
  settings_t loaded;
  read_from_nvs(&loaded);
  settings = loaded;
  settings_ok = true;
  unlock();

  ESP_LOGI(LOG_SET, "Settings loaded (role=%d)", (int)settings.role);
}

void settings_save(void)
{
  settings_init();

  // Snapshot under the mutex: write_to_nvs performs (blocking) NVS operations
  // outside the critical section.
  settings_t snapshot;
  lock();
  snapshot = settings;
  unlock();

  write_to_nvs(&snapshot);
}

void settings_request_save(void)
{
  s_dirty = true;
}

void settings_process(void)
{
  if (s_dirty) {
    s_dirty = false;
    settings_save();
  }
}
