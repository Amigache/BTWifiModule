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

- **ESP32-C3 SuperMini** (primary target); the other boards are listed in
  `platformio.ini`.
- UART to the radio: **GPIO20 (RX) / GPIO21 (TX)** on the C3 SuperMini.
- **Radiomaster TX16S MK3** with **EdgeTX** (a recent nightly is recommended).

> **TX16S MK3 note.** EdgeTX needs PR #7855 (`BT_PWR_GPIO = PC13` in
> `targets/tx16smk3/hal.h`) to power the AUX2/BT connector. It is included in
> current nightlies.

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

| Environment | BLE host | Flash (C3, 1 MB app) | Notes |
|---|---|---|---|
| `C3SuperMini` | Bluedroid (default) | ~84 % | |
| `C3SuperMini_nimble` | NimBLE | ~52 % | smaller footprint, recommended |

```bash
pio run -e C3SuperMini -t upload          # Bluedroid
pio run -e C3SuperMini_nimble -t upload   # NimBLE
```

The Bluedroid path is kept behind `#if !defined(USE_NIMBLE)`, so both hosts
share the same application code.

## Web flasher

A browser-based flasher (ESP Web Tools) is published from this repository
through GitHub Pages. Open the Pages URL in a Chromium-based browser
(Chrome/Edge) over HTTPS, connect the board over USB and pick a build:

- **Bluedroid** (`C3SuperMini`)
- **NimBLE** (`C3SuperMini_nimble`)

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

The other board environments in `platformio.ini` (ESP32 Pico, C3 DevKitM-1,
Node32s Wroom32, Radiomaster Pico) are kept for reference but are neither built
by CI nor hardware-validated with this version.

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
