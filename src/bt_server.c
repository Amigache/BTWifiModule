/**
 * @file bt_server.c
 * @brief Implementation of bt_server.h (GATT peripheral, Bluedroid/NimBLE).
 *
 * FrSky-compatible GATT exposure:
 *   - Service      : 0xFFF0
 *   - Characteristic: 0xFFF6 (READ | WRITE | NOTIFY)
 *
 * Telemetry / trainer data is sent over the characteristic as notifications;
 * the central subscribes by writing the CCCD.
 */

#include "bt_server.h"

#include <stdio.h>
#include <string.h>

#include "bt.h"
#include "bt_addr.h"
#include "board.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#if !defined(USE_NIMBLE)
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatts_api.h"
#endif

#define GATTS_TAG "BTSERVER"

#define BT_SVC_UUID 0xFFF0
#define BT_CHR_UUID 0xFFF6

volatile bool bt_server_connected = false;

/* ========================================================================== */
/* NimBLE                                                                     */
/* ========================================================================== */
#if defined(USE_NIMBLE)

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "os/os_mbuf.h"

static const ble_uuid16_t bt_svc_uuid = BLE_UUID16_INIT(BT_SVC_UUID);
static const ble_uuid16_t bt_chr_uuid = BLE_UUID16_INIT(BT_CHR_UUID);

static uint16_t bt_chr_val_handle = 0;
static uint16_t bt_conn_handle = 0xFFFF;
static uint8_t bt_own_addr_type = 0;
static uint8_t bt_chr_value[3] = {0x11, 0x22, 0x33};

static uint8_t raw_adv_data[31];
static uint8_t raw_adv_data_len = 0;

/** Build the advertising data (flags + UUID + name + TX power). */
static void buildAdvData(void)
{
  const char *name = settings.name[0] ? settings.name : "BTWifiMod";
  uint8_t nameLen = (uint8_t)strnlen(name, LEN_BLUETOOTH_NAME);
  uint8_t idx = 0;

  // Flags: LE General Discoverable | BR/EDR Not Supported
  raw_adv_data[idx++] = 0x02;
  raw_adv_data[idx++] = 0x01;
  raw_adv_data[idx++] = 0x06;

  // Complete list of 16-bit Service UUIDs (0xFFF0, little endian)
  raw_adv_data[idx++] = 0x03;
  raw_adv_data[idx++] = 0x02;
  raw_adv_data[idx++] = 0xF0;
  raw_adv_data[idx++] = 0xFF;

  // Complete Local Name
  if (nameLen > 0) {
    raw_adv_data[idx++] = (uint8_t)(nameLen + 1);
    raw_adv_data[idx++] = 0x09;
    memcpy(&raw_adv_data[idx], name, nameLen);
    idx += nameLen;
  }

  // TX Power Level
  raw_adv_data[idx++] = 0x02;
  raw_adv_data[idx++] = 0x0A;
  raw_adv_data[idx++] = 0x00;

  raw_adv_data_len = idx;
  ESP_LOGI(GATTS_TAG, "Advertising as [%s]", name);
}

static int bt_gap_event(struct ble_gap_event *event, void *arg);

static void bt_advertise(void)
{
  struct ble_gap_adv_params adv_params = {0};
  adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
  adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

  int rc = ble_gap_adv_start(bt_own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                             bt_gap_event, NULL);
  if (rc != 0) {
    ESP_LOGE(GATTS_TAG, "advertise start failed rc=%d", rc);
  }
}

static int bt_chr_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
  (void)conn_handle;
  (void)attr_handle;
  (void)arg;

  switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_READ_CHR:
      return os_mbuf_append(ctxt->om, bt_chr_value, sizeof(bt_chr_value)) == 0
                 ? 0
                 : BLE_ATT_ERR_INSUFFICIENT_RES;
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
      // Incoming writes are not forwarded to the radio (same as Bluedroid).
      return 0;
    default:
      return BLE_ATT_ERR_UNLIKELY;
  }
}

static const struct ble_gatt_chr_def bt_chrs[] = {
    {
        .uuid = (ble_uuid_t *)&bt_chr_uuid,
        .access_cb = bt_chr_access,
        .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &bt_chr_val_handle,
    },
    {0},
};

static const struct ble_gatt_svc_def bt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = (ble_uuid_t *)&bt_svc_uuid,
        .characteristics = bt_chrs,
    },
    {0},
};

