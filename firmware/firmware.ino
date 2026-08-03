// N-Bus ESP32-C3 firmware — passively reads a Büttner/Dometic NDS "N-Bus" (LIN,
// 19200 baud) and republishes values over MQTT with Home Assistant auto-discovery,
// WiFiManager provisioning and ElegantOTA web updates.
//
// SAFETY: read-only. The LIN UART is opened RX-only (TX pin = -1). Never transmit.
//
// Board: ESP32-C3 Super Mini  ·  FQBN esp32:esp32:esp32c3 (USB CDC On Boot enabled).

#include <Preferences.h>
#include <WebServer.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <WiFiManager.h>     // tzapu
#include <PubSubClient.h>    // knolleary
#include <ArduinoJson.h>     // bblanchon (v7)
#include <ElegantOTA.h>      // ayushsharma82
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_task_wdt.h"

#include "Config.h"
#include "NBusParser.h"

// Print raw frame windows and decoded registers over USB serial. Comment out once the
// bus is fully mapped; the raw dump is what makes unknown registers visible.
#define NBUS_DEBUG 1

// --------------------------------------------------------------------------
// Globals
// --------------------------------------------------------------------------
// Declared up here rather than beside the ring-buffer code that uses it: the .ino
// preprocessor hoists generated prototypes to the top of the file, so any type
// named in a function signature must already exist before the first function.
struct RawEntry {
  uint32_t ms;
  uint8_t  d[8];   // NAD PCI SID reg d0 d1 d2 d3 — checksum already verified, so not stored
};

HardwareSerial   LinSerial(NBUS_UART_NUM);
NBusParser       parser;
// Decides which pack a NAD 0x85 frame belongs to. See NBusParser.h for why this is done
// by position in the poll cycle rather than by timing.
NBusCycleTracker cycles;
Preferences      prefs;
WiFiClient       wifiClient;
PubSubClient     mqtt(wifiClient);
WebServer        httpServer(80);

// The portal runs non-blocking, so WiFiManager and its parameters must outlive
// startProvisioning() — it keeps pointers to them and services them from loop().
WiFiManager           wm;
WiFiManagerParameter  pHost("host", "MQTT host", "", 64);
WiFiManagerParameter  pPort("port", "MQTT port", "", 8);
WiFiManagerParameter  pUser("user", "MQTT user", "", 48);
WiFiManagerParameter  pPass("pass", "MQTT password", "", 48);
WiFiManagerParameter  pBase("base", "Base topic", "", 32);
WiFiManagerParameter  pOtaU("otau", "OTA user (optional)", "", 32);
WiFiManagerParameter  pOtaP("otap", "OTA password (optional)", "", 32);

struct MqttConfig {
  String host;
  uint16_t port = 1883;
  String user;
  String pass;
  String base;       // base topic, e.g. "nbus"
  String otaUser;
  String otaPass;
} cfg;

bool g_shouldSaveConfig = false;
bool g_portalActive     = false;
bool g_netServicesUp    = false;

// Per-group last-seen timestamps for staleness.
// Freshness is per pack, not per NAD: both packs answer on 0x85, so a single timestamp
// would let a live pack vouch for a silent one.
uint32_t g_lastBattMs[NBUS_MAX_BATTERIES] = {0};
uint32_t g_lastCellMs[NBUS_MAX_BATTERIES] = {0};
uint32_t g_lastSolarMs   = 0;
uint32_t g_lastStarterMs = 0;

uint32_t g_lastPublishMs   = 0;
uint32_t g_lastMqttTryMs   = 0;
uint32_t g_lastHeartbeatMs = 0;
bool     g_discoverySent   = false;
uint8_t  g_mqttFailCount   = 0;

// Up here rather than with the rest of the ring state further down, because the OTA
// callbacks in setupWeb() touch both and are registered before that block.
uint32_t g_lastFrameMs = 0;
bool     g_otaActive   = false;

// --------------------------------------------------------------------------
// LED helpers
// --------------------------------------------------------------------------
static inline void ledWrite(bool on) {
#if NBUS_LED_ACTIVE_LOW
  digitalWrite(NBUS_LED_PIN, on ? LOW : HIGH);
#else
  digitalWrite(NBUS_LED_PIN, on ? HIGH : LOW);
#endif
}

// --------------------------------------------------------------------------
// Persistent config (NVS)
// --------------------------------------------------------------------------
void loadConfig() {
  prefs.begin("nbus", true);
  cfg.host    = prefs.getString("host", NBUS_DEFAULT_MQTT_HOST);
  cfg.port    = prefs.getString("port", NBUS_DEFAULT_MQTT_PORT).toInt();
  cfg.user    = prefs.getString("user", NBUS_DEFAULT_MQTT_USER);
  cfg.pass    = prefs.getString("pass", NBUS_DEFAULT_MQTT_PASS);
  cfg.base    = prefs.getString("base", NBUS_DEFAULT_BASE_TOPIC);
  cfg.otaUser = prefs.getString("otau", "");
  cfg.otaPass = prefs.getString("otap", "");
  prefs.end();
  if (cfg.port == 0) cfg.port = 1883;
  if (cfg.base.isEmpty()) cfg.base = NBUS_DEFAULT_BASE_TOPIC;
}

void saveConfig() {
  prefs.begin("nbus", false);
  prefs.putString("host", cfg.host);
  prefs.putString("port", String(cfg.port));
  prefs.putString("user", cfg.user);
  prefs.putString("pass", cfg.pass);
  prefs.putString("base", cfg.base);
  prefs.putString("otau", cfg.otaUser);
  prefs.putString("otap", cfg.otaPass);
  prefs.end();
}

// --------------------------------------------------------------------------
// Wi-Fi provisioning
// --------------------------------------------------------------------------
bool mqttConnect();
void onWifiUp();

void onSaveConfig() { g_shouldSaveConfig = true; }

void eraseSettings() {
  Serial.println(F("[setup] erasing settings"));
  wm.resetSettings();
  prefs.begin("nbus", false);
  prefs.clear();
  prefs.end();
  for (int i = 0; i < 6; ++i) { ledWrite(true); delay(80); ledWrite(false); delay(80); }
}

// Wipe Wi-Fi + MQTT settings when the BOOT button is held at power-on.
//
// GPIO9 doubles as the boot strapping pin that the USB host drives via DTR, so a
// serial terminal opening the port can pull it low a few hundred ms after the ROM
// has already sampled it high. Two gates make that harmless: the erase only runs
// after a true power-on reset (every USB-induced reset is excluded), and the pin
// must stay low for NBUS_FACTORY_HOLD_MS — far longer than any DTR pulse.
void maybeFactoryReset() {
  if (esp_reset_reason() != ESP_RST_POWERON) return;

  pinMode(NBUS_SETUP_BTN_PIN, INPUT_PULLUP);
  delay(50);
  if (digitalRead(NBUS_SETUP_BTN_PIN) != LOW) return;

  Serial.println(F("[setup] BOOT held — keep holding to erase settings"));
  const uint32_t start = millis();
  while (digitalRead(NBUS_SETUP_BTN_PIN) == LOW) {
    if (millis() - start >= NBUS_FACTORY_HOLD_MS) {
      eraseSettings();
      return;
    }
    ledWrite((((millis() - start) / 100) % 2) != 0);  // blink while counting
    delay(10);
  }
  ledWrite(false);
  Serial.println(F("[setup] BOOT released early — settings kept"));
}

// The ESP's disconnect reason is the only thing that separates a wrong passphrase
// (reason 15, 4-way handshake timeout) from an AP that refuses or drops us.
// Every line carries millis() so the association attempts can be lined up against the
// packet counters the router reports for this MAC.
void onWifiEvent(WiFiEvent_t event, WiFiEventInfo_t info) {
  switch (event) {
    case ARDUINO_EVENT_WIFI_STA_START:
      Serial.printf("[wifi %8lu] STA started\n", millis());
      break;
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      // Association + 4-way handshake done. Reaching this at all rules out the whole
      // authentication class of faults; only DHCP can still fail after this point.
      Serial.printf("[wifi %8lu] associated, ch %d, waiting for DHCP\n",
                    millis(), (int)info.wifi_sta_connected.channel);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      Serial.printf("[wifi %8lu] got IP\n", millis());
      break;
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      Serial.printf("[wifi %8lu] lost IP\n", millis());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED:
      Serial.printf("[wifi %8lu] disconnected, reason %d\n",
                    millis(), (int)info.wifi_sta_disconnected.reason);
      break;
    default:
      break;
  }
}

// Lists what the radio can actually see. The C3 is 2.4 GHz only, so an AP that is
// missing here but visible on a phone is almost always 5 GHz.
const char* authModeName(wifi_auth_mode_t m) {
  switch (m) {
    case WIFI_AUTH_OPEN:            return "open";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ent";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "other";
  }
}

