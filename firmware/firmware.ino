// N-Bus ESP32-C3 firmware — passively reads a Büttner/Dometic NDS "N-Bus" (LIN,
// 19200 baud) and republishes values over MQTT with Home Assistant auto-discovery,
// WiFiManager provisioning and ElegantOTA web updates.
//
// SAFETY: read-only. The LIN UART is opened RX-only (TX pin = -1). Never transmit.
//
// Board: ESP32-C3 Super Mini  ·  FQBN esp32:esp32:esp32c3 (USB CDC On Boot enabled).

#include <Preferences.h>
#include <WebServer.h>
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
HardwareSerial   LinSerial(NBUS_UART_NUM);
NBusParser       parser;
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
uint32_t g_lastBattMs    = 0;
uint32_t g_lastSolarMs   = 0;
uint32_t g_lastStarterMs = 0;
uint32_t g_lastCellMs    = 0;

uint32_t g_lastPublishMs   = 0;
uint32_t g_lastMqttTryMs   = 0;
uint32_t g_lastHeartbeatMs = 0;
bool     g_discoverySent   = false;
uint8_t  g_mqttFailCount   = 0;

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

void handleConfigGet() {
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
  p += F("<button type='submit'>Save &amp; reconnect</button></form>");
  httpServer.send(200, "text/html", p);
}

void handleConfigPost() {
  if (httpServer.hasArg("host")) cfg.host = httpServer.arg("host");
  if (httpServer.hasArg("port")) cfg.port = httpServer.arg("port").toInt();
  if (httpServer.hasArg("user")) cfg.user = httpServer.arg("user");
  // An empty password field means "unchanged", so the other fields can be edited
  // without having to retype the broker password.
  if (httpServer.arg("pass").length()) cfg.pass = httpServer.arg("pass");
  if (httpServer.hasArg("base")) cfg.base = httpServer.arg("base");
  if (cfg.port == 0) cfg.port = 1883;
  if (cfg.base.isEmpty()) cfg.base = NBUS_DEFAULT_BASE_TOPIC;
  saveConfig();
  Serial.printf("[cfg] saved MQTT %s:%u user '%s' base '%s'\n",
                cfg.host.c_str(), cfg.port, cfg.user.c_str(), cfg.base.c_str());

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
                      "<p><a href='/update'>Firmware update</a></p>"));
  });
  httpServer.on("/config", HTTP_GET, handleConfigGet);
  httpServer.on("/config", HTTP_POST, handleConfigPost);
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
String batteryTopic() { return cfg.base + "/battery"; }
String solarTopic()   { return cfg.base + "/solar"; }
String starterTopic() { return cfg.base + "/starter"; }

void addDevice(JsonObject dev) {
  JsonArray ids = dev["ids"].to<JsonArray>();
  ids.add(NBUS_DEVICE_ID);
  dev["name"] = NBUS_DEVICE_NAME;
  dev["mf"]   = NBUS_DEVICE_MF;
  dev["mdl"]  = NBUS_DEVICE_MDL;
}

void publishDiscoverySensor(const char* key, const char* name, const char* stateTopic,
                            const char* valTpl, const char* unit, const char* devCla) {
  JsonDocument doc;
  doc["name"]         = name;
  doc["uniq_id"]      = String(NBUS_DEVICE_ID) + "_" + key;
  doc["stat_t"]       = stateTopic;
  doc["val_tpl"]      = valTpl;
  if (unit && unit[0])   doc["unit_of_meas"] = unit;
  if (devCla && devCla[0]) doc["dev_cla"]    = devCla;
  doc["stat_cla"]     = "measurement";
  doc["avty_t"]       = statusTopic();
  addDevice(doc["dev"].to<JsonObject>());

  String topic = String(NBUS_HA_PREFIX) + "/sensor/" + NBUS_DEVICE_ID + "_" + key + "/config";
  String payload;
  serializeJson(doc, payload);
  mqtt.publish(topic.c_str(), payload.c_str(), true);
}

