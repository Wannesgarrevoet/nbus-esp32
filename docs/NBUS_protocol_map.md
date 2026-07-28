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
| 0x14 | `a b c 0A` | **new in firmware 5.1** — did not exist on 4.x. Three temperatures as `°C + 50`, one byte each, whole degrees, and **three physically separate sensors** — they spread to 2 °C in a fixed order under a 50 A charge. Responds fast; this is the register to use for live pack temperature. `d3 = 0x0A` is constant and is probably the scale factor 10 that turns them into 0x0C's units. **Not state of charge** — see the correction below | °C | CONFIRMED (independent sensor) |
| 0x90 | `01 0E 01 0E` on 4.x; `00 00 00 n` on 5.1 | **not a constant and not a temperature** — see the correction below. On 5.1 the first three bytes are zero and `d3` is a small per-pack number that matches the pack's **position in the poll cycle**: 1 on slot 1 and 2 on slot 2, in every one of 1364 consecutive complete cycles, without an exception. An earlier capture read 1 and 0, so the numbering is not fixed for all time | — | LIKELY for `d3` = poll-cycle index; `d0..d2` UNRESOLVED |
| 0x0C | `02 E4 FF FF` | 16-bit in `d0 d1`: **pack temperature** as `(°C + 50) × 10`, so 730 = 23 °C. Always an exact multiple of ten because the pack measures whole degrees; it dithers when a sensor sits on a degree boundary. **Lagged** — a first-order response with a time constant of about 3 hours. Unity gain, so it is right once things settle, but it trails badly during a change; prefer 0x14 for a live reading. Neither an SoC reading nor a counter — see the corrections below | 0.1 °C, offset 50 | CONFIRMED (independent sensor) |
| 0xC0 / 0xF1 | all zero | never move; candidate alarm/fault bitmaps | — | UNRESOLVED |
| 0xF2 | `00 02 00 00` on both accus since 2026-07-27 20:07 | **a state, and no longer one that tells the packs apart.** Accu 1 has read 2 in every capture, including when it was alone on the bus. Accu 2 read 0 for months, went to 2 for ~2 minutes right after its own firmware update, fell back, and then went to 2 again on 2026-07-27 at 20:07 local and has stayed there since — so "accu 2 reads 0" is dead. It is not a discharge-enable flag: accu 2 was discharging at up to 2.1 A while reading 0. The 20:07 transition is the one to explain; it falls within minutes of the solar charger dropping from 2.1 A to zero | — | UNRESOLVED |

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

> **`d3` of 0x90 is probably the pack's index in the poll cycle.** A live capture of 4096
> frames on 2026-07-28 split into 1364 runs, every one of them exactly two frames long, so
> nothing had to be dropped and nothing was merged. Across it 0x90 read `00 00 00 01` on
> slot 1 (37 frames) and `00 00 00 02` on slot 2 (36 frames), with no third value anywhere.
> It sits alongside 0x0B, 0x54, 0x55 and 0x60 as one of five registers that are constant
> within a pack and differ between packs.
>
> That would make it the first field in the protocol that states the split **from inside**.
> Everything else here attributes frames from the outside — by counting position between
> two charger frames — and is checked against the serial, which arrives only once per poll
> of 0x54 and 0x55. A per-frame index would confirm the same split on every frame.
>
> It is LIKELY rather than CONFIRMED because it is one capture of one bus topology, and
> because an earlier capture read 1 and **0**, which this reading does not explain. The test
> that settles it is to power one pack down and back up so the poll order changes: if `d3`
> follows the new position it is an index the master hands out, and it can be used to
> *verify* the ordinal split but never to replace it; if it stays with its pack it is a
> device property, and then it identifies a pack on every single frame. The second outcome
> is the valuable one, so the test is worth doing deliberately rather than waiting for it to
> happen by accident.

