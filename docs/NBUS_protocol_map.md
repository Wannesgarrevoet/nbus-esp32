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

### How the bus is tapped

The register map below was originally reverse-engineered with a **logic analyzer**
hanging off pin 3 through a resistive divider (988 Ω + 2×220 Ω → ~3.7 V), with GND on
pin 2. That was a bench measurement only — it is **not** how this firmware reads the bus.

The firmware uses a **LIN transceiver** (TJA1027 preferred, TJA1021 works) which does the
level conversion properly:

- transceiver LIN pin → bus pin 3; VSUP → bus pin 1/5 (12 V); GND → bus pin 2
- transceiver RXD → ESP32-C3 **GPIO20** (UART1 RX, `NBUS_RX_PIN`)
- transceiver TXD **left unconnected** — the firmware never transmits (`NBUS_TX_PIN -1`)
- the C3 runs from its **own USB supply**, not from the bus; both supplies share a ground

No divider is involved on the TJA1027 (its VIO pin sets the RXD level to 3.3 V). See
[`wiring.md`](wiring.md) for the full wiring.

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

The trailing checksum is the LIN **classic** checksum — inverted sum-with-carry over the
8 data bytes, *excluding* the PID. (The enhanced variant, which includes the PID, is
never used on diagnostic IDs; verified on this bus.)

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
| 0x02 | `Vh Vl Ih Il` | battery voltage + battery current | V=0.01 V; I=0.01 A, **bit15=1 → discharging, bit15=0 → charging** | CONFIRMED (both directions) |
| 0x07 | `00 00 Ah Al` | nominal capacity (150 Ah) | Ah, big-endian 16-bit in `d2 d3` | CONFIRMED |
| 0x0B | `b0` | State of Charge | % (0x4B = 75%) | CONFIRMED |
| 0x0E | `b0` | "Quality" (SoH-like health/quality) | % | CONFIRMED |
| 0x34 | `H1h H1l H2h H2l` | H1 = time to full, H2 = time to empty; the inactive one reads FFFF | minutes, big-endian 16-bit | CONFIRMED (both halves) |
| 0x35 | `Ch Cl Dh Dl` | cumulative energy counters: C counts up only while charging, D only while discharging | **1 Wh per count**, big-endian 16-bit | CONFIRMED (direction and unit) |
| 0x36 | `Wh Wl (FF FF)` | remaining energy — the gauge's *estimate*, revised on load changes, not a coulomb integral | Wh, big-endian 16-bit | CONFIRMED |
| 0x56 | `c1h c1l c2h c2l` | cell voltages, cells 1 & 2 | 0.001 V (~3.3 V), big-endian | LIKELY — values seen on our bus, cell order from BLE |
| 0x57 | `c3h c3l c4h c4l` | cell voltages, cells 3 & 4 | 0.001 V, big-endian | LIKELY — values seen on our bus, cell order from BLE |
| 0x54 | `idx` + ASCII | serial-number fragment (index 0x0F, "KAA") | text | CONFIRMED |
| 0x55 | `idx` + `** ** **` | same index byte as 0x54; likely the numeric part of the serial | ? | LIKELY |
| 0x90 | `01 0E 01 0E` | two identical 16-bit values of 270. Read as 2 temperature sensors at 27.0 °C, but both held exactly 270 through a 504 Wh charge at up to 49.7 A — a live sensor would have moved | 0.1 °C? | DOUBTED (looks like a constant) |
| 0x0C | `02 Ex FF FF` | not a constant and not a counter: sits on a multiple of 10, dithers one step either way, and the centre rises with state of charge (740 at 50 %, 780 at 73 %) | ? | UNRESOLVED |
| 0x60 / 0xA0 / 0xA1 | constant | `60 00 01 00` / `00 05 00 08` / `04 02 02 08` — never move; look like model/version/date identity | — | UNRESOLVED |
| 0xC0 / 0xF1 | all zero | never move; candidate alarm/fault bitmaps | — | UNRESOLVED |
| 0xF2 | `00 02 00 00` | constant 2 | — | UNRESOLVED |

