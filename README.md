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
- **Handles more than one battery pack.** Packs share NAD 0x85, so they cannot be told
  apart by address; the frames are attributed by their **position in the poll cycle**
  instead, and each pack appears in Home Assistant as its own device keyed on its
  **serial number** — see [multiple packs](#multiple-battery-packs) below
- Also decodes remaining energy (Wh), "quality" (SoH-like %) and nominal capacity (Ah) —
  these three come from a cross-check against a BLE reader for the same battery and are
  **not yet verified on our own bus**
- Reads out device identity: serial number, model, bus address and firmware version per
  node, all cross-checked against the Dometic Power app
- Solar charger: charge stage (off / bulk / absorption / float), panel voltage and starter-
  battery voltage alongside output voltage and current
- Home Assistant MQTT auto-discovery (sensors appear automatically)
- Republishes registers it cannot decode as raw hex — `<base>/reg/85_<N>/<REG>` per pack,
  `<base>/reg/81/<REG>` for the charger — and gives a hand-picked few of them their own
  entities named after the register address, so a value that only matters the day it
  changes ends up in the recorder
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

There is a second harness that replays a **real bus capture** through the same parser and
cycle tracker the firmware runs, and asserts that each poll slot carries exactly one serial
number. Hand-built test vectors cannot catch a mis-split, because they encode the same
assumption the code does; the replay is what tells you whether the split holds on the bus
you actually have.

```bash
g++ -std=c++17 -I firmware test/replay_dump.cpp firmware/NBusParser.cpp -o /tmp/r
/tmp/r capture.txt          # a file fetched from /raw/file?n=<name>
```

## First-time setup

1. Flash the firmware over USB once.
2. The ESP32 starts a Wi-Fi hotspot `NBus-Setup`. Connect with your phone.
3. Enter your Wi-Fi and MQTT broker details in the captive portal.
4. The device reboots, connects, and Home Assistant discovers the sensors.

After that, update wirelessly at `http://<device-ip>/update`.

No YAML is needed for any of the sensors, including the raw register ones. One detail is
worth knowing before you tune `recorder.purge_keep_days`: a register published as a hex
string gets no long-term statistics, so it lives exactly as long as the retention window,
while a numeric one carries `state_class: measurement` and keeps an hourly min/max
indefinitely. Every raw-hex register therefore also appears as a `… value` entity holding
the same four bytes as one 32-bit number. It is unitless — a bit pattern, not a quantity —
but it means a short retention costs only the fine detail, and a bit that flips for two
seconds survives in that hour's maximum long after the states themselves are purged.

To wipe the stored Wi-Fi and MQTT settings, hold the **BOOT** button while applying
power and keep holding it for 3 seconds — the LED blinks while counting, then flashes
six times to confirm. Releasing early keeps the settings. A reset triggered over USB
never erases anything, so opening a serial monitor is always safe.

## Multiple battery packs

Every pack on the bus answers to the **same NAD (0x85)**. There is no address in the
frame that distinguishes them, so a decoder written for one pack silently averages two.

This firmware splits them by **ordinal position in the poll cycle**. The master polls the
nodes in a fixed rotation; the charger's NAD 0x81 is unique, so its frames delimit the
cycle, and the battery answers between two charger frames are pack 1, pack 2, … in order.
Runs whose length is not a whole number of cycles are **discarded rather than attributed** —
losing one sample in thousands costs nothing, while misattributing one quietly corrupts a
register table. Splitting on inter-frame timing was tried first and abandoned: it leaked
frames between packs, and it broke outright when a firmware update reshuffled the poll order.

A run that is *longer* than one cycle is kept, not dropped. Those runs turned out to be
cycles merged by a missing charger frame, with both battery answers intact: across four
captures every run was an even multiple of the two-pack cycle, and inside the merged runs
every frame carrying a per-pack-fixed register landed on the right pack. Requiring an exact
match was discarding about one cycle in eight for nothing — and not evenly. The charger only
drops poll answers while it is actually charging: hourly captures put the merge rate at
11–16 % with the charger running and 0.0 % with it off. The old rule therefore took all of
its losses from the hours that have something to show, and none from the flat nights.

The cycle length is learned and re-learned, so a pack can be switched on or off while the
device is running. When the length changes, everything held per slot is discarded — slot 1
before the change and slot 1 after it are not the same pack.

Each pack becomes its own Home Assistant device, identified by its **serial number**,
because a firmware update was observed to change the bus addresses while leaving the
serials alone. `<base>/cycle` publishes the learned cycle length together with the accepted
and dropped frame counts, so the split can be audited from the outside.

> **Breaking change.** Multi-pack support moved the MQTT topics:
> `<base>/battery` → `<base>/battery/<N>`, and the raw register mirror `<base>/reg/85/<REG>`
> → `<base>/reg/85_<N>/<REG>`. Home Assistant entity IDs now include the pack serial. If
> you are upgrading and want to keep recorder history, rename the old entity IDs onto the
> new entities before restarting, or the history is orphaned rather than lost.

## Status

Reverse-engineering is functional for the core values (SoC, V, I, solar, starter),
for the energy counters and the time-to-full / time-to-empty estimates, and for device
identity (serial, model, address, firmware version). Still open: the exact cell mapping,
the candidate alarm registers — which read all zero on a healthy bus and so cannot be
told apart from unused ones until something goes wrong — and registers 0x0C, 0x14 and
0x90. Contributions welcome; `docs/NBUS_protocol_map.md` marks every entry with how
strongly it is actually evidenced, including the ones that were wrong.

## Credits & related work

[**ESP32-BLE-Reader-for-Buettner-Dometic-Tempra-TLB150-BMS**](https://github.com/MartinusTech/ESP32-BLE-Reader-for-Buettner-Dometic-Tempra-TLB150-BMS)
by **MartinusTech** reads the same battery over its **BLE** interface rather than the
wired N-Bus. Its telemetry frames carry the same node byte and the same parameter
numbering, so the two protocols decode identically.

That project's research resolved several things this one had open: the remaining-energy
(0x36), quality (0x0E) and capacity (0x07) registers, the cell ordering behind 0x56/0x57,
and the insight that **remaining runtime is computed, not transmitted** — the app derives
it from remaining Wh divided by smoothed power. Credit for that work goes to MartinusTech.
The register statuses in [`docs/NBUS_protocol_map.md`](docs/NBUS_protocol_map.md) mark
which of those we have since seen on our own bus and which we have not.

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
