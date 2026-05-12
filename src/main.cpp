// ai-cam-bridge: XIAO ESP32-C3 firmware that bridges Grove Vision AI V2
// detections to a Meshtastic node over BLE. Talks I²C to the WE-2 (Seeed
// SSCMA library) and acts as a BLE central to the Meshtastic node. Type
// `help` on the C3 USB CDC for the interactive REPL.

#include <Seeed_Arduino_SSCMA.h>
#include <Preferences.h>
#include <vector>

#include "bridge_actions.h"
#include "mesh_ble.h"
#include "webui.h"

SSCMA    AI;
MeshBLE  mesh;

// ---- NVS-backed BLE PIN ---------------------------------------------------
// Pushed by `make set-ble-pin` or the REPL `ble-pin <N>` command. Must match
// the Fixed PIN configured on the Meshtastic node in the Android app.
static const char *kBlePrefsNamespace = "aicam";
static const char *kBlePrefsKey       = "ble_pin";
static const uint32_t kBlePinDefault  = 123456;

static uint32_t load_ble_pin() {
  Preferences prefs;
  prefs.begin(kBlePrefsNamespace, /*readOnly=*/true);
  uint32_t pin = prefs.getUInt(kBlePrefsKey, kBlePinDefault);
  prefs.end();
  return pin;
}

static bool store_ble_pin(uint32_t pin) {
  Preferences prefs;
  if (!prefs.begin(kBlePrefsNamespace, /*readOnly=*/false)) return false;
  size_t n = prefs.putUInt(kBlePrefsKey, pin);
  prefs.end();
  return n == sizeof(uint32_t);
}

// ---- NVS-backed heartbeat config ------------------------------------------
// Family contract (matches gatesensor/halberd): operator addresses the camera
// with @<target> HB_ON / HB_OFF / HB_INTERVAL:<n>. Interval is in MINUTES,
// bounded [1,60] across all sibling firmwares. Defaults to enabled, 30 min.
static const char    *kHbEnPrefsKey   = "hb_en";
static const char    *kHbMinPrefsKey  = "hb_min";
static const uint8_t  kHbMinDefault   = 30;
static const uint8_t  kHbMinMin       = 1;
static const uint8_t  kHbMinMax       = 60;

static bool     hb_enabled      = true;
static uint8_t  hb_interval_min = kHbMinDefault;
static uint32_t hb_last_send_ms = 0;

static void load_hb_config() {
  Preferences prefs;
  prefs.begin(kBlePrefsNamespace, /*readOnly=*/true);
  hb_enabled      = prefs.getBool (kHbEnPrefsKey,  true);
  hb_interval_min = prefs.getUChar(kHbMinPrefsKey, kHbMinDefault);
  prefs.end();
  if (hb_interval_min < kHbMinMin || hb_interval_min > kHbMinMax) {
    hb_interval_min = kHbMinDefault;
  }
}

static void store_hb_enabled(bool en) {
  Preferences prefs;
  if (prefs.begin(kBlePrefsNamespace, /*readOnly=*/false)) {
    prefs.putBool(kHbEnPrefsKey, en);
    prefs.end();
  }
}

static void store_hb_interval(uint8_t m) {
  Preferences prefs;
  if (prefs.begin(kBlePrefsNamespace, /*readOnly=*/false)) {
    prefs.putUChar(kHbMinPrefsKey, m);
    prefs.end();
  }
}

// ---- NVS-backed active model slot -----------------------------------------
// Persists the operator's last MODEL_SET / `bind` / `model` choice so the
// camera comes back up on the same slot rather than always defaulting to 1.
static const char    *kCurSlotPrefsKey = "cur_slot";
static const uint8_t  kCurSlotDefault  = 1;

static uint8_t load_active_slot() {
  Preferences prefs;
  prefs.begin(kBlePrefsNamespace, /*readOnly=*/true);
  uint8_t s = prefs.getUChar(kCurSlotPrefsKey, kCurSlotDefault);
  prefs.end();
  return s ? s : kCurSlotDefault;
}

static void store_active_slot(uint8_t id) {
  Preferences prefs;
  if (prefs.begin(kBlePrefsNamespace, /*readOnly=*/false)) {
    prefs.putUChar(kCurSlotPrefsKey, id);
    prefs.end();
  }
}

// ---- NVS-backed per-class dedup interval ----------------------------------
// Wire verb DEDUP_INTERVAL:<sec> bounded [1, 3600]. Default 30 s — cuts the
// mesh-flood of a person standing in frame down to one frame every half-minute.
// Per-class: each COCO index has its own timer in last_fired_ms[80].
static const char    *kDedupSecPrefsKey = "dedup_sec";
static const uint16_t kDedupSecDefault  = 30;
static const uint16_t kDedupSecMin      = 1;
static const uint16_t kDedupSecMax      = 3600;

static uint16_t load_dedup_sec() {
  Preferences prefs;
  prefs.begin(kBlePrefsNamespace, /*readOnly=*/true);
  uint16_t s = prefs.getUShort(kDedupSecPrefsKey, kDedupSecDefault);
  prefs.end();
  if (s < kDedupSecMin || s > kDedupSecMax) s = kDedupSecDefault;
  return s;
}

static void store_dedup_sec(uint16_t s) {
  Preferences prefs;
  if (prefs.begin(kBlePrefsNamespace, /*readOnly=*/false)) {
    prefs.putUShort(kDedupSecPrefsKey, s);
    prefs.end();
  }
}

// ---- NVS-backed minimum detection confidence ------------------------------
// Wire verb CONF_THRESHOLD:<n> bounded [1, 100]. Default 65 % — a field-tuned
// floor for the YOLO-class models on the WE-2. Strict comparator: a score
// equal to the threshold does NOT fire; only score > conf_min fires.
static const char    *kConfMinPrefsKey = "conf_min";
static const uint8_t  kConfMinDefault  = 65;
static const uint8_t  kConfMinAbsMin   = 1;
static const uint8_t  kConfMinAbsMax   = 100;