> **How 0x34 H2 was confirmed.** Over a 300 s capture (26 samples) H2 ranged 2155–2859
> while remaining-Wh ÷ instantaneous power ranged 2185–2806; the means agree to **1.0 %**.
> Per-sample correlation is only 0.41 because 0x02 and 0x34 arrive at different points in
> the poll rotation, so the current used for the prediction is never quite the one the
> gauge used. The distributions matching that closely with no fitted parameter is what
> makes the reading solid.
>
> This **supersedes** the earlier note here that remaining runtime is not a register. It
> is one. The official app may still compute its own estimate — that claim is untested —
> but the battery publishes the figure itself, and the H1/H2 charge/discharge asymmetry
> already documented is exactly the time-to-full / time-to-empty pairing.

> **The charging capture (300 s, solar active, 3601 frames).** This one capture settled the
> three questions that had been open longest, because it caught the bus swinging from
> +5.5 A charge to −5.7 A discharge and back.
>
> *0x02 bit15.* Previously only ever seen set. Here both states occur with real magnitude,
> and the direction is checked against a register that is not part of the formula:
> **remaining energy 0x36 rises while bit15 is clear (1084 → 1086 Wh) and falls while it is
> set (1080 → 1078 Wh).** Since 0x36 is decoded independently, this is not circular.
> See the caveat on 0x36 below: that witness holds at the multi-amp currents of this
> capture, but 0x36 is an estimate and not a pure integral, so it is not reliable as a
> direction witness at low current.
>
> *0x34.* Exactly one half is populated at any moment, and which half it is tracks the sign
> of 0x02. The arithmetic closes without a fitted parameter:
>
> | half | reg value | remaining Wh | power | prediction | error |
> |------|-----------|--------------|-------|-----------|-------|
> | H2 (discharge) | 918 min | 1079 | 70.2 W | 922 min | 0.4 % |
> | H2 (discharge) | 866 min | 1078 | 74.6 W | 867 min | 0.1 % |
> | H2 (discharge) | 996 min | 1080 | 65.6 W | 988 min | 0.8 % |
>
> H2 is simply `remaining / power`. **H1 is not** — it only closes against the *deficit*,
> `(full − remaining) / power`, which is what makes it time-to-full rather than a second
> estimate of the same thing. Solving four charging samples for the implied full capacity
> gives 1983, 1972, 1839 and 1932 Wh, mean **1932 Wh** — against the nominal 150 Ah × 12.8 V
> = 1920 Wh from register 0x07, a **0.6 % match**. The ±4 % spread is expected: the gauge
> filters its current, we use the instantaneous sample.
>
> *0x35.* Its high half rose 2848 → 2850 and its low half 2609 → 2610, and the two never
> moved at the same time: **the high half incremented only in samples where 0x02 showed
> charge, the low half only where it showed discharge.** That direction split is what fixes
> C = charged, D = discharged. The unit is consistent with 1 Wh/count on both this capture
> and the earlier one (0.9 and 1.07 Wh/count), but both estimates depend on my guessing the
> width of the window from frame ordering — there are no timestamps in the capture yet. The
> unit stays "~1 Wh" until the timestamped ring buffer can measure it properly.
>
> *Three unexplained samples.* In 3 of 53 0x34 frames the same value appears in the other
> half from the one the current sign implies (e.g. `02 B6 FF FF` followed by `FF FF 02 B6`
> at unchanged current). They cluster at direction changes, so the likely cause is the
> snapshot lag described above rather than a decoding error — but it is not proven, and it
> is recorded here rather than smoothed away.

