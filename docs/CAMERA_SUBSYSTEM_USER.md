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

The boot log on the C3's USB CDC (115200 baud, `dtr=False, rts=False`) shows roughly:

```
=== ai-cam-bridge (interactive SSCMA) ===
BLE PIN: <value> (from NVS, default 123456)
mesh: BLE central started, scanning for Meshtastic 'cam_*' peer...
grove: 5 slot(s) in alias table (from NVS)
heartbeat: ON, interval=30min
dedup: interval=30s (from NVS or default)
conf: threshold=65 (from NVS or default)
AI.begin -> OK
device='Grove Vision AI V2' id='<hex>'
model: bound slot 1 (boot)
wifi: OFF (default)
```

Reading the USB CDC reboots the C3 once on host-open. The BLE handshake to the Meshtastic node settles about 5 seconds after the boot banner.

## Runtime control surfaces

Three ways to control the running bridge — in increasing order of remoteness:

| Surface | Used by | When |
|---|---|---|
| **USB REPL** | bench operator with the C3 plugged in | initial setup, deep debugging |
| **Wi-Fi operator console** | operator on-site with a phone or laptop in proximity | field tuning without diginode-cc in range |
| **Mesh verbs** | diginode-cc dashboard or any peer on the LoRa channel | remote operation, automation |

All three surfaces converge on the same NVS-backed state, so a change made via any one of them is reflected by the others.

## Mesh verbs

The bridge listens for text frames on the primary channel addressed to it. Frame format:

```
@<target> <VERB>[:<param>]
```

`<target>` is matched case-insensitively against three values:

- `ALL` — broadcast to every device
- `CAM` — fleet-wide camera class
- the bridge's discovered short name (`cam` by default — see `MESHTASTIC_NODE_USER.md`)

Every reply starts with `Cam: ` so the dashboard can filter responses.

### Status and arming

| Verb | Reply | Notes |
|---|---|---|
| `@CAM STATUS` | `Cam: STATUS: Mode:<ARMED\|DISARMED> Scan:CAM Hits:0 Temp:0.0C Up:<HH:MM:SS> Type:AICAMERA Slots:<N> Model:<alias>` | One-shot snapshot. Same shape as the boot frame and the periodic heartbeat. |
| `@CAM START` | `Cam: START_ACK:OK` + STATUS | Arms the watch loop. Forwards detections to the mesh. |
| `@CAM STOP` | `Cam: STOP_ACK:OK` + STATUS | Disarms. The bridge stays connected but emits nothing. |

### Heartbeat

| Verb | Reply | Notes |
|---|---|---|
| `@CAM HB_ON` | `Cam: HB_ACK:OK` | Enables periodic STATUS broadcasts (default on). Persisted. |
| `@CAM HB_OFF` | `Cam: HB_ACK:OK` | Disables them. Persisted. |
| `@CAM HB_INTERVAL:<n>` | `Cam: HB_ACK:OK` / `Cam: HB_ACK:ERROR` | Set interval in minutes, `n` in `[1, 60]`. Persisted. Default 30 min. |

### Model registry

| Verb | Reply | Notes |
|---|---|---|
| `@CAM MODEL_LIST` | `Cam: MODELS:1=person*,2=face,3=gesture,...` or `Cam: MODELS:NONE` | `*` marks the active slot. |
| `@CAM MODEL_SET:<id>` | `Cam: MODEL_SET_ACK:OK` + STATUS / `Cam: MODEL_SET_ACK:ERROR` | Bind a different model by slot id. Persisted; the camera comes back up on this slot after reboot. |

### Detection tuning

| Verb | Reply | Notes |
|---|---|---|
| `@CAM CONF_THRESHOLD:<n>` | `Cam: CONF_ACK:OK` / `Cam: CONF_ACK:ERROR` | Minimum confidence `n` in `[1, 100]`. Strict comparator: a score equal to the threshold does **not** fire — only score `> n` does. Persisted. Default 65. |
| `@CAM DEDUP_INTERVAL:<n>` | `Cam: DEDUP_ACK:OK` / `Cam: DEDUP_ACK:ERROR` | Per-class cooldown in seconds, `n` in `[1, 3600]`. Each COCO class has its own timer; a `person` trigger does not gate a `dog` trigger. Persisted. Default 30 s. |

### Wi-Fi operator console

