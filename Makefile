# ai-cam-bridge — build + flash Makefile.
#
# Three pieces of hardware ship with each ai-cam unit:
#   1. Grove Vision AI V2 carrier   Himax WE-2 SoC, runs an ML model
#   2. XIAO ESP32-C3                the bridge MCU in the AI V2 socket (this repo)
#   3. XIAO ESP32-S3 + Wio-SX1262   stock Meshtastic on a Grove Base for XIAO
#
# Per-kit config (BLE PIN, Grove firmware version) lives in `.env` — see
# `.env.example` for the template.
#
# The Meshtastic node is configured manually via the Meshtastic Android/iOS
# app over BLE — see docs/MESHTASTIC_NODE_USER.md. The node CANNOT be
# re-flashed over USB without physically separating the Wio-SX1262 stack
# from the XIAO-S3; see memory/feedback_xiaos3_wio_no_software_flash.md.
#
# Run `make help` for a list of targets.

# Load per-kit overrides from .env (BLE_PIN, GROVE_FIRMWARE_VERSION).
# `-include` means the missing-file case is non-fatal — defaults below apply.
-include .env
export

# ── XIAO ESP32-C3 (ai-cam-bridge firmware) ─────────────────────────────────
# Default port matches the C3's native USB-JTAG enumeration. Override:
#   make flash-c3 C3_PORT=/dev/ttyACMx
C3_PORT ?= $(firstword $(wildcard /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_*-if00))

# ── Grove Vision AI V2 (Himax WE-2) ────────────────────────────────────────
# Enumerates via the carrier's CH343 USB-UART (VID:PID 1a86:55d3).
WE2_PORT ?= $(firstword $(wildcard /dev/serial/by-id/usb-1a86_USB_Single_Serial_*-if00))

# Firmware version pinned via .env. GitHub release tag from
# Seeed-Studio/sscma-example-we2. 20250102 is the verified baseline with
# 10 MB model partition support. Don't downgrade.
GROVE_FIRMWARE_VERSION ?= 20250102
GROVE_FIRMWARE_IMG := firmware/grove/grove_vision_ai_v2_$(GROVE_FIRMWARE_VERSION).img
GROVE_FIRMWARE_URL := https://github.com/Seeed-Studio/sscma-example-we2/releases/download/$(GROVE_FIRMWARE_VERSION)/grove_vision_ai_v2_$(GROVE_FIRMWARE_VERSION).img

# WE-2 model bundle. The actual set is owned by scripts/grove_select.py and
# persisted to firmware/grove/models/selected.txt. flash-grove parses that
# manifest and assembles xmodem --model arguments from it.
GROVE_MODEL_DIR := firmware/grove/models
GROVE_MANIFEST  := $(GROVE_MODEL_DIR)/selected.txt

# ── Meshtastic node (XIAO ESP32-S3 + Wio-SX1262) ───────────────────────────
# MESH_PORT empty = auto-detect /dev/serial/by-id/usb-Espressif_Systems_seeed-xiao-s3_*-if00
MESH_PORT ?=

# ── Targets ────────────────────────────────────────────────────────────────

.PHONY: help
help: ## Show this help.
	@awk 'BEGIN {FS = ":.*?## "} /^[a-zA-Z0-9_-]+:.*?## / {printf "  %-22s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

.PHONY: build
build: ## Compile the C3 bridge firmware without flashing.
	pio run

.PHONY: clean
clean: ## Remove PlatformIO build artifacts.
	pio run -t clean

.PHONY: flash-c3
flash-c3: ## Flash the C3 bridge firmware via USB CDC (PlatformIO upload).
	@if [ -z "$(C3_PORT)" ] || [ ! -e "$(C3_PORT)" ]; then \
		echo "C3 USB device not found. Plug in the XIAO ESP32-C3 or override C3_PORT=..."; \
		exit 1; \
	fi
	pio run -t upload --upload-port $(C3_PORT)

