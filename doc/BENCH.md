# Bench setup

Reference bench for validating the BTWifiModule firmware on hardware. Use it
to repeat the same checks for each BLE host (Bluedroid and NimBLE).

## Hardware

- ESP32-C3 SuperMini (`PCB_C3SUPERMINI`), flashed over native USB.
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
pio run -e C3SuperMini -t upload          # Bluedroid
pio run -e C3SuperMini_nimble -t upload   # NimBLE
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
