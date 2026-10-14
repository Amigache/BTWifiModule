/**
 * @file bt_client.c
 * @brief Implementation of bt_client.h (GATT central, Bluedroid/NimBLE).
 *
 * Central mode (trainer master): scans, connects to the BLE trainer/headtracker
 * and forwards its notifications to the radio over UART. Includes auto-connect
 * to the last known device.
 */

#include "bt_client.h"

#include <stdio.h>
#include <string.h>

#include "bt.h"
#include "board.h"
#include "settings.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#if !defined(USE_NIMBLE)
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#endif

#define GATTC_TAG "BTCLIENT"

#define REMOTE_SERVICE_UUID 0xFFF0
#define REMOTE_FRSKY_CHAR_UUID 0xFFF6
#define REMOTE_HTRESET_CHAR_UUID 0xAFF2
#define REMOTE_HTCHAN_CHAR_UUID 0xAFF1

#define PROFILE_NUM 1
#define PROFILE_A_APP_ID 0
#define INVALID_HANDLE 0
#define MAX_CHAR_TO_SCAN 15

char *ble_board_type_names[BLE_BOARD_COUNT] = {"Unknown", "CC2540", "PARA",
                                               "HeadTracker", "FlySky", "HM10"};

volatile bool bt_client_connected = false;
volatile bool bt_client_scan_complete = true;
volatile bool bt_client_validslavefound = false;
volatile bool bt_client_ht_reset = false;
volatile bool bt_client_forwardHt = false;
volatile ble_board_type bt_client_board_type = BLE_BOARD_UNKNOWN;

/* --- Scanned-device list (mutex protected) -------------------------------- */

static bt_scanned_dev_t s_scanned[MAX_BLE_ADDRESSES];
static uint8_t s_scanned_count = 0;
static SemaphoreHandle_t s_scanned_mutex = NULL;

static bool s_autoconnect_active = false;
static bt_addr_t s_autoconnect_target;

static void scanned_lock_init(void)
{
  if (s_scanned_mutex == NULL) {
    s_scanned_mutex = xSemaphoreCreateMutex();
    configASSERT(s_scanned_mutex != NULL);
  }
}

static void scanned_lock(void)
{
  if (s_scanned_mutex != NULL) {
    xSemaphoreTake(s_scanned_mutex, portMAX_DELAY);
  }
}

static void scanned_unlock(void)
{
  if (s_scanned_mutex != NULL) {
    xSemaphoreGive(s_scanned_mutex);
  }
}

static void scanned_clear(void)
{
  scanned_lock();
  s_scanned_count = 0;
  scanned_unlock();
}

static void scanned_add(const bt_addr_t addr, uint8_t type)
{
  scanned_lock();
  if (s_scanned_count < MAX_BLE_ADDRESSES) {
    for (uint8_t i = 0; i < s_scanned_count; i++) {
      if (memcmp(s_scanned[i].addr, addr, sizeof(bt_addr_t)) == 0) {
        scanned_unlock();
        return;  // already present
      }
    }
    memcpy(s_scanned[s_scanned_count].addr, addr, sizeof(bt_addr_t));
    s_scanned[s_scanned_count].type = type;
    s_scanned_count++;
  }
  scanned_unlock();
}

uint8_t bt_client_scanned_count(void)
{
  scanned_lock();
  uint8_t count = s_scanned_count;
  scanned_unlock();
  return count;
}

bool bt_client_scanned_get(uint8_t index, bt_addr_t addr_out, uint8_t *type_out)
{
  scanned_lock();
  if (index >= s_scanned_count) {
    scanned_unlock();
    return false;
  }
  memcpy(addr_out, s_scanned[index].addr, sizeof(bt_addr_t));
  if (type_out != NULL) {
    *type_out = s_scanned[index].type;
  }
  scanned_unlock();
  return true;
}

static bool scanned_find_type(const bt_addr_t addr, uint8_t *type_out)
{
  scanned_lock();
  for (uint8_t i = 0; i < s_scanned_count; i++) {
    if (memcmp(s_scanned[i].addr, addr, sizeof(bt_addr_t)) == 0) {
      if (type_out != NULL) {
        *type_out = s_scanned[i].type;
      }
      scanned_unlock();
      return true;
    }
  }
  scanned_unlock();
  return false;
}

/* ========================================================================== */
/* NimBLE                                                                     */
/* ========================================================================== */
#if defined(USE_NIMBLE)

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_hs_mbuf.h"
#include "host/util/util.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "os/os_mbuf.h"

