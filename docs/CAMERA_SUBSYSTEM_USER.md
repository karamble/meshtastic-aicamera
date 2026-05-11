# Camera Subsystem Setup

The imaging side of the kit is a Seeed Grove Vision AI V2 carrier with a Himax WE-2 SoC and an OV5647 camera. A XIAO ESP32-C3 sits in the carrier socket and bridges WE-2 detections to the Meshtastic node over Bluetooth.

For protocol-level details (SSCMA AT commands, flash layout, model addresses) see `CAMERA_SUBSYSTEM_TECHNICAL.md`.

## Quick deploy

From the repo root, with both the AI carrier and the XIAO ESP32-C3 plugged into USB:

```
make grove-pick          # optional, pick a model bundle from the SenseCraft catalog
make flash-grove         # WE-2 firmware + selected models   (press RESET when prompted)
make flash-c3            # bridge firmware
make set-ble-pin         # writes BLE_PIN from .env to the bridge's NVS
make set-grove-aliases   # writes the per-slot alias + class table to the bridge's NVS
```

Skip `grove-pick` to use the default 5-model bundle (`person`, `face`, `gesture`, `pet`, `apple`). Skip `set-grove-aliases` if you stuck with that default.

## Make targets

| Target | What it does |
|---|---|
| `grove-pick` | Interactive picker. Fetches the SenseCraft catalog, prints a numbered table, prompts for up to 5 models. Empty input keeps the current selection. Writes `firmware/grove/models/selected.txt`. |
| `fetch-grove-firmware` | Downloads the pinned WE-2 firmware (`GROVE_FIRMWARE_VERSION` in `.env`) to `firmware/grove/`. |
| `flash-grove` | Streams firmware + the manifest's models to the WE-2 over xmodem. Models land at `0x400000`, `0x600000`, `0x800000`, `0xA00000`, `0xB7B000`. |
| `flash-c3` | PlatformIO upload of the bridge firmware to the XIAO ESP32-C3. |
| `set-ble-pin` | Pushes `BLE_PIN` from `.env` to the C3's NVS over USB. No firmware reflash. |
| `set-grove-aliases` | Pushes one `grove-set <slot> <alias> <classes>` per manifest entry to the C3's NVS. |
| `grove-show` | Reads the current per-slot alias and class table from the C3's NVS. |
| `mesh-info` | Reads Meshtastic node state over USB. |
| `help` | List all targets. |

## Prerequisites

- Both USB-C ports cabled: the **AI carrier** (WE-2 via WCH CH343, `1a86:55d3`) and the **XIAO module** (ESP32-C3, `303a:1001`). The Makefile auto-detects each via `/dev/serial/by-id/`.
- Python tooling:

  ```
  pip3 install --user pyserial==3.5 xmodem==0.4.7
  ```

- Your user in the `dialout` group:

  ```
  sudo usermod -aG dialout "$USER"   # log out and back in after
  ```

- `.env` at the repo root. `cp .env.example .env` and set `BLE_PIN` to match the Fixed PIN you configured in the Meshtastic Android app.

## During `make flash-grove`

The xmodem script will print `Please press reset button!!` and stall. Press the small **RESET button on the AI carrier board** (not the XIAO) to hand the bootloader to xmodem. The transfer takes about a minute end-to-end.

A trailing `termios.error` after `xmodem_send bin file done!!` is the script's self-reboot handshake and is harmless.

Once the script returns, power-cycle the carrier (unplug + replug its USB-C) to boot the new firmware.

## Verifying the bridge

```
make grove-show          # dump the per-slot alias + class table
```

Expected output for the default bundle:

```
current bound slot: 1
alias table:
  slot  1  alias=person        classes=person
  slot  2  alias=face          classes=face
  slot  3  alias=gesture       classes=paper|rock|scissors
  slot  4  alias=pet           classes=cat|dog
  slot  5  alias=apple         classes=apple
```

The boot log on the C3's USB CDC (115200 baud, `dtr=False, rts=False`) shows:

```
=== ai-cam-bridge (interactive SSCMA) ===
AI.begin -> OK
device='Grove Vision AI V2' id='<hex>'
auto: bound model 1, algo=0 (firmware auto-detect)
BLE PIN: <value> (from NVS, default 123456)
grove: 5 slot(s) in alias table (from NVS)
mesh: BLE central started, scanning for Meshtastic 'cam_*' peer...
```

Reading the USB CDC reboots the C3 once on host-open. The BLE handshake to the Meshtastic node settles about 5 seconds after the boot banner.

## REPL commands

The bridge exposes an interactive REPL on its USB CDC. Type `help` for the full list. Most-used:

| Command | Effect |
|---|---|
| `model` | Print the per-slot alias and class-label table |
| `model <name>` | Hot-swap the bound model by alias |
| `bind <id>` | Hot-swap by numeric slot id |
| `invoke 1` | Run one inference, print boxes |
| `watch on` / `watch off` | Toggle continuous-poll-and-forward (default on) |
| `mesh-status` | Show BLE state |
| `mesh-test <text>` | Send a one-off broadcast over the mesh |
| `ble-pin` / `ble-pin <N>` | Show or set the BLE pairing PIN |
| `grove-show` | Dump the alias and class table |
| `grove-set <n> <alias> <cls\|...>` | Edit one slot |
| `grove-reset` | Clear all per-slot NVS, revert to firmware defaults |

## Reading the C3's USB CDC

Use this pyserial pattern, not `cat`:

```python
import serial
s = serial.Serial()
s.port = '/dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_<id>-if00'
s.baudrate = 115200
s.timeout = 0.2
s.dtr = False
s.rts = False
s.open()
while True:
    print(s.read(s.in_waiting or 1).decode('ascii', errors='replace'), end='', flush=True)
```

The same rule applies to the WE-2's CH343 port. Default `cat` or `screen` reads as silent because DTR/RTS are asserted, holding the firmware in reset.
