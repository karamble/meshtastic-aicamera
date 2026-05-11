#!/usr/bin/env python3
# Push the picker manifest to the C3's NVS as `grove-reset` + per-slot
# `grove-set <slot> <alias> <classes>` REPL commands.
# Usage: set_grove_aliases.py PORT [MANIFEST]

import sys
import time
from pathlib import Path

try:
    import serial
except ImportError:
    sys.stderr.write("pyserial not installed (pip3 install --user pyserial)\n")
    sys.exit(2)

REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_MANIFEST = REPO_ROOT / "firmware" / "grove" / "models" / "selected.txt"


def open_serial(port):
    p = serial.Serial()
    p.port     = port
    p.baudrate = 115200
    p.timeout  = 0.3
    p.dtr      = False
    p.rts      = False
    p.open()
    return p


def safe_readline(p):
    try:
        return p.readline()
    except serial.SerialException:
        time.sleep(0.1)
        return b""


def wait_for_repl(p):
    # Host-open reboots the C3; ~6-7 s to REPL prompt. Wait 8 then drain.
    time.sleep(8.0)
    try:
        p.reset_input_buffer()
    except serial.SerialException:
        pass


def send_and_wait(p, cmd, marker, timeout=3.0):
    p.write((cmd + "\n").encode("ascii"))
    p.flush()
    end = time.time() + timeout
    saw = None
    while time.time() < end:
        line = safe_readline(p)
        if not line:
            continue
        try:
            text = line.decode("utf-8", "replace").strip()
        except Exception:
            continue
        if not text:
            continue
        if marker in text:
            saw = text
            break
    return saw


def main(argv):
    if len(argv) < 2 or len(argv) > 3:
        sys.stderr.write("usage: set_grove_aliases.py PORT [MANIFEST]\n")
        return 2

    port     = argv[1]
    manifest = Path(argv[2]) if len(argv) == 3 else DEFAULT_MANIFEST

    if not manifest.exists():
        sys.stderr.write("manifest not found: %s\n" % manifest)
        sys.stderr.write("run `make grove-pick` (or `python3 scripts/grove_select.py --defaults`) first\n")
        return 1

    lines = []
    for raw in manifest.read_text().splitlines():
        if not raw.strip():
            continue
        parts = raw.split("\t")
        if len(parts) < 5:
            sys.stderr.write("malformed manifest line: %r\n" % raw)
            return 1
        slot, _addr, _path, alias, classes = parts[:5]
        lines.append((int(slot), alias, classes))

    if not lines:
        sys.stderr.write("manifest is empty\n")
        return 1

    try:
        p = open_serial(port)
    except serial.SerialException as e:
        sys.stderr.write("could not open %s: %s\n" % (port, e))
        return 1

    wait_for_repl(p)

    # Marker must be specific: the boot banner already contains "grove:"
    # ("grove: N slot(s) in alias table ..."), so we look for the more
    # specific reset reply prefix.
    reset_reply = send_and_wait(p, "grove-reset", "cleared NVS", timeout=4.0)
    if reset_reply:
        sys.stdout.write("%s\n" % reset_reply)
    else:
        sys.stderr.write("warning: no `grove-reset` ack from C3 within 4s. "
                         "The next slot writes may overlap stale NVS data.\n")

    failures = 0
    for slot, alias, classes in lines:
        cmd    = "grove-set %d %s %s" % (slot, alias, classes if classes else "?")
        marker = "grove-set: slot %d " % slot       # slot-specific to avoid cross-slot match
        reply  = send_and_wait(p, cmd, marker, timeout=4.0)
        if reply:
            sys.stdout.write("%s\n" % reply)
        else:
            sys.stderr.write("FAILED for slot %d (%s)\n" % (slot, alias))
            failures += 1

    p.close()

    if failures:
        sys.stderr.write("%d slot(s) failed\n" % failures)
        return 1
    sys.stdout.write("\nAlias + class table pushed for %d slot(s).\n" % len(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