static uint8_t s_own_addr_type = BLE_OWN_ADDR_PUBLIC;
static uint16_t s_conn_handle = 0xFFFF;
static uint16_t s_svc_start = 0, s_svc_end = 0;
static uint16_t s_data_handle = 0;      //< 0xFFF6 trainer characteristic
static uint16_t s_htreset_handle = 0;   //< 0xAFF2 headtracker reset
static uint16_t s_cccd_handle = 0;
static bool s_found_svc = false;
static bool s_readytoscan = false;

/** Return the 16-bit alias of a UUID (native 16-bit, or 128-bit on the base). */
static uint16_t uuid16(const ble_uuid_any_t *uuid)
{
  if (uuid->u.type == BLE_UUID_TYPE_16) {
    return uuid->u16.value;
  }
  if (uuid->u.type == BLE_UUID_TYPE_128) {
    static const uint8_t base[12] = {0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00,
                                     0x00, 0x80, 0x00, 0x10, 0x00, 0x00};
    if (memcmp(uuid->u128.value, base, sizeof(base)) == 0 &&
        uuid->u128.value[14] == 0x00 && uuid->u128.value[15] == 0x00) {
      return (uint16_t)(uuid->u128.value[12] | (uuid->u128.value[13] << 8));
    }
  }
  return 0;
}

static int bt_gap_event(struct ble_gap_event *event, void *arg);
static int on_disc_svc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg);
static int on_disc_chr(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg);
static int on_disc_dsc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg);

static void connect_internal(bt_addr_t addr_display, uint8_t type)
{
  if (bt_client_connected) {
    return;
  }
  memcpy(rmtbtaddress, addr_display, sizeof(bt_addr_t));

  ble_addr_t peer;
  bt_addr_reverse(addr_display, peer.val);
  peer.type = type;

  struct ble_gap_conn_params cp = {0};
  cp.scan_itvl = 0x0010;
  cp.scan_window = 0x0010;
  cp.itvl_min = BT_CON_INT_MIN;
  cp.itvl_max = BT_CON_INT_MAX;
  cp.latency = 0;
  cp.supervision_timeout = BT_CON_TIMEOUT;

  char saddr[BT_ADDR_STR_LEN];
  printf("Connecting to %s\r\n", bt_addr_to_str(saddr, addr_display));
  int rc = ble_gap_connect(s_own_addr_type, &peer, 30000, &cp, bt_gap_event, NULL);
  if (rc != 0) {
    printf("Unable to connect to %s (rc=%d)\r\n", bt_addr_to_str(saddr, addr_display), rc);
  }
}

static int on_disc_dsc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc, void *arg)
{
  (void)chr_val_handle;
  (void)arg;

  if (error->status == 0 && dsc != NULL) {
    if (uuid16(&dsc->uuid) == BLE_GATT_DSC_CLT_CFG_UUID16) {
      s_cccd_handle = dsc->handle;
    }
  } else if (error->status == BLE_HS_EDONE) {
    if (s_cccd_handle) {
      uint8_t val[2] = {0x01, 0x00};
      ble_gattc_write_flat(conn_handle, s_cccd_handle, val, sizeof(val), NULL, NULL);
    }
    bt_client_scan_complete = true;
  }
  return 0;
}

static int on_disc_chr(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_chr *chr, void *arg)
{
  (void)arg;

  if (error->status == 0 && chr != NULL) {
    uint16_t u = uuid16(&chr->uuid);
    if (u != 0) {
      if (u == REMOTE_FRSKY_CHAR_UUID) {
        bt_client_validslavefound = true;
        s_data_handle = chr->val_handle;
        ESP_LOGI(GATTC_TAG, "Found the Trainer Characteristic");
      } else if (u == REMOTE_HTRESET_CHAR_UUID) {
        bt_client_board_type = BLE_BOARD_HEADTRACKER;
        s_htreset_handle = chr->val_handle;
      } else if (u == REMOTE_HTCHAN_CHAR_UUID) {
        bt_client_board_type = BLE_BOARD_HEADTRACKER;
      }
    }
  } else if (error->status == BLE_HS_EDONE) {
    if (bt_client_validslavefound) {
      // Save the remote address for auto-connect.
      struct ble_gap_conn_desc desc;
      if (ble_gap_conn_find(conn_handle, &desc) == 0) {
        bt_addr_reverse(desc.peer_ota_addr.val, rmtbtaddress);
        bt_addr_to_str(settings.rmtbtaddr, rmtbtaddress);
        settings_request_save();
      }
      // Subscribe to notifications on the trainer characteristic.
      ble_gattc_disc_all_dscs(conn_handle, s_data_handle, s_svc_end, on_disc_dsc, NULL);
    } else {
      ble_gap_terminate(conn_handle, 0x13 /* remote user terminated */);
    }
  }
  return 0;
}