static uint8_t load_conf_min() {
  Preferences prefs;
  prefs.begin(kBlePrefsNamespace, /*readOnly=*/true);
  uint8_t v = prefs.getUChar(kConfMinPrefsKey, kConfMinDefault);
  prefs.end();
  if (v < kConfMinAbsMin || v > kConfMinAbsMax) v = kConfMinDefault;
  return v;
}

static void store_conf_min(uint8_t v) {
  Preferences prefs;
  if (prefs.begin(kBlePrefsNamespace, /*readOnly=*/false)) {
    prefs.putUChar(kConfMinPrefsKey, v);
    prefs.end();
  }
}

// ---- COCO class labels (indices 0..79), fallback for slots without NVS data
static const char *const COCO[80] = {
  "person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck",
  "boat", "traffic_light", "fire_hydrant", "stop_sign", "parking_meter", "bench",
  "bird", "cat", "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra",
  "giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
  "skis", "snowboard", "sports_ball", "kite", "baseball_bat", "baseball_glove",
  "skateboard", "surfboard", "tennis_racket", "bottle", "wine_glass", "cup",
  "fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
  "broccoli", "carrot", "hot_dog", "pizza", "donut", "cake", "chair", "couch",
  "potted_plant", "bed", "dining_table", "toilet", "tv", "laptop", "mouse",
  "remote", "keyboard", "cell_phone", "microwave", "oven", "toaster", "sink",
  "refrigerator", "book", "clock", "vase", "scissors", "teddy_bear", "hair_drier",
  "toothbrush",
};

static const char *coco_name(int idx) {
  return (idx >= 0 && idx < 80) ? COCO[idx] : "?";
}

// ---- Watch loop: poll AI.invoke at kInvokePeriodMs and forward each box
// above kForwardScoreThreshold to the mesh, with per-class cooldown.
// Enabled by default; REPL `watch on|off` toggles.
static bool          watch_enabled         = true;
static uint32_t      last_invoke_ms        = 0;
static const uint32_t kInvokePeriodMs      = 250;     // 4 Hz
static uint8_t conf_min = kConfMinDefault;            // 1..100, overwritten from NVS in setup()
static uint32_t cooldown_ms = (uint32_t)kDedupSecDefault * 1000UL;  // overwritten from NVS in setup()
static uint32_t      last_fired_ms[80]     = {0};

// ---- helpers --------------------------------------------------------------

static void send_at(const String &body) {
  String cmd = "AT+" + body + "\r\n";
  Serial.printf("> %s", cmd.c_str());
  AI.write(cmd.c_str(), cmd.length());
}

// Drain SSCMA responses for up to `max_ms`. Truncates the base64 `image`
// field on INVOKE events to keep the console readable.
static void drain_responses(uint32_t max_ms) {
  static String buf;
  uint32_t end = millis() + max_ms;
  while ((int32_t)(millis() - end) < 0) {
    int avail = AI.available();
    if (avail > 0) {
      char chunk[256];
      int n = AI.read(chunk, avail > (int)sizeof(chunk) ? (int)sizeof(chunk) : avail);
      for (int i = 0; i < n; ++i) {
        char c = chunk[i];
        if (c == '\r') continue;
        if (c == '\n') {
          if (buf.length()) {
            int img_pos = buf.indexOf("\"image\":");
            if (img_pos >= 0) {
              int end_pos = buf.indexOf("\"", img_pos + 9 + 1);
              if (end_pos > img_pos) {
                buf = buf.substring(0, img_pos + 9) + "\"<...>\"" + buf.substring(end_pos + 1);
              }
            }
            Serial.print("< "); Serial.println(buf);
            buf = "";
          }
        } else {
          buf += c;
        }
      }
      end = millis() + max_ms;
    } else {
      delay(5);
    }
  }
}

// ---- Per-slot model metadata (NVS-backed) ---------------------------------
// id matches AT+MODEL=<n>. alias drives `model <name>` REPL lookups.
// classes drives `@CAM TRIGGERED:<class>@<score>` labels for that slot.
// NVS keys: slot<n>_name, slot<n>_cls (pipe-separated). Pushed by
// `make set-grove-aliases`; defaults populated if NVS is empty.
struct GroveSlot {
  int id;
  String alias;
  std::vector<String> classes;
};
static std::vector<GroveSlot>  g_slots;
static int                     g_current_slot = 1;     // last bound by us

static void populate_default_slots() {
  g_slots.clear();
  g_slots.push_back({1, "person",  {"person"}});
  g_slots.push_back({2, "face",    {"face"}});
  g_slots.push_back({3, "gesture", {"paper", "rock", "scissors"}});
  g_slots.push_back({4, "pet",     {"cat", "dog"}});
  g_slots.push_back({5, "apple",   {"apple"}});
}

static std::vector<String> split_pipe(const String &s) {
  std::vector<String> out;
  int start = 0;
  while (start <= (int)s.length()) {
    int idx = s.indexOf('|', start);
    if (idx < 0) { out.push_back(s.substring(start)); break; }
    out.push_back(s.substring(start, idx));
    start = idx + 1;
  }
  // Drop trailing empty token from a terminal '|'.
  if (!out.empty() && out.back().length() == 0) out.pop_back();
  return out;
}

static void load_grove_slots() {
  Preferences prefs;
  prefs.begin(kBlePrefsNamespace, /*readOnly=*/true);
  g_slots.clear();
  char key_name[16], key_cls[16];
  for (int i = 1; i <= 31; ++i) {
    snprintf(key_name, sizeof(key_name), "slot%d_name", i);
    snprintf(key_cls,  sizeof(key_cls),  "slot%d_cls",  i);
    if (!prefs.isKey(key_name)) continue;
    GroveSlot s;
    s.id      = i;
    s.alias   = prefs.getString(key_name);
    s.classes = split_pipe(prefs.getString(key_cls));
    g_slots.push_back(s);
  }
  prefs.end();
  if (g_slots.empty()) populate_default_slots();
}

