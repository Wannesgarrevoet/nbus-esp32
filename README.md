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
- 12 V → 5 V buck converter to power the board from the bus (the C3 has only a linear
  LDO on board — **never feed it 12 V directly**)
- 6P6C (RJ12) connector / pigtail for the N-Bus

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


## First-time setup

1. Flash the firmware over USB once.
2. The ESP32 starts a Wi-Fi hotspot `NBus-Setup`. Connect with your phone.
3. Enter your Wi-Fi and MQTT broker details in the captive portal.
4. The device reboots, connects, and Home Assistant discovers the sensors.

After that, update wirelessly at `http://<device-ip>/update`.

## Status

Reverse-engineering is functional for the core values (SoC, V, I, solar, starter).
A few registers (remaining Wh, runtime estimate, exact cell mapping) are still being
mapped — contributions welcome.

## License

MIT — see [`LICENSE`](LICENSE).
