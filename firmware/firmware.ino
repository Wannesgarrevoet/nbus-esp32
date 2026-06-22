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
#include "esp_task_wdt.h"

#include "Config.h"
#include "NBusParser.h"

// Uncomment to print decoded frames over USB serial for validation.
// #define NBUS_DEBUG 1

// --------------------------------------------------------------------------
// Globals
// --------------------------------------------------------------------------
HardwareSerial   LinSerial(NBUS_UART_NUM);
NBusParser       parser;
Preferences      prefs;
WiFiClient       wifiClient;
PubSubClient     mqtt(wifiClient);
WebServer        httpServer(80);

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
void onSaveConfig() { g_shouldSaveConfig = true; }

void maybeFactoryReset() {
  // Hold the BOOT button at power-on to wipe Wi-Fi + MQTT settings.
  pinMode(NBUS_SETUP_BTN_PIN, INPUT_PULLUP);
  delay(50);
  if (digitalRead(NBUS_SETUP_BTN_PIN) == LOW) {
    Serial.println(F("[setup] BOOT held — erasing settings"));
    WiFiManager wm;
    wm.resetSettings();
    prefs.begin("nbus", false);
    prefs.clear();
    prefs.end();
    for (int i = 0; i < 6; ++i) { ledWrite(true); delay(80); ledWrite(false); delay(80); }
  }
}

void startProvisioning() {
  WiFiManager wm;
  wm.setSaveConfigCallback(onSaveConfig);
  wm.setConfigPortalTimeout(NBUS_AP_TIMEOUT_S);

  char portBuf[8];
  snprintf(portBuf, sizeof portBuf, "%u", cfg.port);

  WiFiManagerParameter pHost("host", "MQTT host", cfg.host.c_str(), 64);
  WiFiManagerParameter pPort("port", "MQTT port", portBuf, 8);
  WiFiManagerParameter pUser("user", "MQTT user", cfg.user.c_str(), 48);
  WiFiManagerParameter pPass("pass", "MQTT password", cfg.pass.c_str(), 48);
  WiFiManagerParameter pBase("base", "Base topic", cfg.base.c_str(), 32);
  WiFiManagerParameter pOtaU("otau", "OTA user (optional)", cfg.otaUser.c_str(), 32);
  WiFiManagerParameter pOtaP("otap", "OTA password (optional)", cfg.otaPass.c_str(), 32);
  wm.addParameter(&pHost);
  wm.addParameter(&pPort);
  wm.addParameter(&pUser);
  wm.addParameter(&pPass);
  wm.addParameter(&pBase);
  wm.addParameter(&pOtaU);
  wm.addParameter(&pOtaP);

  if (!wm.autoConnect(NBUS_AP_NAME)) {
    Serial.println(F("[wifi] portal timed out — rebooting"));
    delay(1000);
    ESP.restart();
  }

  if (g_shouldSaveConfig) {
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
  // A frame window (delimited by an idle gap) holds: [break 0x00?] 0x55 PID data... checksum.
  for (size_t i = 0; i + 1 < len; ++i) {
    if (buf[i] != 0x55) continue;
    const uint8_t pid = buf[i + 1];
    if (pid != 0x7D) return;          // not a slave response; ignore this window
    const size_t dStart = i + 2;
    if (len - dStart < 8) return;     // not enough data bytes yet
    const uint8_t* d = &buf[dStart];

    // Optional checksum verification (enhanced checksum over PID + 8 data bytes).
    if (len - dStart >= 9) {
      uint8_t expect = NBusParser::enhancedChecksum(pid, d, 8);
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

  // LIN UART: RX-only. TX pin is -1 — the bus is never driven.
  LinSerial.begin(NBUS_BAUD, SERIAL_8N1, NBUS_RX_PIN, NBUS_TX_PIN);
  Serial.printf("[lin] UART%d RX=GPIO%d @ %d 8N1 (read-only)\n",
                NBUS_UART_NUM, NBUS_RX_PIN, NBUS_BAUD);

  startProvisioning();

  // OTA web server.
  if (!cfg.otaUser.isEmpty()) {
    ElegantOTA.setAuth(cfg.otaUser.c_str(), cfg.otaPass.c_str());
  }
  httpServer.on("/", []() {
    httpServer.send(200, "text/plain", "N-Bus camper bridge. OTA at /update");
  });
  ElegantOTA.begin(&httpServer);
  httpServer.begin();
  Serial.println(F("[ota] web server on :80 (/update)"));

  mqttConnect();
  watchdogInit();
}

void loop() {
  esp_task_wdt_reset();

  readLin();
  httpServer.handleClient();
  ElegantOTA.loop();

  // MQTT keepalive + non-blocking reconnect.
  if (mqtt.connected()) {
    mqtt.loop();
  } else if (!cfg.host.isEmpty() && (millis() - g_lastMqttTryMs) > NBUS_MQTT_RETRY_MS) {
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