static bool nvs_clear_slots() {
  Preferences prefs;
  if (!prefs.begin(kBlePrefsNamespace, /*readOnly=*/false)) return false;
  char key_name[16], key_cls[16];
  for (int i = 1; i <= 31; ++i) {
    snprintf(key_name, sizeof(key_name), "slot%d_name", i);
    snprintf(key_cls,  sizeof(key_cls),  "slot%d_cls",  i);
    if (prefs.isKey(key_name)) prefs.remove(key_name);
    if (prefs.isKey(key_cls))  prefs.remove(key_cls);
  }
  prefs.end();
  return true;
}

static bool nvs_set_slot(int id, const String &alias, const String &classes_pipe) {
  if (id < 1 || id > 31) return false;
  Preferences prefs;
  if (!prefs.begin(kBlePrefsNamespace, /*readOnly=*/false)) return false;
  char key_name[16], key_cls[16];
  snprintf(key_name, sizeof(key_name), "slot%d_name", id);
  snprintf(key_cls,  sizeof(key_cls),  "slot%d_cls",  id);
  prefs.putString(key_name, alias);
  prefs.putString(key_cls,  classes_pipe);
  prefs.end();
  return true;
}

static const GroveSlot *slot_by_id(int id) {
  for (auto &s : g_slots) if (s.id == id) return &s;
  return nullptr;
}

static const GroveSlot *slot_by_alias(const String &alias) {
  for (auto &s : g_slots) if (s.alias.equalsIgnoreCase(alias)) return &s;
  return nullptr;
}

// Resolve a class index for the bound slot, COCO fallback on miss.
static String class_label_for(int slot_id, int class_idx) {
  const GroveSlot *s = slot_by_id(slot_id);
  if (s && class_idx >= 0 && (size_t)class_idx < s->classes.size()) {
    return s->classes[class_idx];
  }
  return String(coco_name(class_idx));
}

// STATUS frame in the gatesensor/halberd family format, matching
// diginode-cc's parser (Mode/Scan/Hits/Temp/Up are required in this order);
// Type/Slots/Model are camera-specific tail tokens the regex ignores.
// Same frame on boot (latched once via sent_armed in loop()) and on demand
// (via on_mesh_text() when an @ALL/@CAM/@<shortname> STATUS arrives).
// Mode flips ARMED <-> DISARMED based on watch_enabled.
static bool sent_armed = false;

static bool send_status_frame() {
  uint32_t s_total = millis() / 1000UL;
  unsigned h   = (unsigned)(s_total / 3600UL);
  unsigned m   = (unsigned)((s_total / 60UL) % 60UL);
  unsigned sec = (unsigned)(s_total % 60UL);

  const GroveSlot *cur = slot_by_id(g_current_slot);
  const char *alias = cur ? cur->alias.c_str() : "?";
  const char *mode  = watch_enabled ? "ARMED" : "DISARMED";

  char buf[128];
  snprintf(buf, sizeof(buf),
    "Cam: STATUS: Mode:%s Scan:CAM Hits:0 Temp:0.0C "
    "Up:%02u:%02u:%02u Type:AICAMERA Slots:%u Model:%s",
    mode, h, m, sec, (unsigned)g_slots.size(), alias);

  bool ok = mesh.send_text(buf);
  Serial.printf("mesh: STATUS %s (%s)\n", ok ? "queued" : "DROPPED", buf);
  return ok;
}

// MODEL_LIST response: compact id=alias[*] tokens, '*' marks the bound slot.
// "Cam: MODELS:1=person*,2=face,..." or "Cam: MODELS:NONE". diginode-cc's
// textparser.handleAIModels parses this and synthesizes MODEL_LIST_ACK,
// mirroring CODE_LIST / CODES: in gatesensor.
static void send_model_list() {
  char buf[224];
  int off = snprintf(buf, sizeof(buf), "Cam: MODELS:");

  if (g_slots.empty()) {
    snprintf(buf + off, sizeof(buf) - off, "NONE");
  } else {
    bool first = true;
    for (auto &s : g_slots) {
      if (off >= (int)sizeof(buf) - 24) break;  // leave tail margin
      off += snprintf(buf + off, sizeof(buf) - off,
                      "%s%d=%s%s",
                      first ? "" : ",",
                      s.id,
                      s.alias.c_str(),
                      s.id == g_current_slot ? "*" : "");
      first = false;
    }
  }

  bool ok = mesh.send_text(buf);
  Serial.printf("mesh: MODELS %s (%s)\n", ok ? "queued" : "DROPPED", buf);
}

// State transitions shared by the mesh dispatcher and the local `watch` REPL
// command. AT+BREAK halts any pending invoke on the WE-2 (idempotent); the
// watch_enabled flag gates watch_tick() in loop().
static void arm_camera(const char *source) {
  watch_enabled = true;
  last_invoke_ms = 0;                       // fire next tick immediately
  for (uint32_t &t : last_fired_ms) t = 0;  // reset per-class debounce
  send_at("BREAK"); drain_responses(200);
  Serial.printf("camera: ARMED (%s)\n", source);
}

static void disarm_camera(const char *source) {
  watch_enabled = false;
  send_at("BREAK"); drain_responses(200);
  Serial.printf("camera: DISARMED (%s)\n", source);
}