> **Correction: 0x14 and 0x0C are temperature, not state of charge and not an accumulator.**
> Both SoC readings were written down while the two packs were still being averaged into a
> single set of entities. Splitting them apart killed both; a night of Home Assistant history
> then supplied what they actually are. The path is worth keeping because two wrong answers
> were passed through on the way, and both were plausible at the time.
>
> 0x14 was read as a per-cell or per-string SoC because its three bytes sat in the SoC range
> and differed between the packs (76/75/76 against 75/75/75). They differ the way two
> measurements of the same thing differ. In the four two-pack captures the packs' own SoCs
> are **73 % and 99 %** — 26 points apart — while 0x14 reads 75–76 on *both*, and in the most
> recent capture both packs report the identical triple `4C 4B 4C`. A per-cell SoC on a pack
> that is 99 % full cannot read 75.
>
> 0x0C was read as rising with SoC, on 740 at 50 % and 780 at 73 %. Across a day of captures
> it read 730 (SoC 64), 730 (66), 740 (71), 750 (73) and 750 (73), and in every one of those
> it was byte-identical on two packs whose SoCs are 73 and 99. Its replacement — a slowly
> accumulating quantity shared by both packs — did not survive either: it reverses, so it
> accumulates nothing, and on 2026-07-28 the two packs read 740 and 730 at the same moment.
>
> **Both are the same quantity, and that quantity is temperature.** See the temperature
> encoding section below. 0x0C is `(°C + 50) × 10`; the three 0x14 bytes are `°C + 50` in
> whole degrees. Everything that had to be retracted follows from that and needed no separate
> explanation: an exact multiple of ten because the source is a whole degree; reversal because
> temperature reverses; a difference between packs because they do not sit at the same
> temperature; dithering because a sensor can land exactly on a degree boundary; and an
> apparent SoC correlation because the day it was measured, the sun warmed the van while the
> panels charged the packs.
>
> The methodological point is the reason multi-pack support was worth building at all. A
> second, nominally identical device on the same bus is a control. Two of the register
> readings here survived years of single-pack observation and did not survive a week of
> having something to compare against. The temperature answer needed a third thing: an
> independent sensor measuring the same physical quantity, which is what the IBS meter and
> the van's own climate sensors are.

## The temperature encoding

Every temperature on this bus is `(raw / 10) − 50` degrees Celsius. The offset makes the
range roughly −50 to +205 °C, and it is why these registers all read in the 700–1000 band
and never looked like temperatures. Note that the more common automotive convention is an
offset of 40, not 50; 50 is what the data fits, but it is not the value to guess from
habit if a fourth temperature register turns up.

| where | register | resolution | example |
|-------|----------|-----------|---------|
| battery | 0x0C, 16-bit in `d0 d1` | whole degrees, so always ×10 | 730 = 23 °C |
| battery | 0x14, bytes `d0 d1 d2` | whole degrees, offset only, no ×10 | 73 = 23 °C |
| charger | 0x11, 16-bit in `d0 d1` | full 0.1 °C — consecutive counts observed | 815 = 31.5 °C |

The first evidence was a night of Home Assistant history on 2026-07-27/28, in which
everything cooled from about 26 °C to 22 °C. Against the IBS meter's battery temperature — an
independent sensor, on the same bank, from another manufacturer — the packs' 0x14 byte 0
correlates at **r = 0.93 and 0.83** and 0x0C at **0.82 and 0.83**, while the van's inside
and outside temperatures, used as controls, reach only 0.50–0.53. After subtracting the
offset, the packs read **+0.4 to +1.2 °C** above the IBS with a standard deviation below
0.7 — the size of a difference in sensor placement, not of a wrong scale.

The charger's 0x11 is the stronger evidence of the two, because it is not a correlation.
Over the 24 h to 2026-07-28 it peaks at 973 → **47.3 °C** at 19:02, when the panels were
delivering 3.05 A and the van read 30.0 °C inside — 17.3 °C above its own surroundings —
and falls to 677 → 17.7 °C at 03:01 at zero output. A heatsink does that; the absolute
values only land in the right place if the offset really is 50, and the same constant has
now been read off two devices from different manufacturers.

