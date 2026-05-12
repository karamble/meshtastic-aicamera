#include "webui.h"

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <Preferences.h>

#include "bridge_actions.h"

// ---- NVS contract ---------------------------------------------------------
// Shares the "aicam" Preferences namespace with the other bridge settings
// (BLE PIN, HB config, active slot, dedup, conf). Keys owned by this module:
// "wifi_ssid", "wifi_psk", "wifi_en".
static const char *kPrefsNs   = "aicam";
static const char *kKeySsid   = "wifi_ssid";
static const char *kKeyPsk    = "wifi_psk";
static const char *kKeyEn     = "wifi_en";

static const char *kDefaultSsid = "aicam-bridge";
static const char *kDefaultPsk  = "aicam-12345";  // 11 chars, satisfies WPA2 ≥ 8

static const size_t kSsidMin = 1,  kSsidMax = 32;
static const size_t kPskMin  = 8,  kPskMax  = 63;

static const uint8_t kApChannel  = 1;
static const uint8_t kApMaxConn  = 2;     // operator console; one at a time is plenty
static const uint16_t kDnsPort   = 53;
static const uint16_t kHttpPort  = 80;

// ---- module state ---------------------------------------------------------
static WebServer *s_http = nullptr;
static DNSServer *s_dns  = nullptr;
static bool      s_on    = false;
static String    s_ssid;
static String    s_psk;

WebUI webui;

// ---- NVS helpers ----------------------------------------------------------
static String nvs_load_ssid() {
  Preferences p; p.begin(kPrefsNs, true);
  String s = p.getString(kKeySsid, kDefaultSsid);
  p.end();
  if (s.length() < kSsidMin || s.length() > kSsidMax) s = kDefaultSsid;
  return s;
}

static String nvs_load_psk() {
  Preferences p; p.begin(kPrefsNs, true);
  String s = p.getString(kKeyPsk, kDefaultPsk);
  p.end();
  if (s.length() < kPskMin || s.length() > kPskMax) s = kDefaultPsk;
  return s;
}

// On a fresh-flashed bridge with no NVS preference, the AP comes up ON so
// the operator can do first-boot setup over Wi-Fi without diginode-cc or
// USB access. The OFF state is only persisted once an operator explicitly
// sends WIFI_OFF / `wifi off`; after that, the choice survives reboots.
struct WifiEn { bool en; bool from_nvs; };

static WifiEn nvs_load_en() {
  Preferences p; p.begin(kPrefsNs, true);
  bool present = p.isKey(kKeyEn);
  bool en = present ? p.getBool(kKeyEn) : true;
  p.end();
  return {en, present};
}

static bool nvs_store_ssid(const String &s) {
  if (s.length() < kSsidMin || s.length() > kSsidMax) return false;
  Preferences p;
  if (!p.begin(kPrefsNs, false)) return false;
  p.putString(kKeySsid, s);
  p.end();
  return true;
}

static bool nvs_store_psk(const String &s) {
  if (s.length() < kPskMin || s.length() > kPskMax) return false;
  Preferences p;
  if (!p.begin(kPrefsNs, false)) return false;
  p.putString(kKeyPsk, s);
  p.end();
  return true;
}

static void nvs_store_en(bool en) {
  Preferences p;
  if (p.begin(kPrefsNs, false)) {
    p.putBool(kKeyEn, en);
    p.end();
  }
}