static int on_disc_svc(uint16_t conn_handle, const struct ble_gatt_error *error,
                       const struct ble_gatt_svc *service, void *arg)
{
  (void)arg;

  if (error->status == 0 && service != NULL) {
    if (uuid16(&service->uuid) == REMOTE_SERVICE_UUID) {
      s_svc_start = service->start_handle;
      s_svc_end = service->end_handle;
      s_found_svc = true;
      ESP_LOGI(GATTC_TAG, "service found 0x%04X", REMOTE_SERVICE_UUID);
    }
  } else if (error->status == BLE_HS_EDONE) {
    if (s_found_svc) {
      ble_gattc_disc_all_chrs(conn_handle, s_svc_start, s_svc_end, on_disc_chr, NULL);
    } else {
      ble_gap_terminate(conn_handle, 0x13 /* remote user terminated */);
    }
  }
  return 0;
}

static int bt_gap_event(struct ble_gap_event *event, void *arg)
{
  (void)arg;

  switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
      // NimBLE reports in wire order; convert to display order.
      bt_addr_t peer;
      bt_addr_reverse(event->disc.addr.val, peer);

      if (event->disc.rssi > MIN_BLE_RSSI) {
        scanned_add(peer, event->disc.addr.type);
      }

      char addr[BT_ADDR_STR_LEN];
      printf("Disc BT Address %s, RSSI=%d, Addr Type=%d\n", bt_addr_to_str(addr, peer),
             event->disc.rssi, event->disc.addr.type);

      if (s_autoconnect_active && !bt_client_connected &&
          memcmp(s_autoconnect_target, peer, sizeof(bt_addr_t)) == 0) {
        printf("Auto-connect: saved device found\r\n");
        s_autoconnect_active = false;
        ble_gap_disc_cancel();
        connect_internal(peer, event->disc.addr.type);
      }
      return 0;
    }

    case BLE_GAP_EVENT_DISC_COMPLETE:
      bt_client_scan_complete = true;
      return 0;

    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status == 0) {
        bt_client_connected = true;
        s_conn_handle = event->connect.conn_handle;
        bt_client_scan_complete = false;
        bt_client_validslavefound = false;
        s_found_svc = false;
        bt_client_board_type = BLE_BOARD_UNKNOWN;
        ESP_LOGI(GATTC_TAG, "connected handle=%d", s_conn_handle);
        ble_gattc_disc_all_svcs(s_conn_handle, on_disc_svc, NULL);
      } else {
        bt_client_connected = false;
        bt_client_scan_complete = true;
      }
      return 0;

    case BLE_GAP_EVENT_DISCONNECT:
      bt_client_connected = false;
      s_conn_handle = 0xFFFF;
      bt_client_scan_complete = false;
      ESP_LOGI(GATTC_TAG, "disconnect reason=%d", event->disconnect.reason);
      return 0;

    case BLE_GAP_EVENT_NOTIFY_RX: {
      uint8_t buf[256];
      uint16_t len = 0;
      if (bt_client_forwardHt &&
          ble_hs_mbuf_to_flat(event->notify_rx.om, buf, sizeof(buf), &len) == 0 && len > 0) {
        uart_write_bytes(uart_num, buf, len);
      }
      return 0;
    }

    case BLE_GAP_EVENT_MTU:
      ESP_LOGI(GATTC_TAG, "mtu=%d", event->mtu.value);
      return 0;

    default:
      return 0;
  }
}

static void bt_on_sync(void)
{
  int rc = ble_hs_util_ensure_addr(0);
  if (rc != 0) {
    ESP_LOGE(GATTC_TAG, "ensure_addr rc=%d", rc);
    return;
  }
  rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
  if (rc != 0) {
    ESP_LOGE(GATTC_TAG, "infer_auto rc=%d", rc);
    return;
  }
  esp_bd_addr_t own;
  ble_hs_id_copy_addr(s_own_addr_type, own, NULL);
  bt_addr_reverse(own, localbtaddress);
  s_readytoscan = true;
}

