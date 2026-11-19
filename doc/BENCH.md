# Bench setup

Reference bench for validating the BTWifiModule firmware on hardware. Use it
to repeat the same checks for each BLE host (Bluedroid and NimBLE).

## Hardware

- ESP32-C3 SuperMini (`PCB_C3SUPERMINI`), flashed over native USB.
- ESP32-DEV-38P (ESP32 classic, `ESP32_DEV38P*`): radio UART on
  GPIO18 (TX) / GPIO19 (RX); logs on UART0 (its CP2102 bridge). Its auto-reset
  does not drive IO0, so enter download mode manually (hold BOOT, tap RESET)
  before flashing.
- Reference only (not validated): ESP32-S3 SuperMini (`ESP32S3SuperMini`,
  `ESP32S3SuperMini_nimble`, radio UART GPIO43 TX / GPIO44 RX, native USB
  console), ESP32 Pico Kit (`pico32`, `pico32_nimble`, radio UART GPIO4 TX /
  GPIO5 RX) and Radiomaster Pico (`RM_pico`, `RM_pico_nimble`, radio UART GPIO1
  TX / GPIO3 RX, console on UART1 GPIO20/19).
- RadioMaster Wireless Module (`RM_Wireless`, `RM_Wireless_nimble`, radio UART0
  GPIO1 TX / GPIO3 RX shared with the USB-C CP2102 bridge, no log console).
  Partially validated over USB/BLE (see its notes below); the radio bay has not
  been tested yet.
- USB-UART adapter (FTDI) on COM10: TX wired to the module RX (UART link
  used for AT commands and telemetry injection), 115200 baud.
- Phone with the telemetry app (Android Taranis SmartPort app, RomanLut fork).
- The USB Serial/JTAG port (COM11) is only used for flashing and logs; it is
  unreliable on the reference Windows host, so prefer the FTDI port for tests.

## Software

- `espressif32@6.10.0` (ESP-IDF 5.4.0), pinned in `platformio.ini`.
- Telemetry sender: `tools/sport_sender.py` (needs `pyserial`).
  Run `python tools/sport_sender.py COM10`.

## Flashing

Build and flash with PlatformIO, or with esptool from `.pio/build/<env>/`:

```bash
pio run -e C3SuperMini -t upload          # C3 SuperMini, Bluedroid
pio run -e C3SuperMini_nimble -t upload   # C3 SuperMini, NimBLE
pio run -e ESP32_DEV38P -t upload         # ESP32-DEV-38P, Bluedroid
pio run -e ESP32_DEV38P_nimble -t upload  # ESP32-DEV-38P, NimBLE
pio run -e ESP32S3SuperMini -t upload     # ESP32-S3 SuperMini, Bluedroid
pio run -e ESP32S3SuperMini_nimble -t upload  # ESP32-S3 SuperMini, NimBLE
pio run -e RM_Wireless -t upload          # RadioMaster Wireless Module, Bluedroid
pio run -e RM_Wireless_nimble -t upload   # RadioMaster Wireless Module, NimBLE
```

If the board does not reset cleanly, power-cycle it and use the manual
`esptool.py` command with `--before usb_reset --after no_reset --no-stub`.

## Test sequence

1. AT protocol over COM10 (both variants): `AT+NAME`, `AT+TXPW`,
   `AT+ROLE0`, `AT+ROLE1`, `AT+CLEAR`, `AT+HTRESET`, `AT+DISC?`, `AT+CON`.
   Each command must return the expected response terminated with `\r\n`.
2. Peripheral mode (`AT+ROLE0`): advertising visible, connect from the phone,
   reconnect at least 3 times.
3. Telemetry: start `tools/sport_sender.py`, connect the phone app, confirm
   voltage 11.40 V, current 15.20 A, altitude 123 m, speed 83 km/h and RSSI 95.
   Keep the connection for at least 2 minutes.
4. Central mode (`AT+ROLE1`): `AT+DISC?` lists advertising peers, `AT+CON`
   connects to the selected peer.

## AT command bench (automated)

`python tools/at_bench.py COM10` drives the UART link and checks the response
of every AT command, the role restrictions and the commands that produce no
response. Run it once per firmware variant.

