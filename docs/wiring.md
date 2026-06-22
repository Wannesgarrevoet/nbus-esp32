# Wiring

> ⚠️ **Read-only.** Wire the transceiver so the ESP32 only *receives*. Do not drive
> the bus. Leave the ESP32 TX pin disconnected from the transceiver TXD (or keep TXD
> idle/recessive). The NDS master already provides the bus pull-up; **do not add a
> 1 kΩ master pull-up** — if your breakout has a master/slave jumper, select **slave**.

## N-Bus connector (6P6C / RJ12)

Pin numbering: clip down, gold contacts toward you → pin 1 left, pin 6 right.

| Pin | Function | Use |
|-----|----------|-----|
| 1 | +12 V | → buck converter input (with pin 5) |
| 2 | GND | common ground |
| 3 | LIN data | → transceiver LIN pin |
| 4 | 2nd data / wake (usually idle) | leave unconnected |
| 5 | +12 V (cleanest) | → buck converter input |
| 6 | NC | — |

## Power

```
N-Bus pin 1/5 (+12 V) ──► 12 V→5 V buck ──► ESP32 5V (VIN)
N-Bus pin 2 (GND) ─────────────────────────► common GND (ESP32 + transceiver + buck)
```

## TJA1021 / TJA1027 LIN transceiver

Breakouts vary — **verify pin names against your board's silkscreen and the datasheet**.
Functional connections:

| Transceiver | Connect to |
|-------------|-----------|
| VSUP / VBAT (12 V supply) | N-Bus +12 V (pin 1/5) |
| GND | common GND (pin 2) |
| LIN | N-Bus data (pin 3) |
| RXD (data out to MCU) | ESP32 UART2 RX (GPIO16) |
| TXD (data in from MCU) | **leave unconnected** (read-only) |
| EN / enable | tie to logic-high so the transceiver is active (or an ESP32 GPIO held high) |
| VIO (TJA1027 only) | ESP32 **3.3 V** — sets RXD logic level to 3.3 V |

Notes:
- **TJA1027 is preferred**: its VIO pin makes RXD natively 3.3 V-safe. Tie VIO to the
  ESP32 3.3 V rail.
- **TJA1021 has no VIO.** Verify the RXD output high level before wiring it straight to
  a GPIO; if it can exceed 3.3 V, add a level shifter or a divider on RXD.
- If the breakout exposes a sleep/NSLP pin, hold it in normal (awake) mode.

## Quick sanity check before powering the ESP32

- Buck output measures ~5.0 V.
- With the bus active, RXD shows ~3.3 V idle high with brief dips (the LIN traffic).
- Never connect N-Bus +12 V (pin 1/5) directly to any ESP32 or transceiver logic pin.