.PHONY: set-ble-pin
set-ble-pin: ## Push BLE_PIN from .env to the C3's NVS (no firmware reflash).
	@if [ ! -f .env ]; then \
		echo ".env not found — copy .env.example to .env and set BLE_PIN."; exit 1; \
	fi
	@if [ -z "$(BLE_PIN)" ]; then \
		echo "BLE_PIN is empty. Set it in .env (e.g. BLE_PIN=123456)."; exit 1; \
	fi
	@if [ -z "$(C3_PORT)" ] || [ ! -e "$(C3_PORT)" ]; then \
		echo "C3 USB device not found. Plug in the XIAO ESP32-C3 or override C3_PORT=..."; \
		exit 1; \
	fi
	@echo "Pushing BLE_PIN=$(BLE_PIN) to C3 NVS via $(C3_PORT)..."
	python3 scripts/set_ble_pin.py $(C3_PORT) $(BLE_PIN)

.PHONY: fetch-grove-firmware
fetch-grove-firmware: $(GROVE_FIRMWARE_IMG) ## Download Grove WE-2 firmware to firmware/grove/ if missing.

$(GROVE_FIRMWARE_IMG):
	@mkdir -p firmware/grove
	@echo "Downloading $(notdir $@) from GitHub..."
	curl -fL -o $@ $(GROVE_FIRMWARE_URL)

.PHONY: grove-pick
grove-pick: ## Run the interactive WE-2 model bundle picker. Updates the manifest.
	python3 scripts/grove_select.py

$(GROVE_MANIFEST):
	@echo "No model manifest at $@ - writing the default 5-model bundle..."
	GROVE_NONINTERACTIVE=1 python3 scripts/grove_select.py

.PHONY: flash-grove
flash-grove: fetch-grove-firmware $(GROVE_MANIFEST) ## Flash WE-2 firmware + the models listed in selected.txt via xmodem.
	@if [ -z "$(WE2_PORT)" ] || [ ! -e "$(WE2_PORT)" ]; then \
		echo "Grove WE-2 not found at /dev/serial/by-id/usb-1a86_USB_Single_Serial_*"; \
		echo "Plug into the AI carrier's USB-C (the camera board, not the XIAO)."; \
		exit 1; \
	fi
	@echo "Flashing $(GROVE_FIRMWARE_VERSION) firmware + manifest models on $(WE2_PORT)."
	@echo "When the script prints 'Please press reset button!!', press the small RESET"
	@echo "button on the Grove AI carrier so the bootloader hands control to xmodem."
	@echo "After the final 'xmodem_send bin file result = True' line the script will"
	@echo "either crash with termios.error or hang on a serial read. Either way,"
	@echo "the flash is done. If it hangs, Ctrl-C it. Then power-cycle the AI carrier."
	@echo
	-python3 scripts/flash_grove.py $(WE2_PORT) $(GROVE_FIRMWARE_IMG) $(GROVE_MANIFEST)
	@echo
	@echo "Once the bridge firmware is also flashed and the WE-2 is power-cycled, run:"
	@echo "  make set-grove-aliases    # pushes the alias + class table to the C3's NVS"

.PHONY: set-grove-aliases
set-grove-aliases: $(GROVE_MANIFEST) ## Push the alias + class metadata for each flashed model to the C3's NVS.
	@if [ -z "$(C3_PORT)" ] || [ ! -e "$(C3_PORT)" ]; then \
		echo "C3 USB device not found. Plug in the XIAO ESP32-C3."; \
		exit 1; \
	fi
	python3 scripts/set_grove_aliases.py $(C3_PORT) $(GROVE_MANIFEST)

.PHONY: grove-show
grove-show: ## Read the per-slot alias + class table currently in the C3's NVS.
	@if [ -z "$(C3_PORT)" ] || [ ! -e "$(C3_PORT)" ]; then \
		echo "C3 USB device not found. Plug in the XIAO ESP32-C3."; \
		exit 1; \
	fi
	@python3 scripts/grove_show.py $(C3_PORT)

.PHONY: mesh-info
mesh-info: ## Read Meshtastic node state over USB (read-only; for verification).
	@PORT="$(MESH_PORT)"; \
	if [ -z "$$PORT" ]; then \
		PORT="$$(ls /dev/serial/by-id/usb-Espressif_Systems_seeed-xiao-s3_*-if00 2>/dev/null | head -1)"; \
	fi; \
	if [ -z "$$PORT" ]; then \
		echo "Meshtastic node not found. Plug in the XIAO-S3 or override MESH_PORT=..."; \
		exit 1; \
	fi; \
	echo "Querying $$PORT ..."; \
	meshtastic --port "$$PORT" --info