static int bt_gap_event(struct ble_gap_event *event, void *arg)
{
  (void)arg;

  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        bt_server_connected = true;
        bt_conn_handle = event->connect.conn_handle;
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0) {
          bt_addr_reverse(desc.peer_ota_addr.val, rmtbtaddress);
        }
        ESP_LOGI(GATTS_TAG, "connect handle=%d", bt_conn_handle);
      } else {
        bt_advertise();
      }
      return 0;

    case BLE_GAP_EVENT_DISCONNECT:
      bt_server_connected = false;
      bt_conn_handle = 0xFFFF;
      ESP_LOGI(GATTS_TAG, "disconnect reason=%d", event->disconnect.reason);
      bt_advertise();
      return 0;

    case BLE_GAP_EVENT_SUBSCRIBE:
      ESP_LOGI(GATTS_TAG, "subscribe handle=%d notify=%d", event->subscribe.attr_handle,
               event->subscribe.cur_notify);
      return 0;

    case BLE_GAP_EVENT_MTU:
      ESP_LOGI(GATTS_TAG, "mtu=%d", event->mtu.value);
      return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
      bt_advertise();
      return 0;

    default:
      return 0;
  }
}

static void bt_on_sync(void)
{
  int rc = ble_hs_util_ensure_addr(0);
  if (rc != 0) {
    ESP_LOGE(GATTS_TAG, "ensure_addr rc=%d", rc);
    return;
  }

  rc = ble_hs_id_infer_auto(0, &bt_own_addr_type);
  if (rc != 0) {
    ESP_LOGE(GATTS_TAG, "infer_auto rc=%d", rc);
    return;
  }

  esp_bd_addr_t own;
  ble_hs_id_copy_addr(bt_own_addr_type, own, NULL);
  bt_addr_reverse(own, localbtaddress);

  buildAdvData();
  ble_gap_adv_set_data(raw_adv_data, raw_adv_data_len);
  bt_advertise();
}

static void bt_gatts_register_cb(struct ble_gatt_register_ctxt *ctxt, void *arg)
{
  (void)ctxt;
  (void)arg;
  // no-op (kept for completeness)
}

int bt_server_send_data(const uint8_t *data, int len)
{
  if (!bt_server_connected || bt_conn_handle == 0xFFFF) {
    return -1;
  }

  struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
  if (!om) {
    return -1;
  }

  int rc = ble_gatts_notify_custom(bt_conn_handle, bt_chr_val_handle, om);
  return (rc == 0) ? 0 : -1;
}

void bt_server_init(void)
{
  ESP_LOGI(GATTS_TAG, "Starting Peripheral (NimBLE)");

  ble_hs_cfg.sync_cb = bt_on_sync;
  ble_hs_cfg.gatts_register_cb = bt_gatts_register_cb;

  ble_gatts_count_cfg(bt_svcs);
  ble_gatts_add_svcs(bt_svcs);

  nimble_port_freertos_init(bt_host_task);
}

void bt_server_refresh_advertising(void)
{
  buildAdvData();
  ble_gap_adv_set_data(raw_adv_data, raw_adv_data_len);
}

/* ========================================================================== */
/* Bluedroid                                                                  */
/* ========================================================================== */
#else  // !USE_NIMBLE

#define GATTS_SERVICE_FRSKY_UUID 0xFFF0
#define GATTS_CHAR_FRSKY_UUID 0xFFF6
#define GATTS_NUM_HANDLE_TEST_A 8

#define ADV_DATA_MAX_LEN 31

static void gatts_profile_a_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                          esp_ble_gatts_cb_param_t *param);

static uint8_t char1_str[] = {0x11, 0x22, 0x33};
static esp_gatt_char_prop_t a_property = 0;

static esp_attr_value_t gatts_demo_char1_val = {
    .attr_max_len = sizeof(char1_str),
    .attr_len = sizeof(char1_str),
    .attr_value = char1_str,
};

static uint8_t adv_config_done = 0;
#define adv_config_flag (1 << 0)
#define scan_rsp_config_flag (1 << 1)

static uint8_t raw_adv_data[ADV_DATA_MAX_LEN];
static uint8_t raw_adv_data_len = 0;

