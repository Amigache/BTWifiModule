/**
 * @file terminal.c
 * @brief Serial link with the radio: UART, AT commands and state machines.
 *
 * Responsibilities:
 *  - Receive UART bytes and detect AT commands (parsing is delegated to
 *    at_command.h).
 *  - In peripheral mode: forward telemetry/trainer traffic to BLE.
 *  - In central mode: forward BLE notifications to the UART.
 *  - Run the state machines that announce connections/disconnections to EdgeTX
 *    with the expected format (see AGENTS.md).
 */

#include "terminal.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <driver/uart.h>

#include "at_command.h"
#include "board.h"
#include "bt.h"
#include "bt_addr.h"
#include "bt_client.h"
#include "bt_server.h"
#include "cb.h"
#include "settings.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LOG_UART "UART"

/** Reusable buffer size for composing responses. */
#define REUSABLE_BUFFER 250
/** Maximum AT command length (including argument). */
#define AT_CMD_MAX_LEN 40
/** Peripheral passthrough buffer size (<= minimum ATT MTU payload). */
#define PASSTHROUGH_BUF 20

/** UART number used to talk to the radio (see board.h). */
const uart_port_t uart_num = UART_NUM;

/* --- Central state machine states ---------------------------------------- */
typedef enum {
  CENTRAL_STATE_DISCONNECT,
  CENTRAL_STATE_IDLE,
  CENTRAL_STATE_AUTOCONNECT,
  CENTRAL_STATE_SCAN_START,
  CENTRAL_STATE_SCANNING,
  CENTRAL_STATE_SCAN_COMPLETE,
  CENTRAL_STATE_CONNECT,
  CENTRAL_STATE_WAITING_CONNECTION,
  CENTRAL_STATE_CONNECTED,
} btcentralstate;

/* --- Peripheral state machine states ------------------------------------- */
typedef enum {
  PERIPHERIAL_STATE_DISCONNECTED,
  PERIPHERIAL_STATE_CONNECTED,
} btperipheralstate;

static role_t curMode = ROLE_UNKNOWN;
static btcentralstate btCentralState = CENTRAL_STATE_DISCONNECT;
static btperipheralstate btPeripherialState = PERIPHERIAL_STATE_DISCONNECTED;

/** Current remote address (text) and the reusable response buffer. */
static char rmtaddress[BT_ADDR_STR_LEN] = "000000000000";
static char reusablebuff[REUSABLE_BUFFER];

/** Index of the last device already announced during the scan. */
static int laddcnt = 0;

static void runBT(void);
static void setRole(role_t role);

/* ========================================================================== */
/* UART output helpers                                                        */
/* ========================================================================== */

static void write_str(const char *s)
{
  uart_write_bytes(uart_num, s, strlen(s));
}

static void write_fmt(const char *fmt, ...)
{
  va_list ap;
  va_start(ap, fmt);
  int len = vsnprintf(reusablebuff, sizeof(reusablebuff), fmt, ap);
  va_end(ap);
  if (len < 0) {
    return;
  }
  if ((size_t)len >= sizeof(reusablebuff)) {
    len = sizeof(reusablebuff) - 1;
  }
  uart_write_bytes(uart_num, reusablebuff, (size_t)len);
}

/* ========================================================================== */
/* EdgeTX announcements                                                       */
/* ========================================================================== */

/** Send "Peripheral:<addr>" or "Central:<addr>" according to the current role. */
static void sendBTMode(void)
{
  char lcladdress[BT_ADDR_STR_LEN];
  bt_addr_to_str(lcladdress, localbtaddress);
  ESP_LOGI(LOG_UART, "Local Addr %s", lcladdress);
  if (curMode == ROLE_BLE_PERIPHERAL) {
    write_fmt("Peripheral:%s\r\n", lcladdress);
  } else if (curMode == ROLE_BLE_CENTRAL) {
    write_fmt("Central:%s\r\n", lcladdress);
  }
}

/** Announce the connection in central mode (also re-announces periodically). */
static void sendCentralConnected(void)
{
  if (curMode != ROLE_BLE_CENTRAL) {
    return;
  }

  bt_addr_to_str(rmtaddress, rmtbtaddress);
  write_fmt("Connected:%s\r\n", rmtaddress);
  write_str("MTU Size:65\r\nMTU Size: 65\r\nPHT Update Complete\r\nCurrent PHY:2M\r\n");
  write_fmt("Board:%s\r\n", ble_board_type_names[bt_client_board_type]);

  btCentralState = CENTRAL_STATE_CONNECTED;
}

/** Announce the connection in peripheral mode. */
static void sendPeripheralConnected(void)
{
  if (curMode != ROLE_BLE_PERIPHERAL) {
    return;
  }

  bt_addr_to_str(rmtaddress, rmtbtaddress);
  write_fmt("Connected:%s\r\n", rmtaddress);
}