void publishDiscovery() {
  const String bt = batteryTopic();
  const String st = solarTopic();
  const String rt = starterTopic();

  publishDiscoverySensor("battery_soc",     "Leisure battery SoC",     bt.c_str(), "{{ value_json.soc }}",     "%", "battery");
  publishDiscoverySensor("battery_voltage", "Leisure battery voltage", bt.c_str(), "{{ value_json.voltage }}", "V", "voltage");
  publishDiscoverySensor("battery_current", "Leisure battery current", bt.c_str(), "{{ value_json.current }}", "A", "current");
  publishDiscoverySensor("battery_power",   "Leisure battery power",   bt.c_str(), "{{ value_json.power }}",   "W", "power");
  publishDiscoverySensor("cell1_voltage",   "Battery cell 1 voltage",  bt.c_str(), "{{ value_json.cells[0] }}", "V", "voltage");
  publishDiscoverySensor("cell2_voltage",   "Battery cell 2 voltage",  bt.c_str(), "{{ value_json.cells[1] }}", "V", "voltage");
  publishDiscoverySensor("cell3_voltage",   "Battery cell 3 voltage",  bt.c_str(), "{{ value_json.cells[2] }}", "V", "voltage");
  publishDiscoverySensor("cell4_voltage",   "Battery cell 4 voltage",  bt.c_str(), "{{ value_json.cells[3] }}", "V", "voltage");
  publishDiscoverySensor("battery_energy",   "Leisure battery energy remaining", bt.c_str(), "{{ value_json.energy }}",   "Wh", "energy_storage");
  publishDiscoverySensor("battery_quality",  "Leisure battery quality",          bt.c_str(), "{{ value_json.quality }}",  "%",  "");
  publishDiscoverySensor("battery_capacity", "Leisure battery capacity",         bt.c_str(), "{{ value_json.capacity }}", "Ah", "");
  publishDiscoverySensor("solar_voltage",   "Solar charger voltage",   st.c_str(), "{{ value_json.voltage }}", "V", "voltage");
  publishDiscoverySensor("solar_current",   "Solar charge current",    st.c_str(), "{{ value_json.current }}", "A", "current");
  publishDiscoverySensor("starter_voltage", "Starter battery voltage", rt.c_str(), "{{ value_json.voltage }}", "V", "voltage");
  Serial.println(F("[mqtt] discovery published"));
}

bool mqttConnect() {
  if (cfg.host.isEmpty()) return false;
  mqtt.setServer(cfg.host.c_str(), cfg.port);
  mqtt.setBufferSize(512);

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

  // Battery
  if (s.batt_voltage_valid && (now - g_lastBattMs) < NBUS_STALE_MS) {
    JsonDocument doc;
    if (s.batt_soc_valid)     doc["soc"]     = s.batt_soc;
    if (s.batt_voltage_valid) doc["voltage"] = roundf(s.batt_voltage * 100) / 100.0;
    if (s.batt_wh_valid)       doc["energy"]   = s.batt_wh;
    if (s.batt_quality_valid)  doc["quality"]  = s.batt_quality;
    if (s.batt_capacity_valid) doc["capacity"] = s.batt_capacity_ah;
    if (s.batt_current_valid) {
      doc["current"] = roundf(s.batt_current * 100) / 100.0;
      doc["power"]   = roundf(s.batt_voltage * s.batt_current * 10) / 10.0;
    }
    bool anyCell = s.cell_valid[0] || s.cell_valid[1] || s.cell_valid[2] || s.cell_valid[3];
    if (anyCell && (now - g_lastCellMs) < NBUS_STALE_MS) {
      JsonArray cells = doc["cells"].to<JsonArray>();
      for (int i = 0; i < 4; ++i) cells.add(roundf(s.cell_v[i] * 1000) / 1000.0);
    }
    String payload; serializeJson(doc, payload);
    mqtt.publish(batteryTopic().c_str(), payload.c_str(), true);
  }

  // Solar charger
  if (s.solar_valid && (now - g_lastSolarMs) < NBUS_STALE_MS) {
    JsonDocument doc;
    doc["voltage"] = roundf(s.solar_voltage * 100) / 100.0;
    doc["current"] = roundf(s.solar_current * 100) / 100.0;
    String payload; serializeJson(doc, payload);
    mqtt.publish(solarTopic().c_str(), payload.c_str(), true);
  }

  // Starter battery
  if (s.starter_valid && (now - g_lastStarterMs) < NBUS_STALE_MS) {
    JsonDocument doc;
    doc["voltage"] = roundf(s.starter_voltage * 100) / 100.0;
    String payload; serializeJson(doc, payload);
    mqtt.publish(starterTopic().c_str(), payload.c_str(), true);
  }
}

// --------------------------------------------------------------------------
// LIN frame capture (read-only)
// --------------------------------------------------------------------------
void markFresh(uint8_t nad, uint8_t reg) {
  const uint32_t now = millis();
  if (nad == NBUS_NAD_BATTERY) {
    g_lastBattMs = now;
    if (reg == 0x56 || reg == 0x57) g_lastCellMs = now;
  } else if (nad == NBUS_NAD_SOLAR) {
    if (reg == 0x01) g_lastStarterMs = now;
    else             g_lastSolarMs = now;
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

    if (parser.feedResponse(d, 8)) {
      markFresh(d[0], d[3]);
#ifdef NBUS_DEBUG
      Serial.printf("[lin] NAD %02X reg %02X : %02X %02X %02X %02X\n",
                    d[0], d[3], d[4], d[5], d[6], d[7]);
#endif
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
  Serial.println(F("\n[boot] N-Bus ESP32-C3 firmware"));

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
    if (!g_discoverySent) {
      publishDiscovery();
      g_discoverySent = true;
    }
    if (millis() - g_lastPublishMs > NBUS_PUBLISH_MS) {
      g_lastPublishMs = millis();
      publishState();
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