void logVisibleNetworks() {
  // Read from efuse, not WiFi.macAddress(): the driver is already stopped here and
  // would report all zeros.
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  Serial.printf("[wifi] stored SSID '%s', STA MAC %02X:%02X:%02X:%02X:%02X:%02X, hostname %s\n",
                wm.getWiFiSSID().c_str(), mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                NBUS_HOSTNAME);
  // Length and character mix of the stored passphrase, never the passphrase itself.
  // A mismatch with what was typed points at the portal form mangling a character.
  const String pass = wm.getWiFiPass();
  int nonAlnum = 0, nonAscii = 0;
  for (size_t i = 0; i < pass.length(); ++i) {
    const uint8_t c = (uint8_t)pass[i];
    if (c < 32 || c > 126) ++nonAscii;
    else if (!isalnum(c))  ++nonAlnum;
  }
  Serial.printf("[wifi] stored passphrase: %u chars, %d punctuation, %d non-ASCII\n",
                (unsigned)pass.length(), nonAlnum, nonAscii);

  const int n = WiFi.scanNetworks();
  Serial.printf("[wifi] %d networks visible (2.4 GHz only):\n", n);
  for (int i = 0; i < n; ++i) {
    Serial.printf("  %-28s ch%-3d %4d dBm %s\n", WiFi.SSID(i).c_str(), WiFi.channel(i),
                  WiFi.RSSI(i), authModeName(WiFi.encryptionType(i)));
  }
  WiFi.scanDelete();
}

// The portal never blocks: reading the bus is this device's primary job and must not
// depend on Wi-Fi being provisioned. loop() drives the portal via handleProvisioning().
void startProvisioning() {
  char portBuf[8];
  snprintf(portBuf, sizeof portBuf, "%u", cfg.port);
  pHost.setValue(cfg.host.c_str(), 64);
  pPort.setValue(portBuf, 8);
  pUser.setValue(cfg.user.c_str(), 48);
  pPass.setValue(cfg.pass.c_str(), 48);
  pBase.setValue(cfg.base.c_str(), 32);
  pOtaU.setValue(cfg.otaUser.c_str(), 32);
  pOtaP.setValue(cfg.otaPass.c_str(), 32);

  // Makes the device identifiable in the router's client list instead of showing up
  // as a nameless MAC address.
  wm.setHostname(NBUS_HOSTNAME);
  wm.setSaveConfigCallback(onSaveConfig);
  wm.setConfigPortalBlocking(false);
  // Bounds the one blocking step left in autoConnect(): the attempt on the saved AP.
  // Without it WiFiManager waits on the ESP's own result, which took 60 s on a failure.
  wm.setConnectTimeout(NBUS_WIFI_CONNECT_TIMEOUT_S);
  wm.addParameter(&pHost);
  wm.addParameter(&pPort);
  wm.addParameter(&pUser);
  wm.addParameter(&pPass);
  wm.addParameter(&pBase);
  wm.addParameter(&pOtaU);
  wm.addParameter(&pOtaP);

  if (wm.autoConnect(NBUS_AP_NAME)) {
    onWifiUp();
  } else {
    g_portalActive = true;
    Serial.printf("[wifi] connect failed (status %d) - portal %s open at 192.168.4.1\n",
                  (int)WiFi.status(), NBUS_AP_NAME);
    logVisibleNetworks();
  }
}

// ---------------------------------------------------------------------------
// MQTT settings page, served over the LAN on the device's own IP. The captive
// portal exists for first-time Wi-Fi setup only; changing the broker afterwards
// should not require dropping a working device back into AP mode.
// ---------------------------------------------------------------------------
String htmlEscape(const String& s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    if      (c == '&') out += F("&amp;");
    else if (c == '<') out += F("&lt;");
    else if (c == '>') out += F("&gt;");
    else if (c == '"') out += F("&quot;");
    else if (c == '\'') out += F("&#39;");
    else               out += c;
  }
  return out;
}

void addField(String& p, const char* name, const char* label, const String& value,
              const char* type) {
  p += F("<label>");
  p += label;
  p += F("<input name='");
  p += name;
  p += F("' type='");
  p += type;
  p += F("' value='");
  p += htmlEscape(value);
  p += F("'></label>");
}

// --------------------------------------------------------------------------
// HTTP authentication
//
// These endpoints are reachable from the whole tailnet and from anyone on the
// camper LAN. /config can repoint the device at a different broker and /update
// replaces the firmware outright, so neither can stay open. The credentials are
// the ones already stored for ElegantOTA rather than a second set, because two
// passwords for one device is how one of them ends up forgotten.
//
// With no credentials configured the device stays open: a freshly provisioned
// unit has none yet, and locking it out of its own first configuration would
// need a USB cable to undo — the exact dependency this whole exercise removes.
// --------------------------------------------------------------------------
bool requireAuth() {
  if (cfg.otaUser.isEmpty()) return true;
  if (httpServer.authenticate(cfg.otaUser.c_str(), cfg.otaPass.c_str())) return true;
  httpServer.requestAuthentication();
  return false;
}

void handleConfigGet() {
  if (!requireAuth()) return;
  String p = F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
               "<title>N-Bus settings</title><style>"
               "body{font-family:sans-serif;max-width:24em;margin:2em auto;padding:0 1em}"
               "label{display:block;margin:.7em 0}"
               "input{width:100%;padding:.4em;box-sizing:border-box}"
               "button{padding:.6em 1.2em;margin-top:1em}</style>"
               "<h2>MQTT settings</h2><form method='POST'>");
  addField(p, "host", "Broker host (Home Assistant IP)", cfg.host, "text");
  addField(p, "port", "Port", String(cfg.port), "number");
  addField(p, "user", "Username", cfg.user, "text");
  addField(p, "pass", "Password (blank = keep current)", "", "password");
  addField(p, "base", "Base topic", cfg.base, "text");
  // Exposed here and not only in the setup portal: the portal needs the button held
  // on the physical device, and a credential you can only set by standing next to the
  // camper is no use for a box that is meant to be administered over the network.
  p += F("<h2>Access</h2><p style='font-size:.85em;color:#555'>Guards /config, /update "
         "and /raw. Leave the username blank to disable authentication.</p>");
  addField(p, "otau", "Username", cfg.otaUser, "text");
  addField(p, "otap", "Password (blank = keep current)", "", "password");
  p += F("<button type='submit'>Save &amp; reconnect</button></form>");
  httpServer.send(200, "text/html", p);
}

void handleConfigPost() {
  if (!requireAuth()) return;
  if (httpServer.hasArg("host")) cfg.host = httpServer.arg("host");
  if (httpServer.hasArg("port")) cfg.port = httpServer.arg("port").toInt();
  if (httpServer.hasArg("user")) cfg.user = httpServer.arg("user");
  // An empty password field means "unchanged", so the other fields can be edited
  // without having to retype the broker password.
  if (httpServer.arg("pass").length()) cfg.pass = httpServer.arg("pass");
  if (httpServer.hasArg("base")) cfg.base = httpServer.arg("base");
  if (httpServer.hasArg("otau")) cfg.otaUser = httpServer.arg("otau");
  if (httpServer.arg("otap").length()) cfg.otaPass = httpServer.arg("otap");
  if (cfg.port == 0) cfg.port = 1883;
  if (cfg.base.isEmpty()) cfg.base = NBUS_DEFAULT_BASE_TOPIC;
  saveConfig();
  Serial.printf("[cfg] saved MQTT %s:%u user '%s' base '%s', auth %s\n",
                cfg.host.c_str(), cfg.port, cfg.user.c_str(), cfg.base.c_str(),
                cfg.otaUser.isEmpty() ? "off" : "on");

  // Apply the new credentials at once. requireAuth() reads cfg directly and needs
  // nothing, but ElegantOTA keeps its own copy, so /update would otherwise stay on
  // the old setting until a reboot — the one endpoint where that gap matters most.
  if (cfg.otaUser.isEmpty()) ElegantOTA.clearAuth();
  else ElegantOTA.setAuth(cfg.otaUser.c_str(), cfg.otaPass.c_str());

  // Reconnect immediately with the new settings and re-announce discovery, so a
  // changed base topic reaches Home Assistant without a reboot.
  mqtt.disconnect();
  g_discoverySent = false;
  g_mqttFailCount = 0;
  g_lastMqttTryMs = 0;
  httpServer.send(200, "text/html",
                  F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                    "<p>Saved — reconnecting to the broker.</p><p><a href='/config'>Back</a></p>"));
}