/** Build the advertising data (flags + UUID + name + TX power). */
static void buildAdvData(void)
{
  const char *name = settings.name[0] ? settings.name : "BTWifiMod";
  uint8_t nameLen = (uint8_t)strnlen(name, LEN_BLUETOOTH_NAME);
  uint8_t idx = 0;

  raw_adv_data[idx++] = 0x02;
  raw_adv_data[idx++] = 0x01;
  raw_adv_data[idx++] = 0x06;

  raw_adv_data[idx++] = 0x03;
  raw_adv_data[idx++] = 0x02;
  raw_adv_data[idx++] = 0xF0;
  raw_adv_data[idx++] = 0xFF;

  if (nameLen > 0) {
    raw_adv_data[idx++] = (uint8_t)(nameLen + 1);
    raw_adv_data[idx++] = 0x09;
    memcpy(&raw_adv_data[idx], name, nameLen);
    idx += nameLen;
  }

  raw_adv_data[idx++] = 0x02;
  raw_adv_data[idx++] = 0x0A;
  raw_adv_data[idx++] = 0x00;

  raw_adv_data_len = idx;
  ESP_LOGI(GATTS_TAG, "Advertising as [%s]", name);
}

static esp_ble_adv_params_t adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

#define PROFILE_NUM 1
#define PROFILE_TRAINER_SL_ID 0

struct gatts_profile_inst {
  esp_gatts_cb_t gatts_cb;
  uint16_t gatts_if;
  uint16_t app_id;
  uint16_t conn_id;
  uint16_t service_handle;
  esp_gatt_srvc_id_t service_id;
  uint16_t char_handle;
  esp_bt_uuid_t char_uuid;
  esp_gatt_perm_t perm;
  esp_gatt_char_prop_t property;
  uint16_t descr_handle;
  esp_bt_uuid_t descr_uuid;
};

static struct gatts_profile_inst gl_profile_tab[PROFILE_NUM] = {
    [PROFILE_TRAINER_SL_ID] = {
        .gatts_cb = gatts_profile_a_event_handler,
        .gatts_if = ESP_GATT_IF_NONE,
    }};

static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
  switch (event) {
    case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
      adv_config_done &= (~adv_config_flag);
      if (adv_config_done == 0) {
        esp_ble_gap_start_advertising(&adv_params);
      }
      break;
    case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
      adv_config_done &= (~scan_rsp_config_flag);
      if (adv_config_done == 0) {
        esp_ble_gap_start_advertising(&adv_params);
      }
      break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
      if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
        ESP_LOGE(GATTS_TAG, "Advertising start failed");
      }
      break;
    case ESP_GAP_BLE_ADV_STOP_COMPLETE_EVT:
      if (param->adv_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
        ESP_LOGE(GATTS_TAG, "Advertising stop failed");
      }
      break;
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
      ESP_LOGI(GATTS_TAG,
               "update conn params status=%d min=%d max=%d conn=%d latency=%d timeout=%d",
               param->update_conn_params.status, param->update_conn_params.min_int,
               param->update_conn_params.max_int, param->update_conn_params.conn_int,
               param->update_conn_params.latency, param->update_conn_params.timeout);
      break;
    default:
      break;
  }
}

