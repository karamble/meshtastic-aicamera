# meshtastic-ai-camera

An ML camera that broadcasts detections over a Meshtastic LoRa mesh. A XIAO ESP32-C3 bridges a Seeed Grove Vision AI V2 (Himax WE-2 NPU running a YOLO-family model) to a Meshtastic node (XIAO ESP32-S3 + Wio-SX1262) over Bluetooth. The bridge sends one text line per detection in the form `@CAM TRIGGERED:<class>@<score>` on the primary channel.

## Hardware

| Component | Role |
|---|---|
| Seeed Grove Vision AI V2 | Camera + Himax WE-2 SoC running the ML model |
| XIAO ESP32-C3 (in the carrier socket) | Bridge MCU, runs the firmware in this repo |
| XIAO ESP32-S3 + Wio-SX1262 | Meshtastic LoRa node |

The C3 talks I²C to the WE-2 (camera detections in) and BLE to the Meshtastic node (text frames out). No host PC is needed at runtime.

## Quick start

Prerequisites once per machine:

```
pip3 install --user pyserial==3.5 xmodem==0.4.7 meshtastic
```

Per kit, from the repo root:

```
cp .env.example .env                # adjust BLE_PIN if you set a non-default in the Meshtastic app
make grove-pick                     # optional, pick a custom model bundle from the SenseCraft catalog
make flash-grove                    # WE-2 firmware + selected models
make flash-c3                       # bridge firmware
make set-ble-pin                    # writes BLE_PIN from .env to the bridge's NVS
make set-grove-aliases              # pushes the per-slot alias + class labels to the bridge's NVS
```

Then configure the Meshtastic node from the Meshtastic Android/iOS app per `docs/MESHTASTIC_NODE_USER.md`. The bridge will pair automatically and announce `@CAM bridge online` on the mesh once the node is up.

## Make targets

| Target | What it does |
|---|---|
| `build` | Compile bridge firmware without flashing |
| `flash-c3` | Flash bridge firmware to the XIAO ESP32-C3 |
| `flash-grove` | Flash WE-2 firmware + the manifest models to the Grove Vision AI V2 |
| `fetch-grove-firmware` | Download the pinned WE-2 firmware to `firmware/grove/` |
| `grove-pick` | Interactive picker for the WE-2 model bundle from the SenseCraft catalog |
| `grove-show` | Read the per-slot alias and class table from the C3's NVS |
| `set-ble-pin` | Push `BLE_PIN` from `.env` into the C3's NVS (no reflash) |
| `set-grove-aliases` | Push the per-slot alias and class-label table to the C3's NVS (no reflash) |
| `mesh-info` | Read Meshtastic node state over USB |
| `help` | List all targets |

## Documentation

- `docs/PROJECT_OVERVIEW.md` - architecture and data flow
- `docs/CAMERA_SUBSYSTEM_USER.md` - bring up the Grove Vision AI V2, plus the full runtime reference (mesh verbs, Wi-Fi operator console, USB REPL)
- `docs/CAMERA_SUBSYSTEM_TECHNICAL.md` - WE-2 protocol and flash layout reference
- `docs/MESHTASTIC_NODE_USER.md` - configure the Meshtastic node via the mobile app

## License

MIT. See `LICENSE`. The vendored `scripts/xmodem_send.py` is MIT (c) 2023 Himax Technologies, Inc.