The cold end is the weaker half of that argument and should not be overstated. At 02:13 the
charger reads 18.4 °C against 18.5 °C outside, which looks like an exact settle, but at its
own minimum an hour later it reads 17.7 °C against 20.0 °C outside — 2.3 °C *below* ambient,
which no passive heatsink does. Either the charger's sensor is offset from the van's outside
sensor, or the two are measuring genuinely different air. The overnight *shape* is what
carries the argument; the overnight *absolute agreement* is a coincidence of one sample.

**Fahrenheit is excluded, and not by the offset fit.** `raw − 50` read as °F crosses
`(raw / 10) − 50` read as °C at exactly 22.5 °C, and the first night of data sat right on
that crossing — so the mean bias against the IBS actually *favoured* Fahrenheit (−0.21 and
+0.07 °C, against Celsius's +0.63 and +1.14). The discriminator is the slope: one Fahrenheit
count is 0.556 °C, one Celsius count is 1.000. Whenever this reading is re-tested, test the
slope; the offset fit will agree with the wrong answer.

### The 2026-07-28 morning settles it

The map asked for a reversal, and got one for free. Between 00:00 and 10:00 the packs cooled
from 24 °C to their minimum at 08:00–09:00 and then turned back up, while the solar charger
went from nothing to 5.1 A. Over 66 ten-minute samples against the IBS meter:

| | vs IBS | vs charge current | slope on IBS | offset from the fit |
|---|---|---|---|---|
| accu 1 0x0C | **+0.98** | −0.53 | 0.952 ± 0.028 | **49.9** |
| accu 2 0x0C | **+0.98** | −0.55 | 0.982 ± 0.029 | **50.5** |
| accu 1 0x14 b0 | +0.95 | −0.28 | 1.075 ± 0.056 | 52.4 |
| accu 2 0x14 b0 | +0.97 | −0.35 | 1.170 ± 0.042 | 54.4 |
| charger 0x11 | −0.52 | **+0.96** | — | — |

Three things fall out of this at once.

**The offset is 50, to within half a degree.** It no longer has to be assumed: extrapolating
the regression to 0 °C gives 49.9 and 50.5 on the two packs independently. The earlier
"50 ± 1" was as good as the cooling curve allowed; a turning point is worth more than a
longer straight line. 0x14's single byte gives a worse offset here (52–54) because
one-degree quantisation on a 5 °C swing steepens the apparent slope. *(An earlier revision
concluded from this that 0x0C is the estimator to use rather than 0x14. The drive that
afternoon reversed it — see below.)*

**Fahrenheit is now 14 standard errors away** on both packs' 0x0C, against 2 or less for
Celsius. That question is closed.

**Charge current is excluded as the driver, decisively.** This is what the reversal bought.
Current ran 0 → 5.1 A from 06:00 to 10:00 while both packs kept *falling* until 09:00; the
correlation with current is negative on all four pack registers. Nothing charge-related
behaves that way, which retires the state-of-charge and accumulator readings for good rather
than merely leaving them unsupported.

The charger sits on the other side of the same table and is the control that worked. It ran
17.0 → 51.0 °C in five hours, correlating +0.96 with its own output current and −0.52 with
the packs, while they moved one degree. Same encoding, opposite driver: a heatsink under
load against two thermal masses tracking the air. The van's own inside and outside sensors
could not serve as controls here — they have exactly one recorder point in the window and
are flat by artefact, not by physics.

### 0x0C is not derivable from 0x14

`d3 = 0x0A` in 0x14 is still probably the factor 10 that converts its bytes to 0x0C's scale,
but the tempting next step — that 0x0C is a summary of 0x14's three sensors — **does not
survive the full dataset**. Pairing every 0x0C frame with the nearest 0x14 frame in time
across all captures (n = 3825) and testing six aggregators:

| aggregator | exact match |
|---|---|
| byte 2 alone | 62.6 % |
| rounded mean | 57.3 % |
| truncated mean | 50.0 % |
| minimum | 48.8 % |
| maximum | 38.3 % |
| byte 0 alone | 32.9 % |

None of them is the rule, and the afternoon drive explains why: **0x0C lags**, so no
instantaneous pairing can ever reproduce it. An earlier revision of this section claimed the
rounded mean fit every window; that was four hand-picked windows and it was overfitting —
recorded here because it is the third reading of these two registers to be withdrawn.

Over the 3825 overnight pairs `0x0C / 10` and the mean of 0x14's bytes never differ by more
than **1.0 °C**, centred about +0.2 °C. That agreement is a property of a slow night, not of
the registers: under the drive below the same difference reached **5.3 °C**. The three 0x14
bytes themselves differ by 1 °C in 75 % of overnight frames and never by more than 2 °C. So they are
measuring the same pack and are consistent with each other, but 0x0C is sampled or filtered
on its own schedule and cannot be reconstructed from the other three.

### The 2026-07-28 drive: 0x0C is slow, 0x14 is not

Four short engine runs between 11:55 and 13:08 (about 36 minutes of driving in total, from
the Westfalia bus's own `ontsteking` / `motor` and the Orion's `charge_state`) put roughly
50–56 A of alternator charge into the bank and heated it 20 → 29.5 °C in two and a half
hours — four times faster than any change the registers had been characterised on. The two
pack registers came apart immediately, and re-regressing them on the IBS over just that
window shows how far:

| | slope on IBS, slow morning | slope on IBS, fast drive |
|---|---|---|
| accu 1 0x14 b0 | 1.075 | 1.160 |
| accu 2 0x14 b0 | 1.170 | 1.505 |
| accu 1 0x0C | 0.952 | **2.325** |
| accu 2 0x0C | 0.982 | **2.351** |

0x14 keeps roughly unity. 0x0C needs 2.33 °C of real temperature per degree it reports, and
by the end of the drive it read 25 °C while 0x14 on the same pack read 30–31 °C and the IBS
read 29.5 °C. **That is a lag, not a different scale**, because the same register tracked
1:1 through the slow night.

**The evening confirmed it, and measured the time constant.** The prediction written here at
13:20 was that 0x0C would keep climbing toward 30 °C after the engine stopped while 0x14
flattened. It did. 0x14 sat at exactly 30 °C on both packs for the following eight hours
while 0x0C walked up 25 → 26 → 27 → 28 → 29 and stopped, and the IBS held 29.5–30.0
throughout. Fitting the closing gap as an exponential gives a first-order time constant of
**2.7 h on accu 1 and 3.5 h on accu 2**, decaying from 5.6 K and 4.3 K to about 1 K.

That kills the "43 % of the true rate" reading written earlier the same afternoon. **A
first-order lag has unity gain**; under a ramp its output ends up rising at the same rate as
its input, displaced in time. The 2.33 slope was the transient part of a ramp response
measured over 2.5 h — less than one time constant — not a gain. Tonight's full convergence
is the proof: a register with a gain of 0.43 could not have closed to within 1 °C. So:

- **Use 0x14 for temperature, not 0x0C.** 0x0C's offset of 50 is right and its settled value
  is right to about a degree; it simply trails by hours. That is exactly the wrong behaviour
  if it were ever used for a high-temperature warning, and exactly the right behaviour if
  what you want is a pack's bulk temperature rather than its skin.
- At equilibrium tonight both packs read 0x0C = 29 °C against 0x14 = 30 °C, a −1.0 °C
  offset that sits at the edge of the ±1 °C envelope measured overnight. Whether that is
  placement or a rounding rule is not worth chasing yet.

Two other things the drive settled.

**0x14's three bytes are three physically separate sensors.** Overnight they sit within
1 °C of each other and could have been one sensor reported three times. Under 50 A they
spread to 2 °C and, more tellingly, in a fixed order — byte 1 consistently the coolest
during the rise (29 / 27 / 28 at 12:47). A single value copied three times cannot do that.

**The hotter pack is the one doing the work.** Accu 1 took 30–35 A throughout while accu 2
took 20–24 A, and accu 1 ran about 1.5–2 °C hotter for the whole drive. That is an
independent physical check on two separate readings at once — the per-pack current split
from the ordinal attribution, and the temperature scale — and neither was used to derive
the other.

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
| 0x1B | `Vh Vl (FF FF)` | panel/input voltage. **The dark test has now run itself**: it sits at 0x000A = 0.10 V all night and steps to 13.28 V within one capture of first light, then 14.86 → 21.99 V as the sun climbs | 0.01 V (0.10 V dark, 13.5–24.5 V lit) | CONFIRMED (dark test) |
| 0x1C | `xh xl (FF FF)` | flickers 0/1/2/5 with no correlation to charge current | ? | UNRESOLVED |
| 0x35 | `Eh El 00 00` | cumulative energy produced — rose 178 → 183 and **only while the panel was delivering**, mirroring the battery's 0x35 layout | ~1 Wh/count | LIKELY |
| 0x11 | `xh xl (FF FF)` | **charger temperature** as `(°C + 50) × 10`, in consecutive counts, so the full 0.1 °C. Correlates **+0.96 with its own output current** and −0.52 with the packs: 17.0 °C at dawn to 51.0 °C at 5.1 A five hours later, while the packs moved one degree. The lag that looked like filtering or accumulation is a heatsink. Same encoding as the packs' 0x0C | 0.1 °C, offset 50 | CONFIRMED (independent sensor) |
| 0x0B | `b0` | 78 — same layout as the battery's SoC register, but the battery reported 56 % at the same moment | % | UNCERTAIN |
| 0x54 | `01 41 43 44` | serial prefix, three ASCII characters in `d1..d3` ("ACD") | text | CONFIRMED (app + label) |
| 0x55 | `00 ** ** **` | serial number, 24-bit big-endian in `d1..d3` (= 2****99) | — | CONFIRMED (app + label) |
| 0x60 | `42 s 0D 00` | `d2` = bus address (13; was 9 before the update). **`d1` is not identity** — it is 3 by day and 0 by night, switching with 0x26 `d3` (see below) | — | CONFIRMED (app) for `d2`; `d1` = charger state |
| 0xA0 | `01 01 00 05` | `d1` = IAD, `d2 d3` = model number (1 / 0005) | — | CONFIRMED (app) |
| 0xA1 | `05 04 03 04` | firmware version `d0`.`d1` = 5.4 | — | CONFIRMED (app) |
| 0x26 | `01 00 05 s` | **not constant** — `d3` is 3 by day and 0 by night, moving in the same capture as 0x60 `d1` | — | LIKELY (charger state) |
| 0xD0 | constant | `00 20 00 00` | — | UNRESOLVED |
| 0x0C / 0xC0 / 0xE0 / 0xF0 / 0xF1 | `FF FF FF FF` or all zero | never move; candidate unsupported-parameter and alarm/fault slots | — | UNRESOLVED |

> **Why 0x1B is read as panel voltage.** It looks like noise at first — 187 distinct values
> in 222 samples, swinging 13.5–24.5 V from one frame to the next. But the sequence is not
> random: it repeatedly dips to ~14.2 V and then jumps straight to ~24.2 V, roughly every
> five samples. That sawtooth is an MPPT sweeping its operating point, and the range sits
> exactly between battery voltage and a 12 V panel's open-circuit voltage. The decisive test
> was to cover the panel and check the value collapses; **nightfall performed it**, and it
> collapses to 0.10 V.

> **The charger has a day/night state, and two registers carry it.** On 2026-07-28 the
> 05:13 capture shows 0x26 = `01 00 05 00` and 0x60 = `42 00 0D 00`; the 06:13 capture shows
> `01 00 05 03` and `42 03 0D 00`, and they have held 3 all day. The same capture is the one
> where 0x1B leaves 0.10 V and charge current first becomes non-zero, so the trigger is
> input from the panel rather than a clock.
>
> Both registers were previously written down as constants, because every capture used to
> characterise them was taken by day. `01 00 05 03` and `42 03 0D 00` are in this document as
> fixed values for exactly that reason — a reminder that "constant across all captures" is
> only as strong as the diversity of the captures, and that hourly sampling around the clock
> is what exposed it. The charger's 0x60 `d1` is the more consequential of the two: 0x60 is
> the bus-address register and its other bytes really are identity, so `d1` was assumed to be
> as well.
>
> What the value 3 *means* is open. It is not a simple boolean, or it would be 1. A bulk /
> absorption / float stage number is the obvious guess, and it predicts that `d1` takes other
> values later in a full charge — which is checkable at no cost from the same hourly captures
> on a day that reaches absorption.

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

### The signals that are not on the bus

A capture is five minutes long; the registers that are still unresolved move over hours.
Those get settled in Home Assistant's recorder instead, and the useful part is that the
recorder also holds **context the N-Bus does not carry**. The bus says how much current is
flowing but not *why*, and "why" is usually what separates two candidate meanings.

The Victron **Orion XS** DC-DC charger is read by Home Assistant directly over BLE and
supplies exactly that. Its entity IDs embed the charger's own serial, written `<serial>`
here:

| entity | tells you |
|--------|-----------|
| `sensor.orion_xs_<serial>_input_voltage` | alternator voltage — **≈0 V parked, ≈14 V with the engine running.** The cleanest "is the vehicle driving" signal available |
| `sensor.orion_xs_<serial>_charge_state` | `off` / bulk / absorption / float — which charge stage the living bank is being driven through |
| `sensor.orion_xs_<serial>_output_voltage` | what the DC-DC is imposing on the bank, independent of what the packs report |
| `sensor.orion_xs_<serial>_off_reason` | why it is not charging (`no_input_power` when parked) |

So the bank has **three** current sources — alternator via the Orion XS, solar via NAD 0x81,
and mains when hooked up — and the packs' 0x02 only shows the sum. Anything that correlates
with "charging" should be checked against which source is actually running before it is
written down: a register that tracks absorption voltage and a register that tracks solar
yield look identical if every capture happens to be taken while driving.

The pending 0x14 test needs this. "Charge to absorption and see whether 0x14 climbs while
0x0B barely moves" is a Victron `charge_state` transition, not something the N-Bus
announces.

### The Westfalia bus, and an independent meter on the same bank

The Ford/Westfalia body bus is a **separate LIN bus with its own wiring**, read by a second
listener that also publishes into Home Assistant (`sensor.nugget_lin_listener_*`). It has
nothing to do with the NDS N-Bus at the protocol level — but it is wired to the same
battery, and that makes it the closest thing to ground truth this project has.

**Its IBS sits on the leisure bank, not on the starter battery.** Measured over 30 h and
2227 sample pairs, `orion_xs_..._output_voltage` minus `nugget_lin_listener_ibs_spanning`
is **+0.118 V with a standard deviation of 0.024 V** — a fixed cable drop between the
charger's terminals and the battery post, not two different batteries drifting apart. The
IBS current also peaks at **+54.5 A**, matching the ~49.7 A the N-Bus packs reported during
the same drive. Same bank, two independent meters.

What that buys:

| `sensor.nugget_lin_listener_…` | what it is worth here |
|---|---|
| `ibs_stroom` | **bank current from outside the BMS**, ~2 Hz. `pack1 0x02 + pack2 0x02` has to equal it. That is a direct test of the ordinal split and of the 0.01 A scaling, and any residual is a load or source wired past the shunt |
| `ibs_temperatuur` | **a battery temperature that actually moves**: 22 → 31 °C over the drive, and it does *not* follow cabin air (31 °C while inside was 23.5 °C). This is the reference curve to test any N-Bus register against — anything claiming to be pack temperature must track this, not the cabin |
| `binnentemperatuur` / `buitentemperatuur` | the control for the above: a register following cabin air is not measuring the battery |
| `ibs_spanning` | second opinion on bank voltage, offset known and constant |
| `ibs_soc` / `ibs_soh` / `ibs_max_capaciteit` | a Ford lead-acid gauge applied to LiFePO4 (it settles on 190 Ah against the packs' 2 × 150 Ah), so not truth — but its *shape* over a cycle is still a comparison |
| `ontsteking`, `motor` | ignition and engine state, independent of the Victron input voltage |
| `compressor`, `waterpomp`, `kraan`, `verwarming_*` | which load just switched — the step in bank current has a named cause |

The temperature line is the important one. The search for a temperature register on the
N-Bus failed so far because there was nothing to correlate against; with `ibs_temperatuur`
recording a real thermal curve, any candidate register can be tested against a day that
contains a 50 A charge instead of against a guess about what 27.0 °C should look like.

## Still to determine

- Whether 0xC0 / 0xF0 / 0xF1 really are alarm bitmaps. They are all zero on a healthy bus,
  which is exactly what an alarm register looks like when nothing is wrong — and also
  exactly what an unused register looks like. **A capture taken during a power loss would
  separate the two**, which makes them worth publishing to MQTT even unnamed. They stayed
  zero throughout the 2026-07-26 drive, including engine cranking, so they are at least not
  set by ordinary events.
- **What 0xF2 = 2 means — and what it is not.** It has been caught changing twice on one
  pack: briefly after a firmware update, and again on 2026-07-27 at 20:07 local, within
  minutes of solar output falling to zero. That coincidence suggested a charge-related state,
  and **the sunrise of 2026-07-28 refuted it**: 0xF2 held `00 02 00 00` on both packs through
  every hourly capture from 00:13 to 10:13, across first light at 06:13 and across charge
  current climbing from 0 to 5.1 A. Nothing charge-related stays still through that. The
  reading now is that 2 is the resting value for a healthy pack and the 20:07 transition was
  pack 2 settling into it late — which puts 0xF2 back alongside 0xC0 / 0xF0 / 0xF1 as a
  register that needs a fault to reveal itself, with the difference that its healthy value is
  2 rather than 0.
- ~~Whether the temperature reading survives a real swing.~~ **Answered on 2026-07-28** — the
  packs reversed at 08:00–09:00 in step with the IBS meter while the charger ran away to
  51 °C on its own current. See *The 2026-07-28 morning settles it* above. The afternoon
  drive then answered the two sub-questions as well: 0x14's bytes **are** three separate
  sensors (they spread 2 °C in a fixed order under 50 A), and 0x0C cannot be reconstructed
  from them because it is a **lagged** quantity, not an aggregate.
- **Whether 0x0C's lag is a filter or thermal mass.** The time constant is now measured at
  2.7–3.5 h (see the drive section), but not explained. A digital filter and a sensor potted
  deeper in the cell stack look identical from the bus. A fast *electrical* transient with no
  thermal content — a big load step at constant ambient — would separate them, since a filter
  would still lag and a buried sensor would have nothing to lag behind.
- **What the charger's state nibble counts.** 0x26 `d3` and 0x60 `d1` are both 3 whenever the
  panel delivers and 0 when it does not. A bulk/absorption/float stage number is the obvious
  guess and predicts other values on a day that charges to absorption; the hourly captures
  will catch it without anything being set up.
- **Whether `d3` of 0x90 belongs to the pack or to the slot.** It now reads 1 and 2 in poll
  order across 1364 consecutive cycles, which is why it is marked LIKELY as a poll-cycle
  index. The open half is what happens when the order changes: power one pack down and back
  up, and either `d3` follows the new position — an index the master assigns — or it stays
  with its pack, in which case it identifies a pack on every frame instead of once per
  identity poll. Until that test is run, it may be read but must not be used to attribute
  frames; that would be circular, since the reading was derived from the attribution.
  Note also that an earlier capture read 1 and **0**, which the index hypothesis does not
  yet account for.
- Whether solar 0x0B (78) is the charger's own cruder SoC estimate alongside the battery's
  coulomb-counted 56 %. Solar 0x11's slow fall is answered: it is the charger cooling down.
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
>
> In hindsight this is a pack warming from 24 °C to 28 °C over two hours of driving and
> then holding its heat after the engine stopped, which is exactly what the reading below
> says it should do. The observation was right and the interpretation was missing a unit.
- Whether solar 0x0B (78) is the charger's own cruder SoC estimate alongside the battery's
  coulomb-counted 56 %. Solar 0x11's slow fall is answered: it is the charger cooling down.
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
