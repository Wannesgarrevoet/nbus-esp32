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

**The NAD identifies a device *type*, not a device.** Both battery packs answer on 0x85.
Everything below about telling them apart follows from that one fact.

| NAD | Device | Serial | Bus address (0x60) | Firmware (0xA1) |
|-----|--------|--------|--------------------|-----------------|
| 0x85 | Leisure battery 1 (Tempra TLB150) | KAA2****53 | 1 | 5.1 (was 4.2) |
| 0x85 | Leisure battery 2 (Tempra TLB150) | KAA2****95 | 11 (was 3) | 5.1 (was 4.0) |
| 0x81 | Solar charger (MPPT) | ACD2****99 | 13 (was 9) | 5.4 |

## Telling the two battery packs apart

This is the part that took the longest to get right, and the part most likely to be
reused, so the failed approaches are recorded alongside the working one.

**What does not work: timing.** The master polls the devices at stable offsets, so
"a 0x85 frame arriving 50 ms after the 0x81 frame is pack A, one arriving 150 ms after it
is pack B" looks convincing and produces a clean-looking split. It is wrong twice over:

- A battery firmware update moved the slots outright. The cycle went from ~200 ms to
  ~240 ms, the poll order swapped, and the hard-coded thresholds put **every frame of one
  pack into an empty group** — a whole device silently vanished from the dashboard.
- Even re-tuned to the new timing, roughly **9 % of frames** landed on the wrong side of
  the threshold whenever a pack answered late. A register table that is 91 % right is
  worse than useless: nothing tells you which 9 % is wrong.

**What works: ordinal position.** The master interrogates the devices in a fixed order,
and the charger's 0x81 frame marks where one cycle ends and the next begins. The *n*-th
0x85 response after an 0x81 frame is always the same pack. There is no grey zone.

The one failure mode left is a lost frame: if a 0x85 frame goes missing, every position
after the gap shifts onto the wrong pack. So **a run whose length is not a whole number of
cycles is discarded rather than attributed.** Misattributing one frame silently corrupts a
register table and nothing downstream would flag it.

**Merged runs are not lost frames, and throwing them away was costing a sixth of the data.**
The first version of this rule required the run length to *equal* the cycle length, which
also discarded every run where an 0x81 frame had gone missing and two cycles had merged.
That was about 12 % of all cycles, every day. Two measurements say those runs are intact:

- Across four two-pack captures — roughly 4900 runs — **every run was an even multiple of
  the two-pack cycle**: 2, 4, 6 or 8, never 3, 5 or 7. The single odd run in the whole set
  was the truncated first run of a ring buffer. Frames are not being lost in pairs by
  coincidence; battery frames are not being lost at all.
- The gap timing agrees. Inside a pair the frames are 60 ms apart; where a run of 4 joins
  two pairs the gap is 189 ms, which is exactly the two gaps that would have straddled a
  missing 0x81 frame.

So a run of 4 is two cycles with nothing missing from either, and position modulo the cycle
length attributes it correctly. The check that this is sound: inside the merged runs of one
capture, 143 frames carried a register whose value is fixed per pack (0x54, 0x55, 0x60,
0x90, 0xA0, 0xA1), and **all 143 landed on the pack that value belongs to, none wrong.**
Replaying the captures with the change drops the discard rate from ~12 % to ~0.5 %, and on
the live bus from ~11 % to ~0.1 %, while the one-serial-per-slot assertion still holds.

What would defeat it is a battery frame *and* an 0x81 frame going missing in the same run,
in a pattern that leaves the length a multiple of the cycle anyway. That takes two
coincident losses of a kind the parity measurement says are not happening, and the old rule
was paying a sixth of the data every day to guard against it.

The cycle length is *learned*, not hard-coded, and the histogram behind it decays so that
recent cycles outweigh old ones. That is what lets the split survive a pack being added or
removed instead of stalling on a length the bus no longer uses.

**Registers 0x54/0x55 must be accumulated per device.** The serial arrives split over two
registers, and with two packs interleaved on one bus a single shared pair of buffers will
splice one pack's prefix onto the other's number. Each device needs its own halves.

Replaying five real captures through this scheme puts exactly one serial number in each
slot. See `test/replay_dump.cpp`, which is the check that matters — the hand-written unit
vectors only prove the arithmetic, not the attribution. The captures themselves are kept
out of this repository: they contain the devices' real serial numbers, and they are
evidence about one particular installation rather than about the protocol.

