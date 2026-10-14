"""AT command bench for the BTWifiModule firmware.

Drives the module over the UART link (FTDI adapter) and checks the response of
every AT command exposed by the firmware, including the role restrictions and
the commands that intentionally produce no response. Run it once per firmware
variant (Bluedroid and NimBLE).

The BLE side (master/slave/telemetry) needs a phone peer and is tested
manually following doc/BENCH.md.

Usage:
    python tools/at_bench.py COM10
"""

import argparse
import sys
import time

import serial


class Bench:
    def __init__(self, port, baud):
        # A finite write timeout avoids hanging forever if the serial driver
        # stalls (which would leave the port locked by a zombie process).
        self.ser = serial.Serial(port, baud, timeout=0.1, write_timeout=2)
        self.ser.reset_input_buffer()
        self.results = []

    def drain(self, idle=0.4, cap=6.0):
        """Read until the line goes idle for `idle` seconds or `cap` elapses."""
        start = time.time()
        last = time.time()
        buf = b''
        while True:
            chunk = self.ser.read(128)
            if chunk:
                buf += chunk
                last = time.time()
            if time.time() - last > idle:
                break
            if time.time() - start > cap:
                break
        return buf

    def send(self, cmd, idle=0.8, cap=8.0):
        try:
            self.ser.reset_input_buffer()
            self.ser.write(cmd + b'\r\n')
        except serial.SerialException as exc:
            return f"<serial error: {exc}>"
        return self.drain(idle=idle, cap=cap).decode('ascii', 'replace')

    def check(self, name, response, expected):
        ok = all(e in response for e in expected)
        self.results.append((name, ok, response, expected))
        status = 'PASS' if ok else 'FAIL'
        print(f"[{status}] {name}")
        if not ok:
            print(f"        expected any of: {expected}")
            print(f"        got: {response!r}")

    def check_empty(self, name, response):
        ok = response.strip() == ''
        self.results.append((name, ok, response, 'no response'))
        status = 'PASS' if ok else 'FAIL'
        print(f"[{status}] {name}")
        if not ok:
            print(f"        expected no response, got: {response!r}")

    def summary(self):
        failed = [r for r in self.results if not r[1]]
        print(f"\n{len(self.results) - len(failed)}/{len(self.results)} passed")
        return 1 if failed else 0


def run(port, baud, name):
    b = Bench(port, baud)

    # Settle first: switching roles (and tearing down a previous connection)
    # can delay the response lines of the first command, which would otherwise
    # spill into the next test window.
    b.send(b'AT+ROLE0', idle=1.5, cap=10.0)
    time.sleep(1.5)
    b.drain(idle=1.5, cap=3.0)

    # --- Peripheral (slave) mode -------------------------------------------
    b.check('AT+ROLE0', b.send(b'AT+ROLE0'), ['OK+Role:0', 'Peripheral:'])
    b.check('AT+CON in peripheral -> ERROR', b.send(b'AT+CONaabbccddeeff'), ['ERROR'])
    b.check_empty('AT+DISC? in peripheral -> no response', b.send(b'AT+DISC?', idle=0.8, cap=2.0))
    b.check_empty('AT+CLEAR in peripheral -> no response', b.send(b'AT+CLEAR', idle=0.8, cap=2.0))
    b.check('AT+HTRESET in peripheral -> ERROR', b.send(b'AT+HTRESET'), ['ERROR'])

    # --- Common commands (mode line follows) -------------------------------
    cmd = f'AT+NAME{name}'.encode()
    b.check('AT+NAME', b.send(cmd), [f'OK+Name:{name}', 'Peripheral:'])
    b.check('AT+TXPW', b.send(b'AT+TXPW3'), ['OK+Txpw:0', 'Peripheral:'])
    b.check_empty('AT+BAUD -> no response', b.send(b'AT+BAUD4', idle=0.8, cap=2.0))
    b.check('link alive after AT+BAUD', b.send(b'AT+TXPW3'), ['OK+Txpw:0'])
    b.check_empty('unknown command -> no response', b.send(b'AT+FOO', idle=0.8, cap=2.0))

    # --- Central (master) mode ---------------------------------------------
    b.check('AT+ROLE1', b.send(b'AT+ROLE1'), ['OK+Role:1', 'Central:'])
    b.check('AT+CLEAR in central', b.send(b'AT+CLEAR'), ['OK+CLEAR'])
    b.check('AT+HTRESET in central -> ERROR', b.send(b'AT+HTRESET'), ['ERROR'])
    b.check('AT+DISC? in central', b.send(b'AT+DISC?', idle=1.0, cap=12.0), ['OK+DISCS', 'OK+DISCE'])
    b.check('AT+CON in central', b.send(b'AT+CONaabbccddeeff'),
            ['OK+CONNA', 'Connecting to:aabbccddeeff'])
    b.send(b'AT+CLEAR')  # stop the connect attempt before leaving central

    # --- Back to peripheral for the telemetry bench ------------------------
    b.check('AT+ROLE0 (restore)', b.send(b'AT+ROLE0'), ['OK+Role:0', 'Peripheral:'])

    b.ser.close()
    return b.summary()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('port', nargs='?', default='COM10', help='serial port wired to module UART (default COM10)')
    parser.add_argument('--baud', type=int, default=115200)
    parser.add_argument('--name', default='NIMBLE1', help='name used by the AT+NAME test')
    args = parser.parse_args()
    print(f"AT bench on {args.port}\n")
    return run(args.port, args.baud, args.name)


if __name__ == '__main__':
    sys.exit(main())