// ---- HTML template --------------------------------------------------------
// Palette matches diginode-cc/web/tailwind.config.js + index.css:
//   bg #0F172A, surface #111C32, sidebar #0B1220, text #E2E8F0,
//   muted #94A3B8, accent #3B82F6 / #60A5FA, border #334155,
//   notice #22C55E, alert #F97316, critical #EF4444.
// Single inline <style>, no JS, no external assets. ~3 KB rendered.
static const char kHtmlStyle[] PROGMEM =
  "<style>"
  ":root{--bg:#0F172A;--surface:#111C32;--sidebar:#0B1220;--text:#E2E8F0;"
  "--muted:#94A3B8;--border:#334155;--accent:#3B82F6;--accent2:#60A5FA;"
  "--ok:#22C55E;--warn:#F97316;--bad:#EF4444}"
  "*{box-sizing:border-box}"
  "html,body{margin:0;padding:0;background:var(--bg);color:var(--text);"
  "font-family:system-ui,-apple-system,Segoe UI,Roboto,Helvetica,Arial,sans-serif;"
  "font-size:15px;line-height:1.4}"
  ".wrap{max-width:640px;margin:0 auto;padding:18px 16px 48px}"
  "header{padding:8px 0 16px;border-bottom:1px solid var(--border);margin-bottom:18px}"
  "header h1{margin:0 0 4px;font-size:20px;font-weight:600;letter-spacing:.2px}"
  "header h1 span{color:var(--accent2);font-weight:500}"
  ".meta{color:var(--muted);font-size:13px;display:flex;flex-wrap:wrap;gap:12px}"
  ".meta b{color:var(--text);font-weight:500}"
  ".panel{background:var(--surface);border:1px solid var(--border);"
  "border-radius:8px;padding:14px 16px;margin:12px 0}"
  ".panel h2{margin:0 0 10px;font-size:14px;font-weight:600;"
  "text-transform:uppercase;letter-spacing:1px;color:var(--muted)}"
  ".row{display:flex;align-items:center;gap:10px;flex-wrap:wrap;margin:6px 0}"
  ".row label{flex:1;min-width:160px}"
  "input[type=number],input[type=text],input[type=password]{"
  "background:var(--sidebar);color:var(--text);border:1px solid var(--border);"
  "border-radius:6px;padding:6px 8px;font:inherit;width:90px}"
  "input[type=number]:focus,input[type=text]:focus{outline:none;"
  "border-color:var(--accent);box-shadow:0 0 0 1px var(--accent)}"
  "button{background:var(--accent);color:#fff;border:0;border-radius:6px;"
  "padding:7px 14px;font:inherit;font-weight:500;cursor:pointer}"
  "button:hover{background:var(--accent2)}"
  "button.alt{background:transparent;color:var(--accent2);"
  "border:1px solid var(--accent)}"
  "button.alt:hover{background:rgba(59,130,246,.12)}"
  ".slot{display:flex;align-items:center;gap:10px;padding:6px 8px;"
  "border-radius:6px;border:1px solid transparent}"
  ".slot.active{border-color:var(--accent);background:rgba(59,130,246,.08)}"
  ".slot input{margin:0}"
  ".badge{display:inline-block;padding:2px 8px;border-radius:999px;"
  "font-size:12px;font-weight:500}"
  ".badge.ok{background:rgba(34,197,94,.18);color:var(--ok);"
  "border:1px solid rgba(34,197,94,.4)}"
  ".badge.bad{background:rgba(239,68,68,.18);color:var(--bad);"
  "border:1px solid rgba(239,68,68,.4)}"
  ".badge.warn{background:rgba(249,115,22,.18);color:var(--warn);"
  "border:1px solid rgba(249,115,22,.4)}"
  ".muted{color:var(--muted)}"
  "form{margin:0}"
  "footer{margin-top:24px;color:var(--muted);font-size:12px;text-align:center}"
  "</style>";

// ---- HTML escape ----------------------------------------------------------
// Appends `in` to `out` with &, <, >, ", ' replaced by their entity refs.
// Used wherever user-controlled state lands inside the page body or an
// attribute value. Keeps the SSID-with-special-chars case from breaking
// the form or leaking script.
static void html_escape(const String &in, String &out) {
  for (size_t i = 0; i < in.length(); ++i) {
    char c = in[i];
    switch (c) {
      case '&':  out += "&amp;";  break;
      case '<':  out += "&lt;";   break;
      case '>':  out += "&gt;";   break;
      case '"':  out += "&quot;"; break;
      case '\'': out += "&#39;";  break;
      default:   out += c;        break;
    }
  }
}