void onWifiUp() {
  if (g_shouldSaveConfig) {
    g_shouldSaveConfig = false;
    cfg.host = pHost.getValue();
    cfg.port = atoi(pPort.getValue());
    cfg.user = pUser.getValue();
    cfg.pass = pPass.getValue();
    cfg.base = pBase.getValue();
    cfg.otaUser = pOtaU.getValue();
    cfg.otaPass = pOtaP.getValue();
    if (cfg.port == 0) cfg.port = 1883;
    if (cfg.base.isEmpty()) cfg.base = NBUS_DEFAULT_BASE_TOPIC;
    saveConfig();
    Serial.println(F("[wifi] saved MQTT config"));
  }
  Serial.print(F("[wifi] connected, IP "));
  Serial.println(WiFi.localIP());

  // Deferred until now: the portal serves its own page on :80 while it is up.
  if (!cfg.otaUser.isEmpty()) {
    ElegantOTA.setAuth(cfg.otaUser.c_str(), cfg.otaPass.c_str());
  }
  httpServer.on("/", []() {
    httpServer.send(200, "text/html",
                    F("<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
                      "<h2>N-Bus camper bridge</h2>"
                      "<p><a href='/config'>MQTT settings</a></p>"
                      "<p><a href='/update'>Firmware update</a></p>"
                      "<p><a href='/status'>Status (JSON)</a></p>"
                      "<p><a href='/raw/dump'>Raw bus capture</a> &middot; "
                      "<a href='/raw/files'>saved dumps</a></p>"));
  });
  httpServer.on("/config", HTTP_GET, handleConfigGet);
  httpServer.on("/config", HTTP_POST, handleConfigPost);
  httpServer.on("/status",     HTTP_GET, handleStatus);
  httpServer.on("/raw/dump",   HTTP_GET, handleRawDump);
  httpServer.on("/raw/save",   HTTP_GET, handleRawSave);
  httpServer.on("/raw/files",  HTTP_GET, handleRawFiles);
  httpServer.on("/raw/file",   HTTP_GET, handleRawFile);
  httpServer.on("/raw/clear",  HTTP_GET, handleRawClear);
  // An upload starves the LIN reader for far longer than NBUS_RAW_SILENCE_MS, so without
  // this the silence detector fires on every firmware update: a wasted flash write, and
  // worse, a junk dump that evicts a real one from the NBUS_RAW_MAX_DUMPS slots.
  ElegantOTA.onStart([]() { g_otaActive = true; });
  ElegantOTA.onEnd([](bool success) {
    g_otaActive = false;
    g_lastFrameMs = millis();  // a failed upload leaves us running; don't fire on the gap
  });
  ElegantOTA.begin(&httpServer);
  httpServer.begin();
  Serial.println(F("[ota] web server on :80 (/update)"));
  g_netServicesUp = true;

  mqttConnect();
}

void handleProvisioning() {
  if (!g_portalActive) return;
  if (wm.process()) {
    g_portalActive = false;
    wm.stopConfigPortal();  // frees port 80 before the OTA server binds to it
    onWifiUp();
  }
}

// --------------------------------------------------------------------------
// MQTT + Home Assistant discovery
// --------------------------------------------------------------------------
String statusTopic()  { return cfg.base + "/status"; }
// One topic per poll slot. The slot is only a transport address: which pack sits in it
// can change (the battery firmware update swapped the order). What identifies a pack to
// Home Assistant is its serial number, further down.
String batteryTopic(int slot) { return cfg.base + "/battery/" + String(slot + 1); }
String solarTopic()   { return cfg.base + "/solar"; }
String starterTopic() { return cfg.base + "/starter"; }

// Every device on the bus is published as its own Home Assistant device, linked back to
// the ESP as the bridge that reports it. Grouping all three under one device is what made
// the dashboard mix the two packs together in the first place.
//
// `suffix` empty ⇒ the bridge itself.
void addDevice(JsonObject dev, const String& suffix, const String& name,
               const char* model, const String& sw) {
  JsonArray ids = dev["ids"].to<JsonArray>();
  ids.add(suffix.isEmpty() ? String(NBUS_DEVICE_ID) : String(NBUS_DEVICE_ID) + "_" + suffix);
  dev["name"] = name;
  dev["mf"]   = NBUS_DEVICE_MF;
  dev["mdl"]  = model;
  if (!sw.isEmpty()) dev["sw"] = sw;
  if (!suffix.isEmpty()) dev["via_device"] = NBUS_DEVICE_ID;
}

void addBridgeDevice(JsonObject dev) {
  addDevice(dev, "", NBUS_DEVICE_NAME, NBUS_DEVICE_MDL, NBUS_FW_VERSION);
}

// `devSuffix` empty ⇒ attach the entity to the bridge device.
void publishDiscoverySensor(const char* key, const char* name, const char* stateTopic,
                            const char* valTpl, const char* unit, const char* devCla,
                            const char* stateCla = "measurement",
                            const String& devSuffix = String(),
                            const String& devName = String(),
                            const char* devModel = NBUS_DEVICE_MDL,
                            const String& devSw = String()) {
  JsonDocument doc;
  doc["name"]         = name;
  doc["uniq_id"]      = String(NBUS_DEVICE_ID) + "_" + key;
  doc["stat_t"]       = stateTopic;
  if (valTpl && valTpl[0]) doc["val_tpl"]    = valTpl;
  if (unit && unit[0])   doc["unit_of_meas"] = unit;
  if (devCla && devCla[0]) doc["dev_cla"]    = devCla;
  if (stateCla && stateCla[0]) doc["stat_cla"] = stateCla;
  doc["avty_t"]       = statusTopic();
  JsonObject dev = doc["dev"].to<JsonObject>();
  if (devSuffix.isEmpty()) addBridgeDevice(dev);
  else                     addDevice(dev, devSuffix, devName, devModel, devSw);

  String topic = String(NBUS_HA_PREFIX) + "/sensor/" + NBUS_DEVICE_ID + "_" + key + "/config";
  String payload;
  serializeJson(doc, payload);
  // A discovery message that does not fit the MQTT buffer is dropped without an error, and
  // the only symptom is an entity that never appears — which looks exactly like a register
  // that never answered. Say so on the serial console instead.
  if (!mqtt.publish(topic.c_str(), payload.c_str(), true)) {
    Serial.printf("[mqtt] discovery for %s FAILED (%u byte topic + %u byte payload)\n",
                  key, topic.length(), payload.length());
  }
}

// Discovery for a hand-picked set of registers the parser does not decode.
//
// The mirror republishes every undecoded register, but most of them never move and would
// only clutter Home Assistant. The ones here are those whose *history* is the evidence:
// what a slow register means only shows up in how it moves across a charge cycle or a
// drive, and a retained MQTT message holds one value, not a history. Getting them into the
// recorder is the whole point — it is what settled 0x35 and what killed two register
// readings that had looked solid for years.
//
// Naming them after their address rather than a guess is what keeps this honest — the
// entity claims a register, not a meaning. That is also why an empty template leaves the
// payload as the raw hex string: no unit, no state class, nothing to mislead.
struct RegSensor {
  const char* key;
  uint8_t     nad;
  uint8_t     reg;
  const char* name;
  const char* valTpl;    // "" ⇒ publish the eight hex characters unchanged
  const char* unit;
  const char* stateCla;
};

