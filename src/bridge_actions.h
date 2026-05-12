// Narrow API between main.cpp (which owns the camera-bridge state) and
// webui.cpp (which serves the operator console over Wi-Fi). webui never
// reaches into main.cpp's statics directly; it goes through these.

#pragma once

#include <cstddef>
#include <cstdint>

class String;

struct UiSlot {
  int   id;
  const char *alias;
  bool  active;
};

namespace bridge {

// --- read ---
size_t      slot_count();
UiSlot      slot_at(size_t idx);
int         current_slot();
uint8_t     conf_min();
uint16_t    dedup_sec();
bool        hb_enabled();
uint8_t     hb_interval_min();
bool        armed();
const char *mesh_short_name();
bool        mesh_connected();
uint32_t    uptime_ms();

// --- write (return true on success, false if input rejected) ---
bool set_model       (int id);
bool set_conf        (int v);    // [1, 100]
bool set_dedup_sec   (int v);    // [1, 3600]
bool set_hb_enabled  (bool on);
bool set_hb_interval (int min);  // [1, 60]
bool set_armed       (bool on);

}  // namespace bridge