| Command        | Expected response                                   | Restriction            |
|----------------|-----------------------------------------------------|------------------------|
| `AT+ROLE0`     | `OK+Role:0` + `Peripheral:<addr>`                   | -                      |
| `AT+ROLE1`     | `OK+Role:1` + `Central:<addr>`                      | -                      |
| `AT+CON<addr>` | `OK+CONNA` + `Connecting to:<addr>`                 | central; peripheral `ERROR` |
| `AT+NAME<n>`   | `OK+Name:<n>` + mode line                           | -                      |
| `AT+TXPW<n>`   | `OK+Txpw:0` + mode line                             | -                      |
| `AT+DISC?`     | `OK+DISCS`, `OK+DISC:<addr>` per peer, `OK+DISCE`   | central; peripheral none |
| `AT+CLEAR`     | `OK+CLEAR`                                          | central; peripheral none |
| `AT+BAUD<n>`   | no response (stays at 115200)                       | -                      |
| `AT+HTRESET`   | `OK+HTRESET`, or `ERROR` unless headtracker board   | -                      |
| unknown        | no response                                         | -                      |

## Slave (peripheral) test with a phone peer

1. `AT+ROLE0`, then start `tools/sport_sender.py`.
2. Connect the phone app to the advertised name.
3. Expect `Connected:<addr>` on the UART and the app values below.
4. Disconnect: expect `Disconnected` + `ERROR` + `ERROR`.
5. Reconnect at least 3 times and keep the last link for 2 minutes.

## Master (central) test

The peer must expose service `0xFFF0` with characteristic `0xFFF6`, otherwise
`AT+CON` connects and drops again because the firmware marks the slave valid
only when it finds that characteristic. A phone advertiser (nRF Connect) is
not enough: use a second module running this firmware in peripheral mode.

1. Peer (second module): `AT+ROLE0`, give it a distinct name, keep its UART
   fed with `tools/sport_sender.py`.
2. `AT+ROLE1` on the unit under test, then `AT+CLEAR` to stop auto-connect.
3. `AT+DISC?`: expect `OK+DISCS`, one `OK+DISC:<addr>` per peer, `OK+DISCE`.
4. `AT+CON<addr>` with the peer address: expect `Connecting to:<addr>`, then
   `Connected:<addr>` + `MTU Size`/`PHT`/`PHY` lines + `Board:<type>`
   (`Unknown` unless the peer exposes the headtracker characteristics).
5. Data path: the peer's `0xFFF6` notifications must appear on this unit's
   UART as raw S.Port frames (the firmware is a pure passthrough).

Note: `bt_client_connect` only accepts an address present in the last scan, so
run `AT+DISC?` immediately before `AT+CON`.

## Headtracker test (central)

1. Power on the BLE headtracker and keep it 1-2 m from the module.
2. `AT+ROLE1`, `AT+CLEAR`, `AT+DISC?`: the headtracker must be listed.
3. `AT+CON<addr>`: expect `Board:HeadTracker`, then raw channel frames on the
   UART.
4. `AT+HTRESET`: expect `OK+HTRESET` (only answered for a headtracker board).
5. Re-run `AT+DISC?` while the module is auto-connecting: it must still return
   `OK+DISCS` ... `OK+DISCE`.

Notes:
- The scan only keeps devices stronger than `MIN_BLE_RSSI` (`bt_client.h`).
- The module's receiver is much less sensitive than a phone: the same
  headtracker reads about -80 dBm on the module and -56 dBm on the phone.
  Range is therefore limited by the module antenna.

## Expected values (telemetry)

| Sensor       | Data ID | Sent value | Displayed     |
|--------------|---------|------------|---------------|
| RSSI         | 0xF101  | 95 << 24   | 95            |
| Altitude     | 0x0100  | 12345      | 123 m         |
| Vertical spd | 0x0110  | -250       | -2.50 m/s     |
| Voltage      | 0x0210  | 1140       | 11.40 V       |
| Current      | 0x0200  | 152        | 15.20 A       |
| GPS speed    | 0x0830  | 45000      | 83 km/h       |

RPM and temperature are sent but the app does not display them.
GPS satellites are not sent by the sender, so the app shows 0.

## Validation status

Both BLE hosts validated with two modules (A under test, B as peripheral peer):

| Check                                   | Bluedroid | NimBLE |
|-----------------------------------------|-----------|--------|
| AT command bench (16 checks)            | PASS      | PASS   |
| Slave: telemetry to the phone app       | PASS      | PASS   |
| Slave: reconnect (3x, 2 min held)       | PASS      | PASS   |
| Master: discovery (`AT+DISC?`)          | PASS      | PASS   |
| Master: connection (`AT+CON`)           | PASS      | PASS   |
| Master: data path (peer -> local UART)  | PASS      | PASS   |
| Headtracker: detect + channels + reset  | PASS      | PASS   |
| Cross-host interop (Bluedroid <-> NimBLE) | PASS    | PASS   |

