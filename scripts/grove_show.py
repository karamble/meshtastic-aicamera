#!/usr/bin/env python3
# Send `grove-show` to the C3 and print the per-slot alias/class table.
# Usage: grove_show.py PORT

import sys
import time

try:
    import serial
except ImportError:
    sys.stderr.write("pyserial not installed (pip3 install --user pyserial)\n")
    sys.exit(2)


def main(argv):
    if len(argv) != 2:
        sys.stderr.write("usage: grove_show.py PORT\n")
        return 2
    port = argv[1]

    p = serial.Serial()
    p.port     = port
    p.baudrate = 115200
    p.timeout  = 0.3
    p.dtr      = False
    p.rts      = False
    try:
        p.open()
    except serial.SerialException as e:
        sys.stderr.write("could not open %s: %s\n" % (port, e))
        return 1

    def safe_readline():
        # C3 USB CDC disappears briefly during the host-open reset.
        try:
            return p.readline()
        except serial.SerialException:
            time.sleep(0.1)
            return b""

    # Wait through boot + BLE handshake before sending.
    time.sleep(4.0)
    try:
        p.reset_input_buffer()
    except serial.SerialException:
        pass
    p.write(b"grove-show\n")
    p.flush()

    # Reply: "current bound slot:" header, then "  slot N alias=... classes=..."
    # rows. Stop on "ai>" or a short idle gap.
    end = time.time() + 3.0
    in_reply  = False
    captured  = []
    last_data = time.time()
    while time.time() < end:
        line = safe_readline()
        if not line:
            if in_reply and time.time() - last_data > 0.5:
                break
            continue
        try:
            text = line.decode("utf-8", "replace").rstrip()
        except Exception:
            continue
        if not text:
            continue
        last_data = time.time()
        if text.startswith("current bound slot:"):
            in_reply = True
            captured.append(text)
            continue
        if in_reply:
            if text.startswith("ai>") or text.startswith("alias table:") or text.startswith("  slot "):
                captured.append(text.replace("ai>", "").rstrip())
                if text.startswith("ai>"):
                    break
                continue
            # Other interleaved log lines: ignore.

    p.close()

    if not captured:
        sys.stderr.write("no reply from C3. Is the bridge firmware flashed and running?\n")
        return 1

    for line in captured:
        if line:
            sys.stdout.write(line + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