// Bind a WE-2 model slot. Issues AT+MODEL=<id> + AT+ALGO=0, updates the
// runtime cursor, resets per-class debounce, and persists the choice to NVS.
// Returns false if `id` is not in g_slots (caller decides how to surface it).
static bool bind_slot(int id, const char *source) {
  if (!slot_by_id(id)) return false;
  send_at("MODEL=" + String(id));
  drain_responses(400);
  send_at("ALGO=0");
  drain_responses(200);
  g_current_slot = id;
  for (uint32_t &t : last_fired_ms) t = 0;
  store_active_slot((uint8_t)id);
  Serial.printf("model: bound slot %d (%s)\n", id, source);
  return true;
}

// ---- bridge:: action surface (consumed by webui.cpp) ---------------------
// Thin wrappers around the camera-bridge state above. Defined here so the
// existing helpers (bind_slot, arm/disarm, store_*) and statics (g_slots,
// conf_min, cooldown_ms, hb_*) are all already in scope.

size_t      bridge::slot_count()        { return g_slots.size(); }
UiSlot      bridge::slot_at(size_t idx) {
  const GroveSlot &s = g_slots[idx];
  return UiSlot{ s.id, s.alias.c_str(), s.id == g_current_slot };
}
int         bridge::current_slot()      { return g_current_slot; }
uint8_t     bridge::conf_min()          { return ::conf_min; }
uint16_t    bridge::dedup_sec()         { return (uint16_t)(cooldown_ms / 1000UL); }
bool        bridge::hb_enabled()        { return ::hb_enabled; }
uint8_t     bridge::hb_interval_min()   { return ::hb_interval_min; }
bool        bridge::armed()             { return watch_enabled; }
const char *bridge::mesh_short_name()   { return mesh.short_name(); }
bool        bridge::mesh_connected()    { return mesh.is_connected(); }
uint32_t    bridge::uptime_ms()         { return millis(); }

bool bridge::set_model(int id) {
  return bind_slot(id, "web");
}

bool bridge::set_conf(int v) {
  if (v < kConfMinAbsMin || v > kConfMinAbsMax) return false;
  ::conf_min = (uint8_t)v;
  store_conf_min(::conf_min);
  Serial.printf("conf: threshold=%u [web]\n", (unsigned)::conf_min);
  return true;
}

bool bridge::set_dedup_sec(int v) {
  if (v < kDedupSecMin || v > kDedupSecMax) return false;
  cooldown_ms = (uint32_t)v * 1000UL;
  store_dedup_sec((uint16_t)v);
  for (uint32_t &t : last_fired_ms) t = 0;
  Serial.printf("dedup: interval=%us [web]\n", (unsigned)v);
  return true;
}

bool bridge::set_hb_enabled(bool on) {
  ::hb_enabled = on;
  hb_last_send_ms = millis();
  store_hb_enabled(on);
  Serial.printf("hb: %s [web]\n", on ? "ON" : "OFF");
  return true;
}

bool bridge::set_hb_interval(int min) {
  if (min < kHbMinMin || min > kHbMinMax) return false;
  ::hb_interval_min = (uint8_t)min;
  hb_last_send_ms = millis();
  store_hb_interval(::hb_interval_min);
  Serial.printf("hb: interval=%umin [web]\n", (unsigned)::hb_interval_min);
  return true;
}

bool bridge::set_armed(bool on) {
  if (on) arm_camera("web");
  else    disarm_camera("web");
  return true;
}

