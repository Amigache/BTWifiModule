"""S.Port telemetry sender for bench testing the BTWifiModule.

Injects FrSky S.Port data frames into the module UART (115200 baud) so the
module forwards them over BLE (or classic BT) to the phone app.

Frame layout (after the 0x7E start byte, byte-stuffed with 0x7D ^ 0x20):
    physical_id, 0x10, data_id (LE16), value (LE32), crc

The CRC covers the bytes after the physical ID (frame type through value):
    crc = 0xFF - (carry-folded sum)
which is the check used by EdgeTX and by the Android app.

Sensor values use the encoding expected by the Android app (RomanLut fork):
    RSSI (0xF101) is read from the most significant byte of the value.
    GPS speed (0x0830) is in knots x1000.
"""

import argparse
import time

import serial


def stuff(data):
    """Byte-stuff a frame body: 0x7E and 0x7D are escaped as 0x7D, byte ^ 0x20."""
    out = bytearray()
    for b in data:
        if b in (0x7E, 0x7D):
            out.append(0x7D)
            out.append(b ^ 0x20)
        else:
            out.append(b)
    return bytes(out)


def sport_frame(physical_id, data_id, value):
    """Build one complete S.Port data frame, including start byte and CRC."""
    body = (bytes([0x10])
            + data_id.to_bytes(2, 'little')
            + value.to_bytes(4, 'little', signed=True))
    total = 0
    for b in body:
        total += b
        total = (total & 0xFF) + (total >> 8)
    crc = 0xFF - (total & 0xFF)
    inner = bytes([physical_id]) + body + bytes([crc])
    return b'\x7E' + stuff(inner) + b'\x7E'


SENSORS = [
    (0xF101, 95 << 24),  # RSSI 95
    (0x0100, 12345),     # Altitude 123.45 m
    (0x0110, -250),      # Vertical speed -2.50 m/s
    (0x0210, 1140),      # Battery voltage 11.40 V
    (0x0200, 152),       # Current 15.2 A
    (0x0500, 9800),      # RPM 9800
    (0x0400, 235),       # Temperature 1 23.5 C
    (0x0820, 12000),     # GPS altitude 120.00 m
    (0x0830, 45000),     # GPS speed 45 knots
]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('port', nargs='?', default='COM10', help='serial port wired to module RX (default COM10)')
    parser.add_argument('--baud', type=int, default=115200)
    args = parser.parse_args()

    s = serial.Serial(args.port, args.baud, timeout=0.4)
    print(f"Sending S.Port telemetry on {args.port}...", flush=True)
    try:
        while True:
            for data_id, value in SENSORS:
                s.write(sport_frame(0x1B, data_id, value))
                time.sleep(0.03)
    except KeyboardInterrupt:
        pass
    finally:
        s.close()


if __name__ == '__main__':
    main()
