#!/usr/bin/env python3
# Assemble xmodem_send.py argv from the picker manifest and execvp it so
# xmodem_send.py owns the terminal directly (native progress bars + signals).
#
# Usage: flash_grove.py PORT FIRMWARE_IMG MANIFEST

import os
import sys
from pathlib import Path

REPO_ROOT   = Path(__file__).resolve().parent.parent
XMODEM_SEND = REPO_ROOT / "scripts" / "xmodem_send.py"


def main(argv):
    if len(argv) != 4:
        sys.stderr.write("usage: flash_grove.py PORT FIRMWARE_IMG MANIFEST\n")
        return 2

    port, firmware_img, manifest_path = argv[1], argv[2], argv[3]
    manifest = Path(manifest_path)
    if not manifest.exists():
        sys.stderr.write("manifest not found: %s\n" % manifest)
        return 1

    args = [
        "python3", str(XMODEM_SEND),
        "--port=" + port,
        "--baudrate=921600",
        "--protocol=xmodem",
        "--file=" + firmware_img,
    ]
    for raw in manifest.read_text().splitlines():
        raw = raw.strip()
        if not raw:
            continue
        parts = raw.split("\t")
        if len(parts) < 5:
            sys.stderr.write("malformed manifest line: %r\n" % raw)
            return 1
        _slot, addr, path, _alias, _classes = parts[:5]
        args.append("--model=%s %s 0x00000" % (path, addr))

    os.execvp(args[0], args)   # does not return on success
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