// Inbound text dispatcher. Targets matched case-insensitively against "ALL",
// "CAM", and our discovered Meshtastic short name. Global verbs handled here:
// STATUS, START, STOP. Cam-specific verbs come later.
static void on_mesh_text(const char *text, uint32_t from) {
  Serial.printf("mesh-rx: from=%08x text='%s'\n", (unsigned)from, text);
  if (text[0] != '@') return;
  const char *sp = strchr(text, ' ');
  if (!sp) return;

  size_t tlen = (size_t)(sp - (text + 1));
  if (tlen == 0 || tlen >= 24) return;
  char target[24] = {0};
  memcpy(target, text + 1, tlen);

  bool match = false;
  if      (strcasecmp(target, "ALL") == 0) match = true;
  else if (strcasecmp(target, "CAM") == 0) match = true;
  else {
    const char *sn = mesh.short_name();
    if (sn[0] && strcasecmp(target, sn) == 0) match = true;
  }
  if (!match) return;

  const char *v = sp + 1;
  while (*v == ' ') ++v;
  char verb[24]   = {0};
  // 64 bytes fits a 63-char WPA2 PSK (the largest string-shaped param any
  // verb accepts). Numeric-param verbs are unaffected: atoi() stops at the
  // first non-digit either way.
  char vparam[64] = {0};
  size_t i = 0;
  while (v[i] && v[i] != ' ' && v[i] != ':' && i < sizeof(verb) - 1) {
    verb[i] = v[i]; ++i;
  }
  if (v[i] == ':') {
    ++i;
    size_t j = 0;
    // Param extends to end of string (no space-terminator) so WIFI_SSID /
    // WIFI_PSK can carry spaces. Trim trailing whitespace afterwards.
    while (v[i] && j < sizeof(vparam) - 1) {
      vparam[j++] = v[i++];
    }
    while (j > 0 && (vparam[j-1] == ' ' || vparam[j-1] == '\t' ||
                     vparam[j-1] == '\r' || vparam[j-1] == '\n')) {
      vparam[--j] = '\0';
    }
  }

  if (strcasecmp(verb, "STATUS") == 0) {
    Serial.printf("mesh-rx: @%s STATUS -> responding\n", target);
    send_status_frame();
  } else if (strcasecmp(verb, "START") == 0) {
    Serial.printf("mesh-rx: @%s START -> arming\n", target);
    arm_camera("mesh");
    mesh.send_text("Cam: START_ACK:OK");
    send_status_frame();
  } else if (strcasecmp(verb, "STOP") == 0) {
    Serial.printf("mesh-rx: @%s STOP -> disarming\n", target);
    disarm_camera("mesh");
    mesh.send_text("Cam: STOP_ACK:OK");
    send_status_frame();
  } else if (strcasecmp(verb, "HB_ON") == 0) {
    hb_enabled = true;
    hb_last_send_ms = millis();
    store_hb_enabled(true);
    mesh.send_text("Cam: HB_ACK:OK");
    Serial.printf("hb: enabled (interval=%umin) [mesh]\n",
                  (unsigned)hb_interval_min);
  } else if (strcasecmp(verb, "HB_OFF") == 0) {
    hb_enabled = false;
    store_hb_enabled(false);
    mesh.send_text("Cam: HB_ACK:OK");
    Serial.println("hb: disabled [mesh]");
  } else if (strcasecmp(verb, "HB_INTERVAL") == 0) {
    int m = vparam[0] ? atoi(vparam) : 0;
    if (m < kHbMinMin || m > kHbMinMax) {
      mesh.send_text("Cam: HB_ACK:ERROR");
      Serial.printf("hb: bad interval '%s' (must be %u..%u) [mesh]\n",
                    vparam, kHbMinMin, kHbMinMax);
    } else {
      hb_interval_min = (uint8_t)m;
      hb_last_send_ms = millis();
      store_hb_interval(hb_interval_min);
      mesh.send_text("Cam: HB_ACK:OK");
      Serial.printf("hb: interval=%umin [mesh]\n", (unsigned)hb_interval_min);
    }
  } else if (strcasecmp(verb, "MODEL_LIST") == 0) {
    Serial.printf("mesh-rx: @%s MODEL_LIST -> responding\n", target);
    send_model_list();
  } else if (strcasecmp(verb, "MODEL_SET") == 0) {
    int id = vparam[0] ? atoi(vparam) : 0;
    if (id <= 0 || !bind_slot(id, "mesh")) {
      mesh.send_text("Cam: MODEL_SET_ACK:ERROR");
      Serial.printf("mesh-rx: @%s MODEL_SET:'%s' -> unknown slot (ERROR)\n",
                    target, vparam);
    } else {
      mesh.send_text("Cam: MODEL_SET_ACK:OK");
      send_status_frame();
      Serial.printf("mesh-rx: @%s MODEL_SET:%d -> OK\n", target, id);
    }
  } else if (strcasecmp(verb, "DEDUP_INTERVAL") == 0) {
    int s = vparam[0] ? atoi(vparam) : 0;
    if (s < kDedupSecMin || s > kDedupSecMax) {
      mesh.send_text("Cam: DEDUP_ACK:ERROR");
      Serial.printf("dedup: bad interval '%s' (must be %u..%u) [mesh]\n",
                    vparam, kDedupSecMin, kDedupSecMax);
    } else {
      cooldown_ms = (uint32_t)s * 1000UL;
      store_dedup_sec((uint16_t)s);
      for (uint32_t &t : last_fired_ms) t = 0;
      mesh.send_text("Cam: DEDUP_ACK:OK");
      Serial.printf("dedup: interval=%us [mesh]\n", (unsigned)s);
    }
  } else if (strcasecmp(verb, "CONF_THRESHOLD") == 0) {
    int v = vparam[0] ? atoi(vparam) : 0;
    if (v < kConfMinAbsMin || v > kConfMinAbsMax) {
      mesh.send_text("Cam: CONF_ACK:ERROR");
      Serial.printf("conf: bad threshold '%s' (must be %u..%u) [mesh]\n",
                    vparam, kConfMinAbsMin, kConfMinAbsMax);
    } else {
      conf_min = (uint8_t)v;
      store_conf_min(conf_min);
      mesh.send_text("Cam: CONF_ACK:OK");
      Serial.printf("conf: threshold=%u [mesh]\n", (unsigned)conf_min);
    }
  } else if (strcasecmp(verb, "WIFI_ON") == 0) {
    bool ok = webui.turn_on();
    mesh.send_text(ok ? "Cam: WIFI_ON_ACK:OK" : "Cam: WIFI_ON_ACK:ERROR");
    Serial.printf("wifi: %s [mesh]\n", ok ? "ON" : "FAIL");
  } else if (strcasecmp(verb, "WIFI_OFF") == 0) {
    webui.turn_off();
    mesh.send_text("Cam: WIFI_OFF_ACK:OK");
    Serial.println("wifi: OFF [mesh]");
  } else if (strcasecmp(verb, "WIFI_SSID") == 0) {
    if (webui.set_ssid(String(vparam))) {
      mesh.send_text("Cam: WIFI_SSID_ACK:OK");
      Serial.printf("wifi: ssid stored '%s' (cycle WIFI_OFF/ON to apply) [mesh]\n", vparam);
    } else {
      mesh.send_text("Cam: WIFI_SSID_ACK:ERROR");
      Serial.printf("wifi: bad ssid (must be 1..32 chars) [mesh]\n");
    }
  } else if (strcasecmp(verb, "WIFI_PSK") == 0) {
    if (webui.set_psk(String(vparam))) {
      mesh.send_text("Cam: WIFI_PSK_ACK:OK");
      Serial.printf("wifi: psk stored (cycle WIFI_OFF/ON to apply) [mesh]\n");
    } else {
      mesh.send_text("Cam: WIFI_PSK_ACK:ERROR");
      Serial.printf("wifi: bad psk (WPA2 requires 8..63 chars) [mesh]\n");
    }
  } else {
    Serial.printf("mesh-rx: @%s %s -> unknown verb (ignored)\n", target, verb);
  }
}

