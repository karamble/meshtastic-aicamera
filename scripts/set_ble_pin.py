#!/usr/bin/env python3
# Push a new BLE PIN to the C3's NVS via its USB CDC REPL.
# Usage: set_ble_pin.py PORT PIN     (PIN: 1..999999)

import sys
import time

try:
    import serial
except ImportError:
    sys.stderr.write("pyserial not installed (try: pip3 install --user pyserial)\n")
    sys.exit(2)


def main(argv):
    if len(argv) != 3:
        sys.stderr.write("usage: set_ble_pin.py PORT PIN\n")
        return 2

    port = argv[1]
    try:
        pin = int(argv[2])
    except ValueError:
        sys.stderr.write("PIN must be an integer (1..999999)\n")
        return 2
    if not (1 <= pin <= 999999):
        sys.stderr.write("PIN must be 1..999999\n")
        return 2

    p = serial.Serial()
    p.port = port
    p.baudrate = 115200
    p.timeout = 0.3
    p.dtr = False
    p.rts = False
    try:
        p.open()
    except serial.SerialException as e:
        sys.stderr.write("could not open %s: %s\n" % (port, e))
        return 1

    # Opening the CDC reboots the C3; drain boot banner before sending.
    end = time.time() + 4.0
    boot_seen = False
    while time.time() < end:
        line = p.readline()
        if not line:
            continue
        try:
            text = line.decode("utf-8", "replace")
        except Exception:
            continue
        if "ai>" in text or "ai-cam-bridge" in text:
            boot_seen = True
    if not boot_seen:
        sys.stderr.write("warning: didn't see boot banner; sending anyway\n")

    cmd = "ble-pin %d\n" % pin
    p.write(cmd.encode("ascii"))
    p.flush()

    # Accept either the explicit "stored N in NVS" log line or a fresh
    # post-restart boot banner as evidence the write+reboot path fired.
    end = time.time() + 10.0
    success = False
    failure_line = None
    while time.time() < end:
        line = p.readline()
        if not line:
            continue
        try:
            text = line.decode("utf-8", "replace").strip()
        except Exception:
            continue
        if not text:
            continue
        if text.startswith("ble-pin:") or "ble-pin" in text:
            sys.stdout.write(text + "\n")
            if "stored" in text and "NVS" in text:
                success = True
                break
            if "FAILED" in text or "must be" in text:
                failure_line = text
                break
        elif "=== ai-cam-bridge" in text:
            sys.stdout.write("(detected post-restart boot banner)\n")
            success = True
            break

    p.close()

    if success:
        sys.stdout.write("BLE PIN written: %d. The C3 will reboot and re-pair using the new PIN.\n" % pin)
        return 0
    if failure_line:
        sys.stderr.write("firmware rejected the command: %s\n" % failure_line)
        return 1
    sys.stderr.write("no confirmation from the C3 within 2.5s. Check that the firmware build includes the `ble-pin` REPL command.\n")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