### What the split was actually for: how the two packs share current

The reason to separate the packs is that averaging them hides the one thing worth knowing.
Per-pack current (0x02) over four captures of the same installation, two packs in parallel:

| capture | pack 1 (SoC 71–73 %) | pack 2 (SoC 99 %) | charger output |
|---|---|---|---|
| before the pack firmware update | +1.25 A | −0.20 A (down to −2.10 A) | 2.0 A |
| just after it | +1.67 A | +0.50 A | 3.2 A |
| a few hours later, charging | +0.46 A | +0.58 A | 2.4 A |
| that evening, discharging | **−0.64 A** | **−0.00 A** (range −0.10…+0.12) | 0.24 A |

Charging is shared. Discharging, in the last capture, is not: pack 1 supplied the entire
house load and pack 2 supplied nothing, *despite sitting 26 SoC points higher*. Two packs in
parallel share a terminal voltage, so the one with the higher open-circuit voltage should
supply **more** current, not exactly none. And pack 2 is not incapable of it — before the
firmware update it was discharging at up to 2.1 A.

Two cautions on reading too much into this. The load was small (~0.6 A total), where the
split is dominated by millivolt differences and cable resistance; and a persistent SoC gap
between parallel packs is *not* by itself evidence of anything, because coulomb-counted SoC
drifts apart whenever one pack is taken off the bus and is never re-synced. The current
measurement is the evidence; the SoC gap is not. The measurement that would settle it is a
real load — a few tens of amps — with both packs' 0x02 recorded through it.

## Device identity registers (both node types)

These are the same on 0x85 and 0x81, and every field was read off the Dometic Power app's
device page **before** decoding it: three devices × four fields, all twelve matching.

| reg | bytes | meaning | status |
|-----|-------|---------|--------|
| 0x54 | `idx K A A` | three ASCII characters, the serial prefix (`d1..d3`) | CONFIRMED (app + label) |
| 0x55 | `idx n n n` | serial number, 24-bit big-endian in `d1..d3` | CONFIRMED (app + label) |
| 0x60 | `f0 00 A 00` | **`d2` = bus address** — the app's "Address" field: 1, 11, 13 | CONFIRMED (app) |
| 0xA0 | `00 I Mh Ml` | `d1` = IAD, `d2 d3` = model number (battery 5/0008, charger 1/0005) | CONFIRMED (app) |
| 0xA1 | `Ma mi x x` | **firmware version `d0`.`d1`** — 5.1 on the packs, 5.4 on the charger | CONFIRMED (app) |

The prefix from 0x54 concatenated with the number from 0x55 reproduces the serial printed
on the pack's label exactly, for all three devices.

> Serial numbers are **redacted throughout this file** (`KAA2****53`) and the example frame
> bytes in the tests are fictional. They identify specific hardware and have no bearing on
> the protocol: what matters is the field layout, not the value in it. Substitute your own
> when reproducing this.

Three registers previously written off as "constant / UNRESOLVED identity" are therefore
resolved. The lesson is worth keeping: they looked constant only because every capture was
short and single-device. A field that never moves is not necessarily meaningless — it may
just be identity, and identity is exactly what was needed here.

`d0` of 0x54/0x55 is **not** a fixed index, as it first appeared: it reads `0F` on pack 1,
`00` on pack 2 and `01` on the charger, and it is the same in both registers for a given
device. Only `d1..d3` carry the serial, which is why the decoder ignores `d0` entirely
rather than validating it.

`d0` of 0x60 is `60` on pack 1 and `40` on pack 2 / `42` on the charger. Bit 0x20 is set
only on the device at address 1, so it may be a master/first-in-chain flag — **one
observation per device, no better than a guess.**

### Bus address is not a stable identity

