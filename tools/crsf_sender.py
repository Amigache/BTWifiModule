"""CRSF telemetry sender for bench testing the BTWifiModule.

Injects CRSF frames into the module UART (115200 baud) so the module forwards
them over BLE to the phone app. Frames use the EdgeTX/CRSF flight-controller
address 0xC8, which the firmware rewrites to 0xEA for TelemetryViewer
(`BT_CRSF_VIEWER_COMPAT`).

Frame layout:
    addr, length, type, payload..., crc8
    length = payload length + 2 (type and crc)
    crc8  = DVB-S2 (poly 0xD5, init 0) over type + payload

Sensor values follow the CRSF units used by EdgeTX and TelemetryViewer:
    battery voltage/current in 0.1 V / 0.1 A, GPS altitude - 1000, GPS
    groundspeed in 0.1 km/h steps and vario in cm/s.

Usage:
    python tools/crsf_sender.py COM10
"""

import argparse
import struct
import time

import serial

ADDR_FLIGHT_CONTROLLER = 0xC8

TYPE_GPS = 0x02
TYPE_VARIO = 0x07
TYPE_BATTERY = 0x08
TYPE_LINK_STATS = 0x14
TYPE_ATTITUDE = 0x1E
TYPE_FLIGHT_MODE = 0x21


def crc8(data):
    """DVB-S2 CRC8 (poly 0xD5, init 0) as used by CRSF."""
    crc = 0
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = ((crc << 1) ^ 0xD5) & 0xFF if crc & 0x80 else (crc << 1) & 0xFF
    return crc


def crsf_frame(frame_type, payload):
    """Build one complete CRSF frame, including address, length and CRC."""
    body = bytes([frame_type]) + payload
    return bytes([ADDR_FLIGHT_CONTROLLER, len(body) + 1]) + body + bytes([crc8(body)])


FRAMES = [
    # Link statistics: RSSI -95 dBm, 100 % LQ, 8 dB SNR (uplink and downlink).
    crsf_frame(TYPE_LINK_STATS, bytes([95, 95, 100, 8, 0, 2, 3, 95, 100, 8])),
    # Battery: 12.6 V, 10.5 A, 1500 mAh used, 70 % remaining.
    crsf_frame(TYPE_BATTERY, struct.pack('>HH', 126, 105) + (1500).to_bytes(3, 'big') + bytes([70])),
    # Attitude: pitch 0.1 rad, roll -0.2 rad, yaw 1.5 rad.
    crsf_frame(TYPE_ATTITUDE, struct.pack('>hhh', 1000, -2000, 15000)),
    # Vario: -2.50 m/s (cm/s).
    crsf_frame(TYPE_VARIO, struct.pack('>h', -250)),
    # GPS: 321 m altitude, 9 satellites, ~83 km/h groundspeed, heading 90 deg.
    # Groundspeed is in 0.1 km/h steps (the app shows 830 as 83 km/h).
    crsf_frame(TYPE_GPS, struct.pack('>ii', 0, 0) + struct.pack('>HHH', 830, 9000, 1321) + bytes([9])),
    # Flight mode string.
    crsf_frame(TYPE_FLIGHT_MODE, b'ACRO'),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('port', nargs='?', default='COM10', help='serial port wired to module RX (default COM10)')
    parser.add_argument('--baud', type=int, default=115200)
    args = parser.parse_args()

    s = serial.Serial(args.port, args.baud, timeout=0.4)
    print(f"Sending CRSF telemetry on {args.port}...", flush=True)
    try:
        while True:
            for frame in FRAMES:
                s.write(frame)
                time.sleep(0.05)
    except KeyboardInterrupt:
        pass
    finally:
        s.close()


if __name__ == '__main__':
    main()
