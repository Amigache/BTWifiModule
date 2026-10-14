"""Hardware validation battery for the BTWifiModule.

Uses two boards (one per FTDI adapter) and, optionally, a BLE headtracker:

  - AT command bench on each board (delegated to tools/at_bench.py).
  - Telemetry passthrough board-to-board, in both directions, which also
    exercises interop between the Bluedroid and NimBLE builds.
  - Headtracker detection, channel passthrough and reset.

Usage:
    python tools/battery.py --a COM10 --b COM33 --ht EA651F6D3D87

Board A is expected on the first FTDI, board B on the second. Run it with the
two boards flashed with the firmware under test (any mix of BLE hosts).
"""

import argparse
import os
import re
import subprocess
import sys
import time

import serial

BAUD = 115200
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

results = []


def check(name, ok, detail=''):
    results.append((name, bool(ok)))
    print(f"[{'PASS' if ok else 'FAIL'}] {name}" + (f"  ({detail})" if detail else ''))


def drain(s, idle=0.6, cap=10.0):
    start = time.time()
    last = time.time()
    buf = b''
    while True:
        chunk = s.read(256)
        if chunk:
            buf += chunk
            last = time.time()
        if time.time() - last > idle:
            break
        if time.time() - start > cap:
            break
    return buf


def at(port, cmd, wait=2.0, idle=0.6):
    """Send one AT command and return everything read back."""
    with serial.Serial(port, BAUD, timeout=0.1, write_timeout=2) as s:
        s.reset_input_buffer()
        s.write(cmd.encode() + b'\r\n')
        time.sleep(wait)
        return drain(s, idle=idle)


def read_frames(port, seconds):
    """Open the port, wait, and return the raw bytes read."""
    with serial.Serial(port, BAUD, timeout=0.1, write_timeout=2) as s:
        s.reset_input_buffer()
        time.sleep(seconds)
        return s.read(16000)


def frames(buf):
    return buf.count(b'\x7e')


def local_addr(resp, role):
    m = re.search(rb'(' + role.encode() + rb'):([0-9A-Fa-f]{12})', resp)
    return m.group(2).decode().upper() if m else None


def at_bench(port, label):
    code = subprocess.call([sys.executable, os.path.join(HERE, 'at_bench.py'), port], cwd=ROOT)
    check(f'AT bench {label}', code == 0)


def start_sender(port):
    return subprocess.Popen([sys.executable, os.path.join(HERE, 'sport_sender.py'), port],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def telemetry_pair(periph_port, central_port, label):
    """Peripheral on `periph_port`, central on `central_port`."""
    addr = local_addr(at(periph_port, 'AT+ROLE0'), 'Peripheral')
    check(f'{label}: peripheral announces address', addr is not None, addr or '')

    at(central_port, 'AT+ROLE1')
    at(central_port, 'AT+CLEAR', wait=1.0)
    scan = at(central_port, 'AT+DISC?', wait=6.0)
    check(f'{label}: central scans the peer', bool(addr) and addr.encode() in scan)

    conn = at(central_port, f'AT+CON{addr}', wait=6.0)
    check(f'{label}: central connects', b'Connected:' in conn)

    sender = start_sender(periph_port)
    time.sleep(1.0)
    data = read_frames(central_port, 5.0)
    sender.terminate()
    sender.wait()
    check(f'{label}: telemetry reaches the central UART', frames(data) > 0,
          f'{frames(data)} frames')

    at(central_port, 'AT+CLEAR', wait=1.0)
    at(periph_port, 'AT+ROLE0', wait=1.0)


def headtracker(port, addr):
    at(port, 'AT+ROLE1')
    at(port, 'AT+CLEAR', wait=1.0)
    scan = at(port, 'AT+DISC?', wait=6.0)
    if addr.encode() not in scan:
        check('Headtracker: found in scan', False, 'not advertising?')
        return
    conn = at(port, f'AT+CON{addr}', wait=6.0)
    check('Headtracker: detected as HeadTracker', b'Board:HeadTracker' in conn)
    data = read_frames(port, 5.0)
    check('Headtracker: channels reach the UART', frames(data) > 0, f'{frames(data)} frames')
    check('Headtracker: AT+HTRESET', b'OK+HTRESET' in at(port, 'AT+HTRESET', wait=1.0))
    at(port, 'AT+CLEAR', wait=1.0)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--a', default='COM10', help='FTDI port of board A')
    parser.add_argument('--b', default='COM33', help='FTDI port of board B')
    parser.add_argument('--ht', default='', help='BLE headtracker address (optional)')
    args = parser.parse_args()

    print('=== AT command bench ===')
    at_bench(args.a, f'A ({args.a})')
    at_bench(args.b, f'B ({args.b})')

    print('\n=== Telemetry board-to-board ===')
    telemetry_pair(args.a, args.b, f'A(peripheral {args.a}) -> B(central {args.b})')
    telemetry_pair(args.b, args.a, f'B(peripheral {args.b}) -> A(central {args.a})')

    if args.ht:
        print('\n=== Headtracker ===')
        headtracker(args.a, args.ht.upper())

    failed = [r for r in results if not r[1]]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed")
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