ESP32-DEV-38P (`ESP32_DEV38P*`), single board against the phone and the
headtracker (no board-to-board master run):

| Check                                   | Bluedroid | NimBLE |
|-----------------------------------------|-----------|--------|
| Boot + advertising                      | PASS      | PASS   |
| AT command bench (16 checks)            | PASS      | PASS   |
| Slave: telemetry to the phone app       | PASS      | PASS   |
| Headtracker: detect + channels + reset  | PASS      | PASS   |

## Automated battery

`python tools/battery.py --a <portA> --b <portB> [--ht <addr>]` drives two
boards at once and runs:

- the AT command bench on each board;
- telemetry passthrough board-to-board in both directions (this also exercises
  Bluedroid/NimBLE interop);
- headtracker detection, channels and reset.

Last run (A = NimBLE, B = Bluedroid, headtracker at -84 dBm):

| Check | Result |
|---|---|
| AT bench A (NimBLE) | 16/16 |
| AT bench B (Bluedroid) | 16/16 |
| Telemetry A(peripheral) -> B(central) | PASS (336 frames) |
| Telemetry B(peripheral) -> A(central) | PASS (334 frames) |
| Headtracker (NimBLE central) | PASS (549 frames) |

## Known issues and caveats

- Peer power: a module that browns out (`BOD: Brownout detector was triggered`)
  reboots in a loop and never advertises reliably. Power it from a solid USB
  port/cable; the FTDI is only used for GND/TX/RX.
- Phone GATT cache: after many connect/disconnect cycles the Android app can
  stop connecting while the module still advertises. Toggling Bluetooth off/on
  (or rebooting the phone) clears it; the module itself is fine.
- `AT+CON` only accepts an address present in the last scan, so run
  `AT+DISC?` immediately before `AT+CON`.
- `AT+DISC?` now cancels any in-progress scan first, so it also completes
  while the module is auto-connecting (previously it returned `OK+DISCS` with
  no `OK+DISCE`).
- Telemetry app: connection can drop right after connecting when the
  S.Port frames are invalid (fixed by the checksum in `tools/sport_sender.py`).

## RadioMaster Wireless Module notes

Reference target (`RM_Wireless`, `RM_Wireless_nimble`). Verified on hardware:
ESP32-D0WDQ6-V3, W25Q64 (8 MB), 40 MHz crystal, CP2102N bridge, flash erased
from the factory, 3 MB application partition.

The board shares UART0 (GPIO1 TX / GPIO3 RX) between the radio bay (J4) and the
CP2102N through a CH442E mux selected by USB VBUS. The build therefore runs with
the log console disabled (`CONFIG_LOG_DEFAULT_LEVEL_NONE`): with the USB
unplugged the bay carries the AT protocol, so any log output would corrupt it.
**A silent serial monitor is the expected result, not a fault.** Only the ROM
banner (`ets Jul 29 2019 ...`, `entry 0x40080578`) is visible, and only after a
reset.

### Talking AT over USB

The CP2102N drives `EN_ESP32` and `BOOT` through Q1/Q2 (S8050), so the modem
control lines matter:

- Open the port with **DTR and RTS deasserted**. Most Python tooling (pyserial,
  and therefore `pio device monitor`) asserts both at open, which resets the
  board and can leave it in the ROM download mode.
- If `GPIO0` was low when the chip booted, it stays in download mode and never
  starts the application, so every AT command times out. Pulse `EN` with `GPIO0`
  already high (assert RTS, release, with DTR low) and send the command on the
  same open handle. The banner must end in `entry 0x40080578`, not in
  `waiting for download`.
- `esptool` is unaffected: its reset sequence enters download mode on purpose.

Verified PowerShell snippet (no reset, no dependencies):

```powershell
$port = New-Object System.IO.Ports.SerialPort '<COMx>',115200,'None',8,'One'
$port.DtrEnable = $false
$port.RtsEnable = $false
$port.Open()
$port.Write("AT+ROLE0`r`n")
Start-Sleep -Milliseconds 700
$port.ReadExisting()
$port.Close()
```

### The USB link is picky

On the reference Windows host the CP2102N only enumerates on some ports with some
cables. Symptoms seen during bring-up:

- `Dispositivo USB desconocido (Error de solicitud de descriptor de dispositivo)`,
  shown as `USB\VID_0000&PID_0002` (Code 43): the device never completes USB
  enumeration.
- The COM port drops off the bus while the ESP32 keeps running; reads fail with
  `ClearCommError failed (PermissionError(13, 'Acceso denegado'))`, and the port
  only comes back after unplugging and replugging.
