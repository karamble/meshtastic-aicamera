# Camera Subsystem Technical Reference

Protocol and flash-layout reference for the Grove Vision AI V2 board (Himax WE-2). Companion to `CAMERA_SUBSYSTEM_USER.md`. Assumes familiarity with embedded toolchains and USB serial.

## Hardware

| Component | Identifier | Notes |
|---|---|---|
| Application SoC | Himax HX6538 (WE-2) | Cortex-M55 @ 400 MHz + Ethos-U55 NPU, on-die SRAM, external SPI NOR flash |
| Camera sensor | OV5647 default | MIPI CSI-2 to WE-2. `CIS_IMX219` / `CIS_IMX477` selectable in source builds. |
| USB-UART | WCH CH343 | USB `1a86:55d3`, one CDC-ACM port |
| Bridge MCU | XIAO ESP32-C3 in carrier socket | USB `303a:1001`, separate enumeration |

The two USB-C ports are independent. Flashing the WE-2 does not touch the XIAO, and vice versa.

## Bootloader and flash protocol

The WE-2 ROM bootloader runs on every power-on. It waits at an interactive UART menu for one character.

- `1` enters XMODEM upload mode (target: main application area).

The bootloader prints `Send data using the xmodem protocol from your terminal`, then accepts XMODEM-1K (1024-byte packets, CRC-16) at the chosen baud (use 921600). After EOT it asks `Do you want to end file transmission and reboot system? (y)`.

- `y` reboots into the new application.
- `n` keeps the bootloader in upload mode for the next file (used to chain firmware + multiple models in one session).

The repo's `scripts/xmodem_send.py` (vendored from Seeed) wraps this. `make flash-grove` invokes it with the pinned GitHub-release firmware downloaded to `firmware/grove/`.

### Flash arguments

```
--file=<.img>                                     # main app, written to base
--model="<tflite> <flash_addr_hex> <offset_hex>"  # one model, written via a 12-byte preamble
```

`--model` is repeatable. Each model triggers an extra XMODEM session in the same bootloader run with this 128-byte preamble (padded with `0xFF`):

```
[0xC0, 0x5A] + flash_addr_le32 + offset_le32 + [0x5A, 0xC0]
```

### Working model layout

The `grove_vision_ai_v2_20250102` firmware scans `0x400000..0xE00000` for the TFLite `'TFL3'` magic on boot and registers every model it finds. The canonical 5-model bundle:

| id | Address | Model | Inference |
|---|---|---|---|
| 1 | `0x400000` | Swift YOLO Nano Person Detection | 48-76 ms |
| 2 | `0x600000` | Face Detection | 20 ms |
| 3 | `0x800000` | Gesture (rock/paper/scissors) | 28 ms |
| 4 | `0xA00000` | Pet (cat/dog) | 28 ms |
| 5 | `0xB7B000` | Apple | 28 ms |

Model ids are assigned ascending by address. After flash, the C3's `auto_configure()` issues `AT+MODEL=1` and `AT+ALGO=0` on boot. The bridge REPL command `model <alias>` hot-swaps to any of the five.

## Runtime serial protocol (SSCMA)

Both the WE-2's CH343 USB-UART (921600, 8N1) and its I²C slave at `0x62` speak the SSCMA line protocol.

- Request: `AT+<NAME>?\r\n` (query) or `AT+<NAME>=<args>\r\n` (command).
- Response: `\r{"type":<int>,"name":"<NAME>","code":<int>,"data":<value>}\n`.
- Spontaneous events use the same JSON envelope.

`code` semantics:

- `0` OK
- `5` not present or not loaded
- non-zero, non-5 error

### Useful queries

| Command | Returns |
|---|---|
| `AT+ID?` | Device id |
| `AT+VER?` | Build version (`{"software":"2025.01.02",...}` for the pinned firmware) |
| `AT+MODELS?` | All registered models with ids, addresses, sizes |
| `AT+MODEL?` | Currently bound model |
| `AT+MODEL=<id>` | Bind a model |
| `AT+ALGO=0` | Re-arm algorithm auto-detect |
| `AT+INVOKE=<times>,<filter>,<show>` | Run inference. `times=0` means continuous. |
| `AT+BREAK` | Stop a running invoke loop |

### INVOKE event

```json
{"type":1,"name":"INVOKE","code":0,"data":{
   "count": 0,
   "algo_tick": [[76023]],
   "boxes": [[117, 122, 240, 240, 71, 0]],
   "image": "<base64 JPEG>"
}}
```

| Field | Meaning |
|---|---|
| `count` | Frame counter since boot |
| `algo_tick` | Inference time in microseconds. Nested as list-of-list (one inner array per stage in chained-model scenarios). |
| `boxes` | `[x_center, y_center, width, height, score, class_id]` in pixels of the model input size. `score` is `0..100`. `class_id` is COCO 0-79 for the YOLO-family detection models. |
| `image` | Base64-encoded JPEG of the frame the model saw. Decode with `base64 -d`. |

Pose and classification models emit `keypoints` or `classes` in place of `boxes`. The C3 bridge today parses `boxes` only.

## Algorithm and model-type enums

The firmware reports algorithm integers in `AT+ALGOS?`, `AT+MODELS?`, and `INVOKE` events. Values for the `20250102` baseline:

**`type`** (algorithm class):

