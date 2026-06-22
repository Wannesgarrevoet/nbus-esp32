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

## Board: ESP32-C3 Super Mini

This build targets an **ESP32-C3 Super Mini**. It has **no on-board buck** — only a
linear LDO (5 V→3.3 V). Both its `5V` and `3V3` pins feed that LDO, which tolerates only
a few volts above 5 V (≈6 V max).

> ⚠️ **Never put 12 V on any board pin.** The N-Bus 12 V must pass through the buck first.
> 12 V on the C3's `5V` or `3V3` pin destroys the LDO instantly.

The C3 has only two UARTs (UART0 + UART1) and no fixed "UART2/GPIO16". Because the C3's
UART pins are routed through the GPIO matrix, you can map the LIN RX to any free GPIO.
Avoid the strapping pins **GPIO2, GPIO8, GPIO9** (GPIO8 also drives the on-board LED) so
the idle-high LIN line can't disturb boot.

## Power

```
N-Bus pin 1/5 (+12 V) ──► 12 V→5 V buck ──► C3 "5V" pin (feeds on-board LDO → 3.3 V)
N-Bus pin 2 (GND) ─────────────────────────► common GND (C3 + transceiver + buck)
```

The 12 V from the bus is fine *as a source*; it just must pass through the buck. The
transceiver's 12 V supply (VSUP/VBAT) is the **only** thing that takes the bus 12 V
directly — that chip is rated for it. All logic pins stay at 3.3 V.

## TJA1021 / TJA1027 LIN transceiver

Breakouts vary — **verify pin names against your board's silkscreen and the datasheet**.
Functional connections (ESP32-C3 Super Mini):

| Transceiver | Connect to (C3 Super Mini) |
|-------------|-----------|
| VSUP / VBAT (12 V supply) | N-Bus +12 V (pin 1/5) — directly |
| GND | common GND (pin 2) |
| LIN | N-Bus data (pin 3) |
| RXD (data out to MCU) | C3 **GPIO20** (UART1 RX) — any free non-strapping GPIO |
| TXD (data in from MCU) | **leave unconnected** (read-only) |
| EN / enable | C3 **3V3** pin (or a C3 GPIO held high) so the transceiver is active |
| VIO (TJA1027 only) | C3 **3V3** pin — sets RXD logic level to 3.3 V |

The chosen RX GPIO must match `NBUS_RX_PIN` in `firmware/Config.h`.

Notes:
- **TJA1027 is preferred**: its VIO pin makes RXD natively 3.3 V-safe. Tie VIO to the
  C3 **3V3** pin.
- **TJA1021 has no VIO.** Verify the RXD output high level before wiring it straight to
  a GPIO; if it can exceed 3.3 V, add a level shifter or a divider on RXD. The C3 is
  **not** 5 V-tolerant on its GPIOs.
- The C3's `3V3` pin (the LDO output) easily supplies the transceiver's VIO/EN draw
  (sub-mA).
- If the breakout exposes a sleep/NSLP pin, hold it in normal (awake) mode.

## Quick sanity check before powering the C3

- Buck output measures ~5.0 V (and stays under 6 V — never 12 V on the `5V` pin).
- With the bus active, RXD shows ~3.3 V idle high with brief dips (the LIN traffic).
- Never connect N-Bus +12 V (pin 1/5) directly to any C3 or transceiver logic pin.