- Drops are intermittent, and a reset makes them more likely (the ESP32 current
  transient at boot, when the RF calibration runs, is the worst case).

Both are physical-layer problems, not firmware: the ESP32 never panics and the
ROM banner plus `entry 0x40080578` are printed normally every time. Switching to
another USB port, direct on the motherboard, fixed it on the bench. Do not chase
the firmware when the COM port disappears at USB level.

### Running `tools/sport_sender.py` here

On this board the only path to the ESP32 is the CP2102N, so the script has to
drive that COM port: while USB is plugged the CH442E routes UART0 to the bridge,
not to the bay. `serial.Serial(...)` asserts DTR and RTS in `open()`, which
resets the board and can leave it in download mode, so clear the modem lines
first and reuse the module's own builders:

```python
import sys
import time

import serial

sys.path.insert(0, 'tools')
from sport_sender import SENSORS, sport_frame

s = serial.Serial()
s.port, s.baudrate = 'COMx', 115200
s._dtr_state = s._rts_state = False   # keep DTR/RTS low when the port opens
s.open()
while True:
    for data_id, value in SENSORS:
        s.write(sport_frame(0x1B, data_id, value))
        time.sleep(0.03)
```

The same frames decode in the phone app as RSSI 95, altitude 123.45 m,
vertical speed -2.50 m/s, voltage 11.40 V, current 15.20 A, RPM 9800,
temperature 23.5 C, GPS altitude 120 m and GPS speed 45 kn (shown as 83 km/h).

### Status LED

The board has a WS2812-compatible RGB LED (`LED1`, TX1812MJA5-1515) on
**GPIO18**, fed through R15 (300R) on the `RGB_ESP32` net. `src/status_led.c`
drives it with the RMT peripheral (GRB, MSB first) when `HAS_STATUS_LED` is
defined, which only happens for `PCB_RM_WIRELESS`; on the other boards the
functions are no-ops.

| Colour | Meaning |
|---|---|
| Red | Powered, no radio traffic and no BLE peer |
| Green | Radio UART active (a byte arrived in the last 10 s) |
| Blue | BLE peer connected (either role) |
| Cyan | Both at once |

"Radio active" is inferred from UART RX activity because the bay has no
presence pin: telemetry streams continuously, and an AT burst refreshes the
window. During a USB-only bench your own commands and injected frames also
count as radio traffic, so green/cyan is expected there.

### Validation status

Checked on hardware with both BLE hosts:

| Check | Bluedroid | NimBLE |
|---|---|---|
| Chip / flash / crystal (ESP32-D0WDQ6-V3, 8 MB W25Q64, 40 MHz) | PASS | PASS |
| Boot + advertising (`BTWifiMod`, MAC `B4:8A:0A:BE:E6:3A`) | PASS | PASS |
| AT bench (`tools/at_bench.py`, 16 checks) | PASS | PASS |
| GATT `0xFFF0` primary / `0xFFF6` READ+WRITE+NOTIFY / CCCD `0x2902` | PASS | PASS |
| GATT value `11-22-33` read from the phone | PASS | PASS |
| Connection held with no `DisConnected` | PASS (90 s) | PASS |
| UART -> notification path (S.Port frames) | PASS (657) | PASS (657) |
| Telemetry decode in the phone app (nine sensors, right units) | PASS | PASS |
| Status LED: red / green / blue / cyan | PASS | PASS |

The BT MAC is derived from the WiFi MAC (`b4:8a:0a:be:e6:38` ->
`b4:8a:0a:be:e6:3a`).

Note that the `Connected:<addr>` line repeats every 2 s while a peripheral is
connected (`runBTPeripherial()` in `terminal.c`): that is the firmware keep-alive
for EdgeTX, not a reconnect loop. A real disconnect emits `DisConnected` plus
two `ERROR` lines.

Still pending, so the target stays a reference build and is not part of
`build-firmware.yml`:

- central mode against a real peripheral and the headtracker test,
- the radio bay (J4) with EdgeTX, which is the only path that validates the
  CH442E split and the real protocol handshake.

Two bench caveats found on this board:

- Run the AT bench with the phone disconnected: while a peripheral is
  connected, the 2 s `Connected:` keep-alive lands inside the "no response"
  windows of `AT+DISC?` and `AT+CLEAR` and makes those checks fail.
- `tools/at_bench.py` sends `AT+NAME`, which persists in NVS. Restore the name
  afterwards (`AT+NAMEBTWifiMod`) if the board is going to be handed over.