void bt_client_init(void)
{
  scanned_lock_init();
  ESP_LOGI(GATTC_TAG, "Starting Central (NimBLE)");
  ble_hs_cfg.sync_cb = bt_on_sync;
  ble_att_set_preferred_mtu(85);
  nimble_port_freertos_init(bt_host_task);
}

void bt_client_start_scan(void)
{
  if (!s_readytoscan) {
    return;
  }

  struct ble_gap_disc_params params = {0};
  params.passive = 0;
  params.filter_duplicates = 1;

  s_autoconnect_active = false;
  bt_client_scan_complete = false;
  scanned_clear();
  printf("Clearing Addresses\r\n");
  // Cancel any in-progress discovery first (e.g. the endless auto-connect
  // scan): otherwise ble_gap_disc fails with EALREADY, the timed scan never
  // runs and the state machine never reports "OK+DISCE".
  ble_gap_disc_cancel();
  ble_gap_disc(s_own_addr_type, 1000, &params, bt_gap_event, NULL);
}

void bt_client_scan_stop(void)
{
  ble_gap_disc_cancel();
}

bool bt_client_has_saved_address(void)
{
  return settings.rmtbtaddr[0] != '\0' && strcmp(settings.rmtbtaddr, "000000000000") != 0;
}

bool bt_client_start_autoconnect(void)
{
  if (!s_readytoscan || bt_client_connected) {
    return false;
  }

  struct ble_gap_disc_params params = {0};
  params.passive = 0;
  params.filter_duplicates = 1;

  bt_addr_from_str(s_autoconnect_target, settings.rmtbtaddr);
  s_autoconnect_active = true;
  bt_client_scan_complete = false;
  bt_client_validslavefound = false;
  scanned_clear();
  printf("Auto-connect: scanning for saved device\r\n");
  ble_gap_disc(s_own_addr_type, BLE_HS_FOREVER, &params, bt_gap_event, NULL);
  return true;
}

void bt_client_connect(bt_addr_t addr)
{
  if (bt_client_connected) {
    return;
  }
  s_autoconnect_active = false;
  bt_client_scan_complete = false;
  bt_client_validslavefound = false;

  uint8_t type = 0;
  if (!scanned_find_type(addr, &type)) {
    char saddr[BT_ADDR_STR_LEN];
    printf("Unable to connect to %s, address not found in storage\r\n",
           bt_addr_to_str(saddr, addr));
    return;
  }

  connect_internal(addr, type);
}

void bt_client_disconnect(void)
{
  if (bt_client_connected) {
    ble_gap_terminate(s_conn_handle, 0x13 /* remote user terminated */);
  }
  bt_client_connected = false;
  bt_client_scan_complete = false;
  bt_client_validslavefound = false;
}

void bt_client_dohtreset(void)
{
  if (bt_client_connected && s_htreset_handle) {
    ble_gattc_write_flat(s_conn_handle, s_htreset_handle, (uint8_t *)"R", 1, NULL, NULL);
  }
}

/* ========================================================================== */
/* Bluedroid                                                                  */
/* ========================================================================== */
#else  // !USE_NIMBLE

static bool get_server = false;
static esp_gattc_char_elem_t *char_elem_result = NULL;
static esp_gattc_descr_elem_t *descr_elem_result = NULL;

static bool s_readytoscan = false;
static uint16_t s_datahandle;
static uint16_t s_htresethandle;

static esp_bt_uuid_t remote_filter_service_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = REMOTE_SERVICE_UUID},
};

static esp_bt_uuid_t notify_descr_uuid = {
    .len = ESP_UUID_LEN_16,
    .uuid = {.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG},
};

static esp_ble_scan_params_t ble_scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_ENABLE};

struct gattc_profile_inst {
  esp_gattc_cb_t gattc_cb;
  uint16_t gattc_if;
  uint16_t app_id;
  uint16_t conn_id;
  uint16_t service_start_handle;
  uint16_t service_end_handle;
  uint16_t char_handle;
  esp_bd_addr_t remote_bda;
};

/* Forward declaration: referenced from gl_profile_tab before being defined. */
static void gattc_profile_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param);

static struct gattc_profile_inst gl_profile_tab[PROFILE_NUM] = {
    [PROFILE_A_APP_ID] = {.gattc_cb = gattc_profile_event_handler,
                          .gattc_if = ESP_GATT_IF_NONE},
};