const RegSensor kRegSensors[] = {
  // Was a steady 270 on pack firmware 4.x, which made "two temperature sensors at 27.0 °C"
  // look plausible. Firmware 5.1 reports 0 in both halves, so that reading is dead: a
  // temperature does not become exactly zero across an update while the pack sits at the
  // same bench temperature. Published raw now — the °C it used to claim was a guess that
  // the update disproved, and a wrong unit is worse than none.
  { "reg_85_90", 0x85, 0x90, "0x90 raw", "", "", "" },

  // Alarm-bitmap candidates: all zero on a healthy bus, which is indistinguishable from
  // "unused" until one of them flips.
  { "reg_85_c0", 0x85, 0xC0, "0xC0 raw", "", "", "" },
  { "reg_85_f1", 0x85, 0xF1, "0xF1 raw", "", "", "" },
  { "reg_85_f2", 0x85, 0xF2, "0xF2 raw", "", "", "" },
  { "reg_81_c0", 0x81, 0xC0, "0xC0 raw", "", "", "" },
  { "reg_81_f0", 0x81, 0xF0, "0xF0 raw", "", "", "" },
  { "reg_81_f1", 0x81, 0xF1, "0xF1 raw", "", "", "" },

  // The same seven registers a second time, as the 32-bit number the hex spells out. The
  // hex entity stays because it is what you read in the UI; this one exists because a
  // string gets no long-term statistics and so lives exactly as long as the recorder
  // retention. These carry state_class, so Home Assistant keeps an hourly min/max forever:
  // a bit that flips for two seconds is preserved in that hour's maximum and is still
  // there a year later. Unitless on purpose — the number is a bit pattern, not a quantity.
  { "reg_85_90_n", 0x85, 0x90, "0x90 value", "{{ value | int(base=16) }}", "", "measurement" },
  { "reg_85_c0_n", 0x85, 0xC0, "0xC0 value", "{{ value | int(base=16) }}", "", "measurement" },
  { "reg_85_f1_n", 0x85, 0xF1, "0xF1 value", "{{ value | int(base=16) }}", "", "measurement" },
  { "reg_85_f2_n", 0x85, 0xF2, "0xF2 value", "{{ value | int(base=16) }}", "", "measurement" },
  { "reg_81_c0_n", 0x81, 0xC0, "0xC0 value", "{{ value | int(base=16) }}", "", "measurement" },
  { "reg_81_f0_n", 0x81, 0xF0, "0xF0 value", "{{ value | int(base=16) }}", "", "measurement" },
  { "reg_81_f1_n", 0x81, 0xF1, "0xF1 value", "{{ value | int(base=16) }}", "", "measurement" },

  // Recorded as a constant 730 for as long as captures were 5.5 minutes long; the first
  // day of real history put it at 740. Candidate cycle count. Both packs report the same
  // value, so whatever it counts is not a per-pack measurement — worth watching to see
  // whether they ever diverge.
  { "reg_85_0c", 0x85, 0x0C, "0x0C value", "{{ value[0:4] | int(base=16) }}", "", "measurement" },

  // New with pack firmware 5.1; absent on 4.x. Four bytes that read 76/75/76/10 on one
  // pack and 75/75/75/10 on the other — per-pack, roughly equal in the first three, which
  // is the shape three sensors of the same quantity would have. Deliberately unitless:
  // that shape is a hint, not a decode, and the whole point of the mirror is to record
  // candidates without naming them.
  { "reg_85_14_a", 0x85, 0x14, "0x14 byte 0", "{{ value[0:2] | int(base=16) }}", "", "measurement" },
  { "reg_85_14_b", 0x85, 0x14, "0x14 byte 1", "{{ value[2:4] | int(base=16) }}", "", "measurement" },
  { "reg_85_14_c", 0x85, 0x14, "0x14 byte 2", "{{ value[4:6] | int(base=16) }}", "", "measurement" },
  { "reg_85_14_d", 0x85, 0x14, "0x14 byte 3", "{{ value[6:8] | int(base=16) }}", "", "measurement" },

  // Both move, neither follows anything instantaneous; 0x11 lags the output badly.
  { "reg_81_11", 0x81, 0x11, "0x11 value", "{{ value[0:4] | int(base=16) }}", "", "measurement" },
  { "reg_81_1c", 0x81, 0x1C, "0x1C value", "{{ value[0:4] | int(base=16) }}", "", "measurement" },

  // The charge stage: 0 off, 3 bulk, 4 absorption, 6 float. This is the one entry in this
  // table that gets a real name instead of its address, because for once the meaning is
  // established rather than suspected — two regulated voltage plateaus at 14.35 V and
  // 13.76 V, each with its own value, cross-checked against every capture on four days.
  // See the protocol map. Named honestly is not the same as named cautiously.
  //
  // Both a number and a word. The number carries state_class, so Home Assistant keeps an
  // hourly min/max of it forever and a stage that lasts ten minutes survives in that
  // hour's extremes long after the recorder has dropped the detail; the word is what you
  // actually want to read on a dashboard. An unseen value prints as "unknown (5)" rather
  // than being silently mapped to something plausible — values 1, 2 and 5 have never been
  // observed, and the first time one appears it must be obvious that it is new.
  //
  // 6 is labelled float because that is what it does — a regulated 13.76 V maintenance
  // shelf. If the numbering really is Victron's, 6 is "storage" and 5 is float; the number
  // is published alongside the word for exactly that reason, so a relabel costs a template
  // and not a year of history.
  { "reg_81_26_stage", 0x81, 0x26, "Charge stage",
    "{{ value[6:8] | int(base=16) }}", "", "measurement" },
  { "reg_81_26_text", 0x81, 0x26, "Charge stage text",
    "{% set s = value[6:8] | int(base=16) %}"
    "{{ {0: 'off', 3: 'bulk', 4: 'absorption', 6: 'float'}.get(s, 'unknown (' ~ s ~ ')') }}",
    "", "" },

  // 0x60 d1 carries the same stage and is published too — see publishSolarDiscovery, not
  // this table. It cannot live here: 0x60 is an identity register, the parser decodes it,
  // and this table only reaches registers the parser rejects.

  // Panel/input voltage, confirmed by nightfall performing the dark test on its own: 0.10 V
  // in the dark, 13.5-24.5 V lit. The mirror's ten-second throttle means this is a sample of
  // an MPPT sweep and not a smooth curve — individual values bounce across several volts
  // between the sweep's endpoints, and it is the daily envelope that carries the meaning,
  // not any one reading.
  { "reg_81_1b", 0x81, 0x1B, "Panel voltage",
    "{{ (value[0:4] | int(base=16) / 100) | round(2) }}", "V", "measurement" },

  // 0xE0 went 0 -> 1 during the five-day blackout when this laptop could not reach the ESP,
  // and nothing recorded when. The ESP itself never stopped and was publishing to MQTT the
  // whole time — had this register had an entity then, the transition would have a timestamp
  // and probably an explanation. It is here for the next one.
  { "reg_81_e0", 0x81, 0xE0, "0xE0 raw", "", "", "" },
  { "reg_81_e0_n", 0x81, 0xE0, "0xE0 value", "{{ value | int(base=16) }}", "", "measurement" },
};

// Register-mirror topics name the device, not the NAD. Both packs answer on 0x85, so a
// NAD-keyed topic would have them overwrite each other — silently, and with plausible
// values, which is the worst possible way to be wrong.
String regTopic(int slot, uint8_t reg) {
  char buf[32];
  if (slot == NBusCycleTracker::kSolarSlot) snprintf(buf, sizeof buf, "/reg/81/%02X", reg);
  else snprintf(buf, sizeof buf, "/reg/85_%d/%02X", slot + 1, reg);
  return cfg.base + buf;
}

void publishRegDiscovery(int slot, const String& devSuffix, const String& devName,
                         const char* devModel, const String& devSw) {
  const uint8_t nad = (slot == NBusCycleTracker::kSolarSlot) ? NBUS_NAD_SOLAR
                                                             : NBUS_NAD_BATTERY;
  for (const auto& r : kRegSensors) {
    if (r.nad != nad) continue;
    // The entity key carries the serial so the entity follows its pack, not its slot.
    const String key = devSuffix + "_" + r.key;
    publishDiscoverySensor(key.c_str(), r.name, regTopic(slot, r.reg).c_str(),
                           r.valTpl, r.unit, "", r.stateCla,
                           devSuffix, devName, devModel, devSw);
  }
}

// Discovery for one battery pack. Keyed on the pack's serial number, never on its slot
// or bus address: the firmware update moved both of those (slot order swapped, address
// went 3 -> 11) while the serial stayed put. An entity keyed on either would have
// silently started reporting the other pack.
void publishBatteryDiscovery(int slot) {
  const NBusBattery& b = parser.state().batt[slot];
  const String sfx  = String("b") + b.id.serial;              // e.g. bKAA1234567
  const String name = String("Leisure battery ") + b.id.serial;
  const String sw   = b.id.fw_valid
                    ? String(b.id.fw_major) + "." + String(b.id.fw_minor) : String();
  const String bt   = batteryTopic(slot);

  struct { const char* key; const char* name; const char* tpl; const char* unit;
           const char* cla; const char* sta; } kFields[] = {
    { "soc",        "SoC",               "{{ value_json.soc }}",      "%",  "battery",        "measurement" },
    { "voltage",    "Voltage",           "{{ value_json.voltage }}",  "V",  "voltage",        "measurement" },
    { "current",    "Current",           "{{ value_json.current }}",  "A",  "current",        "measurement" },
    { "power",      "Power",             "{{ value_json.power }}",    "W",  "power",          "measurement" },
    { "cell1",      "Cell 1 voltage",    "{{ value_json.cells[0] }}", "V",  "voltage",        "measurement" },
    { "cell2",      "Cell 2 voltage",    "{{ value_json.cells[1] }}", "V",  "voltage",        "measurement" },
    { "cell3",      "Cell 3 voltage",    "{{ value_json.cells[2] }}", "V",  "voltage",        "measurement" },
    { "cell4",      "Cell 4 voltage",    "{{ value_json.cells[3] }}", "V",  "voltage",        "measurement" },
    { "energy",     "Energy remaining",  "{{ value_json.energy }}",   "Wh", "energy_storage", "measurement" },
    { "quality",    "Quality",           "{{ value_json.quality }}",  "%",  "",               "measurement" },
    { "capacity",   "Capacity",          "{{ value_json.capacity }}", "Ah", "",               "measurement" },
    // The pack reports its own runtime estimate in register 0x34; nothing is derived
    // here. Only the half matching the current direction of travel is ever published.
    { "to_empty",   "Time to empty",     "{{ value_json.to_empty }}", "min", "duration",      "measurement" },
    { "to_full",    "Time to full",      "{{ value_json.to_full }}",  "min", "duration",      "measurement" },
    // Lifetime counters, so total_increasing rather than measurement: HA must not read a
    // counter reset as a huge negative reading. The update reset both of these to ~0.
    { "discharged", "Energy discharged", "{{ value_json.discharged }}", "Wh", "energy", "total_increasing" },
    { "charged",    "Energy charged",    "{{ value_json.charged }}",    "Wh", "energy", "total_increasing" },
    // Bus address, so a reshuffle is visible in the history rather than inferred from it.
    { "address",    "Bus address",       "{{ value_json.address }}",    "",   "",       "" },
  };
  for (const auto& f : kFields) {
    const String key = sfx + "_" + f.key;
    publishDiscoverySensor(key.c_str(), f.name, bt.c_str(), f.tpl, f.unit, f.cla, f.sta,
                           sfx, name, "NDS Tempra", sw);
  }
  publishRegDiscovery(slot, sfx, name, "NDS Tempra", sw);
}