static void gatts_profile_a_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                          esp_ble_gatts_cb_param_t *param)
{
  switch (event) {
    case ESP_GATTS_REG_EVT:
      ESP_LOGI(GATTS_TAG, "REGISTER_APP_EVT status=%d app_id=%d", param->reg.status,
               param->reg.app_id);
      gl_profile_tab[PROFILE_TRAINER_SL_ID].service_id.is_primary = true;
      gl_profile_tab[PROFILE_TRAINER_SL_ID].service_id.id.inst_id = 0x00;
      gl_profile_tab[PROFILE_TRAINER_SL_ID].service_id.id.uuid.len = ESP_UUID_LEN_16;
      gl_profile_tab[PROFILE_TRAINER_SL_ID].service_id.id.uuid.uuid.uuid16 =
          GATTS_SERVICE_FRSKY_UUID;

      esp_err_t raw_adv_ret = esp_ble_gap_config_adv_data_raw(raw_adv_data, raw_adv_data_len);
      if (raw_adv_ret) {
        ESP_LOGE(GATTS_TAG, "config raw adv data failed, error code = %x", raw_adv_ret);
      }
      adv_config_done |= adv_config_flag;

      esp_ble_gatts_create_service(gatts_if, &gl_profile_tab[PROFILE_TRAINER_SL_ID].service_id,
                                   GATTS_NUM_HANDLE_TEST_A);
      break;

    case ESP_GATTS_READ_EVT: {
      // Respond with the initial characteristic value.
      esp_gatt_rsp_t rsp;
      memset(&rsp, 0, sizeof(rsp));
      rsp.attr_value.handle = param->read.handle;
      rsp.attr_value.len = sizeof(char1_str);
      memcpy(rsp.attr_value.value, char1_str, sizeof(char1_str));
      esp_ble_gatts_send_response(gatts_if, param->read.conn_id, param->read.trans_id, ESP_GATT_OK,
                                  &rsp);
      break;
    }

    case ESP_GATTS_WRITE_EVT: {
      // CCCD write: record the subscription (no demo data is sent).
      if (!param->write.is_prep &&
          gl_profile_tab[PROFILE_TRAINER_SL_ID].descr_handle == param->write.handle &&
          param->write.len == 2) {
        uint16_t descr_value = (uint16_t)(param->write.value[1] << 8 | param->write.value[0]);
        if (descr_value == 0x0001) {
          ESP_LOGI(GATTS_TAG, "notify enable");
        } else if (descr_value == 0x0002) {
          ESP_LOGI(GATTS_TAG, "indicate enable");
        } else if (descr_value == 0x0000) {
          ESP_LOGI(GATTS_TAG, "notify/indicate disable");
        } else {
          ESP_LOGE(GATTS_TAG, "unknown descr value");
          ESP_LOG_BUFFER_HEX(GATTS_TAG, param->write.value, param->write.len);
        }
      }
      // Acknowledge the write.
      esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id,
                                  ESP_GATT_OK, NULL);
      break;
    }

    case ESP_GATTS_EXEC_WRITE_EVT:
      esp_ble_gatts_send_response(gatts_if, param->write.conn_id, param->write.trans_id,
                                  ESP_GATT_OK, NULL);
      break;

    case ESP_GATTS_MTU_EVT:
      ESP_LOGI(GATTS_TAG, "MTU_EVT, MTU %d", param->mtu.mtu);
      break;

    case ESP_GATTS_CREATE_EVT:
      ESP_LOGI(GATTS_TAG, "CREATE_SERVICE_EVT status=%d service_handle=%d", param->create.status,
               param->create.service_handle);
      gl_profile_tab[PROFILE_TRAINER_SL_ID].service_handle = param->create.service_handle;
      gl_profile_tab[PROFILE_TRAINER_SL_ID].char_uuid.len = ESP_UUID_LEN_16;
      gl_profile_tab[PROFILE_TRAINER_SL_ID].char_uuid.uuid.uuid16 = GATTS_CHAR_FRSKY_UUID;

      esp_ble_gatts_start_service(gl_profile_tab[PROFILE_TRAINER_SL_ID].service_handle);
      a_property = ESP_GATT_CHAR_PROP_BIT_READ | ESP_GATT_CHAR_PROP_BIT_WRITE |
                   ESP_GATT_CHAR_PROP_BIT_NOTIFY;
      esp_err_t add_char_ret = esp_ble_gatts_add_char(
          gl_profile_tab[PROFILE_TRAINER_SL_ID].service_handle,
          &gl_profile_tab[PROFILE_TRAINER_SL_ID].char_uuid,
          ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, a_property, &gatts_demo_char1_val, NULL);
      if (add_char_ret) {
        ESP_LOGE(GATTS_TAG, "add char failed, error code =%x", add_char_ret);
      }
      break;

    case ESP_GATTS_ADD_CHAR_EVT:
      ESP_LOGI(GATTS_TAG, "ADD_CHAR_EVT status=%d attr_handle=%d", param->add_char.status,
               param->add_char.attr_handle);
      gl_profile_tab[PROFILE_TRAINER_SL_ID].char_handle = param->add_char.attr_handle;
      gl_profile_tab[PROFILE_TRAINER_SL_ID].descr_uuid.len = ESP_UUID_LEN_16;
      gl_profile_tab[PROFILE_TRAINER_SL_ID].descr_uuid.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
      esp_err_t add_descr_ret = esp_ble_gatts_add_char_descr(
          gl_profile_tab[PROFILE_TRAINER_SL_ID].service_handle,
          &gl_profile_tab[PROFILE_TRAINER_SL_ID].descr_uuid,
          ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE, NULL, NULL);
      if (add_descr_ret) {
        ESP_LOGE(GATTS_TAG, "add char descr failed, error code =%x", add_descr_ret);
      }
      break;

    case ESP_GATTS_ADD_CHAR_DESCR_EVT:
      gl_profile_tab[PROFILE_TRAINER_SL_ID].descr_handle = param->add_char_descr.attr_handle;
      ESP_LOGI(GATTS_TAG, "ADD_DESCR_EVT status=%d attr_handle=%d", param->add_char_descr.status,
               param->add_char_descr.attr_handle);
      break;

    case ESP_GATTS_CONNECT_EVT: {
      esp_ble_conn_update_params_t conn_params = {0};
      memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
      conn_params.latency = 0;
      conn_params.max_int = BT_CON_INT_MIN;
      conn_params.min_int = BT_CON_INT_MAX;
      conn_params.timeout = BT_CON_TIMEOUT;
      ESP_LOGI(GATTS_TAG, "CONNECT_EVT conn_id=%d remote=%02x:%02x:%02x:%02x:%02x:%02x",
               param->connect.conn_id, param->connect.remote_bda[0], param->connect.remote_bda[1],
               param->connect.remote_bda[2], param->connect.remote_bda[3],
               param->connect.remote_bda[4], param->connect.remote_bda[5]);
      gl_profile_tab[PROFILE_TRAINER_SL_ID].conn_id = param->connect.conn_id;
      esp_ble_gap_update_conn_params(&conn_params);
      memcpy(rmtbtaddress, param->connect.remote_bda, sizeof(esp_bd_addr_t));
      bt_server_connected = true;
      break;
    }

    case ESP_GATTS_DISCONNECT_EVT:
      ESP_LOGI(GATTS_TAG, "DISCONNECT_EVT reason=0x%x", param->disconnect.reason);
      esp_ble_gap_start_advertising(&adv_params);
      bt_server_connected = false;
      break;

    default:
      break;
  }
}