// ---- HTML render ----------------------------------------------------------
static void render_page(String &out) {
  // Header / meta
  uint32_t up = bridge::uptime_ms() / 1000UL;
  unsigned uh = up / 3600U, um = (up / 60U) % 60U, us = up % 60U;
  bool conn = bridge::mesh_connected();
  bool armed = bridge::armed();
  const char *sname = bridge::mesh_short_name();
  if (!sname[0]) sname = "?";

  out.reserve(5120);
  out  = F("<!doctype html><html><head><meta charset=utf-8>"
           "<meta name=viewport content=\"width=device-width,initial-scale=1\">"
           "<title>AI Camera Bridge</title>");
  out += FPSTR(kHtmlStyle);
  out += F("</head><body><div class=wrap>");

  // Header block — mesh_short_name is operator-controlled, HTML-escape it.
  String sname_esc;
  html_escape(String(sname), sname_esc);
  char hdr[320];
  snprintf(hdr, sizeof(hdr),
    "<header><h1>AI Camera Bridge <span>%s</span></h1>"
    "<div class=meta>"
    "<span>Uptime <b>%02u:%02u:%02u</b></span>"
    "<span>Mesh <span class=\"badge %s\">%s</span></span>"
    "<span>State <span class=\"badge %s\">%s</span></span>"
    "<span>Clients <b>%u</b></span>"
    "</div></header>",
    sname_esc.c_str(),
    uh, um, us,
    conn ? "ok" : "bad", conn ? "linked" : "no peer",
    armed ? "ok" : "warn", armed ? "ARMED" : "DISARMED",
    (unsigned)webui.client_count());
  out += hdr;

  // Arm / disarm
  out += F("<div class=panel><h2>Camera</h2>"
           "<form method=post action=/arm>"
           "<div class=row>"
           "<button name=on value=1>Arm</button>"
           "<button class=alt name=on value=0>Disarm</button>"
           "<span class=muted>Watch loop forwards detections to the mesh while armed.</span>"
           "</div></form></div>");

  // Model picker
  out += F("<div class=panel><h2>Model</h2>"
           "<form method=post action=/model>");
  size_t n = bridge::slot_count();
  for (size_t i = 0; i < n; ++i) {
    UiSlot s = bridge::slot_at(i);
    char row[160];
    snprintf(row, sizeof(row),
      "<label class=\"slot%s\">"
      "<input type=radio name=id value=%d%s>"
      "<b>%d</b> &middot; %s%s</label>",
      s.active ? " active" : "",
      s.id, s.active ? " checked" : "",
      s.id, s.alias,
      s.active ? " <span class=\"badge ok\">active</span>" : "");
    out += row;
  }
  out += F("<div class=row><button type=submit>Apply model</button></div></form></div>");

  // Confidence + dedup + heartbeat
  char cfg[1024];
  snprintf(cfg, sizeof(cfg),
    "<div class=panel><h2>Detection</h2>"
    "<form method=post action=/conf><div class=row>"
    "<label>Confidence threshold <span class=muted>(1..100)</span></label>"
    "<input type=number name=v min=1 max=100 value=%u>"
    "<button>Apply</button></div></form>"
    "<form method=post action=/dedup><div class=row>"
    "<label>Dedup interval <span class=muted>(1..3600 s)</span></label>"
    "<input type=number name=v min=1 max=3600 value=%u>"
    "<button>Apply</button></div></form>"
    "</div>"
    "<div class=panel><h2>Heartbeat</h2>"
    "<form method=post action=/hb><div class=row>"
    "<label><input type=checkbox name=on value=1%s> Enabled</label>"
    "<input type=number name=min min=1 max=60 value=%u>"
    "<span class=muted>minutes</span>"
    "<button>Apply</button></div></form></div>",
    (unsigned)bridge::conf_min(),
    (unsigned)bridge::dedup_sec(),
    bridge::hb_enabled() ? " checked" : "",
    (unsigned)bridge::hb_interval_min());
  out += cfg;

  // Wi-Fi credentials editor. SSID/PSK are user-controlled; escape both
  // before rendering inside the value=".." attributes.
  String ssid_esc, psk_esc;
  html_escape(webui.ssid(), ssid_esc);
  html_escape(webui.psk(),  psk_esc);
  out += F("<div class=panel><h2>Wi-Fi credentials</h2>"
           "<form method=post action=/wifi>"
           "<div class=row><label>SSID <span class=muted>(1..32)</span></label>"
           "<input type=text name=ssid maxlength=32 value=\"");
  out += ssid_esc;
  out += F("\"></div>"
           "<div class=row><label>PSK <span class=muted>(8..63)</span></label>"
           "<input type=text name=psk maxlength=63 value=\"");
  out += psk_esc;
  out += F("\"></div>"
           "<div class=row><button type=submit>Apply</button>"
           "<span class=muted>Changes apply on the next Wi-Fi cycle "
           "(<code>WIFI_OFF</code> &rarr; <code>WIFI_ON</code>).</span>"
           "</div></form></div>");

  out += F("<footer>192.168.4.1 &middot; AI Camera Bridge</footer>"
           "</div></body></html>");
}

// ---- HTTP handlers --------------------------------------------------------
static void handle_root() {
  String body;
  render_page(body);
  s_http->send(200, "text/html; charset=utf-8", body);
}

static void redirect_root() {
  s_http->sendHeader("Location", "/", true);
  s_http->send(303, "text/plain", "");
}

static void handle_model() {
  int id = s_http->arg("id").toInt();
  bool ok = bridge::set_model(id);
  if (!ok) { s_http->send(400, "text/plain", "bad slot id"); return; }
  redirect_root();
}

static void handle_conf() {
  int v = s_http->arg("v").toInt();
  if (!bridge::set_conf(v)) { s_http->send(400, "text/plain", "bad value"); return; }
  redirect_root();
}

static void handle_dedup() {
  int v = s_http->arg("v").toInt();
  if (!bridge::set_dedup_sec(v)) { s_http->send(400, "text/plain", "bad value"); return; }
  redirect_root();
}

