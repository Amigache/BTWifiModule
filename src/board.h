/**
 * @file board.h
 * @brief Board configuration: UART link to the radio, pins, baud rates and
 *        shared types.
 *
 * Each `platformio.ini` environment defines exactly one `PCB_xxx` macro (and
 * `CPU_xxx`) via `build_flags`. This header translates that selection into the
 * physical parameters of the serial link with the radio.
 *
 * Primary target: ESP32-C3 SuperMini (`PCB_C3SUPERMINI`), which connects the
 * radio to UART1 on GPIO20 (RX) / GPIO21 (TX). Reference target: ESP32-S3
 * SuperMini (`PCB_S3SUPERMINI`), which uses UART1 on GPIO43 (TX) / GPIO44 (RX).
 */

#pragma once

#include <driver/uart.h>

/* ------------------------------------------------------------------------- */
/* Board selection                                                           */
/* ------------------------------------------------------------------------- */

#if defined(PCB_PICO)
  // ESP32 Pico Kit
  #define UART_NUM   UART_NUM_2
  #define UART_TXPIN 4
  #define UART_RXPIN 5

#elif defined(PCB_RMPICO)
  // Radiomaster Pico
  #define UART_NUM   UART_NUM_0
  #define UART_RXPIN 3
  #define UART_TXPIN 1

#elif defined(PCB_WROOM)
  // Node32s / WROOM-32
  #define UART_NUM   UART_NUM_2
  #define UART_TXPIN 18
  #define UART_RXPIN 19

#elif defined(PCB_C3MINI)
  // ESP32-C3 DevKitM-1
  #define UART_NUM   UART_NUM_1
  #define UART_TXPIN 4
  #define UART_RXPIN 5

#elif defined(PCB_C3SUPERMINI)
  // ESP32-C3 SuperMini (primary target)
  //   GPIO20 = RX (radio TX -> module RX)
  //   GPIO21 = TX (module TX -> radio RX)
  #define UART_NUM   UART_NUM_1
  #define UART_TXPIN 21
  #define UART_RXPIN 20

#elif defined(PCB_S3SUPERMINI)
  // ESP32-S3 SuperMini (reference target)
  //   GPIO43 = TX (module TX -> radio RX)
  //   GPIO44 = RX (radio TX -> module RX)
  #define UART_NUM   UART_NUM_1
  #define UART_TXPIN 43
  #define UART_RXPIN 44

#else
  #error "PCB_NOT_CONFIGURED: define PCB_PICO / PCB_RMPICO / PCB_WROOM / PCB_C3MINI / PCB_C3SUPERMINI / PCB_S3SUPERMINI"
#endif

/* ------------------------------------------------------------------------- */
/* Serial link                                                               */
/* ------------------------------------------------------------------------- */

/** UART receive/transmit buffer size (bytes). */
#define UART_RX_BUFFER 512
#define UART_TX_BUFFER 512

/** Radio link baud rates. The firmware stays fixed at 115200: EdgeTX sends
 *  `AT+BAUD4` at the 57600 factory rate and switches to 115200 without checking
 *  a response, so this module starts directly at 115200. */
#define BAUD_DEFAULT 115200
#define BAUD_MAXIMUM 921600

/** Maximum Bluetooth name length (advertised and persisted). */
#define LEN_BLUETOOTH_NAME 20

/* ------------------------------------------------------------------------- */
/* Shared types                                                              */
/* ------------------------------------------------------------------------- */

/**
 * @brief Bluetooth role of the module.
 *
 * EdgeTX mapping:
 *   - @ref ROLE_BLE_CENTRAL    -> `AT+ROLE1` (trainer master).
 *   - @ref ROLE_BLE_PERIPHERAL -> `AT+ROLE0` (trainer slave / telemetry).
 */
typedef enum {
  ROLE_UNKNOWN = 0,
  ROLE_BLE_PERIPHERAL,
  ROLE_BLE_CENTRAL,
  ROLE_ADVANCE,  //< reserved for future modes
  ROLE_COUNT
} role_t;

/** UART number used to talk to the radio (defined in terminal.c). */
extern const uart_port_t uart_num;