The update changed pack 2's address from 3 to 11 and the charger's from 9 to 13, while the
serials stayed put. Anything that has to survive a firmware update — Home Assistant entity
IDs, long-run history — must key on the **serial number**. The address is worth publishing
so that a reshuffle is visible in the history, but not worth keying on.

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
| 0x54 | `idx K A A` | serial prefix, three ASCII characters in `d1..d3` | text | CONFIRMED (app + label) |
| 0x55 | `idx ** ** **` | serial number, 24-bit big-endian in `d1..d3` (= 2****53) | — | CONFIRMED (app + label) |
| 0x60 | `f0 00 A 00` | `d2` = bus address. `d0` is not the same on both packs — 0x60 on accu 1, 0x40 on accu 2, 0x42 on the charger — and each device keeps its own value across a firmware update and an address change, so it is a device property rather than a slot or address artefact | — | CONFIRMED (app) for `d2`; `d0` UNRESOLVED |
| 0xA0 | `00 I 00 08` | `d1` = IAD, `d2 d3` = model number (5 / 0008) | — | CONFIRMED (app) |
| 0xA1 | `05 01 x x` | firmware version `d0`.`d1` | — | CONFIRMED (app) |
| 0x14 | `a b c 0A` | **new in firmware 5.1** — did not exist on 4.x. Three byte values in the 75–76 range plus a constant 10. **Not state of charge** — see the correction below | ? | UNRESOLVED |
| 0x90 | `01 0E 01 0E` on 4.x; `00 00 00 x` on 5.1 | **not a constant** — see the correction below. Held 270/270 through everything on 4.x; on 5.1 the first three bytes are zero and `d3` is a small per-pack value that *moves*: the capture had accu 1 = 1 and accu 2 = 0 over 39 frames, a later live reading had 1 and 2 | — | UNRESOLVED |
| 0x0C | `02 EE FF FF` | always an exact multiple of 10, and **byte-identical on both packs** in every cycle of every two-pack capture, including moments when their SoCs are 26 points apart — so it is not pack-local. Over one day it read 730 → 740 → 750 and never fell. The earlier reading that its centre rises with SoC does not survive that; see the correction below | ? | UNRESOLVED |
| 0xC0 / 0xF1 | all zero | never move; candidate alarm/fault bitmaps | — | UNRESOLVED |
| 0xF2 | `00 02 00 00` on accu 1, `00 00 00 00` on accu 2 | **differs per device, and is a state rather than a constant.** Accu 1 has read 2 in every capture, including when it was alone on the bus. Accu 2 has read 0 in every capture except the ~2 minutes right after its own firmware update, where it read 2 and then fell back to 0 — the transition is in the capture. It is not a discharge-enable flag: accu 2 was discharging at up to 2.1 A before the update while reading 0 | — | UNRESOLVED |

> **Correction: 0x90 is not a constant, and it is not two temperature sensors.**
> On firmware 4.x this register read `01 0E 01 0E` — two identical 16-bit values of 270 —
> which is why it was read here as two sensors at 27.0 °C. It was then downgraded to
> DOUBTED because both halves held exactly 270 through a 504 Wh charge at up to 49.7 A.
> After the update to 5.1 it reads **zero in the first three bytes on both packs**, with only
> a small per-pack value left in `d3`. A constant does not change
> across a firmware update, so the "constant" reading was wrong too; the °C interpretation
> was never anything more than a plausible scaling of a number that never moved. It is now
> mirrored raw by the firmware with no unit attached, and the claim should be withdrawn
> wherever it was published.
>
> The general lesson is the one recorded under the identity registers: **a field that never
> moves is unexplained, not explained.** The confident reading and the correct reading were
> distinguishable only by data that did not exist yet.

> **Correction: 0x14 is not per-cell state of charge, and 0x0C does not track state of charge.**
> Both readings were written down while the two packs were still being averaged into a single
> set of entities. Splitting them apart killed both.
>
> 0x14 was read as a per-cell or per-string SoC because its three bytes sat in the SoC range
> and differed between the packs (76/75/76 against 75/75/75). They differ the way two
> measurements of the same thing differ. In the four two-pack captures the packs' own SoCs
> are **73 % and 99 %** — 26 points apart — while 0x14 reads 75–76 on *both*, and in the most
> recent capture both packs report the identical triple `4C 4B 4C`. A per-cell SoC on a pack
> that is 99 % full cannot read 75. Whatever 0x14 is, it does not follow the pack's charge
> state, and the ±1 wander is shared between packs rather than independent.
>
> 0x0C was read as rising with SoC, on 740 at 50 % and 780 at 73 %. Across a day of captures
> it read 730 (SoC 64), 730 (66), 740 (71), 750 (73) and 750 (73) — and it is byte-identical
> on two packs whose SoCs are 73 and 99. It moves in exact steps of 10 and, over that whole
> day, only upward. That is the shape of a slowly accumulating quantity that happens to
> correlate with SoC on a day when SoC was also rising, not of an SoC reading.
>
> **The test that separates these.** Both hypotheses now make a prediction that a deep
> discharge falsifies in one direction or the other: an SoC-like field must come back down,
> an accumulator cannot. Neither register has yet been watched through one.
>
> The methodological point is the reason multi-pack support was worth building at all. A
> second, nominally identical device on the same bus is a control. Two of the register
> readings here survived years of single-pack observation and did not survive a week of
> having something to compare against.