void publishSolarDiscovery() {
  const NBusSolar& s = parser.state().solar;
  const String sfx  = String("s") + s.id.serial;
  const String name = String("Solar charger ") + s.id.serial;
  const String sw   = s.id.fw_valid
                    ? String(s.id.fw_major) + "." + String(s.id.fw_minor) : String();
  const String st = solarTopic();
  const String rt = starterTopic();

  publishDiscoverySensor((sfx + "_voltage").c_str(), "Voltage", st.c_str(),
                         "{{ value_json.voltage }}", "V", "voltage", "measurement",
                         sfx, name, "NDS MPPT", sw);
  publishDiscoverySensor((sfx + "_current").c_str(), "Charge current", st.c_str(),
                         "{{ value_json.current }}", "A", "current", "measurement",
                         sfx, name, "NDS MPPT", sw);
  // The starter battery has no node of its own; the charger is what measures it.
  publishDiscoverySensor((sfx + "_starter").c_str(), "Starter battery voltage", rt.c_str(),
                         "{{ value_json.voltage }}", "V", "voltage", "measurement",
                         sfx, name, "NDS MPPT", sw);
  publishDiscoverySensor((sfx + "_address").c_str(), "Bus address", st.c_str(),
                         "{{ value_json.address }}", "", "", "",
                         sfx, name, "NDS MPPT", sw);
  // The charge stage from 0x60 d1. The register mirror publishes the same quantity from
  // 0x26 d3 as "Charge stage", and this second entity exists precisely so the two can be
  // compared: they have been byte-for-byte equal in every capture so far, but "equal in
  // every capture so far" holds only for as long as something keeps checking. If these
  // ever diverge, the reading in the protocol map is wrong and the history will say when.
  publishDiscoverySensor((sfx + "_stage60").c_str(), "Charge stage (0x60)", st.c_str(),
                         "{{ value_json.stage }}", "", "", "measurement",
                         sfx, name, "NDS MPPT", sw);
  publishRegDiscovery(NBusCycleTracker::kSolarSlot, sfx, name, "NDS MPPT", sw);
}

// Bridge-level diagnostics. These describe the reader, not the bus, so they hang off the
// ESP's own device and are published as soon as MQTT is up.
void publishBridgeDiscovery() {
  const String ct = cfg.base + "/cycle";
  publishDiscoverySensor("cycle_expected", "Poll cycle length", ct.c_str(),
                         "{{ value_json.expected }}", "", "", "measurement");
  // If this climbs steadily, frames are being lost and readings are being thrown away
  // rather than misattributed — the trade this design deliberately makes.
  publishDiscoverySensor("cycle_dropped", "Poll cycles dropped", ct.c_str(),
                         "{{ value_json.dropped }}", "", "", "total_increasing");
  publishDiscoverySensor("cycle_accepted", "Poll cycles accepted", ct.c_str(),
                         "{{ value_json.accepted }}", "", "", "total_increasing");
}

// Discovery is (re)published whenever the set of identified devices changes — on connect,
// and again when a pack's serial first arrives or a reshuffle points a slot at a different
// pack. Every message is retained and idempotent, so republishing costs nothing.
String g_discoSignature;

String identitySignature() {
  String s;
  for (int i = 0; i < NBUS_MAX_BATTERIES; ++i) {
    s += parser.state().batt[i].id.serial;
    s += '|';
  }
  s += parser.state().solar.id.serial;
  return s;
}

void publishDiscovery() {
  publishBridgeDiscovery();
  // A pack is published only once its serial is known. Naming it after its slot in the
  // meantime would create an entity that a later reshuffle silently repoints at the other
  // pack — exactly the failure this whole change exists to remove. The serial arrives
  // within seconds of boot; until then the pack is simply absent, which is honest.
  for (int i = 0; i < NBUS_MAX_BATTERIES; ++i) {
    if (parser.state().batt[i].id.serialValid()) publishBatteryDiscovery(i);
  }
  if (parser.state().solar.id.serialValid()) publishSolarDiscovery();
  g_discoSignature = identitySignature();
  Serial.printf("[mqtt] discovery published (%s)\n", g_discoSignature.c_str());
}

bool mqttConnect() {
  if (cfg.host.isEmpty()) return false;
  mqtt.setServer(cfg.host.c_str(), cfg.port);
  // Sized for the largest discovery payload, not for the state messages — discovery is far
  // bigger, because every entity repeats the whole device block. The charge-stage entity
  // with its stage-name template is currently the longest at about 420 bytes of JSON plus a
  // 65-byte topic, which cleared the old 512 by twenty-odd bytes. PubSubClient truncates
  // nothing and warns about nothing: publish() simply returns false, and the entity would
  // have gone quietly missing from Home Assistant. Hence the headroom, and hence the check
  // in publishDiscoverySensor.
  mqtt.setBufferSize(1024);

  String clientId = String(NBUS_DEVICE_ID) + "-" + String((uint32_t)ESP.getEfuseMac(), HEX);
  const char* user = cfg.user.isEmpty() ? nullptr : cfg.user.c_str();
  const char* pass = cfg.pass.isEmpty() ? nullptr : cfg.pass.c_str();

  bool ok = mqtt.connect(clientId.c_str(), user, pass,
                         statusTopic().c_str(), 0, true, "offline");
  if (ok) {
    mqtt.publish(statusTopic().c_str(), "online", true);
    g_discoverySent = false;  // re-send discovery on (re)connect
    g_mqttFailCount = 0;
    Serial.println(F("[mqtt] connected"));
  } else {
    Serial.print(F("[mqtt] connect failed, state="));
    Serial.println(mqtt.state());
  }
  return ok;
}

// --------------------------------------------------------------------------
// State publishing
// --------------------------------------------------------------------------
void publishState() {
  const NBusState& s = parser.state();
  const uint32_t now = millis();

  // One message per pack. Publishing a pack whose serial is unknown would feed a topic
  // that no entity is subscribed to, so skip it.
  for (int i = 0; i < NBUS_MAX_BATTERIES; ++i) {
    const NBusBattery& b = s.batt[i];
    if (!b.id.serialValid()) continue;
    if (!b.voltage_valid || (now - g_lastBattMs[i]) >= NBUS_STALE_MS) continue;

    JsonDocument doc;
    doc["serial"] = b.id.serial;
    if (b.id.address_valid) doc["address"] = b.id.address;
    if (b.soc_valid)      doc["soc"]      = b.soc;
    doc["voltage"] = roundf(b.voltage * 100) / 100.0;
    if (b.wh_valid)       doc["energy"]   = b.wh;
    if (b.quality_valid)  doc["quality"]  = b.quality;
    if (b.capacity_valid) doc["capacity"] = b.capacity_ah;
    // These two alternate: 0x34 sends FFFF for whichever direction does not apply, so one
    // of them is invalid at all times. Omitting the key would leave the template rendering
    // an empty string, which Home Assistant treats as "no update" — the entity would then
    // hold the last figure from the opposite direction indefinitely. An explicit null
    // renders as "None", which does set the state to unknown.
    if (b.to_empty_valid) doc["to_empty"] = b.to_empty_min; else doc["to_empty"] = nullptr;
    if (b.to_full_valid)  doc["to_full"]  = b.to_full_min;  else doc["to_full"]  = nullptr;
    if (b.discharged_valid) doc["discharged"] = b.discharged_wh;
    if (b.charged_valid)    doc["charged"]    = b.charged_wh;
    if (b.current_valid) {
      doc["current"] = roundf(b.current * 100) / 100.0;
      doc["power"]   = roundf(b.voltage * b.current * 10) / 10.0;
    }
    const bool anyCell = b.cell_valid[0] || b.cell_valid[1] ||
                         b.cell_valid[2] || b.cell_valid[3];
    if (anyCell && (now - g_lastCellMs[i]) < NBUS_STALE_MS) {
      JsonArray cells = doc["cells"].to<JsonArray>();
      for (int c = 0; c < 4; ++c) cells.add(roundf(b.cell_v[c] * 1000) / 1000.0);
    }
    String payload; serializeJson(doc, payload);
    mqtt.publish(batteryTopic(i).c_str(), payload.c_str(), true);
  }

  // Solar charger
  if (s.solar.valid && (now - g_lastSolarMs) < NBUS_STALE_MS) {
    JsonDocument doc;
    doc["voltage"] = roundf(s.solar.voltage * 100) / 100.0;
    doc["current"] = roundf(s.solar.current * 100) / 100.0;
    if (s.solar.stage_valid) doc["stage"] = s.solar.stage;
    if (s.solar.id.address_valid) doc["address"] = s.solar.id.address;
    if (s.solar.id.serialValid()) doc["serial"] = s.solar.id.serial;
    String payload; serializeJson(doc, payload);
    mqtt.publish(solarTopic().c_str(), payload.c_str(), true);
  }

  // Starter battery
  if (s.solar.starter_valid && (now - g_lastStarterMs) < NBUS_STALE_MS) {
    JsonDocument doc;
    doc["voltage"] = roundf(s.solar.starter_voltage * 100) / 100.0;
    String payload; serializeJson(doc, payload);
    mqtt.publish(starterTopic().c_str(), payload.c_str(), true);
  }

  // How the frame split is holding up. This is the health of the attribution itself, so
  // it belongs in the recorder alongside the readings it vouches for.
  {
    JsonDocument doc;
    doc["expected"] = cycles.expectedPerCycle();
    doc["accepted"] = cycles.cyclesAccepted();
    doc["dropped"]  = cycles.cyclesDropped();
    doc["epoch"]    = cycles.topologyEpoch();
    String payload; serializeJson(doc, payload);
    mqtt.publish((cfg.base + "/cycle").c_str(), payload.c_str(), true);
  }
}

