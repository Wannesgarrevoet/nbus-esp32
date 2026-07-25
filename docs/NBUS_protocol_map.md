# N-Bus (Büttner/Dometic NDS) protocol map

Reverse-engineered from logic-analyzer captures on the RJ12/6P6C bus cable of a
Ford Nugget with a Büttner/Dometic NDS system (2× Tempra TLB150 battery + MPPT solar charger).

## Physical layer

- **Bus**: LIN (single-wire), idle high at battery voltage, dominant = pulled to 0.
- **Baud rate**: 19200 baud, 8N1.
- **Connector**: 6P6C (RJ12).
  - pin 1 = +12 V
  - pin 2 = GND
  - pin 3 = LIN data
  - pin 4 = 2nd data line / wake (mostly inactive)
  - pin 5 = +12 V (straight from the battery, most stable)
  - pin 6 = NC
- Measurement setup: data via a voltage divider (988 Ω + 2×220 Ω → ~3.7 V) to the
  logic analyzer, GND to pin 2.

## Transport layer (LIN-TP / diagnostic)

The master cyclically polls with diagnostic frames:
- **PID 0x3C** = master request
- **PID 0x3D** (sent as 0x7D incl. parity) = slave response

Frame payload (8 data bytes): `NAD  PCI  SID  reg  d0 d1 d2 d3` + checksum.

- **NAD** = node address (0x81 = solar charger, 0x85 = battery, 0x80 = broadcast)
- **PCI** = 0x06 (single frame, 6 bytes follow)
- **SID** = 0xB4 (read request) → response 0xF4 (= 0xB4 + 0x40, positive response)
- **reg** = parameter index; the node rotates through its parameters, 1 per response
- **d0..d3** = value (FF = padding/unused)

## Nodes

| NAD | Device | Serial |
|-----|--------|--------|
| 0x85 | Leisure battery (Tempra TLB150) | KAA… |
| 0x81 | Solar charger (MPPT) | ACD… |

## Register map — NAD 0x85 (battery)

Status is judged against **our own N-Bus captures**: CONFIRMED means we saw it on this
bus. Anything resting on the BLE cross-check alone stays LIKELY until we capture it.

| reg | bytes | meaning | unit | status |
|-----|-------|---------|------|--------|
| 0x02 | `Vh Vl Ih Il` | battery voltage + battery current | V=0.01 V; I=0.01 A, **bit15=1 → discharging**; bit15=0 → charging (LIKELY, BLE) | CONFIRMED (discharge only) |
| 0x07 | `Ah…` | nominal capacity (150 Ah) | Ah | LIKELY (BLE) — verify layout on bus |
| 0x0B | `b0` | State of Charge | % (0x4B = 75%) | CONFIRMED |
| 0x0E | `b0` | "Quality" (SoH-like health/quality) | % | LIKELY (BLE) — verify on bus |
| 0x34 | `H1h H1l H2h H2l` | H1 non-FFFF only while charging, H2 non-FFFF only while discharging; meaning unknown | ? | UNRESOLVED (also open in BLE) |
| 0x36 | `Wh Wl (FF FF)` | remaining energy | Wh, big-endian 16-bit | LIKELY (BLE) — matches app's ~1487/1479 Wh; verify on bus |
| 0x56 | `c1h c1l c2h c2l` | cell voltages, cells 1 & 2 | 0.001 V (~3.3 V), big-endian | LIKELY — values seen on our bus, cell order from BLE |
| 0x57 | `c3h c3l c4h c4l` | cell voltages, cells 3 & 4 | 0.001 V, big-endian | LIKELY — values seen on our bus, cell order from BLE |
| 0x54 | ASCII | serial-number fragment ("KAA") | text | CONFIRMED |

> **Remaining runtime** is *not* a register. The official app (and the BLE project)
> **compute** it from remaining Wh (0x36) ÷ smoothed power (U·I from 0x02), using a
> ~30 s exponential moving average on the power to stop the estimate from jumping.
> We should do the same rather than hunt for a runtime register.

## Register map — NAD 0x81 (solar charger)

| reg | bytes | meaning | unit | status |
|-----|-------|---------|------|--------|
| 0x02 | `Vh Vl Ih Il` | charge voltage + solar charge current | V=0.01 V; I=0.01 A | CONFIRMED |
| 0x01 | `Vh Vl` | starter-battery voltage | 0.01 V | CONFIRMED |
| 0x1B | `Vh Vl` | likely panel/input voltage (fluctuates) | 0.01 V? | UNCERTAIN |
| 0x54 | ASCII | serial-number fragment ("ACD") | text | CONFIRMED |

## Verification (capture 4/5/6 vs app)

| Quantity | cap4 hex → value | app4 | cap5 → value | app5 | cap6 → value | app6 |
|----------|------|------|------|------|------|------|
| Battery V (85.02) | 0532 → 13.30 | 13.3 | 0530 → 13.28 | 13.3 | 0537 → 13.35 | 13.4 |
| Battery I (85.02) | 8265 → −6.13 | −6.2 | 8473 → −11.39 | −11.4 | 8053 → −0.83 | −1.0 |
| SoC (85.0B) | (74) | 75 | 4B → 75 | 75 | 4B → 75 | 75 |
| Solar I (81.02) | 0012 → 0.18 | 0.2 | 0013 → 0.19 | 0.2 | – | 0.2 |
| Starter battery (81.01) | 04F7 → 12.71 | 12.7 | – | 12.7 | 04F5 → 12.69 | 12.7 |

## Still to determine

- Confirm 0x36 (Wh), 0x0E (quality) and 0x07 (capacity) are actually emitted on the
  battery's **N-Bus** node 0x85 (they are confirmed over BLE for the same battery; the
  parameter numbering is shared, but we haven't yet captured them on the wire).
- Meaning of 0x34's H1/H2 fields (still open in the BLE project too).
- Behaviour while charging (positive battery current): the BLE cross-check says bit15=0,
  but we have never captured a charging frame ourselves. Needs a daytime capture.
- Cell *order* within 0x56/0x57 (which physical cell is which) comes from the BLE
  project; the values themselves we do see on our bus.

## Cross-reference: BLE project (MartinusTech)

An independent project reads the **same battery family over BLE** (Telit BlueMod+S50,
service 0xFEFB) instead of the wired N-Bus:
<https://github.com/MartinusTech/ESP32-BLE-Reader-for-Buettner-Dometic-Tempra-TLB150-BMS>.
Its telemetry frames are `23 85 0F <ParamID> d0 d1 d2 d3` — the **same node byte (0x85)
and the same ParamID/data semantics** we see on the N-Bus, which is why 0x02, 0x0B and
the 0x56/0x57 cell layout match byte-for-byte between the two. It additionally decodes
0x0E (quality), 0x36 (Wh) and 0x07 (capacity), and computes remaining runtime as noted
above. It credits this repo for the 0x02 formula.