> **The firmware update destroyed data, and there was no warning.** Updating accu 1 from
> 4.2 to 5.1 reset its cumulative energy counters (0x35) from 3530 / 2956 Wh to 1–2 Wh.
> Those counters were the only record of the pack's lifetime throughput. Anything derived
> from them — cycle counts, long-run degradation — restarts from zero at the update, and
> the discontinuity has to be carried forward in any analysis that spans it. **Take a full
> register dump before updating any node on this bus**; the baseline capture taken before
> this update is the only reason the change is documented at all rather than merely noticed.
>
> *Cell voltages across the update — checked and not concluded.* The four-cell sum from
> 0x56/0x57 sat **106 mV above** the terminal voltage on 4.x and **43 mV below** it on 5.1,
> which looks at first like the update fixing a reporting error. It is not evidence of one:
> the two captures were taken under different load, and internal resistance puts the cell
> sum above the terminals on discharge and below them on charge, which is the sign flip
> observed. Distinguishing a firmware fix from ordinary loading needs both captures taken at
> the same current, which we do not have. Recorded so it is not re-derived as a discovery.

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
| 0x54 | `01 41 43 44` | serial prefix, three ASCII characters in `d1..d3` ("ACD") | text | CONFIRMED (app + label) |
| 0x55 | `00 ** ** **` | serial number, 24-bit big-endian in `d1..d3` (= 2****99) | — | CONFIRMED (app + label) |
| 0x60 | `42 03 0D 00` | `d2` = bus address (13; was 9 before the update) | — | CONFIRMED (app) |
| 0xA0 | `01 01 00 05` | `d1` = IAD, `d2 d3` = model number (1 / 0005) | — | CONFIRMED (app) |
| 0xA1 | `05 04 03 04` | firmware version `d0`.`d1` = 5.4 | — | CONFIRMED (app) |
| 0x26 / 0xD0 | constant | `01 00 05 03` / `00 20 00 00` | — | UNRESOLVED |
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
- **What 0x0C is.** It sits on an exact multiple of 10 and is **byte-identical on both packs
  in every cycle** while their SoC (0x0B) is 26 points apart, so it is not pack-local: either
  a bus-level or bank-level figure that both nodes echo, or something the packs derive from a
  shared input. Across one day it read 730 → 740 → 750 and never fell. Both earlier readings
  are now dead — not a cycle counter, and not SoC-following either. What is left is a slow,
  coarse, shared quantity, and the single measurement that would separate an accumulator from
  a state is a **deep discharge**: an accumulator cannot come back down.
- **What 0x14 is** (new in firmware 5.1, absent on 4.x). Three bytes in the 75–76 range plus
  a constant 10. It is **not** per-cell state of charge: the pack at 99 % reports the same
  75–76 as the pack at 73 %, and in the latest capture both packs report the identical
  `4C 4B 4C`. The values wander by ±1 and the two packs wander together, which is what a
  shared input looks like and not what four independent cells look like. Whether it tracks
  pack voltage — the one input the two packs genuinely share, since they sit in parallel —
  is the obvious next test, and a charge to absorption voltage would answer it: a
  voltage-derived figure has to move a long way while 0x0B barely moves at all.
- **What `d3` of 0x90 is.** On firmware 5.1 the register reads `00 00 00 x` with a small
  per-pack `x`. The capture showed 1 on pack 1 and 0 on pack 2, stable across all 39 frames,
  which looked like a fixed per-pack flag; a later live reading showed 1 and **2**, so it is
  not fixed. It is small, per-pack and slow-moving — a state or a count, not an identifier.
  What matters is that it is per-pack at all, which the old "constant 270" reading obscured
  completely. Noting it here rather than quietly correcting the earlier sentence: a value
  that appears stable in one capture is only evidence that it is slow.
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
> **Superseded by the firmware update.** The firmware update moved it: 0x90 now reads
> `00 00 00 x` with a per-pack `x`. So it was never a configured constant either — the
> conclusion drawn here was right to reject the temperature reading and wrong in what it
> put in its place. See the correction under the 0x85 register table.
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
