# Project Overview

## What the kit does

A camera looks at the world. When it sees something its model recognises, it broadcasts a text line over a LoRa mesh. No host PC, no Wi-Fi, no cloud.

## Architecture

```
┌──────────────────────────────────────┐    ┌───────────────────────────────┐
│ Grove Vision AI V2 (carrier)         │    │ Grove Base for XIAO + LoRa    │
│                                      │    │                               │
│  camera ──▶ Himax WE-2 ◀──I²C──┐     │    │  XIAO ESP32-S3 + Wio-SX1262   │
│   (OV5647)   runs YOLO         │     │    │  stock Meshtastic firmware    │
│                                ▼     │BLE │                               │
│                          XIAO ESP32-C3 ───▶ Meshtastic node               │
│                          (this repo) ◀──── @CAM TRIGGERED:person@71       │
└──────────────────────────────────────┘    └───────────────────────────────┘
```

## Component roles

| Component | Role |
|---|---|
| Himax WE-2 | Runs the ML model. Exposes detection results over I²C at address `0x62` and emits the same as JSON on its USB-UART. |
| XIAO ESP32-C3 (bridge) | Polls the WE-2 over I²C for detections, filters by confidence, applies per-class debounce, sends each event as a Meshtastic text frame over BLE. |
| XIAO ESP32-S3 + Wio-SX1262 | Stock Meshtastic node. The bridge pairs to it as a BLE client and writes text frames to the node's ToRadio characteristic. |

## Data path

1. WE-2 runs inference every frame, emits results on its I²C slave when polled.
2. Bridge calls `AI.invoke(1)` every 250 ms via the `Seeed_Arduino_SSCMA` library.
3. For each box with `score > conf_min` (default 65), the bridge looks up the COCO class label and builds `@CAM TRIGGERED:<class>@<score>`.
4. A per-class cooldown (default 30 s, configurable) drops repeats.
5. The bridge writes the text as a Meshtastic `ToRadio` protobuf packet (broadcast, channel 0, hop_limit 3) over BLE.
6. The node forwards it on LoRa to the rest of the mesh.

## Configuration model

Two tiers of configuration, separated by what changes how often:

**Build-time and flash-time** — in `.env` at the repo root (template `.env.example`):

- `BLE_PIN` matches the Fixed PIN set in the Meshtastic Android app. Pushed to the C3's NVS via `make set-ble-pin`. Survives firmware reflashes.
- `GROVE_FIRMWARE_VERSION` pins the WE-2 firmware version that `make flash-grove` downloads from the Seeed-Studio/sscma-example-we2 GitHub releases. The default `20250102` has the full 10 MB model partition needed for the 5-model bundle.

**Runtime, NVS-backed** — adjusted via mesh verbs, the Wi-Fi operator console, or the USB REPL while the bridge is running. All survive reboot. See `CAMERA_SUBSYSTEM_USER.md` for the full reference; the bridge exposes runtime controls for:

- The active model slot (`MODEL_SET`).
- The per-class detection cooldown (`DEDUP_INTERVAL`, seconds).
- The minimum confidence floor (`CONF_THRESHOLD`, 1..100, strict `>`).
- The periodic heartbeat (`HB_ON` / `HB_OFF` / `HB_INTERVAL`).
- The armed state (`START` / `STOP`).
- The on-demand Wi-Fi operator console (`WIFI_ON` / `WIFI_OFF`).

Everything else on the Meshtastic side (region, channel-0 PSK, admin key, node identity) is configured from the Meshtastic mobile app and documented in `MESHTASTIC_NODE_USER.md`.

## Wire format on the mesh

| Event | Text |
|---|---|
| Bridge boots and pairs to the node | `@CAM bridge online` |
| Inference fires above threshold | `@CAM TRIGGERED:<class>@<score>` |

`<class>` is a COCO label (`person`, `dog`, `cup`, ...). `<score>` is `0..100`. Multiple objects of different classes in the same frame each generate their own line.

## Naming convention for fleet discovery

The Meshtastic node must be configured with `shortName=cam` in the app. Meshtastic combines the short name with the last 4 hex digits of the node ID and advertises the result as the BLE local name (for example `cam_60a4`). The bridge scans for any BLE peer whose advertised name starts with `cam_`. Any kit following this convention is auto-discovered.
