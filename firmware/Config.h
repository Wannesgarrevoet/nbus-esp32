// Config.h — pin definitions, topic prefixes and tunable constants.
//
// Target board: ESP32-C3 Super Mini (FQBN esp32:esp32:esp32c3).
// Native USB-CDC: build with "USB CDC On Boot" enabled so logs come over USB-C.

#ifndef NBUS_CONFIG_H
#define NBUS_CONFIG_H

// ---------------------------------------------------------------------------
// Firmware identity. Reported by /status, which exists so that after a network
// update there is a way to tell which image actually ended up running. Without
// it an OTA that silently fails over to the other partition looks identical to
// one that succeeded. Bump NBUS_FW_VERSION whenever flashing something you may
// later need to distinguish.
// ---------------------------------------------------------------------------
#define NBUS_FW_VERSION  "0.6.1"
#define NBUS_FW_BUILD    __DATE__ " " __TIME__

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
// BOOT button: hold at power-on to erase settings. GPIO9 is also the C3 boot
// strapping pin, which the USB host drives via DTR over USB-Serial-JTAG — so a
// terminal opening the port can pull it low. The erase is therefore gated on a
// power-on reset plus a sustained hold; a DTR pulse satisfies neither.
#define NBUS_SETUP_BTN_PIN 9
#define NBUS_FACTORY_HOLD_MS 3000

// ---------------------------------------------------------------------------
// Wi-Fi provisioning portal
// ---------------------------------------------------------------------------
#define NBUS_AP_NAME    "NBus-Setup"
#define NBUS_HOSTNAME   "nbus-camper"   // shown in the router's client list
#define NBUS_AP_TIMEOUT_S  180  // portal idle timeout before reboot/retry
// Cap on the saved-AP connect attempt, the only blocking step left in setup().
#define NBUS_WIFI_CONNECT_TIMEOUT_S 15

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

// Mirror of registers the parser does NOT decode, republished raw so Home Assistant's
// recorder timestamps every change. This is the main instrument for decoding the rest of
// the bus: the ring buffer holds minutes, the recorder holds weeks, and the slow registers
// only give themselves away over a charge cycle or a drive. It also means a candidate
// alarm register — all-zero and so indistinguishable from an unused one on a healthy
// bus — leaves a trace on the day it flips.
// Keyed on (device, reg), where the two battery packs count as separate devices — they
// share NAD 0x85, so keying on the NAD alone would have them overwrite each other's
// registers. Two packs of ~24 registers plus ~18 for the charger is ~66 in the captures.
#define NBUS_REG_MIRROR_SLOTS  96      // distinct (device, reg) pairs tracked
#define NBUS_REG_MIRROR_MS     10000   // per-register floor between republishes

// Reconnect backoff and heartbeat.
#define NBUS_MQTT_RETRY_MS     5000
#define NBUS_HEARTBEAT_MS      1000

// Wi-Fi loss recovery. There is no path back from a dropped association on its own:
// the MQTT retry that ends in a reboot is gated behind WL_CONNECTED, so a device that
// loses the AP would sit reading the bus and publishing nowhere, indefinitely. On
// 2026-08-05 it did exactly that for over eight hours.
#define NBUS_WIFI_GRACE_MS     60000    // disconnected this long ⇒ force WiFi.reconnect()
#define NBUS_WIFI_RETRY_MS     30000    // interval between those forced attempts
#define NBUS_WIFI_REBOOT_MS    600000   // disconnected this long ⇒ reboot
// Same failure seen from the other side: if the saved AP is merely down at boot — a router
// that is slower to come up after a shared power cut — WiFiManager opens the setup portal
// and stays there, because it only leaves on a submitted form. Reboot instead and retry the
// saved credentials. Only when credentials exist: a genuinely unprovisioned device must
// keep its portal up for as long as the user needs it.
#define NBUS_PORTAL_REBOOT_MS  300000

// Bridge diagnostics (uptime, RSSI, free heap). Without these a reboot is invisible after
// the fact, and telling "lost power" from "lost Wi-Fi" means guessing.
#define NBUS_DIAG_MS           30000

// Watchdog timeout (seconds).
#define NBUS_WDT_TIMEOUT_S     30

// ---------------------------------------------------------------------------
// Raw frame ring buffer.
//
// Raw frames are what the protocol work runs on: a capture pulled over the network
// replays off-device through the same parser the firmware uses. Frames go into a RAM
// ring continuously and are written to flash only once the bus stops talking — writing
// every frame would destroy the flash in days. Flushing on silence also means the
// minutes leading up to a bus dropout survive it.
//
// 12 bytes per entry (4-byte millis + the 8 payload bytes; the checksum is
// already verified by then and carries no information worth storing).
// 4096 entries = 48 KB of RAM and, at the ~12 frames/s this bus actually runs at,
// about 5.5 minutes of history — long enough to hold several full poll cycles of every
// register on the bus, which is what a capture has to contain to be worth analysing.
// ---------------------------------------------------------------------------
#define NBUS_RAW_RING_ENTRIES  4096
#define NBUS_RAW_SILENCE_MS    5000    // no valid frame this long ⇒ bus considered dead
#define NBUS_RAW_MAX_DUMPS     8       // keep at most this many dump files, oldest pruned
#define NBUS_RAW_DUMP_DIR      "/dumps"

#endif  // NBUS_CONFIG_H