| Verb | Reply | Notes |
|---|---|---|
| `@CAM WIFI_ON` | `Cam: WIFI_ON_ACK:OK` / `Cam: WIFI_ON_ACK:ERROR` | Brings up the softAP at `192.168.4.1`. Persisted. |
| `@CAM WIFI_OFF` | `Cam: WIFI_OFF_ACK:OK` | Brings the AP down. Persisted. |
| `@CAM WIFI_SSID:<name>` | `Cam: WIFI_SSID_ACK:OK` / `Cam: WIFI_SSID_ACK:ERROR` | Set the softAP SSID (1..32 chars). Persisted. Takes effect on next `WIFI_OFF` &rarr; `WIFI_ON`. |
| `@CAM WIFI_PSK:<pass>` | `Cam: WIFI_PSK_ACK:OK` / `Cam: WIFI_PSK_ACK:ERROR` | Set the WPA2 PSK (8..63 chars). Persisted. Takes effect on next `WIFI_OFF` &rarr; `WIFI_ON`. |

On a fresh-flashed bridge with no stored Wi-Fi preference in NVS, the AP comes up **on by default** so first-boot setup can happen over Wi-Fi without LoRa or USB access. Once the operator sends `@CAM WIFI_OFF` (or `wifi off` at the REPL), the disabled state is persisted and survives reboots until `WIFI_ON` flips it back. See the next section for what the console actually does.

`WIFI_SSID` / `WIFI_PSK` accept spaces and special characters in the value (e.g. `@CAM WIFI_SSID:my home wifi`). The value runs to the end of the text frame and trailing whitespace is trimmed. Credentials are **not** included in any STATUS or heartbeat broadcast.

## Wi-Fi operator console

When the bridge brings up its Wi-Fi softAP, a phone or laptop in range can join `aicam-bridge` (default PSK `aicam-12345`, WPA2) and open a small web page that exposes the same controls as the mesh verbs above, plus a live snapshot of the bridge's state.

### Turning it on

From a Meshtastic peer (or diginode-cc):

```
@CAM WIFI_ON           → Cam: WIFI_ON_ACK:OK
```

Or from the bench REPL:

```
wifi on
```

The C3 brings up the SSID and stores the on-state in NVS. On the next boot the AP comes back automatically.

### Joining

| Field | Default |
|---|---|
| SSID | `aicam-bridge` |
| Security | WPA2-PSK |
| PSK | `aicam-12345` |
| Captive portal | yes |

Override SSID and PSK from the REPL (`wifi-ssid <name>`, `wifi-psk <pass>`) before turning the AP on. SSID is 1..32 chars; PSK must be 8..63 chars to satisfy WPA2.

The bridge runs a DNS hijack: every hostname resolves to `192.168.4.1`, so phones and recent desktop OSes auto-open the page from the captive-portal popup the moment you join. Laptops without a captive-portal handler should browse to `http://192.168.4.1/` manually.

### What the page does

A single page, no JavaScript, styled to match the diginode-cc dashboard so an operator coming from there sees the same palette. From top to bottom:

- **Header.** Bridge short name, uptime, mesh peer state, current armed/disarmed state, count of connected Wi-Fi clients.
- **Camera.** Arm / disarm buttons. Same effect as the `@CAM START` / `@CAM STOP` verbs.
- **Model.** Radio-button list of every installed slot with the active one highlighted in accent blue. Pick a row and click **Apply model** to hot-swap.
- **Detection.** Confidence threshold (1..100) and dedup interval (1..3600 s) with number inputs and per-row **Apply** buttons.
- **Heartbeat.** Enable checkbox and interval in minutes (1..60), single **Apply** button.
- **Wi-Fi credentials.** SSID and PSK text inputs. Empty field = leave that one unchanged, so you can rotate just the PSK without retyping the SSID. The change is persisted immediately but does **not** kick your current session off the AP — apply by cycling Wi-Fi (`@CAM WIFI_OFF` then `@CAM WIFI_ON`, or `wifi off` then `wifi on` at the REPL).

Every form does a POST and is redirected back to `/`, so the page always shows the actual post-change state — useful when an input is rejected (validation failure shows a `bad value` page; back up and re-try).

### Turning it off

```
@CAM WIFI_OFF          → Cam: WIFI_OFF_ACK:OK
```

or `wifi off` at the REPL. The AP shuts down and the off-state is persisted; the next boot will not bring the AP back up until `WIFI_ON` is sent again.

### Coexistence note

Wi-Fi and BLE share the C3's single 2.4 GHz radio. While a client is actively loading the page, detection forwarding may show 10-50 ms of extra jitter. This is normal and not a bug; the AP is meant to be on-demand, not continuous.

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
| `hb [on\|off\|<n>]` | Heartbeat status / toggle / set interval in minutes (1..60) |
| `ble-pin` / `ble-pin <N>` | Show or set the BLE pairing PIN |
| `wifi [on\|off]` | Wi-Fi softAP status / toggle (no arg shows status) |
| `wifi-ssid <name>` | Set softAP SSID (1..32 chars), takes effect on next `wifi on` |
| `wifi-psk <pass>` | Set softAP PSK (8..63 chars), takes effect on next `wifi on` |
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
