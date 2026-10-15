# BTWifiModule

FrSky Bluetooth emulator for ESP32, to be used inside an OpenTX/EdgeTX radio.
It emulates a FrSky Bluetooth module and adds **Bluetooth telemetry** (S.Port and
CRSF), **auto-connect** and a **selectable BLE host** (Bluedroid or NimBLE).

Based on the community project by **dlktdr**
(https://github.com/dlktdr/BTWifiModule).
Documentation: https://btwifimod.gitbook.io/

## Features

| Feature | Status |
|---|---|
| Bluetooth Trainer — Master Mode (connect to a BLE trainer / headtracker) | Supported |
| Bluetooth Trainer — Slave Mode | Supported |
| Bluetooth Telemetry over BLE | Supported |
| — S.Port (FrSky models) | Supported |
| — CRSF (ELRS models) | Supported |
| Auto-connect to the last known device | Supported |
| Device name from `AT+NAME` (persisted and advertised) | Supported |
| Selectable BLE host: Bluedroid or NimBLE | Supported |

## How telemetry works

In *Telemetry* mode EdgeTX forwards the radio's telemetry over the BT serial
link. The module passes those bytes to the connected BLE central unchanged:

- FrSky models: S.Port frames, `0x7E`-delimited and byte-stuffed.
- ELRS/CRSF models: raw CRSF frames.

The client app auto-detects the protocol, so both work over the same GATT
service (`0xFFF0`, characteristic `0xFFF6`, NOTIFY).

- S.Port: **INAV / Telemetry Viewer**.
- CRSF: the maintained fork by **RomanLut**
  (https://github.com/RomanLut/android-taranis-smartport-telemetry/releases).

## Hardware

- Primary target: **ESP32-C3 SuperMini**; also supported: **ESP32-DEV-38P**.
  The other board environments in `platformio.ini` are kept for reference (see
  [Validated targets](#validated-targets)).
- **Radiomaster TX16S MK3** with **EdgeTX** (a recent nightly is recommended).

> **TX16S MK3 note.** EdgeTX needs PR #7855 (`BT_PWR_GPIO = PC13` in
> `targets/tx16smk3/hal.h`) to power the AUX2/BT connector. It is included in
> current nightlies.

## Wiring

The module talks to the radio over a 3.3 V UART link (8N1, 115200 baud). Only
TX, RX and GND are required; add +5V if the module is powered from the radio.
The data lines are **crossed**: the module TX goes to the radio RX and the
module RX to the radio TX. Keep GND common with the radio, and power the module
from a supply that can handle the ESP32 current peaks.

| Board | UART | Module TX -> radio RX | Module RX <- radio TX | Flash / logs |
|---|---|---|---|---|
| ESP32-C3 SuperMini | UART1 | GPIO21 | GPIO20 | native USB Serial/JTAG |
| ESP32-DEV-38P | UART2 | GPIO18 | GPIO19 | on-board CP2102 (UART0) |

The pins are selected per environment through the `PCB_*` macro in
`src/board.h`. On the C3 SuperMini the radio uses UART1 on GPIO20/21 (the
default UART0 pins), so the log console is routed to the native USB Serial/JTAG
port instead of UART0; on the ESP32-DEV-38P the on-board CP2102 bridge keeps
UART0 free for flashing and logs.

## EdgeTX protocol

The module speaks the FrSky Bluetooth AT protocol over an 8N1 UART link at
115200 baud. All responses are `\r\n`-terminated; the real ACK that advances
EdgeTX is the `Central:<addr>` / `Peripheral:<addr>` line emitted after
`AT+TXPW` / `AT+ROLE`. Trainer and S.Port frames are `0x7E`-delimited and
byte-stuffed, while CRSF/Ghost are forwarded raw. See `AGENTS.md` for the full
set of protocol invariants.

## Code structure

```
src/
  main.c          Entry point (NVS + UART task).
  board.h         Board config: UART/pins and shared types.
  at_command.c/h  Pure AT command parser (host-testable).
  bt_addr.c/h     Bluetooth address conversion (host-testable).
  cb.c/h          Byte circular buffer (host-testable).
  settings.c/h    Persistent config (NVS) with protected access.
  bt.c/h          Common BT stack control (Bluedroid/NimBLE).
  bt_server.c/h   GATT peripheral (service 0xFFF0).
  bt_client.c/h   GATT central (scan, connect, auto-connect).
  terminal.c/h    UART link: AT commands and state machines.
test/             Unit tests (Unity).
tools/            Bench tooling: sport_sender.py (S.Port injector) and
                  at_bench.py (automated AT command bench).
doc/BENCH.md      Bench setup, procedure and validation status.
```

## Building

PlatformIO (ESP-IDF). Two BLE hosts are selectable:

| Environment | BLE host | Flash (1 MB app) | Notes |
|---|---|---|---|
| `C3SuperMini` | Bluedroid (default) | ~84 % | ESP32-C3 SuperMini |
| `C3SuperMini_nimble` | NimBLE | ~52 % | ESP32-C3 SuperMini, smaller footprint |
| `ESP32_DEV38P` | Bluedroid | ~86 % | ESP32-DEV-38P |
| `ESP32_DEV38P_nimble` | NimBLE | ~53 % | ESP32-DEV-38P |

```bash
pio run -e C3SuperMini -t upload          # Bluedroid
pio run -e C3SuperMini_nimble -t upload   # NimBLE
```

The Bluedroid path is kept behind `#if !defined(USE_NIMBLE)`, so both hosts
share the same application code.

## Web flasher

A browser-based flasher (ESP Web Tools) is published from this repository
through GitHub Pages. Open the Pages URL in a Chromium-based browser
(Chrome/Edge) over HTTPS, connect the board over USB and pick a **board** and a
**Bluetooth host**:

- Boards: **ESP32-C3 SuperMini** (`C3SuperMini*`) and **ESP32-DEV-38P**
  (`ESP32_DEV38P*`).
- Hosts: **Bluedroid** (default) and **NimBLE**.

Every board is offered with both hosts (each combination is a separate build).
The page is built and deployed by `.github/workflows/pages.yml`. Enable it once
under **Settings → Pages → Build and deployment → Source: GitHub Actions**.
Press RESET on the board after flashing.

## Validated targets

Hardware validation status of the current firmware. The bench procedure is in
`doc/BENCH.md`.

| Target | BLE host | AT commands | Slave + telemetry | Master | Headtracker |
|---|---|---|---|---|---|
| ESP32-C3 SuperMini | Bluedroid | PASS | PASS | PASS | PASS |
| ESP32-C3 SuperMini | NimBLE | PASS | PASS | PASS | PASS |
| ESP32-DEV-38P | Bluedroid | PASS | PASS | n/t | PASS |
| ESP32-DEV-38P | NimBLE | PASS | PASS | n/t | PASS |

`n/t` = not tested (the ESP32-DEV-38P was validated against the phone and the
headtracker, without a board-to-board master run). The other board environments
in `platformio.ini` (ESP32 Pico, C3 DevKitM-1, Node32s Wroom32, Radiomaster
Pico) are kept for reference but are neither built by CI nor hardware-validated
with this version.

## Tests

The pure modules (`cb`, `at_command`, `bt_addr`) have unit tests under `test/`:

```bash
pio test -e native
```

## AT command set

The module speaks the same AT protocol EdgeTX uses for the FrSky Bluetooth
module:

| Command | Meaning |
|---|---|
| `AT+BAUD4` | baud rate level (this firmware stays at 115200) |
| `AT+NAME<name>` | set the Bluetooth name (persisted and advertised) |
| `AT+TXPW<n>` | TX power (acknowledged) |
| `AT+ROLE0` | Peripheral (telemetry / trainer slave) |
| `AT+ROLE1` | Central (trainer master) |
| `AT+DISC?` | start a scan (central) |
| `AT+CON<addr>` | connect to a device (central) |
| `AT+CLEAR` | forget the saved device |
| `AT+HTRESET` | reset the headtracker (BTWifiModule extension) |

Responses: `OK+...`, `Central:<addr>`, `Peripheral:<addr>`, `Connected:<addr>`,
`DisConnected`, `ERROR`.

## Credits

Original project: **dlktdr/BTWifiModule**. Bluetooth telemetry, auto-connect,
the CRSF passthrough and the NimBLE port by this fork.