| Value | Symbol | Decoder |
|---|---|---|
| 0 | `EL_ALGO_TYPE_UNDEFINED` | auto-detect on next invoke |
| 1 | `EL_ALGO_TYPE_FOMO` | FOMO grid detector |
| 2 | `EL_ALGO_TYPE_PFLD` | Facial landmark |
| 3 | `EL_ALGO_TYPE_YOLO` | YOLOv5 (used by all 5 stock models in our bundle) |
| 4 | `EL_ALGO_TYPE_IMCLS` | Image classification |
| 5 | `EL_ALGO_TYPE_YOLO_POSE` | YOLOv8 pose |
| 6 | `EL_ALGO_TYPE_YOLO_V8` | YOLOv8 detection |
| 7 | `EL_ALGO_TYPE_NVIDIA_DET` | NVIDIA-style detector |
| 8 | `EL_ALGO_TYPE_YOLO_WORLD` | YOLO-World |

**`categroy`** (note the firmware-side spelling):

| Value | Symbol | Output |
|---|---|---|
| 1 | `EL_ALGO_CAT_DET` | bounding boxes |
| 2 | `EL_ALGO_CAT_POSE` | keypoints |
| 3 | `EL_ALGO_CAT_CLS` | per-class scores |

`AT+ALGO=0` runs the auto-detect cascade `YOLO_POSE → YOLO_WORLD → YOLO_V8 → NVIDIA_DET → YOLO (v5) → FOMO → IMCLS → PFLD` against the bound model's output tensor shape. All 5 stock models in the canonical bundle resolve to `EL_ALGO_TYPE_YOLO` (3).

## CH343 DTR/RTS gotcha

Opening `/dev/ttyACM0` with default settings (or any tool that asserts modem-control lines) holds the WE-2 in a state where the application firmware does not emit. `cat /dev/ttyACM0` reads as silent regardless of baud rate.

Always drive both lines low before opening, and re-assert low after open (some kernels reset on `open()`):

```python
s = serial.Serial()
s.port = '/dev/ttyACM0'
s.baudrate = 921600
s.timeout = 1
s.dtr = False
s.rts = False
s.open()
s.dtr = False
s.rts = False
```

`xmodem_send.py` handles this internally. The same rule applies to the C3's native USB CDC.

## XIAO socket pinout

Empirically verified on the Grove Vision AI V2 carrier:

| XIAO pin | C3 GPIO | WE-2 net | Use |
|---|---|---|---|
| D4 | GPIO6 | PA3 | **I²C SDA** to slave at `0x62` |
| D5 | GPIO7 | PA2 | **I²C SCL** to slave at `0x62` |
| D6 | GPIO21 | PB9 (UART2 RX) | Not usable. PB10 never drives its pad on any firmware tested. |
| D7 | GPIO20 | PB10 (UART2 TX) | Not usable. |
| D8 | GPIO8 | PB4 | SPI master SCLK from WE-2 |
| D9 | GPIO9 | PB3 | SPI master MISO (also XIAO BOOT button) |
| D10 | GPIO10 | PB2 | SPI master MOSI |

The bridge uses I²C on D4/D5. The Grove I²C connector on the carrier shares the same bus, so peripherals there (Grove-cable connected sensors) sit on the same bus as the WE-2 slave.

## SSCMA library notes (C3 side)

`Seeed-Studio/Seeed_Arduino_SSCMA` matches the WE-2's I²C transport protocol. Notes:

- Do not call `Wire.begin()` before `AI.begin()`. The library does it internally. A duplicate call makes `AI.begin()` return false.
- `AI.begin()` only initialises I²C, allocates buffers, and queries `AT+ID?` / `AT+NAME?`. It returns true once the slave responds, not when inference is ready.
- `AI.invoke(times, filter, show)` sends `AT+INVOKE=<times>,<filter>,<!show>` and waits for both the synchronous AT response and the asynchronous INVOKE event.
- `AI.boxes()` returns `std::vector<boxes_t>`. `boxes_t` is `{uint16_t x, y, w, h; uint8_t score, target;}`. `target` is the COCO class id.

## Performance baseline

Verified with the working configuration as of 2026-05-11 (firmware `20250102`, Swift YOLO Nano Person Detection, C3 polling via I²C):

| Metric | Value |
|---|---|
| Inference (NPU) | 76 ms / frame |
| C3 ↔ WE-2 round trip | 13 fps |
| Firmware size on flash | ~615 KB |
| Person model size | ~1.64 MB |

## Source build path (optional)

The repo uses the prebuilt GitHub-release firmware. Building from source is supported but not required.

Upstream source: `~/go/src/github.com/HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2/`. The build needs Arm GNU Toolchain 13.2 Rel1 on `PATH`, then `cd EPII_CM55M_APP_S && make`. The output ELF is packaged into a flashable `.img` (1 MB hard cap) by `we2_image_gen_local/we2_local_image_gen`. The result is flashable through the same `scripts/xmodem_send.py` path.

The standalone scenario apps in the upstream tree (`tflm_yolo11_od`, `tflm_yolov8_pose`, `tflm_fd_fm`, ...) emit detections on the WE-2's CH343 UART only. They do not link an I²C slave and therefore cannot be used through the bridge.

## References

- Upstream repo: <https://github.com/HimaxWiseEyePlus/Seeed_Grove_Vision_AI_Module_V2>
- WE-2 GitHub firmware releases: <https://github.com/Seeed-Studio/sscma-example-we2/releases>
- SenseCraft model catalog: <https://files.seeedstudio.com/sscma/sscma-model-we2.json>
- SSCMA-Micro firmware source: <https://github.com/Seeed-Studio/SSCMA-Micro>
- Seeed Arduino library: <https://github.com/Seeed-Studio/Seeed_Arduino_SSCMA>
