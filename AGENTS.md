# AGENTS.md

Guidance for AI coding agents (and humans) working in this repository. Follow
these rules to keep the codebase coherent and professional.

## Project

BTWifiModule is a FrSky Bluetooth emulator for the ESP32, used inside
OpenTX/EdgeTX radios. It emulates the FrSky Bluetooth module over a UART link
and exposes BLE (GATT service `0xFFF0`, characteristic `0xFFF6`).

- Primary target: **ESP32-C3 SuperMini** (`PCB_C3SUPERMINI`).
- BLE hosts: **Bluedroid** (default) and **NimBLE** (`USE_NIMBLE`), sharing the
  same application code behind `#if defined(USE_NIMBLE)`.
- Reference radio: Radiomaster TX16S MK3 with EdgeTX.

## Language

- **All code comments, docstrings, commit messages, and documentation are in
  English.** Do not introduce Spanish (or any other language) comments.
- Public APIs are documented with Doxygen (`/** ... */`) on every function and
  type in the headers.

## Coding conventions

- C99, ESP-IDF style.
- `snake_case` for functions and files; module prefixes are mandatory:
  - `bt_*` / `bt_addr_*` / `at_command_*` / `cbuf_*` / `settings_*`
  - `bt_server_*` (peripheral), `bt_client_*` (central)
- `role_t`, `ble_board_type`, `at_command_t` are the shared enums.
- Headers use `#pragma once`.
- Pure, platform-independent logic lives in dedicated modules (`bt_addr`,
  `at_command`, `cbuf`) so it can be unit-tested on the host.

## Architecture

```
src/
  main.c          Entry point (NVS + UART task).
  board.h         Board config: UART/pins and shared types.
  at_command.c/h  Pure AT command parser (host-testable).
  bt_addr.c/h     Pure BT address conversion (host-testable).
  cb.c/h          Byte circular buffer (host-testable).
  settings.c/h    Persistent config (NVS) with protected access.
  bt.c/h          Common BT stack control (Bluedroid/NimBLE).
  bt_server.c/h   GATT peripheral (0xFFF0).
  bt_client.c/h   GATT central (scan/connect/auto-connect).
  terminal.c/h    UART link: AT dispatch + connection state machines.
test/             Unity unit tests (native).
tools/            Bench tooling: sport_sender.py, at_bench.py.
doc/BENCH.md      Bench setup, procedure and validated targets.
```

### Test conventions (PlatformIO Unity, native)

PlatformIO links every `test/*.c` file into a single native binary, so there is
**exactly one `main()`** (in `test/test_main.c`). Each module test file only
defines `test_*` functions plus a `run_xxx_tests()` that invokes `RUN_TEST` for
its tests; `test_main.c` calls every `run_xxx_tests()` between
`UNITY_BEGIN()`/`UNITY_END()`. Do not add `main()`/`app_main()` to module test
files.

### Bench conventions (hardware)

Hardware validation lives in `doc/BENCH.md` and the scripts under `tools/`
(`sport_sender.py` for S.Port injection, `at_bench.py` for the AT command
bench). Run `tools/at_bench.py` before marking a target validated; all checks
must pass. Keep the validated-target matrix in `doc/BENCH.md` and the summary in
`README.md` up to date when a target is tested.

## Concurrency rules (do not regress)

- BT GAP/GATT callbacks run in the BT controller task; the UART task runs the
  state machines. Shared state must be synchronized.
- **Never call blocking NVS writes from a BT callback.** Use
  `settings_request_save()` and let the UART task flush it via
  `settings_process()`.
- The scanned-device list is protected by a mutex; use the accessors
  `bt_client_scanned_count()` / `bt_client_scanned_get()` instead of touching
  the array directly.
- `volatile bool` connection flags are single-byte and single-core safe, but do
  not add new shared multi-byte state without a mutex.

## EdgeTX protocol contract (do not change blindly)

Key invariants:

- All responses are terminated with `\r\n`.
- The real ACK that advances EdgeTX is the `Central:<addr>` / `Peripheral:<addr>`
  line emitted by `sendBTMode()` after `AT+TXPW` / `AT+ROLE`.
- Trainer and S.Port are `0x7E`-delimited and byte-stuffed (`0x7D ^ 0x20`);
  CRSF/Ghost are forwarded raw. The module is a pure passthrough.
- Master trainer = central (`AT+ROLE1`), slave/telemetry = peripheral
  (`AT+ROLE0`).
- The firmware stays at 115200 baud; `AT+BAUD4` is acknowledged implicitly by
  EdgeTX switching to 115200 without checking a response.

## Build & test

```bash
pio run -e C3SuperMini          # Bluedroid
pio run -e C3SuperMini_nimble   # NimBLE
pio test -e native              # host unit tests (requires gcc)
```

- Platform: `espressif32@6.10.0` (ESP-IDF v5.4.0), matching the committed
  `sdkconfig.*` files.
- CI builds `C3SuperMini`, `C3SuperMini_nimble`, `C3Mini`, `pico32`,
  `Node32s_Wroom32` and `RM_pico`. Only the ESP32-C3 SuperMini targets are
  hardware-validated with this firmware (see `doc/BENCH.md`).
- The logging console is routed to USB Serial JTAG (not UART0), because the
  radio uses GPIO20/21 on the C3 SuperMini.

## Definition of done

- Compiles clean (no warnings) for `C3SuperMini` and `C3SuperMini_nimble`.
- New pure logic has host unit tests in `test/`.
- Public functions have Doxygen comments; all comments are in English.
