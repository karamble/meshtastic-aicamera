// WebUI: Wi-Fi softAP + captive-portal status/config page for the
// AI-camera bridge. Toggled by @CAM WIFI_ON / @CAM WIFI_OFF mesh verbs
// (or the local `wifi on/off` REPL commands). On a fresh bridge with no
// stored NVS preference, the AP comes up ON by default so first-boot
// setup can happen over Wi-Fi. Once the operator sends WIFI_OFF, the
// disabled state is persisted and survives reboots until WIFI_ON flips
// it back.

#pragma once

#include <Arduino.h>
#include <cstdint>

class WebUI {
public:
  // Reads NVS (wifi_en, wifi_ssid, wifi_psk). If enabled, brings the softAP
  // up and returns true. Safe to call exactly once from setup() AFTER NimBLE
  // and SSCMA have initialized.
  bool begin();

  // Service DNS + HTTP. Call from loop(); cheap when AP is off.
  void tick();

  // Idempotent. Persists wifi_en in NVS.
  bool turn_on();
  bool turn_off();

  bool    is_on() const;
  uint8_t client_count() const;

  // Reads loaded by begin(); useful for the `wifi` REPL command.
  const String &ssid() const;
  const String &psk()  const;

  // Update SSID / PSK in NVS. Validates length per WPA2 rules. Returns false
  // if rejected. New value takes effect on the next turn_on() (caller can do
  // turn_off()/turn_on() to cycle).
  bool set_ssid(const String &s);
  bool set_psk (const String &s);
};

extern WebUI webui;