static void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param)
{
  if (event == ESP_GATTS_REG_EVT) {
    if (param->reg.status == ESP_GATT_OK) {
      gl_profile_tab[param->reg.app_id].gatts_if = gatts_if;
    } else {
      ESP_LOGI(GATTS_TAG, "Reg app failed, app_id %04x, status %d", param->reg.app_id,
               param->reg.status);
      return;
    }
  }

  for (int idx = 0; idx < PROFILE_NUM; idx++) {
    if (gatts_if == ESP_GATT_IF_NONE || gatts_if == gl_profile_tab[idx].gatts_if) {
      if (gl_profile_tab[idx].gatts_cb) {
        gl_profile_tab[idx].gatts_cb(event, gatts_if, param);
      }
    }
  }
}

int bt_server_send_data(const uint8_t *data, int len)
{
  if (!bt_server_connected) {
    return -1;
  }
  if (gl_profile_tab[PROFILE_TRAINER_SL_ID].gatts_if == ESP_GATT_IF_NONE) {
    return -1;
  }

  esp_ble_gatts_send_indicate(gl_profile_tab[PROFILE_TRAINER_SL_ID].gatts_if,
                              gl_profile_tab[PROFILE_TRAINER_SL_ID].conn_id,
                              gl_profile_tab[PROFILE_TRAINER_SL_ID].char_handle, len, (uint8_t *)data,
                              false);
  return 0;
}

void bt_server_init(void)
{
  ESP_LOGI(GATTS_TAG, "Starting Peripheral");

  if (settings.name[0] != '\0') {
    ESP_LOGI(GATTS_TAG, "Setting device name [%s]", settings.name);
    esp_err_t ret = esp_ble_gap_set_device_name(settings.name);
    if (ret) {
      ESP_LOGE(GATTS_TAG, "set device name error, error code = %x", ret);
      return;
    }
  }

  buildAdvData();

  esp_err_t ret = esp_ble_gatts_register_callback(gatts_event_handler);
  if (ret) {
    ESP_LOGE(GATTS_TAG, "gatts register error, error code = %x", ret);
    return;
  }
  ret = esp_ble_gap_register_callback(gap_event_handler);
  if (ret) {
    ESP_LOGE(GATTS_TAG, "gap register error, error code = %x", ret);
    return;
  }
  ret = esp_ble_gatts_app_register(PROFILE_TRAINER_SL_ID);
  if (ret) {
    ESP_LOGE(GATTS_TAG, "gatts app register error, error code = %x", ret);
    return;
  }

  esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(85);
  if (local_mtu_ret) {
    ESP_LOGE(GATTS_TAG, "set local MTU failed, error code = %x", local_mtu_ret);
  }

  uint8_t adrtype;
  esp_ble_gap_get_local_used_addr(localbtaddress, &adrtype);
}

void bt_server_refresh_advertising(void)
{
  buildAdvData();
  esp_ble_gap_config_adv_data_raw(raw_adv_data, raw_adv_data_len);
}

#endif  // USE_NIMBLE