// --------------------------------------------------------------------------
// Raw register mirror
//
// Registers the parser understands become named sensors above. Everything else would
// otherwise be discarded, and the undecoded ones are exactly the ones still being worked
// out — a register cannot be identified from a value, only from how it moves against
// everything else. Some of them will not move for weeks: on a healthy bus the candidate
// alarm registers (0xC0, 0xF0, 0xF1, 0xF2) are all zero, which looks exactly like an
// unused register, so they have to be recorded continuously and unattended to ever be
// told apart.
//
// Each pair gets its own retained topic, so the broker timestamps every change for free
// and a constant register costs one message ever. No discovery config is published:
// naming an entity would claim a meaning we have not established.
// --------------------------------------------------------------------------
struct RegMirror {
  int8_t   busSlot = 0;            // poll slot, or NBusCycleTracker::kSolarSlot
  uint8_t  reg = 0;
  uint8_t  data[4] = {0, 0, 0, 0};
  uint32_t lastPubMs = 0;
  bool     used = false;
};
RegMirror g_regMirror[NBUS_REG_MIRROR_SLOTS];

// Keyed on the poll slot rather than the NAD, because the two packs share one NAD.
void mirrorRegister(int busSlot, uint8_t reg, const uint8_t* d) {
  if (!mqtt.connected()) return;

  RegMirror* slot = nullptr;
  for (auto& m : g_regMirror) {
    if (m.used && m.busSlot == busSlot && m.reg == reg) { slot = &m; break; }
    if (!m.used && slot == nullptr) slot = &m;  // remember the first free slot
  }
  if (slot == nullptr) return;  // table full; a fixed table cannot be flooded by noise

  const bool isNew   = !slot->used;
  const bool changed = isNew || memcmp(slot->data, d, 4) != 0;
  const uint32_t now = millis();

  // Throttle per register rather than globally. A register that never moves publishes
  // the instant it finally does; only a chronically noisy one is rate-limited, and it is
  // the quiet ones that matter here.
  if (!changed) return;
  if (!isNew && (now - slot->lastPubMs) < NBUS_REG_MIRROR_MS) return;

  slot->used    = true;
  slot->busSlot = static_cast<int8_t>(busSlot);
  slot->reg     = reg;
  memcpy(slot->data, d, 4);
  slot->lastPubMs = now;

  char payload[9];
  snprintf(payload, sizeof(payload), "%02X%02X%02X%02X", d[0], d[1], d[2], d[3]);
  mqtt.publish(regTopic(busSlot, reg).c_str(), payload, true);
}

// A reshuffle makes every mirrored battery register describe the wrong pack. Forget them
// so the next frame republishes under the correct topic instead of being suppressed as
// "unchanged" by the throttle above.
void forgetBatteryMirror() {
  for (auto& m : g_regMirror) {
    if (m.used && m.busSlot != NBusCycleTracker::kSolarSlot) m = RegMirror();
  }
}

// --------------------------------------------------------------------------
// Raw frame ring buffer
//
// Every checksum-valid frame lands here with a timestamp. Nothing is written to
// flash while the bus is healthy — at ~12 frames/s that would wear the chip out
// in days, and the recent past is the only part anyone wants anyway. The ring is
// flushed to flash exactly once, when the bus falls silent, which is the event
// this device exists to catch.
//
// Statically allocated on purpose. 48 KB is a large enough bite that a runtime
// malloc failure would be a real possibility, and it would surface as "captures
// nothing" long after the fact. At link time it either fits or it does not.
// --------------------------------------------------------------------------
static RawEntry g_ring[NBUS_RAW_RING_ENTRIES];
static uint32_t g_ringHead  = 0;   // next slot to write
static uint32_t g_ringTotal = 0;   // frames ever recorded; also tells us whether we wrapped
static bool     g_busSilent = false;
static uint32_t g_dumpSeq = 0;

// Single-threaded by construction: readLin() and httpServer.handleClient() both
// run from loop(), so the ring needs no locking. Keep it that way.
inline uint32_t ringCount() {
  return (g_ringTotal < NBUS_RAW_RING_ENTRIES) ? g_ringTotal : NBUS_RAW_RING_ENTRIES;
}
inline uint32_t ringOldest() {
  return (g_ringTotal < NBUS_RAW_RING_ENTRIES) ? 0 : g_ringHead;
}

void ringPush(const uint8_t* d) {
  RawEntry& e = g_ring[g_ringHead];
  e.ms = millis();
  memcpy(e.d, d, 8);
  g_ringHead = (g_ringHead + 1) % NBUS_RAW_RING_ENTRIES;
  g_ringTotal++;
}

int formatEntry(const RawEntry& e, char* out, size_t n) {
  return snprintf(out, n, "%lu %02X %02X %02X %02X %02X %02X %02X %02X\n",
                  (unsigned long)e.ms,
                  e.d[0], e.d[1], e.d[2], e.d[3], e.d[4], e.d[5], e.d[6], e.d[7]);
}

// --------------------------------------------------------------------------
// Raw dump persistence
// --------------------------------------------------------------------------
bool g_fsReady = false;

