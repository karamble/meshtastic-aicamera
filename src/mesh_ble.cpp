// MeshBLE: BLE central that pairs with a Meshtastic node and writes text
// frames to its phone-API. Protocol details (GATT UUIDs, field numbers,
// pairing quirks) live in memory/reference_meshtastic_ble_phoneapi.md.

#include "mesh_ble.h"

#include <NimBLEDevice.h>

// Meshtastic GATT UUIDs.
static const NimBLEUUID kMeshService  ("6ba1b218-15a8-461f-9fa8-5dcae273eafd");
static const NimBLEUUID kCharToRadio  ("f75c76d2-129e-4dad-a1dd-7866124401e7");
static const NimBLEUUID kCharFromRadio("2c55e69e-4993-11ed-b878-0242ac120002");
static const NimBLEUUID kCharFromNum  ("ed9da18c-a800-4f66-a670-aa7547e34453");

// File-scope handles so NimBLE C-style callbacks can reach them.
static NimBLEClient            *s_client       = nullptr;
static NimBLERemoteService     *s_service      = nullptr;
static NimBLERemoteCharacteristic *s_to_radio  = nullptr;
static NimBLERemoteCharacteristic *s_from_radio= nullptr;
static NimBLERemoteCharacteristic *s_from_num  = nullptr;
static NimBLEAdvertisedDevice  *s_peer         = nullptr;

// FromRadio is READ-only, FromNum notifies on each new packet. The notify
// callback sets s_data_pending; loop() drains FromRadio by repeated reads.
static volatile bool             s_data_pending = false;
static uint32_t                  s_passkey     = 123456;
static volatile bool             s_peer_found  = false;
static volatile bool             s_disconnected= false;

// Prefix filter on advertised local-name (empty = accept any Meshtastic peer).
static char                      s_name_filter[32] = {0};

// want_config_id handshake nonce; without it the node drops ToRadio writes.
static uint32_t                  s_handshake_nonce    = 0;
static volatile bool             s_handshake_complete = false;

// Hand-rolled minimal protobuf encoder.
// Wire type: 0=varint, 1=64bit, 2=length-delim, 5=32bit. tag=(field<<3)|wire.

static size_t encode_varint(uint64_t v, uint8_t *out, size_t out_max) {
  size_t n = 0;
  while (v >= 0x80) {
    if (n >= out_max) return 0;
    out[n++] = (uint8_t)(v | 0x80);
    v >>= 7;
  }
  if (n >= out_max) return 0;
  out[n++] = (uint8_t)v;
  return n;
}

static size_t encode_tag(uint32_t field, uint32_t wire, uint8_t *out, size_t out_max) {
  return encode_varint(((uint64_t)field << 3) | wire, out, out_max);
}

// Encode field as varint.
static size_t encode_varint_field(uint32_t field, uint64_t value, uint8_t *out, size_t out_max) {
  size_t n = encode_tag(field, 0, out, out_max);
  if (n == 0) return 0;
  size_t m = encode_varint(value, out + n, out_max - n);
  if (m == 0) return 0;
  return n + m;
}

// Encode fixed32 field (wire type 5, 4 little-endian bytes).
// Used for MeshPacket.from/to/id which are declared `fixed32` in mesh.proto
// — these are NOT varints, so the wire payload is always 4 bytes regardless
// of value magnitude.
static size_t encode_fixed32_field(uint32_t field, uint32_t value,
                                   uint8_t *out, size_t out_max) {
  size_t n = encode_tag(field, 5, out, out_max);
  if (n == 0) return 0;
  if (n + 4 > out_max) return 0;
  out[n + 0] = (uint8_t)(value      );
  out[n + 1] = (uint8_t)(value >>  8);
  out[n + 2] = (uint8_t)(value >> 16);
  out[n + 3] = (uint8_t)(value >> 24);
  return n + 4;
}

// Encode length-delimited field (bytes/string/sub-message).
static size_t encode_bytes_field(uint32_t field, const uint8_t *data, size_t data_len,
                                 uint8_t *out, size_t out_max) {
  size_t n = encode_tag(field, 2, out, out_max);
  if (n == 0) return 0;
  size_t m = encode_varint((uint64_t)data_len, out + n, out_max - n);
  if (m == 0) return 0;
  if (n + m + data_len > out_max) return 0;
  memcpy(out + n + m, data, data_len);
  return n + m + data_len;
}

// ToRadio { want_config_id = nonce } - Meshtastic phone-API handshake.
static size_t encode_want_config_id(uint32_t nonce, uint8_t *out, size_t out_max) {
  return encode_varint_field(3, nonce, out, out_max);  // field 3, uint32 varint
}