static void print_help() {
  Serial.println(
    "\n--- ai-cam-bridge interactive ---\n"
    "  help               this menu\n"
    "  info               AT+ID? AT+NAME? AT+VER? AT+MODEL?\n"
    "  list               AT+MODELS?  (enumerate installed models)\n"
    "  algos              AT+ALGOS?   (enumerate algorithm types)\n"
    "  sensors            AT+SENSORS? (enumerate sensors / cameras)\n"
    "  current            AT+MODEL?   (which model is currently bound)\n"
    "  bind <id>          AT+MODEL=<id>  (bind a model by numeric id)\n"
    "  model <name>       bind by friendly name; type `model` for the list\n"
    "  unbind / stop      AT+BREAK    (stop any running invoke loop)\n"
    "  invoke [N]         AT+INVOKE=<N>,0,0  (default N=1 frame; N=0 means continuous)\n"
    "  sample [N]         AT+SAMPLE=<N>      (raw image sample, N defaults to 1)\n"
    "  reset              AT+RST       (soft-reset the WE-2)\n"
    "  raw <text>         send 'AT+<text>\\r\\n' verbatim\n"
    "  watch on|off       continuous-invoke + forward detections to mesh\n"
    "  hb [on|off|<n>]    heartbeat status / toggle / set interval (minutes, 1..60)\n"
    "  mesh-status        show BLE state (scanning/pairing/ready/...)\n"
    "  mesh-test <text>   broadcast a one-off text over the mesh\n"
    "  whoami             print the discovered Meshtastic short/long name + num\n"
    "  ble-pin            show the current BLE pairing PIN (from NVS)\n"
    "  ble-pin <N>        store new BLE PIN in NVS and reboot (1..999999)\n"
    "  wifi [on|off]      softAP + web console at 192.168.4.1 (no arg = status)\n"
    "  wifi-ssid <name>   SSID for the operator hotspot (1..32 chars)\n"
    "  wifi-psk <pass>    WPA2 PSK for the operator hotspot (8..63 chars)\n"
    "  grove-show         print the per-slot model alias + class metadata\n"
    "  grove-set <n> <alias> <class1|class2|...>   write one slot to NVS\n"
    "  grove-reset        clear all per-slot metadata from NVS\n"
    "----------------------------------\n");
}

// ---- setup / loop ---------------------------------------------------------

// Bind the operator's last chosen slot (from NVS) and re-arm algorithm
// auto-detect on boot. Idempotent — the C3 issues these each cold start.
// Falls back to slot 1 if NVS holds a slot id that's no longer in g_slots.
static void auto_configure() {
  uint8_t s = load_active_slot();
  if (!bind_slot(s, "boot")) bind_slot(1, "boot-fallback");
}

void setup() {
  Serial.begin(115200);
  delay(2000);
  Serial.println("\n=== ai-cam-bridge (interactive SSCMA) ===");
  Serial.printf("compiled %s %s\n", __DATE__, __TIME__);

  // NimBLE init MUST run before SSCMA / std::vector / String allocations.
  // See memory: feedback_init_nimble_before_sscma.md (reverse order triggers
  // a heap-aliasing panic in the BLE scan callback).
  uint32_t ble_pin = load_ble_pin();
  Serial.printf("BLE PIN: %u (from NVS, default %u)\n",
                (unsigned)ble_pin, (unsigned)kBlePinDefault);
  mesh.begin("aicam-bridge", ble_pin, /*peer_name=*/"cam_");
  mesh.set_text_handler(on_mesh_text);
  Serial.println("mesh: BLE central started, scanning for Meshtastic 'cam_*' peer...");

  load_grove_slots();
  Serial.printf("grove: %u slot(s) in alias table%s\n",
                (unsigned)g_slots.size(),
                g_slots.size() == 5 && g_slots.front().alias == "person"
                  ? " (defaults, no NVS override)"
                  : " (from NVS)");

  load_hb_config();
  Serial.printf("heartbeat: %s, interval=%umin\n",
                hb_enabled ? "ON" : "OFF", (unsigned)hb_interval_min);

  uint16_t dedup_sec = load_dedup_sec();
  cooldown_ms = (uint32_t)dedup_sec * 1000UL;
  Serial.printf("dedup: interval=%us (from NVS or default)\n", (unsigned)dedup_sec);

  conf_min = load_conf_min();
  Serial.printf("conf: threshold=%u (from NVS or default)\n", (unsigned)conf_min);

  bool ok = AI.begin();
  Serial.printf("AI.begin -> %s\n", ok ? "OK" : "FAIL");
  if (ok) {
    Serial.printf("device='%s' id='%s'\n", AI.name(), AI.ID());
    auto_configure();
    Serial.println("auto: bound model 1, algo=0 (firmware auto-detect)");
  }

  // Wi-Fi softAP last — NimBLE central is already up and BLE/Wi-Fi
  // coexistence prefers the BLE link to be established first.
  webui.begin();

  print_help();
  Serial.print("ai> ");
}

// Drive a single round of the watch loop: poll the WE-2 for one inference,
// and forward every box above the score threshold whose per-class debounce
// has elapsed. No-op if the SSCMA library returns no boxes or invoke fails.
static void watch_tick() {
  // The library issues AT+INVOKE=1,0,0 internally and blocks until the
  // response (or timeout); on this hardware that's typically ~80 ms.
  int rc = AI.invoke(1, false, false);
  if (rc != 0) return;

  auto &dets = AI.boxes();
  if (dets.empty()) return;

  uint32_t now = millis();
  for (auto &b : dets) {
    if (b.score <= conf_min) continue;
    if (b.target >= 80) continue;
    uint32_t since = now - last_fired_ms[b.target];
    if (last_fired_ms[b.target] != 0 && since < cooldown_ms) continue;
    last_fired_ms[b.target] = now ? now : 1;

    String label = class_label_for(g_current_slot, b.target);
    char msg[64];
    snprintf(msg, sizeof(msg), "@CAM TRIGGERED:%s@%u",
             label.c_str(), (unsigned)b.score);
    bool queued = mesh.send_text(msg);
    Serial.printf("watch: %s (score=%u) -> mesh %s\n",
                  label.c_str(), (unsigned)b.score,
                  queued ? "queued" : "DROPPED (BLE not ready)");
  }
}