/* ========================================================================== */
/* AT command parsing / dispatch                                              */
/* ========================================================================== */

/**
 * @brief Execute an AT command and respond according to the EdgeTX contract.
 *
 * Any AT command means EdgeTX is (re)initializing: the trainer passthrough is
 * paused so the high-rate trainer stream cannot drown the handshake.
 */
static void parserATCommand(char *line)
{
  bt_client_forwardHt = false;

  at_command_parse_t cmd = at_command_parse(line);

  switch (cmd.command) {
    case AT_CMD_ROLE0:
      ESP_LOGI(LOG_UART, "Setting role as Peripheral");
      write_str("OK+Role:0\r\n");
      setRole(ROLE_BLE_PERIPHERAL);
      sendBTMode();
      break;

    case AT_CMD_ROLE1:
      ESP_LOGI(LOG_UART, "Setting role as Central");
      write_str("OK+Role:1\r\n");
      setRole(ROLE_BLE_CENTRAL);
      bt_client_forwardHt = true;
      sendBTMode();
      // If already connected (e.g. auto-connect finished first), re-announce.
      if (bt_client_connected) {
        sendCentralConnected();
      }
      break;

    case AT_CMD_CON:
      if (curMode == ROLE_BLE_CENTRAL) {
        write_fmt("OK+CONNA\r\nConnecting to:%s\r\n", cmd.argument);
        strncpy(rmtaddress, cmd.argument, sizeof(rmtaddress) - 1);
        rmtaddress[sizeof(rmtaddress) - 1] = '\0';
        bt_client_forwardHt = true;
        if (bt_client_connected) {
          sendCentralConnected();
        } else {
          btCentralState = CENTRAL_STATE_CONNECT;
        }
      } else {
        write_str("ERROR");
      }
      break;

    case AT_CMD_NAME: {
      // Bounded copy + persist + apply the name effectively.
      strncpy(settings.name, cmd.argument, sizeof(settings.name) - 1);
      settings.name[sizeof(settings.name) - 1] = '\0';
      settings_request_save();
      bt_set_name(settings.name);
      if (curMode == ROLE_BLE_PERIPHERAL) {
        bt_server_refresh_advertising();
      }
      ESP_LOGI(LOG_UART, "Setting Name to %s", settings.name);
      write_fmt("OK+Name:%s\r\n", settings.name);
      sendBTMode();
      break;
    }

    case AT_CMD_TXPW:
      ESP_LOGI(LOG_UART, "Setting Power to %s", cmd.argument);
      write_str("OK+Txpw:0\r\n");
      sendBTMode();
      break;

    case AT_CMD_DISC:
      if (curMode == ROLE_BLE_CENTRAL) {
        ESP_LOGI(LOG_UART, "Discovery Requested");
        // Drop an existing connection so trainer frames do not interleave with
        // the "OK+DISC" lines.
        if (bt_client_connected) {
          bt_client_disconnect();
        }
        write_str("OK+DISCS\r\n");
        laddcnt = 0;
        btCentralState = CENTRAL_STATE_SCAN_START;
      }
      break;

    case AT_CMD_CLEAR:
      if (curMode == ROLE_BLE_CENTRAL) {
        btCentralState = CENTRAL_STATE_DISCONNECT;
        memset(settings.rmtbtaddr, 0, sizeof(settings.rmtbtaddr));
        settings_request_save();
        write_str("OK+CLEAR\r\n");
      }
      break;

    case AT_CMD_BAUD:
      // The firmware stays at 115200 (see board.h). EdgeTX switches to 115200
      // after AT+BAUD4 without checking a response.
      ESP_LOGI(LOG_UART, "Baud change requested (ignored, staying at 115200)");
      break;

    case AT_CMD_HTRESET:
      if (bt_client_board_type == BLE_BOARD_HEADTRACKER) {
        ESP_LOGI(LOG_UART, "Reset Head Tracker Requested");
        write_str("OK+HTRESET\r\n");
        bt_client_dohtreset();
      } else {
        write_str("ERROR");
      }
      break;

    case AT_CMD_UNKNOWN:
    default:
      ESP_LOGE(LOG_UART, "Unknown AT Cmd: %s", line);
      break;
  }
}

/* ========================================================================== */
/* UART                                                                       */
/* ========================================================================== */

static uart_config_t uart_config = {
    .baud_rate = BAUD_DEFAULT,
    .data_bits = UART_DATA_8_BITS,
    .parity = UART_PARITY_DISABLE,
    .stop_bits = UART_STOP_BITS_1,
    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
    .source_clk = UART_SCLK_APB,
};

