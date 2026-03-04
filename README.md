# BTWifiModule

ESP32-C3 BLE Trainer / Telemetry bridge with WiFi web dashboard.

Emulates a FrSky Bluetooth module over BLE, bridging an RC trainer port (UART) to a BLE connection. Configurable as **Peripheral** (acts like a FrSky BT module), **Central** (connects to another BTWifiModule or compatible device), or **Telemetry** relay. A built-in WiFi access point serves a real-time web dashboard for monitoring and configuration — no app required.

> Extended documentation: **https://btwifimod.gitbook.io/**

---

## Hardware

| Board | Build Env | Notes |
|---|---|---|
| ESP32-C3 SuperMini | `C3SuperMini` | **Primary target** — GPIO8 LED, GPIO9 BOOT |
| ESP32-C3 Mini | `C3Mini` / `C3Mini_2` | |
| ESP32 Node32s (WROOM-32) | `Node32s_Wroom32` | Original target |
| ESP32 Pico | `pico32` / `pico32_debug` | |
| RadioMaster Pocket / ELRS | `RM_pico` / `HappymodelEP82` | |

### C3 SuperMini Pin Assignment

| Pin | Function |
|---|---|
| GPIO 8 | Status LED (active-low) |
| GPIO 9 | BOOT button — also toggles WiFi AP |
| GPIO 20 | UART RX (trainer port) |
| GPIO 21 | UART TX (trainer port) |

---

## Features

- **BLE roles** — Peripheral (TX trainer data), Central (scan & connect to peripherals), Telemetry relay
- **WiFi SoftAP** dashboard at `192.168.4.1` — toggle on/off with BOOT button at any time
- **Real-time WebSocket** push — device info, BLE scan results, and connection state update every 2 s without page refresh
- **BLE Scanner** (Central mode) — discovers nearby devices with name and RSSI; connect and disconnect from the UI
- **OTA firmware update** — drag-and-drop `.bin` via the web dashboard
- **AT command interface** via UART — compatible with HM-10 / FrSky Bluetooth AT command set
- **Persistent settings** — role and last remote address stored in NVS flash
- **Status LED** — fast blink = AP active, solid = BLE connected, off = idle
- **BOOT button** — short press toggles the WiFi AP; hold at power-on for firmware recovery

---

## Getting Started

### 1. Build & Flash

