# nbus-esp32

Read a **Büttner / Dometic NDS "N-Bus"** camper energy system into **MQTT** with an
ESP32, so it can be consumed by Home Assistant, Node-RED, openHAB, or anything else
that speaks MQTT.

The N-Bus is a **LIN bus (19200 baud)** carrying a proprietary NDS diagnostic
protocol. This firmware listens **passively** and decodes battery, solar-charger and
starter-battery data. It was reverse-engineered on a Ford Nugget with 2× Tempra
TLB150 batteries and an MPPT solar charger. See [`docs/NBUS_protocol_map.md`](docs/NBUS_protocol_map.md).

> ⚠️ **Passive / read-only.** This device only listens. It must never transmit on the
> bus — writing could disrupt communication between the battery BMS and the charger.
> See the wiring notes for how to keep the transceiver in listen-only mode.

## Features

- Decodes LIN-TP frames from the NDS bus (node 0x85 = battery, 0x81 = solar charger)
- Publishes SoC, battery voltage/current, cell voltages, solar voltage/current,
  starter-battery voltage over MQTT
- Home Assistant MQTT auto-discovery (sensors appear automatically)
- Wi-Fi + MQTT setup via **WiFiManager** captive portal (no credentials in the repo)
- **Over-the-air updates** via a browser (ElegantOTA at `http://<device-ip>/update`)
- Reusable, platform-independent parser (`NBusParser`) with host-side unit tests

## Hardware

- **ESP32-C3 Super Mini** (other ESP32 boards work too; pins differ — see `docs/wiring.md`)
- **TJA1021 or TJA1027** LIN transceiver breakout (TJA1027 preferred: it has a VIO
  pin for native 3.3 V logic)
- A **separate USB power supply** for the ESP32 (charger, power bank or laptop) — the
  board is *not* powered from the bus; only the transceiver takes the bus 12 V. The two
  supplies must share a common ground.
- 6P6C (RJ12) connector / pigtail for the N-Bus

> ⚠️ **Do not power the ESP32 through a buck converter fed from the bus.** The C3 has
> only a linear LDO on board, and a converter's inrush current can trip the battery
> BMS's short-circuit protection — taking the whole 12 V circuit down. Never feed 12 V
> to a board pin either.

See [`docs/wiring.md`](docs/wiring.md) for the full wiring and the RJ12 pinout.

## Build & flash

This project uses the Arduino toolchain. You can build with the Arduino IDE or
`arduino-cli` (recommended for automation):

```bash
arduino-cli core install esp32:esp32
# Enable USB CDC On Boot so logs appear over the USB-C port:
arduino-cli compile --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc firmware
arduino-cli upload  --fqbn esp32:esp32:esp32c3:CDCOnBoot=cdc -p <PORT> firmware
```

Required libraries (install via `arduino-cli lib install "<name>"`):

- `WiFiManager` (tzapu) — captive-portal provisioning
- `PubSubClient` (knolleary) — MQTT client
- `ElegantOTA` (ayushsharma82) — browser OTA
- `ArduinoJson` (bblanchon) — MQTT JSON payloads & discovery configs

Host-side parser tests build with a plain C++17 compiler:

```bash
g++ -std=c++17 -I firmware test/test_parser.cpp firmware/NBusParser.cpp -o /tmp/t && /tmp/t
```

## First-time setup

1. Flash the firmware over USB once.
2. The ESP32 starts a Wi-Fi hotspot `NBus-Setup`. Connect with your phone.
3. Enter your Wi-Fi and MQTT broker details in the captive portal.
4. The device reboots, connects, and Home Assistant discovers the sensors.

After that, update wirelessly at `http://<device-ip>/update`.

To wipe the stored Wi-Fi and MQTT settings, hold the **BOOT** button while applying
power and keep holding it for 3 seconds — the LED blinks while counting, then flashes
six times to confirm. Releasing early keeps the settings. A reset triggered over USB
never erases anything, so opening a serial monitor is always safe.

## Status

Reverse-engineering is functional for the core values (SoC, V, I, solar, starter).
A few registers (remaining Wh, runtime estimate, exact cell mapping) are still being
mapped — contributions welcome.

## Disclaimer

This is an independent, community reverse-engineering project. It is not affiliated
with, endorsed by, or supported by Dometic, Büttner Elektronik, NDS Energy, or Tempra.
All product and company names are trademarks of their respective owners.

This project exists purely for interoperability: to let owners of such a system read
their own telemetry without depending on the official app or display panel. No
proprietary code, binaries, or assets are included or distributed here — everything in
this repository is original code based on observed protocol behaviour.

This software is provided "AS IS", without warranty of any kind. It is strictly
read-only: the firmware never transmits on the bus, and the transceiver's TX path is
left unconnected in hardware as well. Even so, you are tapping into the LIN bus that a
lithium battery BMS, a solar charger and other vehicle electronics use to talk to each
other, and mis-wiring it can disturb that communication or damage hardware. Use at your
own risk. The author is not responsible for any damage to hardware, batteries, vehicles,
or other systems. See [`LICENSE`](LICENSE) for the full license text.

## License

MIT — see [`LICENSE`](LICENSE).