static void gattc_update_connection_params(esp_bd_addr_t *remote_bda)
{
  esp_ble_conn_update_params_t conn_params;
  memcpy(conn_params.bda, remote_bda, sizeof(esp_bd_addr_t));
  conn_params.min_int = BT_CON_INT_MIN;
  conn_params.max_int = BT_CON_INT_MAX;
  conn_params.latency = 0x00;
  conn_params.timeout = BT_CON_TIMEOUT;
  esp_ble_gap_update_conn_params(&conn_params);
}

static void connect_internal(bt_addr_t addr, uint8_t type)
{
  if (bt_client_connected) {
    return;
  }
  memcpy(rmtbtaddress, addr, sizeof(bt_addr_t));
  char saddr[BT_ADDR_STR_LEN];
  printf("Connecting to %s\r\n", bt_addr_to_str(saddr, addr));
  esp_ble_gattc_open(gl_profile_tab[PROFILE_A_APP_ID].gattc_if, addr, type, true);
}

static void gattc_profile_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param)
{
  esp_ble_gattc_cb_param_t *p_data = (esp_ble_gattc_cb_param_t *)param;

  switch (event) {
    case ESP_GATTC_REG_EVT: {
      ESP_LOGI(GATTC_TAG, "REG_EVT");
      esp_err_t scan_ret = esp_ble_gap_set_scan_params(&ble_scan_params);
      if (scan_ret) {
        ESP_LOGE(GATTC_TAG, "set scan params error, error code = %x", scan_ret);
      }
      break;
    }

    case ESP_GATTC_CONNECT_EVT: {
      bt_client_connected = true;
      esp_ble_conn_update_params_t conn_params = {0};
      memcpy(conn_params.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
      ESP_LOGI(GATTC_TAG, "CONNECT_EVT conn_id %d", p_data->connect.conn_id);
      gl_profile_tab[PROFILE_A_APP_ID].conn_id = p_data->connect.conn_id;
      memcpy(gl_profile_tab[PROFILE_A_APP_ID].remote_bda, p_data->connect.remote_bda,
             sizeof(esp_bd_addr_t));

      esp_err_t mtu_ret = esp_ble_gattc_send_mtu_req(gattc_if, p_data->connect.conn_id);
      if (mtu_ret) {
        ESP_LOGE(GATTC_TAG, "config MTU error, error code = %x", mtu_ret);
      }
#if defined(BLUETOOTH5)
      esp_ble_gap_set_preferred_phy(param->connect.remote_bda,
                                    ESP_BLE_GAP_NO_PREFER_TRANSMIT_PHY |
                                        ESP_BLE_GAP_NO_PREFER_RECEIVE_PHY,
                                    ESP_BLE_GAP_PHY_CODED_PREF_MASK, ESP_BLE_GAP_PHY_CODED_PREF_MASK,
                                    ESP_BLE_GAP_PHY_OPTIONS_PREF_S8_CODING);
#endif
      bt_client_scan_complete = false;
      ESP_LOGI(GATTC_TAG, "Starting Service Scan");
      esp_ble_gattc_search_service(gattc_if, param->cfg_mtu.conn_id, &remote_filter_service_uuid);
      gattc_update_connection_params(&param->connect.remote_bda);
      break;
    }

    case ESP_GATTC_DIS_SRVC_CMPL_EVT:
      if (param->dis_srvc_cmpl.status != ESP_GATT_OK) {
        ESP_LOGE(GATTC_TAG, "discover service failed, status %d", param->dis_srvc_cmpl.status);
        break;
      }
      esp_ble_gattc_search_service(gattc_if, param->cfg_mtu.conn_id, &remote_filter_service_uuid);
      break;

    case ESP_GATTC_SEARCH_RES_EVT:
      if (p_data->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
          p_data->search_res.srvc_id.uuid.uuid.uuid16 == REMOTE_SERVICE_UUID) {
        ESP_LOGI(GATTC_TAG, "service found");
        get_server = true;
        gl_profile_tab[PROFILE_A_APP_ID].service_start_handle = p_data->search_res.start_handle;
        gl_profile_tab[PROFILE_A_APP_ID].service_end_handle = p_data->search_res.end_handle;
      }
      break;

    case ESP_GATTC_SEARCH_CMPL_EVT:
      if (p_data->search_cmpl.status != ESP_GATT_OK) {
        ESP_LOGE(GATTC_TAG, "search service failed, error status = %x", p_data->search_cmpl.status);
        break;
      }
      if (get_server) {
        uint16_t count = 0;
        esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
            gattc_if, p_data->search_cmpl.conn_id, ESP_GATT_DB_CHARACTERISTIC,
            gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
            gl_profile_tab[PROFILE_A_APP_ID].service_end_handle, INVALID_HANDLE, &count);
        if (status != ESP_GATT_OK) {
          ESP_LOGE(GATTC_TAG, "esp_ble_gattc_get_attr_count error");
        }

        if (count > 0) {
          char_elem_result =
              (esp_gattc_char_elem_t *)malloc(sizeof(esp_gattc_char_elem_t) * MAX_CHAR_TO_SCAN);
          if (!char_elem_result) {
            ESP_LOGE(GATTC_TAG, "gattc no mem");
          } else {
            count = MAX_CHAR_TO_SCAN;
            status = esp_ble_gattc_get_all_char(
                gattc_if, p_data->search_cmpl.conn_id,
                gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
                gl_profile_tab[PROFILE_A_APP_ID].service_end_handle, char_elem_result, &count, 0);
            if (status != ESP_GATT_OK) {
              ESP_LOGE(GATTC_TAG, "esp_ble_gattc_get_all_char error");
            }

            bt_client_validslavefound = false;
            bt_client_board_type = BLE_BOARD_UNKNOWN;
            for (int i = 0; i < count; i++) {
              if (char_elem_result[i].uuid.uuid.uuid16 == 0xFFF6) {
                bt_client_validslavefound = true;
                s_datahandle = char_elem_result[i].char_handle;
                ESP_LOGI(GATTC_TAG, "Found the Trainer Characteristic");
                if (char_elem_result[i].properties & ESP_GATT_CHAR_PROP_BIT_NOTIFY) {
                  gl_profile_tab[PROFILE_A_APP_ID].char_handle = char_elem_result[i].char_handle;
                  esp_ble_gattc_register_for_notify(gattc_if,
                                                    gl_profile_tab[PROFILE_A_APP_ID].remote_bda,
                                                    char_elem_result[i].char_handle);
                }
                // Save the address for auto-connect.
                bt_addr_to_str(settings.rmtbtaddr, gl_profile_tab[PROFILE_A_APP_ID].remote_bda);
                settings_request_save();
              } else if (char_elem_result[i].uuid.uuid.uuid16 == 0xAFF2) {
                bt_client_board_type = BLE_BOARD_HEADTRACKER;
                s_htresethandle = char_elem_result[i].char_handle;
              } else if (char_elem_result[i].uuid.uuid.uuid16 == 0xAFF1) {
                bt_client_board_type = BLE_BOARD_HEADTRACKER;
              }
            }
            bt_client_scan_complete = true;
          }
          free(char_elem_result);
        } else {
          ESP_LOGE(GATTC_TAG, "no char found");
        }
      }
      break;

    case ESP_GATTC_REG_FOR_NOTIFY_EVT: {
      ESP_LOGI(GATTC_TAG, "REG_FOR_NOTIFY_EVT");
      if (p_data->reg_for_notify.status != ESP_GATT_OK) {
        ESP_LOGE(GATTC_TAG, "REG FOR NOTIFY failed: error status = %d",
                 p_data->reg_for_notify.status);
      } else {
        uint16_t count = 0;
        uint16_t notify_en = 1;
        esp_gatt_status_t ret_status = esp_ble_gattc_get_attr_count(
            gattc_if, gl_profile_tab[PROFILE_A_APP_ID].conn_id, ESP_GATT_DB_DESCRIPTOR,
            gl_profile_tab[PROFILE_A_APP_ID].service_start_handle,
            gl_profile_tab[PROFILE_A_APP_ID].service_end_handle,
            gl_profile_tab[PROFILE_A_APP_ID].char_handle, &count);
        if (ret_status != ESP_GATT_OK) {
          ESP_LOGE(GATTC_TAG, "esp_ble_gattc_get_attr_count error");
        }
        if (count > 0) {
          descr_elem_result = (esp_gattc_descr_elem_t *)malloc(sizeof(esp_gattc_descr_elem_t) * count);
          if (!descr_elem_result) {
            ESP_LOGE(GATTC_TAG, "malloc error, gattc no mem");
          } else {
            ret_status = esp_ble_gattc_get_descr_by_char_handle(
                gattc_if, gl_profile_tab[PROFILE_A_APP_ID].conn_id, p_data->reg_for_notify.handle,
                notify_descr_uuid, descr_elem_result, &count);
            if (ret_status != ESP_GATT_OK) {
              ESP_LOGE(GATTC_TAG, "esp_ble_gattc_get_descr_by_char_handle error");
            }
            if (count > 0 && descr_elem_result[0].uuid.len == ESP_UUID_LEN_16 &&
                descr_elem_result[0].uuid.uuid.uuid16 == ESP_GATT_UUID_CHAR_CLIENT_CONFIG) {
              ret_status = esp_ble_gattc_write_char_descr(
                  gattc_if, gl_profile_tab[PROFILE_A_APP_ID].conn_id, descr_elem_result[0].handle,
                  sizeof(notify_en), (uint8_t *)&notify_en, ESP_GATT_WRITE_TYPE_RSP,
                  ESP_GATT_AUTH_REQ_NONE);
            }
            if (ret_status != ESP_GATT_OK) {
              ESP_LOGE(GATTC_TAG, "esp_ble_gattc_write_char_descr error");
            }
            free(descr_elem_result);
          }
        } else {
          ESP_LOGE(GATTC_TAG, "descr not found");
        }
      }
      break;
    }

    case ESP_GATTC_NOTIFY_EVT:
      if (p_data->notify.is_notify && bt_client_forwardHt) {
        uart_write_bytes(uart_num, (void *)p_data->notify.value, p_data->notify.value_len);
      }
      break;

    case ESP_GATTC_WRITE_DESCR_EVT:
      if (p_data->write.status != ESP_GATT_OK) {
        ESP_LOGE(GATTC_TAG, "write descr failed, error status = %x", p_data->write.status);
        break;
      }
      ESP_LOGI(GATTC_TAG, "write descr success");
      break;

    case ESP_GATTC_DISCONNECT_EVT:
      bt_client_connected = false;
      get_server = false;
      ESP_LOGI(GATTC_TAG, "DISCONNECT_EVT reason = %d", p_data->disconnect.reason);
      break;

    default:
      break;
  }
}