void rawFsInit() {
  g_fsReady = LittleFS.begin(true);   // format on first boot if unformatted
  if (!g_fsReady) {
    Serial.println(F("[raw] LittleFS mount failed — dumps disabled"));
    return;
  }
  if (!LittleFS.exists(NBUS_RAW_DUMP_DIR)) LittleFS.mkdir(NBUS_RAW_DUMP_DIR);
  prefs.begin("nbus", true);
  g_dumpSeq = prefs.getUInt("dumpseq", 0);
  prefs.end();
  Serial.printf("[raw] LittleFS %u/%u bytes used\n",
                (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
}

// Keep only the newest NBUS_RAW_MAX_DUMPS files. Sequence numbers are monotonic
// across reboots (they live in NVS), so "lowest number" really is "oldest" —
// millis() would not survive the very reset we are trying to record.
// Bounded, and it gives up the moment a removal fails: this runs unattended from
// rawPersist() just as the bus goes quiet, and spinning here would cost the capture
// it was called to write.
void rawPruneDumps() {
  for (int guard = 0; guard < NBUS_RAW_MAX_DUMPS + 4; guard++) {
    File dir = LittleFS.open(NBUS_RAW_DUMP_DIR);
    if (!dir) return;
    int count = 0;
    uint32_t lowest = 0xFFFFFFFF;
    String lowestName;
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      count++;
      String nm = String(f.name());
      uint32_t seq = (uint32_t)strtoul(nm.c_str(), nullptr, 10);
      if (seq < lowest) { lowest = seq; lowestName = nm; }
    }
    dir.close();
    if (count <= NBUS_RAW_MAX_DUMPS || lowestName.isEmpty()) return;
    if (!LittleFS.remove(String(NBUS_RAW_DUMP_DIR) + "/" + lowestName)) return;
  }
}

void rawPersist(const char* reason) {
  if (!g_fsReady || ringCount() == 0) return;
  g_dumpSeq++;
  prefs.begin("nbus", false);
  prefs.putUInt("dumpseq", g_dumpSeq);
  prefs.end();
  char path[48];
  snprintf(path, sizeof(path), "%s/%06lu.txt", NBUS_RAW_DUMP_DIR, (unsigned long)g_dumpSeq);
  File f = LittleFS.open(path, "w");
  if (!f) { Serial.printf("[raw] cannot write %s\n", path); return; }

  const uint32_t n = ringCount();
  f.printf("# nbus raw dump v1\n# fw %s (%s)\n# reason %s\n",
           NBUS_FW_VERSION, NBUS_FW_BUILD, reason);
  f.printf("# written_at_ms %lu  frames %lu  total_seen %lu\n",
           (unsigned long)millis(), (unsigned long)n, (unsigned long)g_ringTotal);
  f.print(F("# t_ms NAD PCI SID REG D0 D1 D2 D3\n"));

  char line[48];
  uint32_t idx = ringOldest();
  for (uint32_t i = 0; i < n; ++i) {
    formatEntry(g_ring[idx], line, sizeof(line));
    f.print(line);
    idx = (idx + 1) % NBUS_RAW_RING_ENTRIES;
    if ((i & 0xFF) == 0) esp_task_wdt_reset();   // 4096 flash writes outlast a 30 s watchdog
  }
  f.close();
  Serial.printf("[raw] wrote %s (%lu frames, reason: %s)\n", path, (unsigned long)n, reason);
  rawPruneDumps();

  if (mqtt.connected()) {
    char topic[64];
    snprintf(topic, sizeof(topic), "%s/raw/dump", cfg.base.c_str());
    mqtt.publish(topic, path, true);
  }
}

// --------------------------------------------------------------------------
// HTTP endpoints for remote capture
// --------------------------------------------------------------------------

// /status is deliberately NOT behind auth. It is the liveness probe: if the
// credentials are ever wrong, this is how you find out the device is alive at
// all rather than guessing between "bad password" and "bricked by that OTA".
// It therefore carries nothing worth protecting — no topics, no hostnames.
void handleStatus() {
  JsonDocument doc;
  doc["version"]   = NBUS_FW_VERSION;
  doc["build"]     = NBUS_FW_BUILD;
  doc["uptime_s"]  = millis() / 1000;
  doc["heap_free"] = ESP.getFreeHeap();
  doc["heap_min"]  = ESP.getMinFreeHeap();
  doc["rssi"]      = WiFi.RSSI();
  doc["mqtt"]      = mqtt.connected();
  doc["frames"]    = g_ringTotal;
  doc["ring_used"] = ringCount();
  doc["ring_size"] = (uint32_t)NBUS_RAW_RING_ENTRIES;
  doc["bus_silent"] = g_busSilent;
  doc["last_frame_age_ms"] = g_lastFrameMs ? (millis() - g_lastFrameMs) : 0;
  // How the two packs are being told apart, and how well. A dropped count that keeps pace
  // with the accepted count means frames are being lost somewhere; readings are still
  // correct, there are just fewer of them.
  doc["cycle_expected"] = cycles.expectedPerCycle();
  doc["cycle_accepted"] = cycles.cyclesAccepted();
  doc["cycle_dropped"]  = cycles.cyclesDropped();
  doc["cycle_epoch"]    = cycles.topologyEpoch();
  // Only the count of identified packs, not which ones. Serial numbers would make this
  // endpoint worth reading, and it is deliberately the one thing served without auth.
  int named = 0;
  for (int i = 0; i < NBUS_MAX_BATTERIES; ++i) {
    if (parser.state().batt[i].id.serialValid()) ++named;
  }
  doc["batteries_identified"] = named;
  doc["fs_used"]   = g_fsReady ? LittleFS.usedBytes() : 0;
  doc["fs_total"]  = g_fsReady ? LittleFS.totalBytes() : 0;
  doc["dump_seq"]  = g_dumpSeq;
  String out;
  serializeJson(doc, out);
  httpServer.send(200, "application/json", out);
}

// Stream the live ring. `?since=<ms>` returns only entries newer than that
// timestamp, which lets a poller follow the bus without the server holding the
// connection open — a long-lived stream would block loop(), and loop() is what
// drains the UART. Starving the reader to watch the reader would be perverse.
void handleRawDump() {
  if (!requireAuth()) return;
  const uint32_t since = httpServer.hasArg("since")
                       ? (uint32_t)strtoul(httpServer.arg("since").c_str(), nullptr, 10) : 0;
  httpServer.setContentLength(CONTENT_LENGTH_UNKNOWN);
  httpServer.send(200, "text/plain", "");

  char hdr[160];
  snprintf(hdr, sizeof(hdr),
           "# nbus raw dump v1\n# fw %s\n# now_ms %lu  frames %lu  total_seen %lu\n"
           "# t_ms NAD PCI SID REG D0 D1 D2 D3\n",
           NBUS_FW_VERSION, (unsigned long)millis(),
           (unsigned long)ringCount(), (unsigned long)g_ringTotal);
  httpServer.sendContent(hdr);

  String chunk;
  chunk.reserve(1700);
  char line[48];
  const uint32_t n = ringCount();
  uint32_t idx = ringOldest();
  for (uint32_t i = 0; i < n; ++i) {
    const RawEntry& e = g_ring[idx];
    idx = (idx + 1) % NBUS_RAW_RING_ENTRIES;
    if (e.ms <= since) continue;
    formatEntry(e, line, sizeof(line));
    chunk += line;
    if (chunk.length() > 1400) {
      httpServer.sendContent(chunk);
      chunk = "";
      esp_task_wdt_reset();
    }
  }
  if (chunk.length()) httpServer.sendContent(chunk);
  httpServer.sendContent("");
}

void handleRawSave() {
  if (!requireAuth()) return;
  rawPersist("manual");
  httpServer.send(200, "text/plain", "saved\n");
}

void handleRawFiles() {
  if (!requireAuth()) return;
  if (!g_fsReady) { httpServer.send(503, "text/plain", "fs unavailable\n"); return; }
  String out;
  File dir = LittleFS.open(NBUS_RAW_DUMP_DIR);
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    out += f.name();
    out += '\t';
    out += String((uint32_t)f.size());
    out += '\n';
  }
  dir.close();
  httpServer.send(200, "text/plain", out.isEmpty() ? String("(none)\n") : out);
}

void handleRawFile() {
  if (!requireAuth()) return;
  if (!g_fsReady || !httpServer.hasArg("n")) {
    httpServer.send(400, "text/plain", "usage: /raw/file?n=<name>\n");
    return;
  }
  // Take only the basename: the argument indexes our own dump directory and is
  // never allowed to walk out of it.
  String name = httpServer.arg("n");
  int slash = name.lastIndexOf('/');
  if (slash >= 0) name = name.substring(slash + 1);
  const String path = String(NBUS_RAW_DUMP_DIR) + "/" + name;
  File f = LittleFS.open(path, "r");
  if (!f) { httpServer.send(404, "text/plain", "no such dump\n"); return; }
  httpServer.streamFile(f, "text/plain");
  f.close();
}

// The only destructive endpoint here, and the file it deletes may be the sole capture of
// a bus state that took weeks to occur — so it demands confirm=yes rather than being
// reachable by a stray click or a browser prefetching a link.
//
// g_dumpSeq is deliberately not reset: sequence numbers stay monotonic across a wipe, so
// "highest number" remains "newest" and a cleared device cannot reuse the name of a dump
// someone has already downloaded.
void handleRawClear() {
  if (!requireAuth()) return;
  if (!g_fsReady) { httpServer.send(503, "text/plain", "fs unavailable\n"); return; }
  if (httpServer.arg("confirm") != "yes") {
    httpServer.send(400, "text/plain",
                    F("refused: this deletes captured evidence\n"
                      "  /raw/clear?confirm=yes           delete every dump\n"
                      "  /raw/clear?confirm=yes&n=<name>  delete one\n"));
    return;
  }

  if (httpServer.hasArg("n")) {
    String name = httpServer.arg("n");
    int slash = name.lastIndexOf('/');
    if (slash >= 0) name = name.substring(slash + 1);
    const String path = String(NBUS_RAW_DUMP_DIR) + "/" + name;
    if (!LittleFS.exists(path)) { httpServer.send(404, "text/plain", "no such dump\n"); return; }
    LittleFS.remove(path);
    httpServer.send(200, "text/plain", "deleted " + name + "\n");
    return;
  }

  // Reopen the directory for each removal, as rawPruneDumps() does: deleting through a
  // live LittleFS directory handle while iterating it does not reliably enumerate. The
  // entry must be closed before removing it — LittleFS refuses to unlink an open file,
  // and an unbounded retry on that failure is a watchdog reset.
  int removed = 0;
  for (int guard = 0; guard < NBUS_RAW_MAX_DUMPS + 4; guard++) {
    File dir = LittleFS.open(NBUS_RAW_DUMP_DIR);
    if (!dir) break;
    File f = dir.openNextFile();
    String nm = f ? String(f.name()) : String();
    if (f) f.close();
    dir.close();
    if (nm.isEmpty()) break;
    if (!LittleFS.remove(String(NBUS_RAW_DUMP_DIR) + "/" + nm)) break;
    removed++;
  }
  httpServer.send(200, "text/plain", "deleted " + String(removed) + " dump(s)\n");
}

// --------------------------------------------------------------------------
// LIN frame capture (read-only)
// --------------------------------------------------------------------------
void markFresh(int busSlot, uint8_t reg) {
  const uint32_t now = millis();
  if (busSlot == NBusCycleTracker::kSolarSlot) {
    if (reg == 0x01) g_lastStarterMs = now;
    else             g_lastSolarMs = now;
  } else if (busSlot >= 0 && busSlot < NBUS_MAX_BATTERIES) {
    g_lastBattMs[busSlot] = now;
    if (reg == 0x56 || reg == 0x57) g_lastCellMs[busSlot] = now;
  }
}