void loop() {
  mesh.loop();           // BLE state machine
  webui.tick();          // DNS + HTTP, cheap when AP is off
  if (!sent_armed && mesh.is_connected()) {
    if (send_status_frame()) {
      sent_armed = true;
      hb_last_send_ms = millis();   // anchor periodic cadence to boot STATUS
    }
  }
  if (sent_armed && hb_enabled &&
      (millis() - hb_last_send_ms) >= (uint32_t)hb_interval_min * 60000UL) {
    hb_last_send_ms = millis();
    send_status_frame();
  }
  drain_responses(0);    // SSCMA spontaneous events
  if (watch_enabled && (millis() - last_invoke_ms) >= kInvokePeriodMs) {
    last_invoke_ms = millis();
    watch_tick();
  }
  // REPL
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      line.trim();
      if (line.length() == 0) { Serial.print("ai> "); return; }

      String cmd = line, arg = "";
      int sp = line.indexOf(' ');
      if (sp >= 0) { cmd = line.substring(0, sp); arg = line.substring(sp + 1); arg.trim(); }
      cmd.toLowerCase();

      if      (cmd == "help" || cmd == "?")     print_help();
      else if (cmd == "info")                   { send_at("ID?"); drain_responses(300);
                                                  send_at("NAME?"); drain_responses(300);
                                                  send_at("VER?"); drain_responses(300);
                                                  send_at("MODEL?"); drain_responses(300); }
      else if (cmd == "list" || cmd == "models")send_at("MODELS?");
      else if (cmd == "algos")                  send_at("ALGOS?");
      else if (cmd == "sensors")                send_at("SENSORS?");
      else if (cmd == "current")                send_at("MODEL?");
      else if (cmd == "bind") {
        if (!arg.length()) Serial.println("usage: bind <id>");
        else if (!bind_slot(arg.toInt(), "repl bind")) {
          Serial.printf("unknown slot id '%s'; type `model` for the list\n", arg.c_str());
        }
      }
      else if (cmd == "model") {
        if (arg.length()) {
          const GroveSlot *s = slot_by_alias(arg);
          if (!s) {
            Serial.printf("unknown model alias '%s'; type `model` for the list\n", arg.c_str());
          } else {
            Serial.printf("binding model %d (%s)\n", s->id, s->alias.c_str());
            bind_slot(s->id, "repl model");
          }
        } else {
          Serial.println("model aliases (from NVS or defaults):");
          for (auto &s : g_slots) {
            Serial.printf("  id %2d  %-12s  classes: ", s.id, s.alias.c_str());
            for (size_t i = 0; i < s.classes.size(); ++i) {
              if (i) Serial.print(", ");
              Serial.print(s.classes[i].c_str());
            }
            Serial.println();
          }
        }
      }
      else if (cmd == "grove-show") {
        Serial.printf("current bound slot: %d\n", g_current_slot);
        Serial.println("alias table:");
        for (auto &s : g_slots) {
          Serial.printf("  slot %2d  alias=%-12s  classes=", s.id, s.alias.c_str());
          for (size_t i = 0; i < s.classes.size(); ++i) {
            if (i) Serial.print('|');
            Serial.print(s.classes[i].c_str());
          }
          Serial.println();
        }
      }
      else if (cmd == "grove-reset") {
        if (nvs_clear_slots()) {
          populate_default_slots();
          Serial.println("grove: cleared NVS slot metadata, reverted to defaults");
        } else {
          Serial.println("grove-reset: NVS write FAILED");
        }
      }
      else if (cmd == "grove-set") {
        // grove-set <slot> <alias> <class1|class2|...>
        int sp1 = arg.indexOf(' ');
        int sp2 = sp1 >= 0 ? arg.indexOf(' ', sp1 + 1) : -1;
        if (sp1 < 0 || sp2 < 0) {
          Serial.println("usage: grove-set <slot> <alias> <class1|class2|...>");
        } else {
          int    slot    = arg.substring(0, sp1).toInt();
          String alias   = arg.substring(sp1 + 1, sp2);
          String classes = arg.substring(sp2 + 1);
          alias.trim(); classes.trim();
          if (slot < 1 || slot > 31 || alias.length() == 0) {
            Serial.println("grove-set: slot must be 1..31 and alias non-empty");
          } else if (!nvs_set_slot(slot, alias, classes)) {
            Serial.println("grove-set: NVS write FAILED");
          } else {
            load_grove_slots();
            Serial.printf("grove-set: slot %d alias='%s' classes='%s'\n",
                          slot, alias.c_str(), classes.c_str());
          }
        }
      }
      else if (cmd == "unbind" || cmd == "stop")send_at("BREAK");
      else if (cmd == "invoke") {
        int n = arg.length() ? arg.toInt() : 1;
        send_at("INVOKE=" + String(n) + ",0,0");
      }
      else if (cmd == "sample") {
        int n = arg.length() ? arg.toInt() : 1;
        send_at("SAMPLE=" + String(n));
      }
      else if (cmd == "reset")                  send_at("RST");
      else if (cmd == "watch") {
        if (arg.equalsIgnoreCase("on") || arg == "1") {
          arm_camera("repl");
          Serial.printf("watch: ON (threshold=%u, cooldown=%lums, every %lums)\n",
                        (unsigned)conf_min,
                        (unsigned long)cooldown_ms,
                        (unsigned long)kInvokePeriodMs);
        } else if (arg.equalsIgnoreCase("off") || arg == "0") {
          disarm_camera("repl");
        } else {
          Serial.printf("watch: %s — usage: watch on|off\n",
                        watch_enabled ? "ON" : "OFF");
        }
      }
      else if (cmd == "hb") {
        if (arg.length() == 0) {
          uint32_t elapsed_ms = millis() - hb_last_send_ms;
          uint32_t period_ms  = (uint32_t)hb_interval_min * 60000UL;
          uint32_t next_in_s  = (hb_enabled && period_ms > elapsed_ms)
                                  ? (period_ms - elapsed_ms) / 1000UL : 0UL;
          Serial.printf("hb: %s, interval=%umin, next in %lus\n",
                        hb_enabled ? "ON" : "OFF",
                        (unsigned)hb_interval_min,
                        (unsigned long)next_in_s);
        } else if (arg.equalsIgnoreCase("on")) {
          hb_enabled = true;
          hb_last_send_ms = millis();
          store_hb_enabled(true);
          Serial.printf("hb: enabled (interval=%umin) [repl]\n",
                        (unsigned)hb_interval_min);
        } else if (arg.equalsIgnoreCase("off")) {
          hb_enabled = false;
          store_hb_enabled(false);
          Serial.println("hb: disabled [repl]");
        } else {
          int m = arg.toInt();
          if (m < kHbMinMin || m > kHbMinMax) {
            Serial.printf("hb: bad interval (must be %u..%u)\n",
                          kHbMinMin, kHbMinMax);
          } else {
            hb_interval_min = (uint8_t)m;
            hb_last_send_ms = millis();
            store_hb_interval(hb_interval_min);
            Serial.printf("hb: interval=%umin [repl]\n",
                          (unsigned)hb_interval_min);
          }
        }
      }
      else if (cmd == "mesh-status") {
        Serial.printf("mesh: state=%s connected=%s\n",
                      mesh.state_name(),
                      mesh.is_connected() ? "yes" : "no");
      }
      else if (cmd == "whoami") {
        const char *sn = mesh.short_name();
        const char *ln = mesh.long_name();
        Serial.printf("identity: short='%s' long='%s' num=%u\n",
                      sn[0] ? sn : "(unknown)",
                      ln[0] ? ln : "(unknown)",
                      (unsigned)mesh.my_node_num());
      }
      else if (cmd == "mesh-test") {
        if (!arg.length()) { Serial.println("usage: mesh-test <text>"); }
        else {
          bool ok = mesh.send_text(arg.c_str());
          Serial.printf("mesh-test: send_text -> %s\n", ok ? "queued" : "DROPPED (not READY)");
        }
      }
      else if (cmd == "ble-pin") {
        if (!arg.length()) {
          Serial.printf("ble-pin: current=%u (NVS '%s/%s', default %u)\n",
                        (unsigned)load_ble_pin(),
                        kBlePrefsNamespace, kBlePrefsKey,
                        (unsigned)kBlePinDefault);
        } else {
          long n = arg.toInt();
          if (n < 1 || n > 999999) {
            Serial.println("ble-pin: PIN must be 1..999999");
          } else if (!store_ble_pin((uint32_t)n)) {
            Serial.println("ble-pin: NVS write FAILED");
          } else {
            Serial.printf("ble-pin: stored %ld in NVS, rebooting to apply...\n", n);
            delay(200);
            ESP.restart();
          }
        }
      }
      else if (cmd == "wifi") {
        if (arg.length() == 0) {
          Serial.printf("wifi: %s ssid='%s' psk='%s' clients=%u\n",
                        webui.is_on() ? "ON" : "OFF",
                        webui.ssid().c_str(), webui.psk().c_str(),
                        (unsigned)webui.client_count());
          if (webui.is_on()) {
            Serial.printf("wifi: open http://192.168.4.1/ to access the operator console\n");
          }
        } else if (arg.equalsIgnoreCase("on") || arg == "1") {
          if (webui.turn_on()) {
            Serial.printf("wifi: ON (ssid='%s' ip=192.168.4.1) [repl]\n",
                          webui.ssid().c_str());
          } else {
            Serial.println("wifi: turn_on FAILED");
          }
        } else if (arg.equalsIgnoreCase("off") || arg == "0") {
          webui.turn_off();
          Serial.println("wifi: OFF [repl]");
        } else {
          Serial.println("usage: wifi [on|off]   (no arg = show status)");
        }
      }
      else if (cmd == "wifi-ssid") {
        if (!arg.length()) {
          Serial.printf("wifi-ssid: current='%s' (length %u..%u)\n",
                        webui.ssid().c_str(), 1u, 32u);
        } else if (!webui.set_ssid(arg)) {
          Serial.println("wifi-ssid: rejected (must be 1..32 chars)");
        } else {
          Serial.printf("wifi-ssid: stored '%s' (takes effect on next `wifi off`/`wifi on`)\n",
                        arg.c_str());
        }
      }
      else if (cmd == "wifi-psk") {
        if (!arg.length()) {
          Serial.printf("wifi-psk: current='%s' (WPA2 requires 8..63 chars)\n",
                        webui.psk().c_str());
        } else if (!webui.set_psk(arg)) {
          Serial.println("wifi-psk: rejected (WPA2 requires 8..63 chars)");
        } else {
          Serial.printf("wifi-psk: stored (takes effect on next `wifi off`/`wifi on`)\n");
        }
      }
      else if (cmd == "scan") {
        Serial.println("I2C bus scan @100kHz on D4(SDA=6)/D5(SCL=7)...");
        Wire.setClock(100000);
        int found = 0;
        for (uint8_t addr = 1; addr < 127; ++addr) {
          Wire.beginTransmission(addr);
          uint8_t err = Wire.endTransmission();
          if (err == 0) {
            Serial.printf("  device at 0x%02X (7-bit) / 0x%02X (write-8b)\n", addr, addr << 1);
            ++found;
          }
        }
        Serial.printf("scan done, %d device(s)\n", found);
      }
      else if (cmd == "raw") {
        if (arg.length()) send_at(arg);
        else Serial.println("usage: raw <at-command-body>");
      }
      else Serial.printf("unknown command: %s (try `help`)\n", cmd.c_str());

      drain_responses(500);   // collect the synchronous response
      line = "";
      Serial.print("ai> ");
    } else {
      line += c;
    }
  }
}