static char atcommand[AT_CMD_MAX_LEN];
static int atcommandlen = -1;
static cbuf_t uartinbuf;

void runUARTHead(void *param)
{
  (void)param;
  ESP_ERROR_CHECK(uart_param_config(uart_num, &uart_config));
  ESP_ERROR_CHECK(
      uart_driver_install(uart_num, UART_RX_BUFFER * 2, UART_RX_BUFFER * 2, 0, NULL, 0));
  ESP_ERROR_CHECK(
      uart_set_pin(uart_num, UART_TXPIN, UART_RXPIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

  if (!cbuf_init(&uartinbuf, UART_RX_BUFFER * 2)) {
    ESP_LOGE(LOG_UART, "Failed to allocate UART buffer");
    vTaskDelete(NULL);
    return;
  }

  ESP_LOGI(LOG_UART, "Waiting for settings to be read");
  while (!settings_ok) {
    vTaskDelay(pdMS_TO_TICKS(50));
  }

  ESP_LOGI(LOG_UART, "Setting initial role");
  if (settings.role == ROLE_UNKNOWN) {
    ESP_LOGE(LOG_UART, "Invalid role loaded, defaulting to central");
    settings.role = ROLE_BLE_CENTRAL;
  }
  setRole(settings.role);

  uint8_t *data = (uint8_t *)malloc(UART_RX_BUFFER + 1);
  if (data == NULL) {
    ESP_LOGE(LOG_UART, "No Memory! HALT");
    for (;;) {
      vTaskDelay(portMAX_DELAY);
    }
  }

  // Peripheral passthrough buffer (<= minimum ATT MTU payload).
  static uint8_t passBuf[PASSTHROUGH_BUF];
  static uint8_t passLen = 0;

  for (;;) {
    int cnt = uart_read_bytes(uart_num, data, UART_RX_BUFFER, 0);
    for (int i = 0; i < cnt; i++) {
      cbuf_push(&uartinbuf, data[i]);
    }

    uint8_t c;
    while (cbuf_pop(&uartinbuf, &c)) {
      if (atcommandlen >= 0) {
        atcommand[atcommandlen++] = (char)c;
        if (atcommandlen >= (int)sizeof(atcommand) - 1) {
          ESP_LOGE(LOG_UART, "AT Command Buffer Overflow");
          atcommandlen = -1;
          continue;
        }
        if (c == '\n') {
          atcommand[atcommandlen] = '\0';
          parserATCommand(atcommand);
          atcommandlen = -1;
        }
      } else {
        // Detect the "AT+" prefix (EdgeTX always sends "AT+..."). Requiring the
        // '+' avoids false positives from 'A','T' sequences in binary traffic.
        static char lc1 = 0;
        static char lc2 = 0;
        if (lc2 == 'A' && lc1 == 'T' && c == '+') {
          atcommandlen = 0;
          atcommand[atcommandlen++] = '+';
        } else if (curMode == ROLE_BLE_PERIPHERAL) {
          // Forward the radio's telemetry/trainer traffic unchanged.
          passBuf[passLen++] = c;
          if (passLen >= sizeof(passBuf)) {
            bt_server_send_data(passBuf, passLen);
            passLen = 0;
          }
        }
        // In central/master the radio only sends AT commands; discard the rest.
        lc2 = lc1;
        lc1 = (char)c;
      }
    }

    if (passLen > 0) {
      bt_server_send_data(passBuf, passLen);
      passLen = 0;
    }

    settings_process();
    runBT();
    // Delay one tick (10 ms at CONFIG_FREERTOS_HZ=100). Do NOT use
    // pdMS_TO_TICKS(1) here: it rounds down to 0 ticks (a bare taskYIELD),
    // which starves the idle task and triggers the task watchdog.
    vTaskDelay(1);
  }

  free(data);
  vTaskDelete(NULL);
}

/* ========================================================================== */
/* Role switching and state machines                                          */
/* ========================================================================== */

static void setRole(role_t role)
{
  ESP_LOGI(LOG_UART, "Switching from mode %d to %d", curMode, role);
  if (role == curMode) {
    return;
  }

  switch (curMode) {
    case ROLE_BLE_CENTRAL:
    case ROLE_BLE_PERIPHERAL:
      bt_disable();
      break;
    default:
      break;
  }

  curMode = role;
  settings.role = curMode;

  switch (curMode) {
    case ROLE_BLE_CENTRAL:
      btCentralState = CENTRAL_STATE_DISCONNECT;
      bt_init();
      bt_client_init();
      break;
    case ROLE_BLE_PERIPHERAL:
      btPeripherialState = PERIPHERIAL_STATE_DISCONNECTED;
      bt_init();
      bt_server_init();
      break;
    default:
      break;
  }

  settings_request_save();
}

static void runBTCentral(void)
{
  switch (btCentralState) {
    case CENTRAL_STATE_DISCONNECT:
      bt_client_disconnect();
      if (!bt_client_connected && bt_client_has_saved_address()) {
        btCentralState = CENTRAL_STATE_AUTOCONNECT;
      }
      break;

    case CENTRAL_STATE_AUTOCONNECT:
      if (bt_client_connected) {
        btCentralState = CENTRAL_STATE_CONNECTED;
      } else if (bt_client_start_autoconnect()) {
        btCentralState = CENTRAL_STATE_WAITING_CONNECTION;
      }
      break;

    case CENTRAL_STATE_SCAN_START:
      laddcnt = 0;
      bt_client_start_scan();
      btCentralState = CENTRAL_STATE_SCANNING;
      break;

    case CENTRAL_STATE_SCANNING: {
      uint8_t cnt = bt_client_scanned_count();
      for (int i = laddcnt; i < cnt; i++) {
        bt_addr_t addr;
        if (bt_client_scanned_get((uint8_t)i, addr, NULL)) {
          char saddr[BT_ADDR_STR_LEN];
          write_fmt("OK+DISC:%s\r\n", bt_addr_to_str(saddr, addr));
        }
      }
      laddcnt = cnt;
      if (bt_client_scan_complete) {
        btCentralState = CENTRAL_STATE_SCAN_COMPLETE;
      }
      break;
    }

    case CENTRAL_STATE_SCAN_COMPLETE:
      write_str("OK+DISCE\r\n");
      btCentralState = CENTRAL_STATE_IDLE;
      break;

    case CENTRAL_STATE_IDLE:
      if (!bt_client_connected && bt_client_has_saved_address()) {
        btCentralState = CENTRAL_STATE_AUTOCONNECT;
      }
      break;

    case CENTRAL_STATE_CONNECT: {
      bt_addr_t addr;
      bt_addr_from_str(addr, rmtaddress);
      bt_client_connect(addr);
      btCentralState = CENTRAL_STATE_WAITING_CONNECTION;
      break;
    }

    case CENTRAL_STATE_WAITING_CONNECTION:
      if (bt_client_scan_complete) {
        if (bt_client_validslavefound) {
          bt_addr_to_str(rmtaddress, rmtbtaddress);
          write_fmt("Connected:%s\r\n", rmtaddress);
          write_str("MTU Size:65\r\nMTU Size: 65\r\nPHT Update Complete\r\nCurrent PHY:2M\r\n");
          write_fmt("Board:%s\r\n", ble_board_type_names[bt_client_board_type]);
          btCentralState = CENTRAL_STATE_CONNECTED;
        } else {
          btCentralState = CENTRAL_STATE_DISCONNECT;
        }
      }
      break;

    case CENTRAL_STATE_CONNECTED:
      if (!bt_client_connected) {
        // The leading 0x7E resets EdgeTX's frame parser so the "DisConnected"
        // text is always recognized.
        static const char disconnMsg[] = "\x7e"
                                         "DisConnected\r\n";
        write_str(disconnMsg);
        if (bt_client_has_saved_address()) {
          btCentralState = CENTRAL_STATE_AUTOCONNECT;
        } else {
          btCentralState = CENTRAL_STATE_CONNECT;
        }
      } else {
        // Re-announce periodically in case the radio missed the first message.
        static int64_t lastAnnounce = 0;
        int64_t now = esp_timer_get_time();
        if (now - lastAnnounce > 2000000) {  // every 2 s
          lastAnnounce = now;
          sendCentralConnected();
        }
      }
      break;
  }
}

static void runBTPeripherial(void)
{
  switch (btPeripherialState) {
    case PERIPHERIAL_STATE_DISCONNECTED:
      if (bt_server_connected) {
        sendPeripheralConnected();
        btPeripherialState = PERIPHERIAL_STATE_CONNECTED;
      }
      break;

    case PERIPHERIAL_STATE_CONNECTED:
      if (!bt_server_connected) {
        btPeripherialState = PERIPHERIAL_STATE_DISCONNECTED;
        // "ERROR" makes EdgeTX reset BT and redo the handshake.
        write_str("DisConnected\r\nERROR\r\nERROR\r\n");
      } else {
        static int64_t lastAnnounce = 0;
        int64_t now = esp_timer_get_time();
        if (now - lastAnnounce > 2000000) {  // every 2 s
          lastAnnounce = now;
          sendPeripheralConnected();
        }
      }
      break;
  }
}

static void runBT(void)
{
  switch (curMode) {
    case ROLE_BLE_CENTRAL:
      runBTCentral();
      break;
    case ROLE_BLE_PERIPHERAL:
      runBTPeripherial();
      break;
    default:
      break;
  }
}