// Hand one attributed frame to the parser and the mirror.
void dispatchFrame(const NBusCycleTracker::Out& o) {
  const bool isSolar = (o.slot == NBusCycleTracker::kSolarSlot);
  const uint8_t nad  = isSolar ? NBUS_NAD_SOLAR : NBUS_NAD_BATTERY;
  const uint8_t frame[8] = { nad, 0x06, 0xF4, o.reg,
                             o.d[0], o.d[1], o.d[2], o.d[3] };

  if (parser.feedResponse(frame, sizeof frame, isSolar ? 0 : o.slot)) {
    markFresh(o.slot, o.reg);
  } else {
    // A well-formed response the parser has no meaning for. Checksum and framing already
    // hold at this point, so it is real bus content, not noise — mirror it.
    mirrorRegister(o.slot, o.reg, o.d);
  }
}

void processFrameWindow(const uint8_t* buf, size_t len) {
#ifdef NBUS_DEBUG
  // Raw window dump: during bring-up this is what separates "nothing on the wire" from
  // "bytes arrive but framing is off".
  Serial.printf("[raw %2u]", (unsigned)len);
  for (size_t i = 0; i < len; ++i) Serial.printf(" %02X", buf[i]);
  Serial.println();
#endif
  // A frame window (delimited by an idle gap) holds: [break 0x00?] 0x55 PID data... checksum.
  for (size_t i = 0; i + 1 < len; ++i) {
    if (buf[i] != 0x55) continue;
    const uint8_t pid = buf[i + 1];
    if (pid != 0x7D) return;          // not a slave response; ignore this window
    const size_t dStart = i + 2;
    if (len - dStart < 8) return;     // not enough data bytes yet
    const uint8_t* d = &buf[dStart];

    // Optional checksum verification (classic checksum: data bytes only, PID excluded).
    if (len - dStart >= 9) {
      uint8_t expect = NBusParser::classicChecksum(d, 8);
      if (expect != d[8]) {
#ifdef NBUS_DEBUG
        Serial.printf("[lin] checksum mismatch reg=%02X exp=%02X got=%02X\n", d[3], expect, d[8]);
#endif
        return;
      }
    }

    // Record before dispatching. The ring is deliberately indiscriminate: a frame
    // the parser already understands is still evidence, and the registers we have
    // not decoded yet are exactly the ones a fault is most likely to show up in.
    ringPush(d);
    g_lastFrameMs = millis();

#ifdef NBUS_DEBUG
    Serial.printf("[lin] NAD %02X reg %02X : %02X %02X %02X %02X\n",
                  d[0], d[3], d[4], d[5], d[6], d[7]);
#endif

    // Nothing is decoded straight from the wire any more. Both packs answer on NAD 0x85,
    // so a frame only acquires an owner once its whole poll cycle has closed and the
    // cycle's length confirms nothing was lost. The tracker therefore returns frames in
    // bursts, one cycle at a time, and returns none at all for a cycle it cannot vouch for.
    if (d[1] == 0x06 && d[2] == 0xF4) {
      NBusCycleTracker::Out out[NBusCycleTracker::kMaxPerCycle + 1];
      const uint32_t epochBefore = cycles.topologyEpoch();
      const int n = cycles.feed(d[0], d[3], &d[4], out, sizeof out / sizeof out[0]);

      // A device joined or left the bus: the slots have been renumbered, so everything
      // held against them describes the wrong pack. Drop it all and let discovery
      // re-publish once the new occupants have named themselves.
      if (cycles.topologyEpoch() != epochBefore) {
        parser.forgetBatteries();
        forgetBatteryMirror();
        for (int i = 0; i < NBUS_MAX_BATTERIES; ++i) g_lastBattMs[i] = g_lastCellMs[i] = 0;
        Serial.printf("[bus] poll cycle changed to %d battery frames; slots cleared\n",
                      cycles.expectedPerCycle());
      }
      for (int i = 0; i < n; ++i) dispatchFrame(out[i]);
    }
    return;  // one response per window
  }
}

void readLin() {
  static uint8_t buf[NBUS_RX_BUF];
  static size_t  len = 0;
  static uint32_t lastByteUs = 0;

  while (LinSerial.available() > 0) {
    int b = LinSerial.read();
    if (b < 0) break;
    if (len < sizeof buf) buf[len++] = (uint8_t)b;
    lastByteUs = micros();
  }

  if (len > 0 && (micros() - lastByteUs) > NBUS_FRAME_GAP_US) {
    processFrameWindow(buf, len);
    len = 0;
  } else if (len >= sizeof buf) {
    processFrameWindow(buf, len);
    len = 0;
  }
}

// --------------------------------------------------------------------------
// Watchdog (Arduino-ESP32 core ≥ 3.x uses a config struct)
// --------------------------------------------------------------------------
void watchdogInit() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  esp_task_wdt_config_t wdt_cfg = {};
  wdt_cfg.timeout_ms = NBUS_WDT_TIMEOUT_S * 1000;
  wdt_cfg.idle_core_mask = 0;
  wdt_cfg.trigger_panic = true;
  esp_task_wdt_reconfigure(&wdt_cfg);  // TWDT already initialised by the core
  esp_task_wdt_add(NULL);
#else
  esp_task_wdt_init(NBUS_WDT_TIMEOUT_S, true);
  esp_task_wdt_add(NULL);
#endif
}

// --------------------------------------------------------------------------
// Arduino entry points
// --------------------------------------------------------------------------
void setup() {
  pinMode(NBUS_LED_PIN, OUTPUT);
  ledWrite(false);

  Serial.begin(115200);
  delay(200);
  Serial.printf("\n[boot] N-Bus ESP32-C3 firmware %s (%s)\n", NBUS_FW_VERSION, NBUS_FW_BUILD);

  maybeFactoryReset();
  loadConfig();
  // An unset broker host silently disables MQTT, which looks identical to a broker
  // that refuses the connection. Say which of the two it is.
  if (cfg.host.isEmpty()) {
    Serial.println(F("[cfg] no MQTT broker configured - set one at http://<device-ip>/config"));
  } else {
    Serial.printf("[cfg] MQTT %s:%u, user '%s', base '%s'\n",
                  cfg.host.c_str(), cfg.port, cfg.user.c_str(), cfg.base.c_str());
  }

  rawFsInit();

  // LIN UART: RX-only. TX pin is -1 — the bus is never driven.
  LinSerial.begin(NBUS_BAUD, SERIAL_8N1, NBUS_RX_PIN, NBUS_TX_PIN);
  Serial.printf("[lin] UART%d RX=GPIO%d @ %d 8N1 (read-only)\n",
                NBUS_UART_NUM, NBUS_RX_PIN, NBUS_BAUD);

  WiFi.onEvent(onWifiEvent);
  startProvisioning();
  watchdogInit();
}

void loop() {
  esp_task_wdt_reset();

  readLin();
  handleProvisioning();
  if (g_netServicesUp) {
    httpServer.handleClient();
    ElegantOTA.loop();
  }

  // MQTT keepalive + non-blocking reconnect.
  if (mqtt.connected()) {
    mqtt.loop();
  } else if (WiFi.status() == WL_CONNECTED && !cfg.host.isEmpty() &&
             (millis() - g_lastMqttTryMs) > NBUS_MQTT_RETRY_MS) {
    g_lastMqttTryMs = millis();
    if (!mqttConnect()) {
      if (++g_mqttFailCount >= 60) {  // ~5 min of failures → reboot
        Serial.println(F("[mqtt] too many failures — rebooting"));
        delay(200);
        ESP.restart();
      }
    }
  }

  // Discovery once per connection, then throttled state publishing.
  if (mqtt.connected()) {
    // Republished when a device first names itself and whenever a reshuffle points a slot
    // at a different pack, not only on connect: at connect time no serial is known yet,
    // so that alone would leave Home Assistant with the bridge and nothing else.
    if (!g_discoverySent || identitySignature() != g_discoSignature) {
      publishDiscovery();
      g_discoverySent = true;
    }
    if (millis() - g_lastPublishMs > NBUS_PUBLISH_MS) {
      g_lastPublishMs = millis();
      publishState();
    }
  }

  // Bus silence is the write trigger: the ring is flushed when there is nothing
  // left to lose by writing. No brownout handler and no hurry — if the ESP loses
  // power along with the bus the dump is simply not written, which is acceptable
  // now that the device is a decoder rather than a fault recorder. Edge-triggered —
  // one dump per silence, not one per loop — and re-armed only once frames resume.
  if (g_lastFrameMs && !g_otaActive) {
    const uint32_t age = millis() - g_lastFrameMs;
    if (!g_busSilent && age > NBUS_RAW_SILENCE_MS) {
      g_busSilent = true;
      Serial.printf("[raw] bus silent for %lu ms — persisting ring\n", (unsigned long)age);
      rawPersist("bus silent");
    } else if (g_busSilent && age < NBUS_RAW_SILENCE_MS) {
      g_busSilent = false;
      Serial.println(F("[raw] bus is back"));
    }
  }

  // Heartbeat: brief blink each second.
  if (millis() - g_lastHeartbeatMs > NBUS_HEARTBEAT_MS) {
    g_lastHeartbeatMs = millis();
    ledWrite(true);
    delay(5);
    ledWrite(false);
  }
}
