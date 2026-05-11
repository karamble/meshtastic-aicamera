# Meshtastic Node Setup

The radio side of the kit is a XIAO ESP32-S3 with a Wio-SX1262 LoRa stack, running stock Meshtastic firmware. Per-node configuration happens in the Meshtastic Android/iOS app over Bluetooth. The dev machine only needs the app to send a few values and `make` to verify and to push the matching BLE PIN to the bridge.

## Quick deploy

App side (one-time, per kit):

1. Pair the app to the node. PIN `123456`.
2. Settings → User: **Long Name `aicam`, Short Name `cam`**.
3. Settings → Radio Configuration → LoRa: pick your **Region**.
4. Settings → Channels → channel 0: paste the fleet **PSK**. Delete channels 1-7.
5. Settings → Security: paste the fleet **Admin Key**. **Is Managed: on**.
6. Settings → Position → GPS Mode: **`NOT_PRESENT`**. Telemetry interval: **`1800`** s.

Dev-machine side, from the repo root:

```
make mesh-info       # confirms the node sees your config over USB
make set-ble-pin     # only needed if you set a non-default Fixed PIN in step 1
```

If you stuck with PIN `123456` and `BLE_PIN=123456` in `.env`, no `set-ble-pin` is needed.

## Make targets that touch the node

| Target | What it does |
|---|---|
| `mesh-info` | Read Meshtastic node state over USB. Verifies identity, region, PSK presence. |
| `set-ble-pin` | Pushes `BLE_PIN` from `.env` to the **bridge's** NVS so its pairing PIN matches the value you set in step 1 of the app. |

The Meshtastic node firmware is preflashed and not reflashed here. The Wio-SX1262 stack physically covers the XIAO's BOOT/RESET buttons, so re-flashing over USB is not possible without disassembling the stack. The shipped Meshtastic 2.5.x firmware has every feature the bridge uses.

## You need

- The **Meshtastic** app on Android (Google Play) or iOS (App Store), with Bluetooth on.
- A USB-C cable for power on the Wio-SX1262's port.
- The fleet's **primary-channel PSK** and **admin public key**. These are standard Meshtastic channel-security settings. Generate them in the app (Channels → regenerate PSK, Security → generate Admin Key) for a fresh fleet, or pull the shared values from your fleet coordinator (for example diginode-cc's *Fleet Security* page at `http://<diginode-cc-ip>:3000`) when joining an existing fleet.
- The kit's `.env` at the repo root. Run `cp .env.example .env` on first checkout. Adjust `BLE_PIN` to match the Fixed PIN you choose in step 1.

## Step-by-step

### 1. Pair the app to the node

Power the node by plugging USB-C into the Wio-SX1262's port. The XIAO's LED blinks and the node advertises on Bluetooth. In the app:

1. Scan and select `Meshtastic <xxxx>` (`xxxx` is the last 4 hex digits of the MAC).
2. Enter pairing PIN **`123456`**.
3. The app downloads the node config and shows the Nodes screen.

The Meshtastic app holds the BLE connection. The bridge cannot pair while the phone is connected. Disconnect the phone before powering the bridge. The bridge re-pairs automatically.

### 2. Identity

Settings → User:

| Field | Value |
|---|---|
| Long Name | `aicam` |
| Short Name | `cam` |
| Is Licensed | unchecked |

`cam` is required. The bridge filters BLE scans for peers advertising as `cam_*`. After save, the node reboots and re-advertises as `cam_<xxxx>`. Re-pair if the app drops.

### 3. Region

Settings → Radio Configuration → LoRa:

| Field | Value |
|---|---|
| Region | Your region (`EU_868`, `US`, `AS_923`, `ANZ`, `CN`, ...). Must match the fleet. |
| Modem preset | `LONG_FAST` (default) |
| Hop limit | `3` (default) |

### 4. Primary channel PSK

Settings → Channels → channel 0:

1. Paste the fleet PSK (base64) into the PSK field.
2. Save.
3. Delete every secondary channel (slots 1-7). The kit must expose only channel 0.

After save the Channels list shows a single row with the custom PSK applied.

### 5. Admin key

Settings → Security:

| Field | Value |
|---|---|
| Admin Key | fleet admin public key (base64) |
| Is Managed | on |

The admin key is required. The fleet rotates the PSK on a schedule and pushes the new value to member nodes as an admin-key-authenticated admin message. A node without the admin key cannot accept the rotation and stops talking to the rest of the fleet.

### 6. Strip extras

- Settings → Position → GPS Mode: `NOT_PRESENT`. The Wio-SX1262 has no GPS.
- Settings → Bluetooth: enabled, Pairing mode `FIXED_PIN`, PIN `123456` (or your custom value).
- Settings → Telemetry → Device update interval: `1800` seconds.

### 7. Custom BLE PIN (optional)

For private deployments, set a non-default Fixed PIN.

1. Settings → Bluetooth → Fixed PIN: enter your 6-digit value, save, re-pair the phone.
2. Edit the kit's `.env`: `BLE_PIN=<your 6-digit pin>`.
3. With the bridge plugged in over USB:

   ```
   make set-ble-pin
   ```

The bridge stores the new PIN in NVS and reboots. No firmware reflash. To rotate later, repeat the same two steps.

## Verify

```
make mesh-info
```

Confirms identity, region, and channel-0 PSK from the dev machine's view of the node.

From any other Meshtastic device on the fleet, the Nodes screen shows a new entry `aicam` (`cam_xxxx`) with a recent Last Heard. When the bridge is also powered, the primary-channel chat shows `@CAM bridge online` followed by `@CAM TRIGGERED:<class>@<score>` lines for each detection.