// Walk a FromRadio packet, return true on FromRadio.config_complete_id == nonce
// (field 7, uint32 varint). Skips unknown top-level fields by wire type.
static bool from_radio_has_config_complete(const uint8_t *buf, size_t len, uint32_t nonce) {
  size_t i = 0;
  while (i < len) {
    uint64_t tag = 0;
    int shift = 0;
    while (i < len) {
      uint8_t b = buf[i++];
      tag |= ((uint64_t)(b & 0x7f)) << shift;
      if (!(b & 0x80)) break;
      shift += 7;
      if (shift > 63) return false;
    }
    uint32_t field = (uint32_t)(tag >> 3);
    uint32_t wire  = (uint32_t)(tag & 7);

    if (field == 7 && wire == 0) {
      uint64_t v = 0; int s = 0;
      while (i < len) {
        uint8_t b = buf[i++];
        v |= ((uint64_t)(b & 0x7f)) << s;
        if (!(b & 0x80)) break;
        s += 7;
        if (s > 63) return false;
      }
      return (uint32_t)v == nonce;
    }

    switch (wire) {
      case 0: while (i < len && (buf[i++] & 0x80)) { } break;
      case 1: i += 8; break;
      case 5: i += 4; break;
      case 2: {
        uint64_t l = 0; int s = 0;
        while (i < len) {
          uint8_t b = buf[i++];
          l |= ((uint64_t)(b & 0x7f)) << s;
          if (!(b & 0x80)) break;
          s += 7;
          if (s > 63) return false;
        }
        if (i + l > len) return false;
        i += (size_t)l;
        break;
      }
      default: return false;
    }
  }
  return false;
}

size_t MeshBLE::encode_text_to_radio(const char *text, uint8_t *out, size_t out_max) {
  if (!text) return 0;
  const size_t text_len = strlen(text);
  if (text_len == 0 || text_len > 200) return 0;  // Meshtastic max payload ~237 bytes

  // Data { portnum=1, payload=text }
  uint8_t data_buf[256];
  size_t  data_len = 0;
  {
    size_t n;
    n = encode_varint_field(1, 1, data_buf + data_len, sizeof(data_buf) - data_len);
    if (n == 0) return 0;
    data_len += n;
    n = encode_bytes_field(2, (const uint8_t *)text, text_len,
                           data_buf + data_len, sizeof(data_buf) - data_len);
    if (n == 0) return 0;
    data_len += n;
  }

  // MeshPacket { to=BROADCAST(fixed32), channel(varint), decoded(Data),
  // id=random non-zero(fixed32), hop_limit=3(varint) }.
  // id MUST be non-zero or the firmware dedupes against the last id=0 packet.
  uint8_t pkt_buf[280];
  size_t  pkt_len = 0;
  {
    size_t n;
    n = encode_fixed32_field(2, 0xFFFFFFFFu, pkt_buf + pkt_len, sizeof(pkt_buf) - pkt_len);
    if (n == 0) return 0;
    pkt_len += n;
    n = encode_varint_field(3, 0, pkt_buf + pkt_len, sizeof(pkt_buf) - pkt_len);
    if (n == 0) return 0;
    pkt_len += n;
    n = encode_bytes_field(4, data_buf, data_len, pkt_buf + pkt_len, sizeof(pkt_buf) - pkt_len);
    if (n == 0) return 0;
    pkt_len += n;
    uint32_t pkt_id = (uint32_t)esp_random();
    if (pkt_id == 0) pkt_id = 1;
    n = encode_fixed32_field(6, pkt_id, pkt_buf + pkt_len, sizeof(pkt_buf) - pkt_len);
    if (n == 0) return 0;
    pkt_len += n;
    n = encode_varint_field(9, 3, pkt_buf + pkt_len, sizeof(pkt_buf) - pkt_len);
    if (n == 0) return 0;
    pkt_len += n;
  }

  // ToRadio { packet = MeshPacket }
  return encode_bytes_field(1, pkt_buf, pkt_len, out, out_max);
}

// ── NimBLE callbacks ───────────────────────────────────────────────────────
class MeshScanCB : public NimBLEScanCallbacks {
  void onResult(const NimBLEAdvertisedDevice *adv) override {
    if (s_peer_found) return;                            // connect already in flight
    if (!adv || !adv->isAdvertisingService(kMeshService)) return;

    String name = String(adv->getName().c_str());
    log_i("mesh: meshtastic peer %s name='%s' rssi=%d",
          adv->getAddress().toString().c_str(),
          name.c_str(),
          adv->getRSSI());

    // Name-prefix filter (Meshtastic advertises as "<shortName>_<id4>").
    if (s_name_filter[0] && !name.startsWith(s_name_filter)) return;

    if (s_peer) { delete s_peer; s_peer = nullptr; }
    s_peer = new NimBLEAdvertisedDevice(*adv);
    s_peer_found = true;
    NimBLEDevice::getScan()->stop();
  }
};
static MeshScanCB s_scan_cb;