Requirements: [PlatformIO](https://platformio.org/) (CLI or VS Code extension), ESP-IDF 5.3.x toolchain.

```bash
# Build for C3 SuperMini
pio run -e C3SuperMini

# Build and upload
pio run -e C3SuperMini --target upload
```

### 2. Connect to the Web Dashboard

1. Power on the module.
2. Press the **BOOT button** once — the LED starts fast-blinking (AP active).
3. On your phone or PC, connect to WiFi network **BTWifiModule** (no password).
4. Open **http://192.168.4.1** in a browser.

### 3. Select a Role

Open the **Bluetooth** card on the dashboard and choose:

| Role | Description |
|---|---|
| Peripheral | BLE GATT server — trainer TX side. Plug into the trainer port and pair from a phone/Taranis. |
| Central | BLE GATT client — trainer RX side. Use the **BLE Scanner** to find and connect to a Peripheral. |
| Telemetry | Raw relay: UART bytes are forwarded transparently over BLE. |

The selected role is saved to flash and restored on next boot.

---

## Web Dashboard

| Card | Description |
|---|---|
| **Bluetooth** | Module name, MAC, role selector, firmware/heap info |
| **BT Controls** | Connect/disconnect, connection status badge with spinner feedback |
| **BLE Scanner** | (Central only) Scan for devices; click a device to connect. Shows device name, MAC, and RSSI. |
| **OTA Update** | Upload a new `.bin` firmware file; module reboots automatically after flash |
| **System** | Free / total heap, firmware version, reboot button |

### WebSocket Messages

The server pushes JSON on `/ws`:

| Message `t` | Fields | Description |
|---|---|---|
| `info` | `bt_name`, `bt_mac`, `role`, `ble_connected`, `bt_enabled`, `bt_role`, `free_heap`, `total_heap`, `fw_version`, `fw_short`, `conn_mac` | Periodic status (every 2 s) |
| `dev` | `mac`, `rssi`, `name` | One BLE scan result |
| `done` | `n` | Scan complete, `n` = total devices found |

---

## AT Command Interface

Commands are sent over UART (default **115200 baud**, 8N1). Prefix all commands with `AT` — e.g. `AT+ROLE0\r\n`.

| Command | Response | Description |
|---|---|---|
| `AT+ROLE0` | `OK+Role:0` | Set role → Peripheral |
| `AT+ROLE1` | `OK+Role:1` | Set role → Central |
| `AT+ROLE2` | `OK+Role:2` | Set role → Telemetry relay |
| `AT+NAME<name>` | `OK+Name:<name>` | Set BLE advertisement name (max 50 chars) |
| `AT+CON<mac>` | `OK+CONNA` | Connect to MAC address (12 hex chars, no separators) |
| `AT+DISC?` | `OK+DISCS` | Start BLE discovery scan (Central mode only) |
| `AT+CLEAR` | `OK+CLEAR` | Disconnect and clear remote address |
| `AT+TXPW<n>` | `OK+Txpw:0` | Set TX power (accepted, not yet implemented) |
| `AT+HTRESET` | `OK+HTRESET` | Send headtracker reset (Central + headtracker device only) |
| `AT+BAUD<rate>` | — | Baud rate change (accepted, not yet implemented) |

The module also outputs unsolicited status lines on connect/disconnect, e.g.:
```
Peripheral:AABBCCDDEEFF
Central:AABBCCDDEEFF
```

---

## Building for Other Boards

Each board has its own PlatformIO environment in `platformio.ini`. The preprocessor flag selects the correct UART pins and LED/button GPIOs:

| Flag | Board |
|---|---|
| `PCB_C3SUPERMINI` | ESP32-C3 SuperMini |
| `PCB_C3MINI` | ESP32-C3 Mini |
| `PCB_WROOM` | ESP32 WROOM-32 |
| `PCB_PICO` | ESP32 Pico |
| `PCB_RMPICO` | RadioMaster |

---

## Roadmap

### Completed ✅
- BLE Peripheral role (FrSky trainer TX)
- BLE Central role with scan and connect
- BLE Telemetry relay
- WiFi SoftAP web dashboard
- WebSocket real-time push
- OTA firmware update via web UI
- AT command interface (HM-10 compatible subset)
- BOOT button AP toggle + 3-state LED
- BLE scanner with device name + RSSI
- Connecting / disconnecting spinners in UI
- Persistent role + remote address in NVS

### Planned ⬜
- WiFi AP password / web UI authentication
- Auto-reconnect to last known BLE device on boot
- AT+BAUD baud-rate change with confirmation handshake
- Multi-board LED/button abstraction
- ESP-NOW support (flag scaffolding exists)
- HeadTracker reset characteristic (partial implementation exists)

---

## Project Structure

```
src/
  main.c        — Entry point; starts UART task + webserver
  bt.c/h        — BLE init, advertisement name, MAC helpers
  bt_client.c/h — Central (GATT client) scan + connect logic
  bt_server.c/h — Peripheral (GATT server) + notify logic
  terminal.c/h  — UART AT-command parser + web-triggered BT actions
  webserver.c   — HTTP server, WebSocket, HTML/CSS/JS dashboard
  frskybt.c/h   — FrSky channel data encoding/decoding
  settings.c/h  — NVS-backed persistent settings
  defines.h     — Board pin assignments + role enum
  cb.h          — Circular buffer (lock-free, single-reader/writer)
```

---

## License

See [LICENSE](LICENSE).