> **The first timestamped capture (339 s, 4096 frames, pulled over the network).** The ring
> buffer records millis() per frame, so energy can finally be integrated against the clock
> instead of guessed from frame ordering.
>
> *0x35 unit.* The discharge counter D incremented twice. Only the second interval is
> usable — the first has an unknown amount of energy already accumulated before the window
> opened — and between those two increments the trapezoidal integral of V·I is
> **1.05 Wh for exactly one count**. With the two order-of-magnitude estimates from the
> untimed captures (0.9 and 1.07), the unit is consistent with exactly **1 Wh/count**. This
> is one clean interval, so it is not yet CONFIRMED; a capture under sustained heavy load
> would produce increments fast enough to settle it outright.
>
> *0x36 is an estimate, not an integral.* Over this capture the battery discharged
> continuously (0.5–5.2 A, never charging) and integrated to 1.57 Wh out, yet 0x36 **rose**
> 1059 → 1063 Wh while SoC fell 55 % → 54 %. The rise coincides with a 5.2 A load dropping
> away and terminal voltage recovering 13.13 → 13.18 V, which is the signature of a gauge
> revising its estimate upward as the load is removed rather than counting coulombs. This
> matters for fault detection: **a rising 0x36 does not prove the battery is charging**, so
> anything watching for the disconnect should trigger on 0x02 and the cell voltages, not on
> remaining energy.

## Register map — NAD 0x81 (solar charger)

| reg | bytes | meaning | unit | status |
|-----|-------|---------|------|--------|
| 0x02 | `Vh Vl Ih Il` | charge voltage + solar charge current | V=0.01 V; I=0.01 A | CONFIRMED |
| 0x01 | `Vh Vl` | starter-battery voltage | 0.01 V | CONFIRMED |
| 0x1B | `Vh Vl (FF FF)` | panel/input voltage | 0.01 V (13.5–24.5 V observed) | LIKELY |
| 0x1C | `xh xl (FF FF)` | flickers 0/1/2/5 with no correlation to charge current | ? | UNRESOLVED |
| 0x35 | `Eh El 00 00` | cumulative energy produced — rose 178 → 183 and **only while the panel was delivering**, mirroring the battery's 0x35 layout | ~1 Wh/count | LIKELY |
| 0x11 | `xh xl (FF FF)` | slow-moving, ~1016–1038. Falls while output is low and rises while it is high, but lags far behind — a filtered or accumulated quantity, not an instantaneous one | ? | UNRESOLVED |
| 0x0B | `b0` | 78 — same layout as the battery's SoC register, but the battery reported 56 % at the same moment | % | UNCERTAIN |
| 0x54 | `idx` + ASCII | serial-number fragment (index 0x00, "ACD") | text | CONFIRMED |
| 0x55 | `00 ** ** **` | same index byte as 0x54; likely the numeric part of the serial | ? | LIKELY |
| 0x26 / 0x60 / 0xA0 / 0xA1 / 0xD0 | constant | `01 00 05 03` / `42 03 09 00` / `01 01 00 05` / `05 04 03 04` / `00 20 00 00` | — | UNRESOLVED |
| 0x0C / 0xC0 / 0xE0 / 0xF0 / 0xF1 | `FF FF FF FF` or all zero | never move; candidate unsupported-parameter and alarm/fault slots | — | UNRESOLVED |

> **Why 0x1B is read as panel voltage.** It looks like noise at first — 187 distinct values
> in 222 samples, swinging 13.5–24.5 V from one frame to the next. But the sequence is not
> random: it repeatedly dips to ~14.2 V and then jumps straight to ~24.2 V, roughly every
> five samples. That sawtooth is an MPPT sweeping its operating point, and the range sits
> exactly between battery voltage and a 12 V panel's open-circuit voltage. The decisive
> test is still to **cover the panel** and check the value collapses.

## Verification (capture 4/5/6 vs app)

| Quantity | cap4 hex → value | app4 | cap5 → value | app5 | cap6 → value | app6 |
|----------|------|------|------|------|------|------|
| Battery V (85.02) | 0532 → 13.30 | 13.3 | 0530 → 13.28 | 13.3 | 0537 → 13.35 | 13.4 |
| Battery I (85.02) | 8265 → −6.13 | −6.2 | 8473 → −11.39 | −11.4 | 8053 → −0.83 | −1.0 |
| SoC (85.0B) | (74) | 75 | 4B → 75 | 75 | 4B → 75 | 75 |
| Solar I (81.02) | 0012 → 0.18 | 0.2 | 0013 → 0.19 | 0.2 | – | 0.2 |
| Starter battery (81.01) | 04F7 → 12.71 | 12.7 | – | 12.7 | 04F5 → 12.69 | 12.7 |

## How to re-derive this