static void esp_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
  switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
      s_readytoscan = true;
      break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
      if (param->scan_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
        ESP_LOGE(GATTC_TAG, "scan start failed, error status = %x", param->scan_start_cmpl.status);
      }
      break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT: {
      switch (param->scan_rst.search_evt) {
        case ESP_GAP_SEARCH_INQ_RES_EVT: {
          if (param->scan_rst.rssi > MIN_BLE_RSSI) {
            scanned_add(param->scan_rst.bda, param->scan_rst.ble_addr_type);
          }

          if (s_autoconnect_active && !bt_client_connected &&
              memcmp(s_autoconnect_target, param->scan_rst.bda, sizeof(bt_addr_t)) == 0) {
            printf("Auto-connect: saved device found\r\n");
            s_autoconnect_active = false;
            bt_client_scan_stop();
            connect_internal(param->scan_rst.bda, param->scan_rst.ble_addr_type);
          }

          char addr[BT_ADDR_STR_LEN];
          bt_addr_to_str(addr, param->scan_rst.bda);
          printf("Disc BT Address %s, RSSI=%d, Addr Type=%d\n", addr, param->scan_rst.rssi,
                 param->scan_rst.ble_addr_type);
          break;
        }
        case ESP_GAP_SEARCH_INQ_CMPL_EVT:
          bt_client_scan_complete = true;
          break;
        default:
          break;
      }
      break;
    }

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
      if (param->scan_stop_cmpl.status != ESP_BT_STATUS_SUCCESS) {
        ESP_LOGE(GATTC_TAG, "scan stop failed, error status = %x", param->scan_stop_cmpl.status);
      }
      break;

    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
      ESP_LOGI(GATTC_TAG,
               "update conn params status=%d min=%d max=%d conn=%d latency=%d timeout=%d",
               param->update_conn_params.status, param->update_conn_params.min_int,
               param->update_conn_params.max_int, param->update_conn_params.conn_int,
               param->update_conn_params.latency, param->update_conn_params.timeout);
      break;

    default:
      break;
  }
}

