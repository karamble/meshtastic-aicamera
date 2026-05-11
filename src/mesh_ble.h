// MeshBLE: BLE central client for a Meshtastic node. Pairs with a fixed
// passkey, completes the want_config_id handshake, then writes text frames
// to the node's ToRadio characteristic. Protocol reference in memory:
// reference_meshtastic_ble_phoneapi.md.
//
// State machine (driven by loop(), non-blocking):
//   IDLE -> SCANNING -> CONNECTING -> PAIRING -> DISCOVERING -> READY
//                                                       (on disconnect: back to SCANNING)

#pragma once

#include <Arduino.h>
#include <stdint.h>

class MeshBLE {
public:
  enum State : uint8_t {
    IDLE = 0,
    SCANNING,
    CONNECTING,
    PAIRING,
    DISCOVERING,
    READY,
    DISCONNECTED,
  };

  // Initialise NimBLE, start scanning. `peer_name` is an optional prefix
  // filter on the peer's advertised local name. Meshtastic advertises as
  // `<shortName>_<id4>`; for this project the convention is shortName="cam"
  // so the default `cam_` prefix matches any aicam node.
  void begin(const char *device_name = "aicam-bridge",
             uint32_t    passkey      = 123456,
             const char *peer_name    = "cam_");

  // Drive the state machine. Call from main loop().
  void loop();

  bool is_connected() const { return _state == READY; }
  State state() const { return _state; }
  const char *state_name() const;

  // Encode `text` as a TEXT_MESSAGE_APP broadcast on channel 0 and write
  // it to ToRadio. Returns false if not READY or handshake not complete.
  bool send_text(const char *text);

private:
  State    _state           = IDLE;
  uint32_t _passkey         = 123456;
  uint32_t _last_scan_start = 0;
  uint32_t _last_attempt    = 0;

  volatile bool _peer_found = false;

  // want_config_id handshake state.
  uint32_t _handshake_nonce    = 0;
  bool     _handshake_sent     = false;
  uint32_t _handshake_sent_at  = 0;
  bool     _handshake_complete = false;

  // Latched after the first READY; gates the one-time "@CAM bridge online".
  bool _sent_hello = false;

  static size_t encode_text_to_radio(const char *text,
                                     uint8_t    *out,
                                     size_t      out_max);
};