class MeshClientCB : public NimBLEClientCallbacks {
  void onConnect(NimBLEClient *client) override {
    log_i("mesh: BLE connected, RSSI=%d", client->getRssi());
  }
  void onDisconnect(NimBLEClient *client, int reason) override {
    log_w("mesh: BLE disconnected reason=0x%02x", reason);
    s_disconnected = true;
  }
  void onPassKeyEntry(NimBLEConnInfo &connInfo) override {
    log_i("mesh: providing PIN %u for pairing", (unsigned)s_passkey);
    NimBLEDevice::injectPassKey(connInfo, s_passkey);
  }
  uint32_t onPassKeyRequest() { return s_passkey; }
  bool onConfirmPIN(const uint32_t pass_key) { return pass_key == s_passkey; }
  void onAuthenticationComplete(NimBLEConnInfo &info) override {
    if (info.isEncrypted()) log_i("mesh: pairing complete (encrypted link)");
    else                    log_w("mesh: pairing failed (link not encrypted)");
  }
};
static MeshClientCB s_client_cb;

// FromNum notify: latch data-pending. loop() drains FromRadio in main task.
static void from_num_notify(NimBLERemoteCharacteristic *,
                            uint8_t *data, size_t len, bool /*isNotify*/) {
  s_data_pending = true;
  log_d("mesh: FromNum notify %zu bytes", len);
}

// Read FromRadio until empty, watching for the handshake config_complete_id.
// (queue exhausted), parsing each protobuf-encoded FromRadio message for the
// config_complete_id marker that ends the want_config handshake.
static void drain_from_radio() {
  if (!s_from_radio) return;
  for (int i = 0; i < 256; ++i) {          // generous cap; Meshtastic config dumps can be 50+ packets
    NimBLEAttValue val = s_from_radio->readValue();
    size_t len = val.length();
    if (len == 0) break;                   // queue empty
    log_d("mesh: FromRadio rx len=%u", (unsigned)len);
    if (!s_handshake_complete && s_handshake_nonce != 0) {
      if (from_radio_has_config_complete(val.data(), len, s_handshake_nonce)) {
        s_handshake_complete = true;
        log_i("mesh: handshake config_complete_id=%u matched", (unsigned)s_handshake_nonce);
      }
    }
  }
}

// ── MeshBLE public methods ──────────────────────────────────────────────────
void MeshBLE::begin(const char *device_name, uint32_t passkey, const char *peer_name) {
  _passkey = passkey;
  s_passkey = passkey;
  s_name_filter[0] = 0;
  if (peer_name && peer_name[0]) {
    strncpy(s_name_filter, peer_name, sizeof(s_name_filter) - 1);
    s_name_filter[sizeof(s_name_filter) - 1] = 0;
    log_i("mesh: peer-name filter set to '%s'", s_name_filter);
  }

  log_i("mesh: NimBLE init (device='%s', passkey=%u)", device_name, (unsigned)passkey);
  NimBLEDevice::init(device_name);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);

  // Legacy pairing only - Meshtastic 2.5.x rejects LESC.
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
  NimBLEDevice::setSecurityAuth(/*bonding=*/true, /*mitm=*/true, /*sc=*/false);

  NimBLEScan *scan = NimBLEDevice::getScan();
  scan->setScanCallbacks(&s_scan_cb, /*wantDuplicates=*/false);
  scan->setActiveScan(true);
  scan->setInterval(45);   // ~28 ms
  scan->setWindow(15);

  _state = SCANNING;
  _last_scan_start = millis();
  scan->start(0, /*continue=*/false);
}

const char *MeshBLE::state_name() const {
  switch (_state) {
    case IDLE:         return "idle";
    case SCANNING:     return "scanning";
    case CONNECTING:   return "connecting";
    case PAIRING:      return "pairing";
    case DISCOVERING:  return "discovering";
    case READY:        return "ready";
    case DISCONNECTED: return "disconnected";
  }
  return "?";
}

