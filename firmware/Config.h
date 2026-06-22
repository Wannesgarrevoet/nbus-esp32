// Config.h — pin definitions, topic prefixes and tunable constants.
//
// Target board: ESP32-C3 Super Mini (FQBN esp32:esp32:esp32c3).
// Native USB-CDC: build with "USB CDC On Boot" enabled so logs come over USB-C.

#ifndef NBUS_CONFIG_H
#define NBUS_CONFIG_H

// ---------------------------------------------------------------------------
// LIN bus (read-only). RX on UART1 / GPIO20. TX is NEVER assigned (-1): driving
// the bus is a hard safety violation. The C3 has no UART2/GPIO16.
// Avoid strapping pins GPIO2/GPIO8/GPIO9 for the LIN line (idle-high bus must not
// disturb boot); GPIO20 is safe.
// ---------------------------------------------------------------------------
#define NBUS_RX_PIN     20
#define NBUS_TX_PIN     -1      // do not assign — read-only
#define NBUS_UART_NUM   1       // Serial1
#define NBUS_BAUD       19200   // 8N1

// ---------------------------------------------------------------------------
// On-board peripherals (ESP32-C3 Super Mini)
// ---------------------------------------------------------------------------
#define NBUS_LED_PIN     8      // on-board LED (active-low), used for heartbeat
#define NBUS_LED_ACTIVE_LOW 1
#define NBUS_SETUP_BTN_PIN 9    // BOOT button: hold at power-on to erase settings

// ---------------------------------------------------------------------------
// Wi-Fi provisioning portal
// ---------------------------------------------------------------------------
#define NBUS_AP_NAME    "NBus-Setup"
#define NBUS_AP_TIMEOUT_S  180  // portal idle timeout before reboot/retry

// ---------------------------------------------------------------------------
// MQTT defaults (overridable in the WiFiManager portal, persisted to NVS)
// ---------------------------------------------------------------------------
#define NBUS_DEFAULT_MQTT_HOST   ""
#define NBUS_DEFAULT_MQTT_PORT   "1883"
#define NBUS_DEFAULT_MQTT_USER   ""
#define NBUS_DEFAULT_MQTT_PASS   ""
#define NBUS_DEFAULT_BASE_TOPIC  "nbus"

// Home Assistant discovery prefix and device identity.
#define NBUS_HA_PREFIX   "homeassistant"
#define NBUS_DEVICE_ID   "nbus_camper"
#define NBUS_DEVICE_NAME "NBus Camper"
#define NBUS_DEVICE_MF   "Buttner/Dometic"
#define NBUS_DEVICE_MDL  "NDS N-Bus"

// ---------------------------------------------------------------------------
// Timing / behaviour
// ---------------------------------------------------------------------------
// Idle gap (microseconds) that marks a LIN frame boundary. One char at 19200 8N1
// is ~520 us; >1.5 ms reliably separates frames.
#define NBUS_FRAME_GAP_US      1500
// Max bytes buffered for one frame window before forced flush.
#define NBUS_RX_BUF            64

// MQTT publish throttle and staleness.
#define NBUS_PUBLISH_MS        5000    // min interval between state publishes
#define NBUS_STALE_MS          30000   // mark a value stale if not refreshed within

// Reconnect backoff and heartbeat.
#define NBUS_MQTT_RETRY_MS     5000
#define NBUS_HEARTBEAT_MS      1000

// Watchdog timeout (seconds).
#define NBUS_WDT_TIMEOUT_S     30

#endif  // NBUS_CONFIG_H