`tools/analyse_raw.py` reads a serial capture containing the firmware's `[raw …]` window
dumps (`NBUS_DEBUG 1`). Unknown registers appear **only** there — the decoded `[lin …]`
lines are emitted after the parser accepts a frame, so a register the parser doesn't know
never reaches them.

```
python tools/analyse_raw.py capture.log            # every register, how much it moves
python tools/analyse_raw.py capture.log 85:34      # one register over time, next to the
                                                   # battery/solar state at that moment
```

The tool re-checks the LIN classic checksum itself and drops anything that fails, so a
noisy tap cannot invent a register. The 300 s capture behind the entries above yielded
1564 frames with **1** checksum reject.

## Still to determine

- Whether 0xC0 / 0xF0 / 0xF1 / 0xF2 really are alarm bitmaps. They are all zero on a
  healthy bus, which is exactly what an alarm register looks like when nothing is wrong —
  and also exactly what an unused register looks like. **A capture taken during a power
  loss would separate the two**, which makes them worth publishing to MQTT even unnamed.
  They stayed zero throughout the 2026-07-26 drive, including engine cranking, so they are
  at least not set by ordinary events.
- **What 0x0C is.** Not a cycle count, and not monotonic — see the drive capture below.
  It sits on a multiple of 10, dithers by one step, and the centre climbs with state of
  charge. That rules out the counter reading it was given the same day, and leaves an
  analogue quantity quantised to 10 units. Correlating its centre against SoC, remaining
  Wh and pack voltage over a full discharge-to-recharge cycle is the next step.
- Solar 0x11's slow monotonic fall, and whether solar 0x0B (78) is the charger's own
  cruder SoC estimate alongside the battery's coulomb-counted 56 %.
- Cell *order* within 0x56/0x57 (which physical cell is which) comes from the BLE
  project; the values themselves we do see on our bus.

> **The drive capture, 2026-07-26 20:45–22:45.** Two hours of driving, recorded in Home
> Assistant rather than the ring: charging peaked at **49.7 A**, SoC went 50 → 73 %, and
> the alternator held the starter battery at up to 14.73 V (11.85 V while cranking).
> Three questions closed and one hypothesis broke.
>
> **0x35 is 1 Wh per count — settled.** Trapezoidal integration of published power over
> the 119-minute window gives **505.6 Wh** of charge against a 0x35 charge-counter delta
> of **+504**, and **−25.4 Wh** of discharge against a discharge-counter delta of **+26**.
> That is 0.3 % on the charge side over 504 counts, which retires the earlier single-
> interval estimate of 1.05 Wh.
>
> **The 0x34 H1 full-capacity figure is ~2033 Wh**, mean of 86 samples with a standard
> deviation of 24 Wh (1.2 %), taken across remaining energies from 988 to 1490 Wh. That
> it stays flat across that range is the real result: the gauge is dividing by a fixed
> figure, not a sliding one. The figure is **6 % above the 1920 Wh nominal**, and it
> inherits whatever bias 0x36 carries, since 0x36 is the numerator and is itself an
> estimate.
>
> **0x90 did not move — evidence against the temperature reading.** Both fields held
> `01 0E` exactly, through a 504 Wh charge at up to 49.7 A, for two hours. A charge that
> size puts on the order of 15 Wh of loss into the pack, which should be a few degrees
> even across a 150 Ah thermal mass, and the field resolves to 0.1 °C. A real temperature
> sensor would have twitched. Treat 27.0 °C as a configured constant until something
> makes it move.
>
> **0x0C reverses.** It stepped 740 → 750 → 760 → 770 → 780 over the drive, but between
> every step it fell back one step and climbed again, and it was still oscillating
> 770 ↔ 780 half an hour after the engine stopped and the charge counter had frozen. It
> is therefore not a counter of anything. Recorded here as a caution: it was written up
> as a monotonic counter earlier the same day on the strength of two readings, 730 and
> 740, taken hours apart. Two samples of a dithering value look exactly like a counter.
- Solar 0x11's slow monotonic fall, and whether solar 0x0B (78) is the charger's own
  cruder SoC estimate alongside the battery's coulomb-counted 56 %.
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