static void handle_hb() {
  bool on = s_http->arg("on") == "1";
  int  m  = s_http->arg("min").toInt();
  bridge::set_hb_enabled(on);
  if (m > 0) bridge::set_hb_interval(m);
  redirect_root();
}

static void handle_arm() {
  bool on = s_http->arg("on") == "1";
  bridge::set_armed(on);
  redirect_root();
}

// Update SSID/PSK in NVS. Empty fields = leave unchanged. Does NOT cycle
// the AP — the operator's current session stays alive on the old creds;
// they must WIFI_OFF / WIFI_ON to apply.
static void handle_wifi() {
  String new_ssid = s_http->arg("ssid");
  String new_psk  = s_http->arg("psk");
  bool ssid_ok = true, psk_ok = true;
  if (new_ssid.length() > 0) ssid_ok = webui.set_ssid(new_ssid);
  if (new_psk.length()  > 0) psk_ok  = webui.set_psk (new_psk);
  if (!ssid_ok || !psk_ok) {
    s_http->send(400, "text/plain",
      !ssid_ok ? "bad ssid (must be 1..32 chars)"
               : "bad psk (WPA2 requires 8..63 chars)");
    return;
  }
  redirect_root();
}

// Catches captive-portal probes (Android /generate_204, iOS
// /hotspot-detect.html, Windows /ncsi.txt, etc) since DNS routes every
// hostname to us. 302 makes the OS open our page automatically.
static void handle_not_found() {
  s_http->sendHeader("Location", "http://192.168.4.1/", true);
  s_http->send(302, "text/plain", "");
}

// ---- public API -----------------------------------------------------------
bool WebUI::begin() {
  s_ssid = nvs_load_ssid();
  s_psk  = nvs_load_psk();
  WifiEn e = nvs_load_en();
  Serial.printf("wifi: %s (%s, ssid='%s')\n",
                e.en ? "ON" : "OFF",
                e.from_nvs ? "from NVS" : "first-boot default",
                s_ssid.c_str());
  if (e.en) return turn_on();
  return false;
}

void WebUI::tick() {
  if (!s_on) return;
  if (s_dns)  s_dns->processNextRequest();
  if (s_http) s_http->handleClient();
}

bool WebUI::turn_on() {
  if (s_on) return true;

  // Wi-Fi softAP. IDF picks 192.168.4.1/24 by default.
  WiFi.mode(WIFI_AP);
  bool ok = WiFi.softAP(s_ssid.c_str(), s_psk.c_str(), kApChannel, /*hidden=*/0, kApMaxConn);
  if (!ok) {
    Serial.println("wifi: softAP() FAILED");
    WiFi.mode(WIFI_OFF);
    return false;
  }

  IPAddress ip = WiFi.softAPIP();

  // Captive-portal DNS — every hostname resolves to us.
  s_dns = new DNSServer();
  s_dns->setErrorReplyCode(DNSReplyCode::NoError);
  s_dns->start(kDnsPort, "*", ip);

  // HTTP routes.
  s_http = new WebServer(kHttpPort);
  s_http->on("/",      HTTP_GET,  handle_root);
  s_http->on("/model", HTTP_POST, handle_model);
  s_http->on("/conf",  HTTP_POST, handle_conf);
  s_http->on("/dedup", HTTP_POST, handle_dedup);
  s_http->on("/hb",    HTTP_POST, handle_hb);
  s_http->on("/arm",   HTTP_POST, handle_arm);
  s_http->on("/wifi",  HTTP_POST, handle_wifi);
  s_http->onNotFound(handle_not_found);
  s_http->begin();

  s_on = true;
  nvs_store_en(true);
  Serial.printf("wifi: ON (ssid='%s' ip=%s)\n", s_ssid.c_str(), ip.toString().c_str());
  return true;
}

bool WebUI::turn_off() {
  if (!s_on) {
    nvs_store_en(false);
    return true;
  }
  if (s_http) { s_http->close(); delete s_http; s_http = nullptr; }
  if (s_dns)  { s_dns->stop();   delete s_dns;  s_dns  = nullptr; }
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_OFF);
  s_on = false;
  nvs_store_en(false);
  Serial.println("wifi: OFF");
  return true;
}

bool    WebUI::is_on()        const { return s_on; }
uint8_t WebUI::client_count() const { return s_on ? WiFi.softAPgetStationNum() : 0; }
const String &WebUI::ssid()   const { return s_ssid; }
const String &WebUI::psk()    const { return s_psk;  }

bool WebUI::set_ssid(const String &s) {
  if (!nvs_store_ssid(s)) return false;
  s_ssid = s;
  return true;
}

bool WebUI::set_psk(const String &s) {
  if (!nvs_store_psk(s)) return false;
  s_psk = s;
  return true;
}