void MeshBLE::loop() {
  if (s_disconnected) {
    s_disconnected = false;
    // s_client is intentionally kept; NimBLE reuses it.
    s_to_radio          = nullptr;
    s_from_radio        = nullptr;
    s_from_num          = nullptr;
    s_service           = nullptr;
    s_handshake_nonce   = 0;
    s_handshake_complete= false;
    s_data_pending      = false;
    _handshake_sent     = false;
    _handshake_complete = false;
    _state              = DISCONNECTED;
  }

  switch (_state) {
  case IDLE:
    break;

  case SCANNING:
    if (s_peer_found) {
      s_peer_found = false;
      _state = CONNECTING;
      _last_attempt = millis();
      log_i("mesh: connecting to %s", s_peer->getAddress().toString().c_str());
      if (!s_client) {
        s_client = NimBLEDevice::createClient();
        s_client->setClientCallbacks(&s_client_cb, /*deleteCallbacks=*/false);
      }
      if (!s_client->connect(s_peer)) {
        log_w("mesh: connect failed, returning to scan");
        _state = SCANNING;
        NimBLEDevice::getScan()->start(0, false);
      } else {
        _state = PAIRING;
        if (!s_client->secureConnection()) {
          log_w("mesh: secureConnection failed");
          s_client->disconnect();
          _state = SCANNING;
          NimBLEDevice::getScan()->start(0, false);
        } else {
          _state = DISCOVERING;
        }
      }
    }
    break;

  case CONNECTING:
  case PAIRING:
    break;        // transitions handled inline above

  case DISCOVERING: {
    log_i("mesh: discovering service");
    s_service = s_client->getService(kMeshService);
    if (!s_service) {
      log_w("mesh: service UUID not found, disconnecting");
      s_client->disconnect();
      _state = SCANNING;
      NimBLEDevice::getScan()->start(0, false);
      break;
    }
    s_to_radio   = s_service->getCharacteristic(kCharToRadio);
    s_from_radio = s_service->getCharacteristic(kCharFromRadio);
    s_from_num   = s_service->getCharacteristic(kCharFromNum);
    if (!s_to_radio || !s_from_radio || !s_from_num) {
      log_w("mesh: characteristic UUIDs not found (to=%p from=%p fromnum=%p)",
            s_to_radio, s_from_radio, s_from_num);
      s_client->disconnect();
      _state = SCANNING;
      NimBLEDevice::getScan()->start(0, false);
      break;
    }
    if (s_from_num->canNotify()) {
      s_from_num->subscribe(true, from_num_notify);
    } else {
      log_w("mesh: FromNum has no NOTIFY property - won't see node replies");
    }
    log_i("mesh: ready");
    _state = READY;

    // Kick off the want_config_id handshake.
    s_handshake_nonce = (uint32_t)esp_random();
    if (s_handshake_nonce == 0) s_handshake_nonce = 0xCAFE0001;
    s_handshake_complete = false;
    uint8_t  hbuf[16];
    size_t   hn = encode_want_config_id(s_handshake_nonce, hbuf, sizeof(hbuf));
    if (hn > 0 && s_to_radio->writeValue(hbuf, hn, /*response=*/false)) {
      _handshake_nonce   = s_handshake_nonce;
      _handshake_sent    = true;
      _handshake_sent_at = millis();
      log_i("mesh: want_config_id=%u sent, awaiting config_complete",
            (unsigned)s_handshake_nonce);
    } else {
      log_w("mesh: failed to write want_config_id");
    }
    s_data_pending = true;     // drain any queued packets
    break;
  }

  case READY:
    if (s_data_pending) {
      s_data_pending = false;
      drain_from_radio();
    }
    if (s_handshake_complete && !_handshake_complete) {
      _handshake_complete = true;
      log_i("mesh: handshake complete after %lums",
            (unsigned long)(millis() - _handshake_sent_at));
      if (!_sent_hello) {
        _sent_hello = true;
        const char *hello = "@CAM bridge online";
        bool ok = send_text(hello);
        log_i("mesh: hello send_text -> %s", ok ? "queued" : "FAILED");
      }
    }
    break;

  case DISCONNECTED:
    if (millis() - _last_attempt > 2000) {
      _last_attempt = millis();
      log_i("mesh: restarting scan after disconnect");
      _state = SCANNING;
      NimBLEDevice::getScan()->start(0, false);
    }
    break;
  }
}

bool MeshBLE::send_text(const char *text) {
  if (_state != READY || !s_to_radio) return false;
  if (!_handshake_complete) {
    log_w("mesh: send_text dropped, want_config handshake not complete");
    return false;
  }

  uint8_t buf[320];
  size_t  n = encode_text_to_radio(text, buf, sizeof(buf));
  if (n == 0) {
    log_w("mesh: encode failed for text '%s'", text ? text : "(null)");
    return false;
  }

  // WRITE_WITH_RESPONSE so we see GATT-level rejection.
  bool ok = s_to_radio->writeValue(buf, n, /*response=*/true);
  if (!ok) log_w("mesh: ToRadio write rejected (text='%s', %u bytes)", text, (unsigned)n);
  else     log_i("mesh: sent '%s' (%u bytes)", text, (unsigned)n);
  return ok;
}