static void esp_gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                         esp_ble_gattc_cb_param_t *param)
{
  if (event == ESP_GATTC_REG_EVT) {
    if (param->reg.status == ESP_GATT_OK) {
      gl_profile_tab[param->reg.app_id].gattc_if = gattc_if;
    } else {
      ESP_LOGI(GATTC_TAG, "reg app failed, app_id %04x, status %d", param->reg.app_id,
               param->reg.status);
      return;
    }
  }

  for (int idx = 0; idx < PROFILE_NUM; idx++) {
    if (gattc_if == ESP_GATT_IF_NONE || gattc_if == gl_profile_tab[idx].gattc_if) {
      if (gl_profile_tab[idx].gattc_cb) {
        gl_profile_tab[idx].gattc_cb(event, gattc_if, param);
      }
    }
  }
}

void bt_client_dohtreset(void)
{
  if (bt_client_connected && bt_client_scan_complete &&
      bt_client_board_type == BLE_BOARD_HEADTRACKER) {
    esp_err_t status = esp_ble_gattc_write_char(gl_profile_tab[PROFILE_A_APP_ID].gattc_if,
                                                gl_profile_tab[PROFILE_A_APP_ID].conn_id,
                                                s_htresethandle, 1, (uint8_t *)"R",
                                                ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
    if (status != ESP_OK) {
      ESP_LOGE(GATTC_TAG, "Error Writing to Characteristic");
    }
  }
}

void bt_client_start_scan(void)
{
  if (!s_readytoscan) {
    return;
  }
  s_autoconnect_active = false;
  bt_client_scan_complete = false;
  scanned_clear();
  printf("Clearing Addresses\r\n");
  // Stop any in-progress scan first (e.g. the endless auto-connect scan);
  // otherwise the new timed scan is rejected and the state machine never
  // reports "OK+DISCE".
  esp_ble_gap_stop_scanning();
  uint32_t duration = 1;
  esp_ble_gap_start_scanning(duration);
}

void bt_client_scan_stop(void)
{
  esp_ble_gap_stop_scanning();
}

bool bt_client_has_saved_address(void)
{
  return settings.rmtbtaddr[0] != '\0' && strcmp(settings.rmtbtaddr, "000000000000") != 0;
}

bool bt_client_start_autoconnect(void)
{
  if (!s_readytoscan || bt_client_connected) {
    return false;
  }
  bt_addr_from_str(s_autoconnect_target, settings.rmtbtaddr);
  s_autoconnect_active = true;
  bt_client_scan_complete = false;
  bt_client_validslavefound = false;
  scanned_clear();
  printf("Auto-connect: scanning for saved device\r\n");
  esp_ble_gap_start_scanning(0);  // 0 = scan continuously until stopped
  return true;
}

void bt_client_connect(bt_addr_t addr)
{
  if (bt_client_connected) {
    return;
  }
  s_autoconnect_active = false;
  bt_client_scan_complete = false;
  bt_client_validslavefound = false;

  uint8_t type = 0;
  if (!scanned_find_type(addr, &type)) {
    char saddr[BT_ADDR_STR_LEN];
    printf("Unable to connect to %s, address not found in storage\r\n",
           bt_addr_to_str(saddr, addr));
    return;
  }

  connect_internal(addr, type);
}

void bt_client_disconnect(void)
{
  if (bt_client_connected) {
    esp_ble_gattc_close(gl_profile_tab[PROFILE_A_APP_ID].gattc_if,
                        gl_profile_tab[PROFILE_A_APP_ID].conn_id);
  }
  bt_client_connected = false;
  bt_client_scan_complete = false;
  bt_client_validslavefound = false;
}

void bt_client_init(void)
{
  scanned_lock_init();
  ESP_LOGI(GATTC_TAG, "Starting Central");

  esp_err_t ret = esp_ble_gap_register_callback(esp_gap_cb);
  if (ret) {
    ESP_LOGE(GATTC_TAG, "%s gap register failed, error code = %x", __func__, ret);
    return;
  }

  ret = esp_ble_gattc_register_callback(esp_gattc_cb);
  if (ret) {
    ESP_LOGE(GATTC_TAG, "%s gattc register failed, error code = %x", __func__, ret);
    return;
  }

  ret = esp_ble_gattc_app_register(PROFILE_A_APP_ID);
  if (ret) {
    ESP_LOGE(GATTC_TAG, "%s gattc app register failed, error code = %x", __func__, ret);
  }

  esp_err_t local_mtu_ret = esp_ble_gatt_set_local_mtu(85);
  if (local_mtu_ret) {
    ESP_LOGE(GATTC_TAG, "set local MTU failed, error code = %x", local_mtu_ret);
  }

  uint8_t adrtype;
  esp_ble_gap_get_local_used_addr(localbtaddress, &adrtype);
}

#endif  // USE_NIMBLE
